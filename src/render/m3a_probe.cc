// M3a observe-only camera intel probe (Sub-project 2). Builds
// m3a_probe.dll, injected into the running game with mecvr_inject.exe
// (single-step: no OpenXR loader dependency, no XR calls at all).
//
// WHAT IT DOES: passively observes the game's immediate D3D11 context
// (constant-buffer binds/updates, draws, RT/viewport state, shaders)
// plus Present cadence, fingerprints every CB upload, and captures
// bounded full-content dumps with matrix-window prefilters for the
// Decision 1 taxonomy. All vtable slots were resolved by the headless
// slot prover (tests/m3a/m3a_slots_main.cc); OMSetRenderTargets[33]
// additionally matches E1 ground truth.
//
// WHAT IT NEVER DOES: no writes to any game buffer/state/shader, no
// Map/Update calls of its own, no XR, no camera/stereo/gameplay work,
// no game-directory writes. Detours record and forward; Draw hooks
// count only (untimed); expensive hooks carry QPC overhead accounting.

#include <windows.h>

#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11_2.h>
#include <d3d11_3.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi.h>
#include <dxgi1_2.h>

#include <malloc.h>
#include <process.h>
#include <strsafe.h>

#include <chrono>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <memory>
#include <string>

#include "render/bone_palette_classifier.h"
#include "render/native_skeleton_adapter.h"

#ifdef MECVR_M3B
#include "camera/pose_mailbox.h"
#include "camera/view_override.h"
#include "input/game_input_synth.h"
#include "input/recenter.h"
#include "ik/arm_ik.h"
#include "ik/full_body_ik.h"
#include "ik/height_calibration.h"
#include "ik/parkour_intent.h"
#include "ik/native_pose_writer.h"
#include "ik/native_bone_map.h"
#include "openxr/real_xr_backend.h"
#include "openxr/xr_frame_worker.h"
#include "render/live_capture.h"
#include "render/body_overlay.h"
#include "render/stereo_eye.h"
#endif

// Named (not anonymous) namespace: symbols must survive optimization
// with external linkage (M2 lesson: anonymous-namespace DllMain TU
// compiled to an empty object under /O2).
namespace m3a_probe {

// ---- Proven vtable slots (slot prover + E1) -----------------------------
constexpr std::size_t kPresentIndex = 8;
constexpr std::size_t kResizeBuffersIndex = 13;
constexpr std::size_t kCreateDeferredContext = 27;
// ID3D11Device1 appends these methods after the ID3D11Device vtable.
constexpr std::size_t kGetImmediateContext1 = 42;
constexpr std::size_t kCreateDeferredContext1 = 43;
constexpr std::size_t kD12QueueExecuteCommandLists = 11;
constexpr std::size_t kD12DeviceCreateCommandQueue = 9;
constexpr std::size_t kD12DeviceCreateCommandList = 13;
constexpr std::size_t kD12GraphicsDrawInstanced = 12;
constexpr std::size_t kD12GraphicsDrawIndexedInstanced = 13;
constexpr std::size_t kD12GraphicsClose = 9;
constexpr std::size_t kVSSetCB = 7;
constexpr std::size_t kVSSetShaderResources = 5;
constexpr std::size_t kPSSetShader = 9;
constexpr std::size_t kPSSetShaderResources = 8;
constexpr std::size_t kVSSetShader = 11;
constexpr std::size_t kDrawIndexed = 12;
constexpr std::size_t kDraw = 13;
constexpr std::size_t kIASetVertexBuffers = 18;
constexpr std::size_t kIASetIndexBuffer = 19;
constexpr std::size_t kMap = 14;
constexpr std::size_t kUnmap = 15;
constexpr std::size_t kPSSetCB = 16;
constexpr std::size_t kDrawIndexedInstanced = 20;
constexpr std::size_t kDrawInstanced = 21;
constexpr std::size_t kDrawAuto = 38;
constexpr std::size_t kDrawIndexedInstancedIndirect = 39;
constexpr std::size_t kDrawInstancedIndirect = 40;
constexpr std::size_t kGSSetCB = 22;
constexpr std::size_t kIATopo = 24;
constexpr std::size_t kOmSetRT = 33;
constexpr std::size_t kRSSetState = 43;
constexpr std::size_t kRSSetViewports = 44;
constexpr std::size_t kUpdateSubresource = 48;
constexpr std::size_t kClearRTV = 50;
constexpr std::size_t kCSSetCB = 71;
// ExecuteCommandList slot: set from the headless slot-prover output.
// The build FAILS until M3A_EXEC_SLOT_SET flips to 1 (never inject a
// guessed slot: patching the wrong index corrupts the vtable).
#define M3A_EXEC_SLOT_SET 1
constexpr std::size_t kExecuteCommandList = 58;  // Slot-prover proven.
constexpr std::size_t kClearState = 110;
constexpr std::size_t kFlush = 111;
constexpr std::size_t kFinishCommandList = 114;
#if !M3A_EXEC_SLOT_SET
#error "set kExecuteCommandList from m3a_slots_test SLOT output first"
#endif

// ---- Bounds (M3a stays lightweight) --------------------------------------
constexpr std::size_t kMaxVtables = 8;
// A device can expose distinct vtables for the base context and the
// ID3D11DeviceContext1/2/3/4 interfaces. Keep enough bounded slots to observe
// all of them without turning interface discovery into an unbounded registry.
constexpr std::size_t kMaxCtxVtables = 8;
constexpr std::size_t kMaxDeviceVtables = 8;
constexpr std::size_t kCbTableSize = 8192;  // Power of two, open-addressed.
constexpr std::size_t kMaxDumpsPerPhase = 256;
constexpr std::size_t kMaxScanBytes = 65536;  // D3D11 CB views top out at 64K.
// Buffers bigger than a CB view can be bound as (mixed-bind megabuffers):
// fingerprint the 4K prefix for cadence only, no matrix scan, no dumps,
// excluded from main-camera candidacy. Camera constants live in small
// CBs; revisit with targeted offsets only if small-CB intel stalls.
constexpr std::size_t kBigBufferBytes = 65536;
constexpr std::size_t kBigBufferFpPrefix = 4096;
constexpr std::size_t kMaxMatWindowsPerDump = 8;
constexpr std::size_t kMaxPendingMaps = 8;
constexpr std::size_t kPhaseRecheckPresents = 60;

// ---- Hook bookkeeping -----------------------------------------------------
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

struct VtableEntry {
  void** vtable = nullptr;
  void* orig_present = nullptr;
  void* orig_resize = nullptr;
};

struct DeviceEntry {
  void** vtable = nullptr;
  void* orig_create_deferred = nullptr;
};

struct Device1Entry {
  void** vtable = nullptr;
  void* orig_get_immediate = nullptr;
  void* orig_create_deferred = nullptr;
};

struct D12QueueEntry {
  void** vtable = nullptr;
  void* orig_execute = nullptr;
};

struct D12DeviceEntry {
  void** vtable = nullptr;
  void* orig_create_queue = nullptr;
  void* orig_create_list = nullptr;
};

struct D12CommandListEntry {
  void** vtable = nullptr;
  void* orig_draw_instanced = nullptr;
  void* orig_draw_indexed_instanced = nullptr;
  void* orig_close = nullptr;
};

struct CtxEntry {
  void** vtable = nullptr;
  // Sized by kH_Count (see static_assert below the CtxHook enum).
  void* orig[27] = {};
};

enum CtxHook {
  kH_VSSetCB = 0,
  kH_PSSetCB,
  kH_GSSetCB,
  kH_CSSetCB,
  kH_VSSetShaderResources,
  kH_PSSetShaderResources,
  kH_VSSetShader,
  kH_PSSetShader,
  kH_IASetVertexBuffers,
  kH_IASetIndexBuffer,
  kH_DrawIndexed,
  kH_Draw,
  kH_DrawIndexedInstanced,
  kH_DrawInstanced,
  kH_DrawAuto,
  kH_DrawIndexedInstancedIndirect,
  kH_DrawInstancedIndirect,
  kH_Update,
  kH_Map,
  kH_Unmap,
  kH_RSViewports,
  kH_OmRT,
  kH_ClearRTV,
  kH_Execute,
  kH_ClearState,
  kH_Flush,
  kH_FinishCommandList,
  kH_Count,
};
static_assert(kH_Count == 27, "CtxEntry::orig + g_detours sized for 27");

constexpr std::size_t kCtxSlots[kH_Count] = {
    kVSSetCB, kPSSetCB, kGSSetCB, kCSSetCB, kVSSetShaderResources,
    kPSSetShaderResources, kVSSetShader, kPSSetShader, kIASetVertexBuffers,
    kIASetIndexBuffer,
    kDrawIndexed, kDraw, kDrawIndexedInstanced, kDrawInstanced, kDrawAuto,
    kDrawIndexedInstancedIndirect, kDrawInstancedIndirect,
    kUpdateSubresource, kMap, kUnmap, kRSSetViewports, kOmSetRT, kClearRTV,
    kExecuteCommandList, kClearState, kFlush, kFinishCommandList};

VtableEntry g_vtables[kMaxVtables];
volatile LONG g_vtable_count = 0;
DeviceEntry g_devices[kMaxDeviceVtables];
volatile LONG g_device_count = 0;
Device1Entry g_devices1[kMaxDeviceVtables];
volatile LONG g_device1_count = 0;
D12QueueEntry g_d12_queues[4];
volatile LONG g_d12_queue_count = 0;
D12DeviceEntry g_d12_devices[kMaxDeviceVtables];
volatile LONG g_d12_device_count = 0;
D12CommandListEntry g_d12_command_lists[8];
volatile LONG g_d12_command_list_count = 0;
CtxEntry g_ctx[kMaxCtxVtables];
volatile LONG g_ctx_count = 0;

HANDLE g_log = INVALID_HANDLE_VALUE;
HANDLE g_shutdown = nullptr;
#ifdef MECVR_M3B
HANDLE g_pose_thread = nullptr;
mecvr::camera::PoseMailbox g_pose_mailbox;
mecvr::ik::BodyPoseMailbox g_body_pose_mailbox;
std::atomic<mecvr::render::LiveCapture*> g_live_capture{nullptr};
std::atomic<std::uint64_t> g_xr_epoch{0};
std::atomic<std::uint64_t> g_xr_pose_sequence{0};
std::atomic<std::uint32_t> g_stereo_eye{0};
std::atomic<bool> g_stereo_eye_valid{false};
std::atomic<std::uint64_t> g_stereo_pair_epoch{0};
std::atomic<std::uint64_t> g_stereo_pair_pose{0};
std::atomic<bool> g_stereo_enabled{false};
bool g_camera_enabled = false;
bool g_preserve_runtime_pacing = true;
volatile LONGLONG g_camera_overrides = 0;
ID3D11Resource* g_last_override_resource = nullptr;
std::uint64_t g_last_override_present = ~std::uint64_t{0};
mecvr::camera::Quat g_camera_anchor;
mecvr::camera::Vec3 g_camera_anchor_position;
bool g_camera_anchor_valid = false;
std::uint64_t g_camera_anchor_generation = 0;
double g_camera_units_per_meter = 0.0;
bool g_input_enabled = false;
bool g_body_overlay_enabled = false;
bool g_physical_jump_enabled = true;
bool g_physical_crouch_input_enabled = true;
bool g_parkour_input_enabled = false;
mecvr::render::BodyOverlay g_body_overlay;
#endif
CRITICAL_SECTION g_lock;
bool g_lock_ready = false;
volatile LONG g_armed = 1;
volatile LONG g_detaching = 0;
DWORD g_pid = 0;
std::int64_t g_qpc_freq = 0;

// ---- Counters --------------------------------------------------------------
volatile LONGLONG g_presents = 0;
volatile LONGLONG g_draws = 0;  // Draws in the current Present interval.
volatile LONGLONG g_draws_last_frame = 0;
volatile LONGLONG g_state_fail = 0;
volatile LONGLONG g_cb_observed = 0;
volatile LONGLONG g_cb_overflow = 0;
volatile LONGLONG g_captures = 0;
volatile LONGLONG g_drops = 0;
volatile LONGLONG g_candidates = 0;  // CBs with matrix windows seen.
volatile LONGLONG g_dumps_this_phase = 0;
char g_phase[32] = "unset";
std::uint64_t g_present_idx = 0;  // Present hook only.
volatile LONGLONG g_draw_idx = 0;  // All Draw hooks (interlocked).

// Overhead accounting (QPC ticks; Draw hooks untimed by design).
struct Ovhd {
  volatile LONGLONG calls = 0;
  volatile LONGLONG ticks = 0;
  volatile LONGLONG max_ticks = 0;
};
Ovhd g_ovh_setcb, g_ovh_shader, g_ovh_update, g_ovh_map, g_ovh_unmap,
    g_ovh_om, g_ovh_rs, g_ovh_present, g_ovh_execl;

// Command-list execution observation (pass-boundary proxy for the
// deferred-rendering finding R1-4). Counted always; logged bounded.
volatile LONGLONG g_execl_total = 0;
volatile LONGLONG g_execl_drops = 0;
volatile LONGLONG g_execl_this_window = 0;
constexpr LONGLONG kMaxExeclPerWindow = 2000;
volatile LONGLONG g_d12_execs = 0;
volatile LONGLONG g_d12_lists = 0;
volatile LONGLONG g_d12_draws = 0;
volatile LONGLONG g_d12_device_hooks = 0;
volatile LONGLONG g_clear_states = 0;
volatile LONGLONG g_flushes = 0;
volatile LONGLONG g_finish_lists = 0;
volatile LONGLONG g_ik_frames = 0;
volatile LONGLONG g_ik_valid_frames = 0;
volatile LONGLONG g_body_overlay_frames = 0;
volatile LONGLONG g_palette_candidates = 0;
#ifdef MECVR_M3B
volatile LONGLONG g_native_pose_write_attempts = 0;
volatile LONGLONG g_native_pose_write_applied = 0;
volatile LONGLONG g_native_pose_write_rejected = 0;
#endif
bool g_palette_discovery_enabled = false;
mecvr::render::NativeSkeletonAdapter g_native_skeleton_adapter;
mecvr::render::NativePaletteTargetTracker g_native_palette_target;
#ifdef MECVR_M3B
mecvr::ik::NativeBoneMap g_native_bone_map;
bool g_native_bone_map_loaded = false;
#endif

// Current GPU state snapshot (stamped on captures).
ID3D11RenderTargetView* g_rtv0 = nullptr;
UINT g_rtv0_w = 0, g_rtv0_h = 0;
ID3D11DepthStencilView* g_dsv = nullptr;
float g_vp_w = 0.0f, g_vp_h = 0.0f;
UINT g_vp_count = 0;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11Buffer* g_ia_vb0 = nullptr;
ID3D11Buffer* g_ia_ib = nullptr;
UINT g_ia_vb_stride = 0;
UINT g_ia_vb_offset = 0;
DXGI_FORMAT g_ia_ib_format = DXGI_FORMAT_UNKNOWN;
ID3D11ShaderResourceView* g_vs_srv0 = nullptr;
ID3D11ShaderResourceView* g_ps_srv0 = nullptr;
DXGI_FRAME_STATISTICS g_fstats{};
UINT g_last_present_count = 0;

struct DrawCorrelationSample {
  std::uint64_t draw = 0;
  std::uint64_t present = 0;
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ID3D11Buffer* vb = nullptr;
  ID3D11Buffer* ib = nullptr;
  ID3D11ShaderResourceView* vs_srv0 = nullptr;
  ID3D11ShaderResourceView* ps_srv0 = nullptr;
  UINT count = 0;
  UINT start = 0;
  INT base = 0;
  UINT stride = 0;
  UINT offset = 0;
  DXGI_FORMAT index_format = DXGI_FORMAT_UNKNOWN;
  UINT kind = 0;
};
constexpr LONG kMaxDrawCorrelationSamples = 256;
DrawCorrelationSample g_draw_correlation[kMaxDrawCorrelationSamples];
volatile LONG g_draw_correlation_count = 0;

// ---- CB registry (open-addressed, keyed by buffer pointer) -----------------
struct CbRecord {
  ID3D11Buffer* buf = nullptr;
  UINT size = 0;
  UINT id = 0;
  UINT vs_mask = 0, ps_mask = 0, gs_mask = 0, cs_mask = 0;
  std::uint64_t binds = 0, updates = 0, maps = 0;
  std::uint64_t first_present = 0, last_present = 0;
  std::uint64_t fp_last = 0, fp_changes = 0;
  UINT mat_windows = 0;
  UINT palette_offset = 0;
  UINT palette_stride = 0;
  UINT palette_matrices = 0;
  UINT palette_layout = 0;
  UINT rt_w = 0, rt_h = 0;
  float vp_w = 0.0f, vp_h = 0.0f;
  std::uint64_t last_draw_idx = 0;
  ID3D11VertexShader* last_vs = nullptr;
  ID3D11PixelShader* last_ps = nullptr;
  char last_phase[32] = {};
};

CbRecord g_cbtable[kCbTableSize];
UINT g_next_cb_id = 1;  // 0 = none.
UINT g_next_palette_id = 1;
CRITICAL_SECTION g_cb_lock;

struct PendingMap {
  ID3D11Resource* res = nullptr;
  void* pdata = nullptr;
  UINT size = 0;
  UINT bind_flags = 0;
  CbRecord* cb = nullptr;
  bool used = false;
};
PendingMap g_pending[kMaxPendingMaps];

void LogF(const char* fmt, ...);
std::uint64_t HashWords64(const void* data, std::size_t len);

struct PaletteResourceRecord {
  ID3D11Resource* res = nullptr;
  UINT id = 0;
  UINT size = 0;
  UINT bind_flags = 0;
  UINT offset = 0;
  UINT stride = 0;
  UINT matrices = 0;
  UINT layout = 0;
  UINT samples = 0;
  bool dumped = false;
};
constexpr std::size_t kPaletteResourceTableSize = 256;
PaletteResourceRecord g_palette_resources[kPaletteResourceTableSize];
volatile LONGLONG g_noncb_map_samples = 0;

PaletteResourceRecord* PaletteRecordFor(ID3D11Resource* res, UINT size,
                                        UINT bind_flags) {
  const std::size_t first =
      (reinterpret_cast<std::uintptr_t>(res) >> 4) &
      (kPaletteResourceTableSize - 1);
  for (std::size_t i = 0; i < kPaletteResourceTableSize; ++i) {
    auto& record = g_palette_resources[(first + i) &
                                       (kPaletteResourceTableSize - 1)];
    if (record.res == res) {
      if (record.size == 0 && size != 0) record.size = size;
      if (record.bind_flags == 0 && bind_flags != 0)
        record.bind_flags = bind_flags;
      return &record;
    }
    if (record.res == nullptr) {
      record.res = res;
      record.id = g_next_palette_id++;
      if (g_next_palette_id == 0) g_next_palette_id = 1;
      record.size = size;
      record.bind_flags = bind_flags;
      return &record;
    }
  }
  return nullptr;
}

void NotePaletteResourceSample(ID3D11Resource* res, UINT size,
                               UINT bind_flags) {
  PaletteResourceRecord* record = PaletteRecordFor(res, size, bind_flags);
  if (record == nullptr) return;
  ++record->samples;
  if (record->samples == 1) {
    LogF("m3a palette-sample res=%p size=%u bind=%08x present=%llu\n",
         static_cast<void*>(res), size, bind_flags, g_present_idx);
  }
}

#ifdef MECVR_M3B
bool TryBuildNativePoseWrite(PaletteResourceRecord* record,
                             const PendingMap& pending,
                             const mecvr::render::BonePaletteCandidate& candidate,
                             const std::uint64_t content_fingerprint) {
  const auto target = g_native_palette_target.snapshot();
  if (!g_native_bone_map_loaded || !target.verified || record == nullptr ||
      target.resource_id != record->id || pending.pdata == nullptr ||
      pending.size == 0) {
    return false;
  }
  InterlockedIncrement64(&g_native_pose_write_attempts);

  mecvr::ik::HumanoidPoseFrame pose;
  if (!g_body_pose_mailbox.latest(&pose)) {
    InterlockedIncrement64(&g_native_pose_write_rejected);
    return false;
  }
  mecvr::ik::NativePoseMatrices matrices{};
  if (!mecvr::ik::BuildNativePoseMatrices(pose, &matrices)) {
    InterlockedIncrement64(&g_native_pose_write_rejected);
    return false;
  }

  mecvr::render::NativePaletteObservation observation;
  observation.present_index = pending.cb != nullptr
                                  ? pending.cb->last_present
                                  : g_present_idx;
  observation.constant_buffer_id = record->id;
  observation.resource_size = record->size != 0 ? record->size : pending.size;
  observation.content_fingerprint = content_fingerprint;
  observation.candidate = candidate;
  mecvr::ik::NativePoseWriteContext context;
  context.executable_fingerprint = g_native_bone_map.executable_fingerprint;
  context.current_pose_sequence = pose.sequence;
  context.maximum_pose_lag = 2;
  context.observation = observation;
  context.adapter.verified = true;
  context.adapter.constant_buffer_id = observation.constant_buffer_id;
  context.adapter.resource_size = observation.resource_size;
  context.adapter.content_fingerprint = observation.content_fingerprint;
  context.adapter.candidate = observation.candidate;

  // The map is opt-in and loaded from a reviewed title-specific contract.
  // Without one, the default-constructed map fails every writer gate and the
  // game's mapped bytes remain untouched.
  constexpr std::size_t kScratchBytes = 65536;
  thread_local std::array<std::uint8_t, kScratchBytes> scratch{};
  const std::size_t source_size =
      pending.size < kScratchBytes ? pending.size : kScratchBytes;
  const auto status = mecvr::ik::RewriteNativePalette(
      g_native_bone_map, context, pose, matrices, pending.pdata, source_size,
      scratch.data(), scratch.size());
  if (status == mecvr::ik::NativePoseWriteStatus::kApplied) {
    std::memcpy(pending.pdata, scratch.data(), source_size);
    InterlockedIncrement64(&g_native_pose_write_applied);
    return true;
  }
  InterlockedIncrement64(&g_native_pose_write_rejected);
  return false;
}
#endif

void NoteNonCbPalette(ID3D11Resource* res, const PendingMap& pending) {
  if (pending.pdata == nullptr || pending.size < 12 * 48) return;
  PaletteResourceRecord* record =
      PaletteRecordFor(res, pending.size, pending.bind_flags);
  if (record == nullptr) return;
  const bool matrix_sized =
      pending.size <= 16384 &&
      (pending.size % 48 == 0 || pending.size % 64 == 0);
  if (matrix_sized && !record->dumped) {
    wchar_t temp[MAX_PATH] = {};
    wchar_t path[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, temp) != 0 &&
        SUCCEEDED(StringCchPrintfW(
            path, MAX_PATH, L"%smecvr_palette_%lu_%p_%u.bin", temp, g_pid,
            static_cast<void*>(res), pending.size))) {
      HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, pending.pdata, pending.size, &written, nullptr);
        CloseHandle(file);
        record->dumped = written == pending.size;
        LogF("m3a palette-dump res=%p size=%u written=%lu path=%ls\n",
             static_cast<void*>(res), pending.size, written, path);
      }
    }
  }
  const std::size_t scan =
      pending.size < kMaxScanBytes ? pending.size : kMaxScanBytes;
  const auto candidate =
      mecvr::render::ClassifyBonePalette(pending.pdata, scan);
  if (!candidate.valid()) return;
  const std::uint64_t fingerprint = HashWords64(pending.pdata, scan);
  mecvr::render::NativePaletteObservation observation;
  observation.present_index = g_present_idx;
  observation.constant_buffer_id = record->id;
  observation.resource_size = record->size;
  observation.content_fingerprint = fingerprint;
  observation.candidate = candidate;
  if (g_native_skeleton_adapter.observe(observation)) {
    const auto verified = g_native_skeleton_adapter.snapshot();
    LogF("m3a native skeleton SRV palette verified id=%u off=%zu stride=%zu "
         "matrices=%zu layout=%u observations=%llu present=%llu phase=%s\n",
         record->id, verified.candidate.offset, verified.candidate.stride,
         verified.candidate.matrix_count,
         static_cast<unsigned>(verified.candidate.layout),
         static_cast<unsigned long long>(verified.stable_observations),
         static_cast<unsigned long long>(g_present_idx), g_phase);
  }
  g_native_palette_target.observePalette(record->id, candidate, g_present_idx);
#ifdef MECVR_M3B
  TryBuildNativePoseWrite(record, pending, candidate, fingerprint);
#endif
  if (candidate.matrix_count <= record->matrices) return;
  if (record->matrices == 0) InterlockedIncrement64(&g_palette_candidates);
  record->offset = static_cast<UINT>(candidate.offset);
  record->stride = static_cast<UINT>(candidate.stride);
  record->matrices = static_cast<UINT>(candidate.matrix_count);
  record->layout = static_cast<UINT>(candidate.layout);
  LogF("m3a palette-resource res=%p size=%u bind=%08x off=%u stride=%u "
       "matrices=%u layout=%u confidence=%.3f present=%llu phase=%s\n",
       static_cast<void*>(res), record->size, record->bind_flags,
       record->offset, record->stride, record->matrices, record->layout,
       candidate.confidence, g_present_idx, g_phase);
}

std::int64_t SteadyNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::uint64_t QpcNow() {
  LARGE_INTEGER t{};
  QueryPerformanceCounter(&t);
  return static_cast<std::uint64_t>(t.QuadPart);
}

void LogRaw(const char* text, std::size_t len) {
  if (g_log == INVALID_HANDLE_VALUE || text == nullptr || len == 0) return;
  DWORD written = 0;
  WriteFile(g_log, text, static_cast<DWORD>(len), &written, nullptr);
}

void LogF(const char* fmt, ...) {
  char buf[2048];
  va_list args;
  va_start(args, fmt);
  _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
  va_end(args);
  size_t len = 0;
  if (SUCCEEDED(StringCchLengthA(buf, sizeof(buf), &len))) LogRaw(buf, len);
}

void OvhdAdd(Ovhd& o, std::uint64_t ticks) {
  InterlockedIncrement64(&o.calls);
  InterlockedAdd64(&o.ticks, static_cast<LONGLONG>(ticks));
  LONGLONG prev = o.max_ticks;
  while (static_cast<std::uint64_t>(prev) < ticks &&
         InterlockedCompareExchange64(&o.max_ticks,
                                      static_cast<LONGLONG>(ticks), prev) !=
             prev) {
    prev = o.max_ticks;
  }
}

bool PatchSlot(void** vtable, std::size_t index, void* detour, void** orig) {
  if (vtable == nullptr || detour == nullptr || orig == nullptr) return false;
  DWORD old = 0;
  if (VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &old) ==
      0) {
    return false;
  }
  *orig = vtable[index];
  vtable[index] = detour;
  DWORD ignored = 0;
  VirtualProtect(&vtable[index], sizeof(void*), old, &ignored);
  return true;
}

bool UnpatchSlot(void** vtable, std::size_t index, void* orig) {
  if (vtable == nullptr || orig == nullptr) return false;
  DWORD old = 0;
  if (VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &old) ==
      0) {
    return false;
  }
  vtable[index] = orig;
  DWORD ignored = 0;
  VirtualProtect(&vtable[index], sizeof(void*), old, &ignored);
  return true;
}

std::uint64_t HashWords64(const void* data, std::size_t len) {
  // 4-lane unrolled FNV-1a over 8-byte words (alignment-safe via
  // memcpy): breaks the serial byte-chain and cuts dependent loads.
  static const std::uint64_t kOff = 14695981039346656037ull;
  static const std::uint64_t kPrime = 1099511628211ull;
  const unsigned char* p = static_cast<const unsigned char*>(data);
  std::uint64_t h[4] = {kOff, kOff ^ 0x9e3779b97f4a7c15ull,
                        kOff ^ 0xc6a4a7935bd1e995ull,
                        kOff ^ 0x165667b19e3779f9ull};
  std::size_t done_words = 0;
  const std::size_t words = len / 8;
  for (std::size_t w = 0; w < words; w += 4) {
    for (int lane = 0; lane < 4; ++lane) {
      if (w + static_cast<std::size_t>(lane) >= words) break;
      std::uint64_t v = 0;
      memcpy(&v, p + (w + static_cast<std::size_t>(lane)) * 8, 8);
      h[lane] ^= v;
      h[lane] *= kPrime;
      ++done_words;
    }
  }
  for (std::size_t i = done_words * 8; i < len; ++i) {
    h[i % 4] ^= static_cast<std::uint64_t>(p[i]);
    h[i % 4] *= kPrime;
  }
  return h[0] ^ (h[1] + 0x9e3779b97f4a7c15ull) ^ (h[2] << 17) ^ (h[3] >> 11);
}

CbRecord* FindCb(ID3D11Buffer* buf) {
  if (buf == nullptr) return nullptr;
  std::size_t h = (static_cast<std::size_t>(
                       reinterpret_cast<std::uintptr_t>(buf)) >>
                   4) &
                  (kCbTableSize - 1);
  for (std::size_t i = 0; i < kCbTableSize; ++i) {
    const std::size_t slot = (h + i) & (kCbTableSize - 1);
    if (g_cbtable[slot].buf == buf) return &g_cbtable[slot];
    if (g_cbtable[slot].buf == nullptr) return nullptr;
  }
  return nullptr;
}

CbRecord* FindOrAddCb(ID3D11Buffer* buf, UINT size) {
  if (buf == nullptr) return nullptr;
  std::size_t h = (static_cast<std::size_t>(
                       reinterpret_cast<std::uintptr_t>(buf)) >>
                   4) &
                  (kCbTableSize - 1);
  for (std::size_t i = 0; i < kCbTableSize; ++i) {
    const std::size_t slot = (h + i) & (kCbTableSize - 1);
    if (g_cbtable[slot].buf == buf) return &g_cbtable[slot];
    if (g_cbtable[slot].buf == nullptr) {
      g_cbtable[slot].buf = buf;
      g_cbtable[slot].size = size;
      g_cbtable[slot].id = g_next_cb_id++;
      g_cbtable[slot].first_present = g_present_idx;
      InterlockedIncrement64(&g_cb_observed);
      return &g_cbtable[slot];
    }
  }
  InterlockedIncrement64(&g_cb_overflow);
  return nullptr;
}

// Negative cache: resources proven NOT constant buffers, so repeat
// texture/vertex uploads skip QI forever. Bounded; when full, unknown
// resources just pay QI again (counted in overflow).
ID3D11Resource* g_noncb[kCbTableSize] = {};

bool IsKnownNonCb(ID3D11Resource* res) {
  std::size_t h = (static_cast<std::size_t>(
                       reinterpret_cast<std::uintptr_t>(res)) >>
                   4) &
                  (kCbTableSize - 1);
  for (std::size_t i = 0; i < kCbTableSize; ++i) {
    const std::size_t slot = (h + i) & (kCbTableSize - 1);
    if (g_noncb[slot] == res) return true;
    if (g_noncb[slot] == nullptr) return false;
  }
  return false;
}

void MarkNonCb(ID3D11Resource* res) {
  std::size_t h = (static_cast<std::size_t>(
                       reinterpret_cast<std::uintptr_t>(res)) >>
                   4) &
                  (kCbTableSize - 1);
  for (std::size_t i = 0; i < kCbTableSize; ++i) {
    const std::size_t slot = (h + i) & (kCbTableSize - 1);
    if (g_noncb[slot] == res || g_noncb[slot] == nullptr) {
      g_noncb[slot] = res;
      return;
    }
  }
}

// Returns the CB record for a resource, or nullptr for non-CBs.
// Assumes g_cb_lock is held (QI + GetDesc are hook-free).
CbRecord* CbForResource(ID3D11Resource* res) {
  if (res == nullptr || IsKnownNonCb(res)) return nullptr;
  ID3D11Buffer* buf = nullptr;
  if (FAILED(res->QueryInterface(__uuidof(ID3D11Buffer),
                                 reinterpret_cast<void**>(&buf))) ||
      buf == nullptr) {
    MarkNonCb(res);
    return nullptr;
  }
  D3D11_BUFFER_DESC desc{};
  buf->GetDesc(&desc);
  buf->Release();  // Registry holds a raw pointer, never a ref.
  if ((desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) == 0) {
    MarkNonCb(res);
    return nullptr;
  }
  return FindOrAddCb(buf, desc.ByteWidth);
}

// Shared upload path (UpdateSubresource + Map/Unmap capture).
// len must already be clamped to kMaxScanBytes by the caller.
void ScanAndLogCb(CbRecord* rec, const void* data, std::size_t len,
                  const char* source, std::uint64_t content_fingerprint);
void NoteUpload(CbRecord* rec, const void* data, std::size_t len,
                const char* source) {
  // Cost discipline (measured 1.9 ms/call before this): mapped game
  // memory is write-combined, so EVERY pass over it costs ~1 ms at
  // 64K. This path therefore (a) copies once to cacheable scratch,
  // (b) hashes + scans from cache, (c) runs full beats at stride 4
  // (off-beats count + stamp only — update cadence stays exact,
  // content sampling at ~50 Hz is plenty for M3a discovery).
  if (rec == nullptr || data == nullptr || len == 0) return;
  ++rec->updates;
  rec->last_present = g_present_idx;
  const bool full_beat = (rec->updates % 4 == 0) || rec->updates <= 2;
  if (!full_beat) return;
  std::size_t want = len;
  if (rec->size > kBigBufferBytes && want > kBigBufferFpPrefix) {
    want = kBigBufferFpPrefix;  // Megabuffer: prefix fingerprint only.
  }
  void* scratch = _malloca(want);
  if (scratch == nullptr) return;
  memcpy(scratch, data, want);  // Single WC pass; everything below cached.
  const std::uint64_t fp = HashWords64(scratch, want);
  // fp-change line: first sightings + every content change. Carries
  // present/phase/RT for motion-response + pass correlation.
  if (rec->updates <= 2 || fp != rec->fp_last) {
    LogF("m3a fp id=%u fp=%016llx present=%llu phase=%s rt=%ux%u vp=%.0fx%.0f "
         "upd=%llu\n",
         rec->id, fp, g_present_idx, g_phase, g_rtv0_w, g_rtv0_h, g_vp_w,
         g_vp_h, rec->updates);
  }
  if (rec->updates > 1 && fp != rec->fp_last) ++rec->fp_changes;
  rec->fp_last = fp;
  if (rec->size <= kBigBufferBytes) {
    ScanAndLogCb(rec, scratch, want, source, fp);
  }
  _freea(scratch);
}

bool FloatNear(float a, float b, float eps) {
  const float d = a - b;
  return d < eps && d > -eps;
}

// Scans one CB upload for matrix-like 4x4 windows and logs the hits
// plus the leading rows. Bounded: kMaxScanBytes, kMaxMatWindowsPerDump.
void ScanAndLogCb(CbRecord* rec, const void* data, std::size_t len,
                  const char* source, std::uint64_t content_fingerprint) {
  if (rec == nullptr || data == nullptr || len < 64) return;
  const std::size_t scan = len < kMaxScanBytes ? len : kMaxScanBytes;
  const unsigned char* bytes = static_cast<const unsigned char*>(data);
  const auto palette = mecvr::render::ClassifyBonePalette(bytes, scan);
  if (g_palette_discovery_enabled && palette.valid()) {
    mecvr::render::NativePaletteObservation observation;
    observation.present_index = g_present_idx;
    auto* palette_record = PaletteRecordFor(
        reinterpret_cast<ID3D11Resource*>(rec->buf), rec->size,
        D3D11_BIND_CONSTANT_BUFFER);
    observation.constant_buffer_id =
        palette_record != nullptr ? palette_record->id : rec->id;
    observation.resource_size = rec->size;
    observation.content_fingerprint = content_fingerprint;
    observation.candidate = palette;
    if (g_native_skeleton_adapter.observe(observation)) {
      const auto verified = g_native_skeleton_adapter.snapshot();
      LogF("m3a native skeleton palette verified off=%zu stride=%zu "
           "matrices=%zu layout=%u confidence=%.3f observations=%llu "
           "present=%llu phase=%s\n",
           verified.candidate.offset, verified.candidate.stride,
           verified.candidate.matrix_count,
           static_cast<unsigned>(verified.candidate.layout),
           verified.candidate.confidence,
           static_cast<unsigned long long>(verified.stable_observations),
           static_cast<unsigned long long>(g_present_idx), g_phase);
    }
  }
  if (palette.valid() && palette.matrix_count > rec->palette_matrices) {
    if (rec->palette_matrices == 0) {
      InterlockedIncrement64(&g_palette_candidates);
    }
    rec->palette_offset = static_cast<UINT>(palette.offset);
    rec->palette_stride = static_cast<UINT>(palette.stride);
    rec->palette_matrices = static_cast<UINT>(palette.matrix_count);
    rec->palette_layout = static_cast<UINT>(palette.layout);
    LogF("m3a palette id=%u size=%u off=%u stride=%u matrices=%u layout=%u "
         "confidence=%.3f src=%s present=%llu phase=%s\n",
         rec->id, rec->size, rec->palette_offset, rec->palette_stride,
         rec->palette_matrices, rec->palette_layout, palette.confidence, source,
         g_present_idx, g_phase);
  }
  if (g_dumps_this_phase >=
      static_cast<LONGLONG>(kMaxDumpsPerPhase)) {
    InterlockedIncrement64(&g_drops);
    return;
  }
  UINT windows = 0;
  bool first = true;
  for (std::size_t off = 0; off + 64 <= scan; off += 16) {
    float w[16];
    memcpy(w, bytes + off, 64);  // Alignment-safe.
    const bool normal = FloatNear(w[12], 0.0f, 1e-3f) &&
                        FloatNear(w[13], 0.0f, 1e-3f) &&
                        FloatNear(w[14], 0.0f, 1e-3f) &&
                        FloatNear(w[15], 1.0f, 1e-3f);
    const bool transposed =
        FloatNear(w[3], 0.0f, 1e-3f) && FloatNear(w[7], 0.0f, 1e-3f) &&
        FloatNear(w[11], 0.0f, 1e-3f) && FloatNear(w[15], 1.0f, 1e-3f);
    // View-like: orthonormal-ish 3x3 (rotation part) + m15 ~= 1,
    // translation free (this catches REAL views, which the affine
    // test misses).
    const float n0 = w[0] * w[0] + w[1] * w[1] + w[2] * w[2];
    const float n1 = w[4] * w[4] + w[5] * w[5] + w[6] * w[6];
    const float n2 = w[8] * w[8] + w[9] * w[9] + w[10] * w[10];
    const float d01 = w[0] * w[4] + w[1] * w[5] + w[2] * w[6];
    const bool view_like =
        FloatNear(n0, 1.0f, 0.02f) && FloatNear(n1, 1.0f, 0.02f) &&
        FloatNear(n2, 1.0f, 0.02f) && FloatNear(d01, 0.0f, 0.05f) &&
        FloatNear(w[15], 1.0f, 1e-3f);
    // Projection-like: m15 ~= 0 with the perspective -1 in the
    // row-major (m11) or column-major (m14) slot. Catches perspective
    // (incl. jittered: jitter lives in m8/m9, not the signature).
    const bool proj_like =
        FloatNear(w[15], 0.0f, 1e-3f) &&
        (FloatNear(w[11], -1.0f, 0.05f) || FloatNear(w[14], -1.0f, 0.05f));
    if (!normal && !transposed && !view_like && !proj_like) continue;
    const char kind = proj_like ? 'P' : (view_like ? 'V' : 'A');
    if (first) {
      first = false;
      InterlockedIncrement64(&g_dumps_this_phase);
      InterlockedIncrement64(&g_captures);
      LogF("m3a dump id=%u size=%u src=%s present=%llu draw=%llu phase=%s "
           "rtv=%ux%u vp=%.0fx%.0f vs=%p ps=%p\n",
           rec->id, rec->size, source, g_present_idx,
           static_cast<unsigned long long>(g_draw_idx), g_phase, g_rtv0_w,
           g_rtv0_h, g_vp_w, g_vp_h, static_cast<void*>(g_vs),
           static_cast<void*>(g_ps));
      float r0[16];
      memcpy(r0, bytes, scan < 64 ? scan : 64);
      LogF("m3a rows0 id=%u | %.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g | "
           "%.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g\n",
           rec->id, r0[0], r0[1], r0[2], r0[3], r0[4], r0[5], r0[6], r0[7],
           r0[8], r0[9], r0[10], r0[11], r0[12], r0[13], r0[14], r0[15]);
    }
    if (windows >= kMaxMatWindowsPerDump) continue;
    LogF("m3a mat id=%u off=%u T=%d K=%c | %.6g %.6g %.6g %.6g | %.6g %.6g "
         "%.6g %.6g | %.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g\n",
         rec->id, static_cast<unsigned>(off), transposed ? 1 : 0, kind, w[0],
         w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11],
         w[12], w[13], w[14], w[15]);
    ++windows;
  }
  if (windows > rec->mat_windows) {
    if (rec->mat_windows == 0) InterlockedIncrement64(&g_candidates);
    rec->mat_windows = windows;
  }
  rec->rt_w = g_rtv0_w;
  rec->rt_h = g_rtv0_h;
  rec->vp_w = g_vp_w;
  rec->vp_h = g_vp_h;
  rec->last_draw_idx = static_cast<std::uint64_t>(g_draw_idx);
  rec->last_vs = g_vs;
  rec->last_ps = g_ps;
  rec->last_present = g_present_idx;
  StringCchCopyA(rec->last_phase, 32, g_phase);
}

// ---- Detours (record + forward; never modify, never call back) ------------
using VsSetCbFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT,
                                           ID3D11Buffer* const*);
using SetShaderResourcesFn = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
using VsSetShaderFn = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, ID3D11VertexShader*, ID3D11ClassInstance* const*,
    UINT);
using PsSetShaderFn = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*,
    UINT);
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT,
                                               INT);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedInstFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
                                                   UINT, UINT, INT, UINT);
using DrawInstFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT,
                                            UINT, UINT);
using DrawAutoFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using DrawIndirectFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,
                                                ID3D11Buffer*, UINT);
using IaSetVertexBuffersFn = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*, const UINT*,
    const UINT*);
using IaSetIndexBufferFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,
                                                    ID3D11Buffer*, DXGI_FORMAT,
                                                    UINT);
using UpdateFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,
                                          ID3D11Resource*, UINT,
                                          const D3D11_BOX*, const void*, UINT,
                                          UINT);
using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,
                                          ID3D11Resource*, UINT,
                                          D3D11_MAP, UINT,
                                          D3D11_MAPPED_SUBRESOURCE*);
using UnmapFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,
                                         ID3D11Resource*, UINT);
using RSViewportsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
                                               const D3D11_VIEWPORT*);
using OmRTFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
                                        ID3D11RenderTargetView* const*,
                                        ID3D11DepthStencilView*);
using ClearRTVFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,
                                            ID3D11RenderTargetView*,
                                            const FLOAT[4]);
using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,
                                           ID3D11CommandList*, BOOL);
using ClearStateFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using FlushFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using FinishCommandListFn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, BOOL, ID3D11CommandList**);
using CreateDeferredContextFn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11Device*, UINT, ID3D11DeviceContext**);
using GetImmediateContext1Fn = void(STDMETHODCALLTYPE*)(
    ID3D11Device1*, ID3D11DeviceContext1**);
using CreateDeferredContext1Fn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11Device1*, UINT, ID3D11DeviceContext1**);

CtxEntry* EntryFor(void** vtable) {
  const LONG c = g_ctx_count;
  for (LONG i = 0; i < c && i < kMaxCtxVtables; ++i) {
    if (g_ctx[i].vtable == vtable) return &g_ctx[i];
  }
  return nullptr;
}

DeviceEntry* DeviceFor(void** vtable) {
  const LONG count = g_device_count;
  for (LONG i = 0; i < count && i < kMaxDeviceVtables; ++i) {
    if (g_devices[i].vtable == vtable) return &g_devices[i];
  }
  return nullptr;
}

Device1Entry* Device1For(void** vtable) {
  const LONG count = g_device1_count;
  for (LONG i = 0; i < count && i < kMaxDeviceVtables; ++i) {
    if (g_devices1[i].vtable == vtable) return &g_devices1[i];
  }
  return nullptr;
}

void NoteBind(ID3D11DeviceContext* self, int stage /*0 VS,1 PS,2 GS,3 CS*/,
              UINT start, UINT count, ID3D11Buffer* const* bufs) {
  (void)self;
  if (bufs == nullptr || count == 0 || count > 14) return;
  EnterCriticalSection(&g_cb_lock);
  for (UINT i = 0; i < count; ++i) {
    ID3D11Buffer* buf = bufs[i];
    if (buf == nullptr) continue;
    D3D11_BUFFER_DESC desc{};
    buf->GetDesc(&desc);
    CbRecord* rec = FindOrAddCb(buf, desc.ByteWidth);
    if (rec == nullptr) continue;
    ++rec->binds;
    const UINT slot = start + i;
    if (slot < 32) {
      const UINT bit = 1u << slot;
      if (stage == 0)
        rec->vs_mask |= bit;
      else if (stage == 1)
        rec->ps_mask |= bit;
      else if (stage == 2)
        rec->gs_mask |= bit;
      else
        rec->cs_mask |= bit;
    }
    rec->last_present = g_present_idx;
  }
  LeaveCriticalSection(&g_cb_lock);
}

void STDMETHODCALLTYPE HookVSSetCB(ID3D11DeviceContext* self, UINT start,
                                   UINT count, ID3D11Buffer* const* bufs) {
  const std::uint64_t t0 = QpcNow();
  CtxEntry* e = EntryFor(*reinterpret_cast<void***>(self));
  VsSetCbFn orig = e != nullptr ? reinterpret_cast<VsSetCbFn>(e->orig[kH_VSSetCB]) : nullptr;
  if (orig != nullptr) orig(self, start, count, bufs);
  if (g_armed) NoteBind(self, 0, start, count, bufs);
  OvhdAdd(g_ovh_setcb, QpcNow() - t0);
}

void STDMETHODCALLTYPE HookPSSetCB(ID3D11DeviceContext* self, UINT start,
                                   UINT count, ID3D11Buffer* const* bufs) {
  const std::uint64_t t0 = QpcNow();
  CtxEntry* e = EntryFor(*reinterpret_cast<void***>(self));
  VsSetCbFn orig = e != nullptr ? reinterpret_cast<VsSetCbFn>(e->orig[kH_PSSetCB]) : nullptr;
  if (orig != nullptr) orig(self, start, count, bufs);
  if (g_armed) NoteBind(self, 1, start, count, bufs);
  OvhdAdd(g_ovh_setcb, QpcNow() - t0);
}

void STDMETHODCALLTYPE HookGSSetCB(ID3D11DeviceContext* self, UINT start,
                                   UINT count, ID3D11Buffer* const* bufs) {
  const std::uint64_t t0 = QpcNow();
  CtxEntry* e = EntryFor(*reinterpret_cast<void***>(self));
  VsSetCbFn orig = e != nullptr ? reinterpret_cast<VsSetCbFn>(e->orig[kH_GSSetCB]) : nullptr;
  if (orig != nullptr) orig(self, start, count, bufs);
  if (g_armed) NoteBind(self, 2, start, count, bufs);
  OvhdAdd(g_ovh_setcb, QpcNow() - t0);
}

void STDMETHODCALLTYPE HookCSSetCB(ID3D11DeviceContext* self, UINT start,
                                   UINT count, ID3D11Buffer* const* bufs) {
  const std::uint64_t t0 = QpcNow();
  CtxEntry* e = EntryFor(*reinterpret_cast<void***>(self));
  VsSetCbFn orig = e != nullptr ? reinterpret_cast<VsSetCbFn>(e->orig[kH_CSSetCB]) : nullptr;
  if (orig != nullptr) orig(self, start, count, bufs);
  if (g_armed) NoteBind(self, 3, start, count, bufs);
  OvhdAdd(g_ovh_setcb, QpcNow() - t0);
}

void STDMETHODCALLTYPE HookVSSetShaderResources(
    ID3D11DeviceContext* self, UINT start, UINT count,
    ID3D11ShaderResourceView* const* views) {
  CtxEntry* e = EntryFor(*reinterpret_cast<void***>(self));
  auto orig = e != nullptr
                  ? reinterpret_cast<SetShaderResourcesFn>(
                        e->orig[kH_VSSetShaderResources])
                  : nullptr;
  if (orig != nullptr) orig(self, start, count, views);
  if (g_armed && g_palette_discovery_enabled && views != nullptr && start == 0 &&
      count > 0)
    g_vs_srv0 = views[0];
}

void STDMETHODCALLTYPE HookPSSetShaderResources(
    ID3D11DeviceContext* self, UINT start, UINT count,
    ID3D11ShaderResourceView* const* views) {
  CtxEntry* e = EntryFor(*reinterpret_cast<void***>(self));
  auto orig = e != nullptr
                  ? reinterpret_cast<SetShaderResourcesFn>(
                        e->orig[kH_PSSetShaderResources])
                  : nullptr;
  if (orig != nullptr) orig(self, start, count, views);
  if (g_armed && g_palette_discovery_enabled && views != nullptr && start == 0 &&
      count > 0)
    g_ps_srv0 = views[0];
}

void STDMETHODCALLTYPE HookIASetVertexBuffers(
    ID3D11DeviceContext* self, UINT start, UINT count,
    ID3D11Buffer* const* buffers, const UINT* strides, const UINT* offsets) {
  CtxEntry* e = EntryFor(*reinterpret_cast<void***>(self));
  auto orig = e != nullptr
                  ? reinterpret_cast<IaSetVertexBuffersFn>(
                        e->orig[kH_IASetVertexBuffers])
                  : nullptr;
  if (orig != nullptr) orig(self, start, count, buffers, strides, offsets);
  if (g_armed && g_palette_discovery_enabled && start == 0 && count > 0) {
    g_ia_vb0 = buffers != nullptr ? buffers[0] : nullptr;
    g_ia_vb_stride = strides != nullptr ? strides[0] : 0;
    g_ia_vb_offset = offsets != nullptr ? offsets[0] : 0;
  }
}

void STDMETHODCALLTYPE HookIASetIndexBuffer(ID3D11DeviceContext* self,
                                             ID3D11Buffer* buffer,
                                             DXGI_FORMAT format, UINT offset) {
  CtxEntry* e = EntryFor(*reinterpret_cast<void***>(self));
  auto orig = e != nullptr
                  ? reinterpret_cast<IaSetIndexBufferFn>(
                        e->orig[kH_IASetIndexBuffer])
                  : nullptr;
  if (orig != nullptr) orig(self, buffer, format, offset);
  if (g_armed && g_palette_discovery_enabled) {
    g_ia_ib = buffer;
    g_ia_ib_format = format;
  }
}

void STDMETHODCALLTYPE HookVSSetShader(ID3D11DeviceContext* self,
                                       ID3D11VertexShader* vs,
                                       ID3D11ClassInstance* const* ci,
                                       UINT n) {
  const std::uint64_t t0 = QpcNow();
  CtxEntry* e = EntryFor(*reinterpret_cast<void***>(self));
  VsSetShaderFn orig = e != nullptr
                           ? reinterpret_cast<VsSetShaderFn>(
                                 e->orig[kH_VSSetShader])
                           : nullptr;
  if (orig != nullptr) orig(self, vs, ci, n);
  if (g_armed) g_vs = vs;
  OvhdAdd(g_ovh_shader, QpcNow() - t0);
}

void STDMETHODCALLTYPE HookPSSetShader(ID3D11DeviceContext* self,
                                       ID3D11PixelShader* ps,
                                       ID3D11ClassInstance* const* ci,
                                       UINT n) {
  const std::uint64_t t0 = QpcNow();
  CtxEntry* e = EntryFor(*reinterpret_cast<void***>(self));
  PsSetShaderFn orig = e != nullptr
                           ? reinterpret_cast<PsSetShaderFn>(
                                 e->orig[kH_PSSetShader])
                           : nullptr;
  if (orig != nullptr) orig(self, ps, ci, n);
  if (g_armed) g_ps = ps;
  OvhdAdd(g_ovh_shader, QpcNow() - t0);
}

// Draw hooks count only: untimed, two increments, no logging.
void STDMETHODCALLTYPE HookDrawIndexed(ID3D11DeviceContext* self, UINT count,
                                       UINT start, INT base) {
  CtxEntry* e = EntryFor(*reinterpret_cast<void***>(self));
  DrawIndexedFn orig = e != nullptr
                           ? reinterpret_cast<DrawIndexedFn>(e->orig[kH_DrawIndexed])
                           : nullptr;
  if (orig != nullptr) orig(self, count, start, base);
  if (g_armed && g_palette_discovery_enabled) {
    const auto note_srv = [](ID3D11ShaderResourceView* srv) {
      if (srv == nullptr) return;
      ID3D11Resource* resource = nullptr;
      srv->GetResource(&resource);
      if (resource != nullptr) {
        auto* record = PaletteRecordFor(resource, 0, 0);
        if (record != nullptr) {
          g_native_palette_target.noteDraw(
              record->id, g_present_idx, static_cast<std::uint64_t>(g_draw_idx),
              reinterpret_cast<std::uintptr_t>(g_vs),
              reinterpret_cast<std::uintptr_t>(g_ps));
        }
        resource->Release();
      }
    };
    note_srv(g_vs_srv0);
    note_srv(g_ps_srv0);
    const LONG slot = InterlockedIncrement(&g_draw_correlation_count) - 1;
    if (slot >= 0 && slot < kMaxDrawCorrelationSamples) {
      auto& s = g_draw_correlation[slot];
      s = {static_cast<std::uint64_t>(g_draw_idx), g_present_idx, g_vs, g_ps,
           g_ia_vb0, g_ia_ib, g_vs_srv0, g_ps_srv0, count, start, base,
           g_ia_vb_stride, g_ia_vb_offset, g_ia_ib_format, 0};
    }
  }
  InterlockedIncrement64(&g_draws);
  InterlockedIncrement64(&g_draw_idx);
}

void STDMETHODCALLTYPE HookDraw(ID3D11DeviceContext* self, UINT count,
                                UINT start) {
  CtxEntry* e = EntryFor(*reinterpret_cast<void***>(self));
  DrawFn orig =
      e != nullptr ? reinterpret_cast<DrawFn>(e->orig[kH_Draw]) : nullptr;
  if (orig != nullptr) orig(self, count, start);
  if (g_armed && g_palette_discovery_enabled) {
    const auto note_srv = [](ID3D11ShaderResourceView* srv) {
      if (srv == nullptr) return;
      ID3D11Resource* resource = nullptr;
      srv->GetResource(&resource);
      if (resource != nullptr) {
        auto* record = PaletteRecordFor(resource, 0, 0);
        if (record != nullptr) {
          g_native_palette_target.noteDraw(
              record->id, g_present_idx, static_cast<std::uint64_t>(g_draw_idx),
              reinterpret_cast<std::uintptr_t>(g_vs),
              reinterpret_cast<std::uintptr_t>(g_ps));
        }
        resource->Release();
      }
    };
    note_srv(g_vs_srv0);
    note_srv(g_ps_srv0);
    const LONG slot = InterlockedIncrement(&g_draw_correlation_count) - 1;
    if (slot >= 0 && slot < kMaxDrawCorrelationSamples) {
      auto& s = g_draw_correlation[slot];
      s = {static_cast<std::uint64_t>(g_draw_idx), g_present_idx, g_vs, g_ps,
           g_ia_vb0, g_ia_ib, g_vs_srv0, g_ps_srv0, count, start, 0,
           g_ia_vb_stride, g_ia_vb_offset, g_ia_ib_format, 1};
    }
  }
  InterlockedIncrement64(&g_draws);
  InterlockedIncrement64(&g_draw_idx);
}

void STDMETHODCALLTYPE HookDrawIndexedInst(ID3D11DeviceContext* self,
                                           UINT cpi, UINT inst, UINT start,
                                           INT base, UINT si) {
  CtxEntry* e = EntryFor(*reinterpret_cast<void***>(self));
  DrawIndexedInstFn orig = e != nullptr
                               ? reinterpret_cast<DrawIndexedInstFn>(
                                     e->orig[kH_DrawIndexedInstanced])
                               : nullptr;
  if (orig != nullptr) orig(self, cpi, inst, start, base, si);
  InterlockedIncrement64(&g_draws);
  InterlockedIncrement64(&g_draw_idx);
}

void STDMETHODCALLTYPE HookDrawInst(ID3D11DeviceContext* self, UINT cpv,
                                    UINT inst, UINT start, UINT si) {
  CtxEntry* e = EntryFor(*reinterpret_cast<void***>(self));
  DrawInstFn orig = e != nullptr
                        ? reinterpret_cast<DrawInstFn>(e->orig[kH_DrawInstanced])
                        : nullptr;
  if (orig != nullptr) orig(self, cpv, inst, start, si);
  InterlockedIncrement64(&g_draws);
  InterlockedIncrement64(&g_draw_idx);
}

void STDMETHODCALLTYPE HookDrawAuto(ID3D11DeviceContext* self) {
  CtxEntry* e = EntryFor(*reinterpret_cast<void***>(self));
  DrawAutoFn orig = e != nullptr
                        ? reinterpret_cast<DrawAutoFn>(e->orig[kH_DrawAuto])
                        : nullptr;
  if (orig != nullptr) orig(self);
  InterlockedIncrement64(&g_draws);
  InterlockedIncrement64(&g_draw_idx);
}

void STDMETHODCALLTYPE HookDrawIndexedInstIndirect(ID3D11DeviceContext* self,
                                                    ID3D11Buffer* args,
                                                    UINT offset) {
  CtxEntry* e = EntryFor(*reinterpret_cast<void***>(self));
  DrawIndirectFn orig =
      e != nullptr
          ? reinterpret_cast<DrawIndirectFn>(
                e->orig[kH_DrawIndexedInstancedIndirect])
          : nullptr;
  if (orig != nullptr) orig(self, args, offset);
  InterlockedIncrement64(&g_draws);
  InterlockedIncrement64(&g_draw_idx);
}

void STDMETHODCALLTYPE HookDrawInstIndirect(ID3D11DeviceContext* self,
                                             ID3D11Buffer* args,
                                             UINT offset) {
  CtxEntry* e = EntryFor(*reinterpret_cast<void***>(self));
  DrawIndirectFn orig =
      e != nullptr
          ? reinterpret_cast<DrawIndirectFn>(
                e->orig[kH_DrawInstancedIndirect])
          : nullptr;
  if (orig != nullptr) orig(self, args, offset);
  InterlockedIncrement64(&g_draws);
  InterlockedIncrement64(&g_draw_idx);
}

#ifdef MECVR_M3B
bool TryApplyCameraOverride(CbRecord* rec, void* data, std::size_t bytes,
                            ID3D11Resource* resource);
#endif

void STDMETHODCALLTYPE HookUpdate(ID3D11DeviceContext* self,
                                  ID3D11Resource* res, UINT sub,
                                  const D3D11_BOX* box, const void* src,
                                  UINT row_pitch, UINT depth_pitch) {
  (void)sub;
  CtxEntry* e = EntryFor(*reinterpret_cast<void***>(self));
  UpdateFn orig =
      e != nullptr ? reinterpret_cast<UpdateFn>(e->orig[kH_Update]) : nullptr;
  const void* forwarded_src = src;
#ifdef MECVR_M3B
  // Catalyst uses UpdateSubresource for the camera constant buffer.  Keep the
  // fast path allocation-free and only copy the one exact buffer shape that
  // the guarded camera seam understands.  Partial updates are never altered.
  alignas(16) std::uint8_t camera_copy[1136]{};
  if (g_armed && res != nullptr && src != nullptr && box == nullptr) {
    EnterCriticalSection(&g_cb_lock);
    CbRecord* camera_rec = CbForResource(res);
    if (camera_rec != nullptr && camera_rec->size == sizeof(camera_copy)) {
      std::memcpy(camera_copy, src, sizeof(camera_copy));
      if (TryApplyCameraOverride(camera_rec, camera_copy, sizeof(camera_copy),
                                 res)) {
        forwarded_src = camera_copy;
      }
    }
    LeaveCriticalSection(&g_cb_lock);
  }
#endif
  if (orig != nullptr)
    orig(self, res, sub, box, forwarded_src, row_pitch, depth_pitch);
  const std::uint64_t t0 = QpcNow();  // Time OUR work only, never orig.
  if (g_armed && res != nullptr && src != nullptr) {
    EnterCriticalSection(&g_cb_lock);
    CbRecord* rec = CbForResource(res);
    if (rec != nullptr) {
      // Read EXACTLY what the call describes: full resource when the
      // box is null, else the box byte range (buffers: left..right).
      std::size_t len = rec->size;
      if (box != nullptr && box->right > box->left) {
        len = static_cast<std::size_t>(box->right - box->left);
        if (len > rec->size) len = rec->size;
      }
      if (len > kMaxScanBytes) len = kMaxScanBytes;
      NoteUpload(rec, src, len, "update");
    }
    LeaveCriticalSection(&g_cb_lock);
  }
  OvhdAdd(g_ovh_update, QpcNow() - t0);
}

HRESULT STDMETHODCALLTYPE HookMap(ID3D11DeviceContext* self,
                                  ID3D11Resource* res, UINT sub, D3D11_MAP type,
                                  UINT flags, D3D11_MAPPED_SUBRESOURCE* mapped) {
  (void)sub;
  (void)flags;
  CtxEntry* e = EntryFor(*reinterpret_cast<void***>(self));
  MapFn orig =
      e != nullptr ? reinterpret_cast<MapFn>(e->orig[kH_Map]) : nullptr;
  const HRESULT hr =
      orig != nullptr ? orig(self, res, sub, type, flags, mapped) : E_FAIL;
  const std::uint64_t t0 = QpcNow();  // Time OUR work only, never orig.
  if (g_armed && SUCCEEDED(hr) && res != nullptr && mapped != nullptr &&
      (type == D3D11_MAP_WRITE_DISCARD || type == D3D11_MAP_WRITE ||
       type == D3D11_MAP_WRITE_NO_OVERWRITE ||
       type == D3D11_MAP_READ_WRITE)) {
    EnterCriticalSection(&g_cb_lock);
    CbRecord* rec = CbForResource(res);
    UINT size = rec != nullptr ? rec->size : 0;
    UINT bind_flags = rec != nullptr ? D3D11_BIND_CONSTANT_BUFFER : 0;
    bool track = rec != nullptr;
    if (!track && g_palette_discovery_enabled) {
      InterlockedIncrement64(&g_noncb_map_samples);
      ID3D11Buffer* buffer = nullptr;
      if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Buffer),
                                        reinterpret_cast<void**>(&buffer))) &&
          buffer != nullptr) {
        D3D11_BUFFER_DESC desc{};
        buffer->GetDesc(&desc);
        buffer->Release();
        size = desc.ByteWidth;
        bind_flags = desc.BindFlags;
        track = size >= 12 * 48 && size <= 16384 &&
                (bind_flags & D3D11_BIND_SHADER_RESOURCE) != 0;
        if (track) NotePaletteResourceSample(res, size, bind_flags);
      }
    }
    if (track) {
      if (rec != nullptr) ++rec->maps;
      for (std::size_t i = 0; i < kMaxPendingMaps; ++i) {
        if (!g_pending[i].used) {
          g_pending[i].used = true;
          g_pending[i].res = res;
          g_pending[i].pdata = mapped->pData;
          g_pending[i].size = size;
          g_pending[i].bind_flags = bind_flags;
          g_pending[i].cb = rec;
          break;
        }
      }
    }
    LeaveCriticalSection(&g_cb_lock);
  }
  OvhdAdd(g_ovh_map, QpcNow() - t0);
  return hr;
}

#ifdef MECVR_M3B
mecvr::camera::Quat ToCameraQuat(const mecvr::openxr::XrQuaternionf& q) {
  return {q.x, q.y, q.z, q.w};
}

bool TryApplyCameraOverride(CbRecord* rec, void* data, std::size_t bytes,
                            ID3D11Resource* resource) {
  if (!g_camera_enabled || rec == nullptr || data == nullptr ||
      resource == nullptr || rec->size != 1136 || bytes < 112) {
    return false;
  }
  // Apply once per resource per game Present to avoid compounding the same
  // HMD delta if the game maps the block more than once.
  if (resource == g_last_override_resource &&
      g_present_idx == g_last_override_present) {
    return false;
  }
  mecvr::camera::XRFramePoseSnapshot snapshot;
  if (!g_pose_mailbox.latest(&snapshot) ||
      !mecvr::camera::SnapshotUsable(snapshot, SteadyNs(), 100000000)) {
    return false;
  }
  const mecvr::camera::Quat head = ToCameraQuat(snapshot.head.orientation);
  const mecvr::camera::Vec3 head_position = {
      snapshot.head.position.x, snapshot.head.position.y,
      snapshot.head.position.z};
  const bool eye_pose_valid =
      g_stereo_enabled.load(std::memory_order_acquire) &&
      g_stereo_eye_valid.load(std::memory_order_acquire) &&
      g_stereo_eye.load(std::memory_order_acquire) < 2;
  const std::uint32_t eye =
      g_stereo_eye.load(std::memory_order_acquire);
  const mecvr::camera::Quat current =
      eye_pose_valid ? ToCameraQuat(snapshot.views[eye].pose.orientation)
                     : head;
  const mecvr::camera::Vec3 current_position =
      eye_pose_valid
          ? mecvr::camera::Vec3{snapshot.views[eye].pose.position.x,
                                snapshot.views[eye].pose.position.y,
                                snapshot.views[eye].pose.position.z}
          : head_position;
  if (!g_camera_anchor_valid ||
      snapshot.space_generation != g_camera_anchor_generation) {
    // Keep the anchor at the viewer origin, not one eye. Eye separation then
    // comes from the located XrView pose without accumulating a false offset.
    g_camera_anchor = head;
    g_camera_anchor_position = head_position;
    g_camera_anchor_generation = snapshot.space_generation;
    g_camera_anchor_valid = true;
  }
  const mecvr::camera::Quat delta = mecvr::camera::QuatNormalize(
      mecvr::camera::QuatMul(mecvr::camera::QuatConjugate(g_camera_anchor),
                             current));
  mecvr::camera::Vec3 local_translation = mecvr::camera::QuatRotate(
      mecvr::camera::QuatConjugate(g_camera_anchor),
      mecvr::camera::VecSub(current_position, g_camera_anchor_position));
  const double length = mecvr::camera::VecLength(local_translation);
  constexpr double kMaxHeadOffsetMeters = 0.75;
  if (length > kMaxHeadOffsetMeters) {
    local_translation = mecvr::camera::VecScale(
        local_translation, kMaxHeadOffsetMeters / length);
  }
  const bool applied = mecvr::camera::RewriteViewPoseMatrix(
      static_cast<float*>(data), bytes / sizeof(float), 8, delta,
      local_translation, g_camera_units_per_meter,
      mecvr::camera::MultOrder::kColumnVector);
  if (applied) {
    g_last_override_resource = resource;
    g_last_override_present = g_present_idx;
    InterlockedIncrement64(&g_camera_overrides);
    if (g_camera_overrides == 1) {
      LogF("m3b camera override enabled: view=off32 units_per_meter=%.3f "
           "snapshot=%llu\n",
           g_camera_units_per_meter,
           static_cast<unsigned long long>(snapshot.sequence));
    }
  }
  return applied;
}
#endif

volatile LONGLONG g_unmap_matched = 0;  // Pending-map hits (CB writes).

void STDMETHODCALLTYPE HookUnmap(ID3D11DeviceContext* self,
                                 ID3D11Resource* res, UINT sub) {
  (void)sub;
  // Overhead measures OUR work only (never the forwarded call).
  std::uint64_t work = 0;
  std::uint64_t t0 = QpcNow();
  // Capture BEFORE forwarding: the mapping is still valid now.
  if (g_armed && res != nullptr) {
    EnterCriticalSection(&g_cb_lock);
    for (std::size_t i = 0; i < kMaxPendingMaps; ++i) {
      if (g_pending[i].used && g_pending[i].res == res) {
        g_pending[i].used = false;
        InterlockedIncrement64(&g_unmap_matched);
        CbRecord* rec = g_pending[i].cb;
        if (rec != nullptr && g_pending[i].pdata != nullptr &&
            g_pending[i].size > 0) {
          std::size_t len = g_pending[i].size;
          if (len > kMaxScanBytes) len = kMaxScanBytes;
#ifdef MECVR_M3B
          TryApplyCameraOverride(rec, g_pending[i].pdata, len, res);
#endif
          NoteUpload(rec, g_pending[i].pdata, len, "map");
        } else {
          NoteNonCbPalette(res, g_pending[i]);
        }
        break;
      }
    }
    LeaveCriticalSection(&g_cb_lock);
  }
  work += QpcNow() - t0;
  CtxEntry* e = EntryFor(*reinterpret_cast<void***>(self));
  UnmapFn orig =
      e != nullptr ? reinterpret_cast<UnmapFn>(e->orig[kH_Unmap]) : nullptr;
  if (orig != nullptr) orig(self, res, sub);
  OvhdAdd(g_ovh_unmap, work);
}

void STDMETHODCALLTYPE HookRSViewports(ID3D11DeviceContext* self, UINT count,
                                       const D3D11_VIEWPORT* vps) {
  const std::uint64_t t0 = QpcNow();
  CtxEntry* e = EntryFor(*reinterpret_cast<void***>(self));
  RSViewportsFn orig = e != nullptr
                           ? reinterpret_cast<RSViewportsFn>(e->orig[kH_RSViewports])
                           : nullptr;
  if (orig != nullptr) orig(self, count, vps);
  if (g_armed && vps != nullptr && count > 0) {
    g_vp_w = vps[0].Width;
    g_vp_h = vps[0].Height;
    g_vp_count = count;
#ifdef MECVR_M3B
    // Full-width desktop rendering is the normal Catalyst path. Temporal
    // stereo owns eye selection there; a viewport classifier must not
    // overwrite the eye between the two presents in a pair.
#endif
  }
  OvhdAdd(g_ovh_rs, QpcNow() - t0);
}

void STDMETHODCALLTYPE HookOmRT(ID3D11DeviceContext* self, UINT count,
                                ID3D11RenderTargetView* const* rtvs,
                                ID3D11DepthStencilView* dsv) {
  const std::uint64_t t0 = QpcNow();
  CtxEntry* e = EntryFor(*reinterpret_cast<void***>(self));
  OmRTFn orig =
      e != nullptr ? reinterpret_cast<OmRTFn>(e->orig[kH_OmRT]) : nullptr;
  if (orig != nullptr) orig(self, count, rtvs, dsv);
  if (g_armed) {
    g_rtv0 = (rtvs != nullptr && count > 0) ? rtvs[0] : nullptr;
    g_dsv = dsv;
    g_rtv0_w = 0;
    g_rtv0_h = 0;
    if (g_rtv0 != nullptr) {
      ID3D11Resource* res = nullptr;
      g_rtv0->GetResource(&res);
      if (res != nullptr) {
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D),
                                          reinterpret_cast<void**>(&tex))) &&
            tex != nullptr) {
          D3D11_TEXTURE2D_DESC desc{};
          tex->GetDesc(&desc);
          g_rtv0_w = desc.Width;
          g_rtv0_h = desc.Height;
          tex->Release();
        }
        res->Release();
      }
    }
  }
  OvhdAdd(g_ovh_om, QpcNow() - t0);
}

void STDMETHODCALLTYPE HookClearRTV(ID3D11DeviceContext* self,
                                    ID3D11RenderTargetView* rtv,
                                    const FLOAT color[4]) {
  const std::uint64_t t0 = QpcNow();
  CtxEntry* e = EntryFor(*reinterpret_cast<void***>(self));
  ClearRTVFn orig = e != nullptr
                        ? reinterpret_cast<ClearRTVFn>(e->orig[kH_ClearRTV])
                        : nullptr;
  if (orig != nullptr) orig(self, rtv, color);
  OvhdAdd(g_ovh_om, QpcNow() - t0);
}

// ---- Swapchain detours ------------------------------------------------------
void STDMETHODCALLTYPE HookExecute(ID3D11DeviceContext* self,
                                     ID3D11CommandList* list,
                                     BOOL restore) {
  const std::uint64_t t0 = QpcNow();
  CtxEntry* e = EntryFor(*reinterpret_cast<void***>(self));
  ExecuteFn orig = e != nullptr
                       ? reinterpret_cast<ExecuteFn>(e->orig[kH_Execute])
                       : nullptr;
  if (orig != nullptr) orig(self, list, restore);
  if (g_armed) {
    const LONGLONG seq = InterlockedIncrement64(&g_execl_total);
    if (g_execl_this_window < kMaxExeclPerWindow) {
      InterlockedIncrement64(&g_execl_this_window);
      LogF("m3a execl present=%llu seq=%lld rtv=%ux%u vp=%.0fx%.0f\n",
           g_present_idx, seq, g_rtv0_w, g_rtv0_h, g_vp_w, g_vp_h);
    } else {
      InterlockedIncrement64(&g_execl_drops);
    }
  }
  OvhdAdd(g_ovh_execl, QpcNow() - t0);
}

void STDMETHODCALLTYPE HookClearState(ID3D11DeviceContext* self) {
  CtxEntry* e = EntryFor(*reinterpret_cast<void***>(self));
  ClearStateFn orig = e != nullptr
                          ? reinterpret_cast<ClearStateFn>(e->orig[kH_ClearState])
                          : nullptr;
  if (orig != nullptr) orig(self);
  if (g_palette_discovery_enabled) {
    g_ia_vb0 = nullptr;
    g_ia_ib = nullptr;
    g_vs_srv0 = nullptr;
    g_ps_srv0 = nullptr;
    g_ia_vb_stride = 0;
    g_ia_vb_offset = 0;
    g_ia_ib_format = DXGI_FORMAT_UNKNOWN;
  }
  InterlockedIncrement64(&g_clear_states);
}

void STDMETHODCALLTYPE HookFlush(ID3D11DeviceContext* self) {
  CtxEntry* e = EntryFor(*reinterpret_cast<void***>(self));
  FlushFn orig = e != nullptr
                     ? reinterpret_cast<FlushFn>(e->orig[kH_Flush])
                     : nullptr;
  if (orig != nullptr) orig(self);
  InterlockedIncrement64(&g_flushes);
}

HRESULT STDMETHODCALLTYPE HookFinishCommandList(ID3D11DeviceContext* self,
                                                 BOOL restore,
                                                 ID3D11CommandList** out) {
  CtxEntry* e = EntryFor(*reinterpret_cast<void***>(self));
  FinishCommandListFn orig =
      e != nullptr ? reinterpret_cast<FinishCommandListFn>(
                          e->orig[kH_FinishCommandList])
                    : nullptr;
  const HRESULT hr = orig != nullptr ? orig(self, restore, out) : E_UNEXPECTED;
  InterlockedIncrement64(&g_finish_lists);
  return hr;
}

void* g_detours[kH_Count] = {
    reinterpret_cast<void*>(&HookVSSetCB),
    reinterpret_cast<void*>(&HookPSSetCB),
    reinterpret_cast<void*>(&HookGSSetCB),
    reinterpret_cast<void*>(&HookCSSetCB),
    reinterpret_cast<void*>(&HookVSSetShaderResources),
    reinterpret_cast<void*>(&HookPSSetShaderResources),
    reinterpret_cast<void*>(&HookVSSetShader),
    reinterpret_cast<void*>(&HookPSSetShader),
    reinterpret_cast<void*>(&HookIASetVertexBuffers),
    reinterpret_cast<void*>(&HookIASetIndexBuffer),
    reinterpret_cast<void*>(&HookDrawIndexed),
    reinterpret_cast<void*>(&HookDraw),
    reinterpret_cast<void*>(&HookDrawIndexedInst),
    reinterpret_cast<void*>(&HookDrawInst),
    reinterpret_cast<void*>(&HookDrawAuto),
    reinterpret_cast<void*>(&HookDrawIndexedInstIndirect),
    reinterpret_cast<void*>(&HookDrawInstIndirect),
    reinterpret_cast<void*>(&HookUpdate),
    reinterpret_cast<void*>(&HookMap),
    reinterpret_cast<void*>(&HookUnmap),
    reinterpret_cast<void*>(&HookRSViewports),
    reinterpret_cast<void*>(&HookOmRT),
    reinterpret_cast<void*>(&HookClearRTV),
    reinterpret_cast<void*>(&HookExecute),
    reinterpret_cast<void*>(&HookClearState),
    reinterpret_cast<void*>(&HookFlush),
    reinterpret_cast<void*>(&HookFinishCommandList),
};

bool HookContextVtable(void** vtable) {
  if (vtable == nullptr) return false;
  for (LONG i = 0; i < g_ctx_count && i < kMaxCtxVtables; ++i) {
    if (g_ctx[i].vtable == vtable) return true;
  }
  if (g_ctx_count >= kMaxCtxVtables) return false;
  CtxEntry& e = g_ctx[g_ctx_count];
  e.vtable = vtable;
  int hooked = 0;
  for (int h = 0; h < kH_Count; ++h) {
    if (PatchSlot(vtable, kCtxSlots[h], g_detours[h], &e.orig[h])) ++hooked;
  }
  if (hooked != kH_Count) {
    for (int h = 0; h < kH_Count; ++h) {
      if (e.orig[h] != nullptr) UnpatchSlot(vtable, kCtxSlots[h], e.orig[h]);
    }
    e.vtable = nullptr;
    return false;
  }
  MemoryBarrier();
  InterlockedExchange(&g_ctx_count, g_ctx_count + 1);
  LogF("m3a ctx hooked vtable=%p slots=%d\n", static_cast<void*>(vtable),
       hooked);
  return true;
}

void HookContextInterfaces(ID3D11DeviceContext* context) {
  if (context == nullptr) return;
  HookContextVtable(*reinterpret_cast<void***>(context));
  const IID interfaces[] = {__uuidof(ID3D11DeviceContext1),
                            __uuidof(ID3D11DeviceContext2),
                            __uuidof(ID3D11DeviceContext3),
                            __uuidof(ID3D11DeviceContext4)};
  for (const IID& iid : interfaces) {
    IUnknown* extended = nullptr;
    if (SUCCEEDED(context->QueryInterface(iid,
                                           reinterpret_cast<void**>(&extended))) &&
        extended != nullptr) {
      HookContextVtable(*reinterpret_cast<void***>(extended));
      extended->Release();
    }
  }
}

HRESULT STDMETHODCALLTYPE HookCreateDeferredContext(
    ID3D11Device* self, UINT flags, ID3D11DeviceContext** out) {
  DeviceEntry* e = DeviceFor(*reinterpret_cast<void***>(self));
  CreateDeferredContextFn orig =
      e != nullptr
          ? reinterpret_cast<CreateDeferredContextFn>(e->orig_create_deferred)
          : nullptr;
  if (orig == nullptr) return E_UNEXPECTED;
  const HRESULT hr = orig(self, flags, out);
  if (SUCCEEDED(hr) && out != nullptr && *out != nullptr) {
    HookContextInterfaces(*out);
    LogF("m3a deferred context created ctx=%p\n", static_cast<void*>(*out));
  }
  return hr;
}

void HookDeviceVtable(ID3D11Device* device) {
  if (device == nullptr) return;
  void** vtable = *reinterpret_cast<void***>(device);
  if (DeviceFor(vtable) != nullptr) return;
  const LONG index = InterlockedCompareExchange(&g_device_count, 0, 0);
  if (index >= static_cast<LONG>(kMaxDeviceVtables)) return;
  void* orig = nullptr;
  if (!PatchSlot(vtable, kCreateDeferredContext,
                 reinterpret_cast<void*>(&HookCreateDeferredContext), &orig)) {
    return;
  }
  DeviceEntry& entry = g_devices[index];
  entry.vtable = vtable;
  entry.orig_create_deferred = orig;
  MemoryBarrier();
  InterlockedExchange(&g_device_count, index + 1);
  LogF("m3a device hooked deferred-context slot=%zu vtable=%p\n",
       kCreateDeferredContext, static_cast<void*>(vtable));
}

void STDMETHODCALLTYPE HookGetImmediateContext1(ID3D11Device1* self,
                                                 ID3D11DeviceContext1** out) {
  Device1Entry* e = Device1For(*reinterpret_cast<void***>(self));
  GetImmediateContext1Fn orig =
      e != nullptr
          ? reinterpret_cast<GetImmediateContext1Fn>(e->orig_get_immediate)
          : nullptr;
  if (orig == nullptr) return;
  orig(self, out);
  if (out != nullptr && *out != nullptr) {
    HookContextInterfaces(*out);
  }
}

HRESULT STDMETHODCALLTYPE HookCreateDeferredContext1(
    ID3D11Device1* self, UINT flags, ID3D11DeviceContext1** out) {
  Device1Entry* e = Device1For(*reinterpret_cast<void***>(self));
  CreateDeferredContext1Fn orig =
      e != nullptr
          ? reinterpret_cast<CreateDeferredContext1Fn>(e->orig_create_deferred)
          : nullptr;
  if (orig == nullptr) return E_UNEXPECTED;
  const HRESULT hr = orig(self, flags, out);
  if (SUCCEEDED(hr) && out != nullptr && *out != nullptr) {
    HookContextInterfaces(*out);
    LogF("m3a deferred-context1 created ctx=%p\n",
         static_cast<void*>(*out));
  }
  return hr;
}

void HookDevice1Vtable(ID3D11Device* device) {
  if (device == nullptr) return;
  ID3D11Device1* device1 = nullptr;
  if (FAILED(device->QueryInterface(__uuidof(ID3D11Device1),
                                    reinterpret_cast<void**>(&device1))) ||
      device1 == nullptr) {
    return;
  }
  void** vtable = *reinterpret_cast<void***>(device1);
  if (Device1For(vtable) != nullptr) {
    device1->Release();
    return;
  }
  const LONG index = InterlockedCompareExchange(&g_device1_count, 0, 0);
  if (index >= static_cast<LONG>(kMaxDeviceVtables)) {
    device1->Release();
    return;
  }
  void* orig_get = nullptr;
  void* orig_create = nullptr;
  const bool get_ok = PatchSlot(
      vtable, kGetImmediateContext1,
      reinterpret_cast<void*>(&HookGetImmediateContext1), &orig_get);
  const bool create_ok =
      get_ok && PatchSlot(vtable, kCreateDeferredContext1,
                          reinterpret_cast<void*>(&HookCreateDeferredContext1),
                          &orig_create);
  if (!create_ok) {
    if (orig_get != nullptr)
      UnpatchSlot(vtable, kGetImmediateContext1, orig_get);
    device1->Release();
    return;
  }
  Device1Entry& entry = g_devices1[index];
  entry.vtable = vtable;
  entry.orig_get_immediate = orig_get;
  entry.orig_create_deferred = orig_create;
  MemoryBarrier();
  InterlockedExchange(&g_device1_count, index + 1);
  LogF("m3a device1 hooked immediate/deferred slots=%zu/%zu vtable=%p\n",
       kGetImmediateContext1, kCreateDeferredContext1,
       static_cast<void*>(vtable));
  device1->Release();
}

using D12QueueExecuteFn = void(STDMETHODCALLTYPE*)(
    ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using D12DrawInstancedFn = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
using D12DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
using D12CloseFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
using D12CreateCommandQueueFn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**);
using D12CreateCommandListFn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*,
    ID3D12PipelineState*, REFIID, void**);

D12QueueEntry* D12QueueFor(void** vtable) {
  const LONG count = g_d12_queue_count;
  for (LONG i = 0; i < count && i < 4; ++i) {
    if (g_d12_queues[i].vtable == vtable) return &g_d12_queues[i];
  }
  return nullptr;
}

D12CommandListEntry* D12CommandListFor(void** vtable) {
  const LONG count = g_d12_command_list_count;
  for (LONG i = 0; i < count && i < 8; ++i) {
    if (g_d12_command_lists[i].vtable == vtable) return &g_d12_command_lists[i];
  }
  return nullptr;
}

D12DeviceEntry* D12DeviceFor(void** vtable) {
  const LONG count = g_d12_device_count;
  for (LONG i = 0; i < count && i < kMaxDeviceVtables; ++i) {
    if (g_d12_devices[i].vtable == vtable) return &g_d12_devices[i];
  }
  return nullptr;
}

void HookD12Device(ID3D12Device* device);

void STDMETHODCALLTYPE HookD12DrawInstanced(ID3D12GraphicsCommandList* self,
                                             UINT vertices, UINT instances,
                                             UINT start_vertex,
                                             UINT start_instance) {
  D12CommandListEntry* e =
      D12CommandListFor(*reinterpret_cast<void***>(self));
  D12DrawInstancedFn orig =
      e != nullptr
          ? reinterpret_cast<D12DrawInstancedFn>(e->orig_draw_instanced)
          : nullptr;
  if (orig != nullptr)
    orig(self, vertices, instances, start_vertex, start_instance);
  InterlockedIncrement64(&g_d12_draws);
}

void STDMETHODCALLTYPE HookD12DrawIndexedInstanced(
    ID3D12GraphicsCommandList* self, UINT indices, UINT instances,
    UINT start_index, INT base_vertex, UINT start_instance) {
  D12CommandListEntry* e =
      D12CommandListFor(*reinterpret_cast<void***>(self));
  D12DrawIndexedInstancedFn orig =
      e != nullptr ? reinterpret_cast<D12DrawIndexedInstancedFn>(
                          e->orig_draw_indexed_instanced)
                    : nullptr;
  if (orig != nullptr)
    orig(self, indices, instances, start_index, base_vertex, start_instance);
  InterlockedIncrement64(&g_d12_draws);
}

HRESULT STDMETHODCALLTYPE HookD12Close(ID3D12GraphicsCommandList* self) {
  D12CommandListEntry* e =
      D12CommandListFor(*reinterpret_cast<void***>(self));
  D12CloseFn orig = e != nullptr
                        ? reinterpret_cast<D12CloseFn>(e->orig_close)
                        : nullptr;
  return orig != nullptr ? orig(self) : E_UNEXPECTED;
}

bool HookD12CommandListVtable(ID3D12CommandList* list) {
  if (list == nullptr) return false;
  ID3D12GraphicsCommandList* graphics = nullptr;
  if (FAILED(list->QueryInterface(
          __uuidof(ID3D12GraphicsCommandList),
          reinterpret_cast<void**>(&graphics))) ||
      graphics == nullptr) {
    return false;
  }
  void** vtable = *reinterpret_cast<void***>(graphics);
  if (D12CommandListFor(vtable) != nullptr) {
    graphics->Release();
    return true;
  }
  const LONG index = InterlockedCompareExchange(
      &g_d12_command_list_count, 0, 0);
  if (index >= 8) {
    graphics->Release();
    return false;
  }
  D12CommandListEntry& e = g_d12_command_lists[index];
  e.vtable = vtable;
  int hooked = 0;
  hooked += PatchSlot(vtable, kD12GraphicsDrawInstanced,
                      reinterpret_cast<void*>(&HookD12DrawInstanced),
                      &e.orig_draw_instanced)
                ? 1
                : 0;
  hooked += PatchSlot(
                vtable, kD12GraphicsDrawIndexedInstanced,
                reinterpret_cast<void*>(&HookD12DrawIndexedInstanced),
                &e.orig_draw_indexed_instanced)
                ? 1
                : 0;
  hooked += PatchSlot(vtable, kD12GraphicsClose,
                      reinterpret_cast<void*>(&HookD12Close), &e.orig_close)
                ? 1
                : 0;
  if (hooked != 3) {
    if (e.orig_draw_instanced != nullptr)
      UnpatchSlot(vtable, kD12GraphicsDrawInstanced, e.orig_draw_instanced);
    if (e.orig_draw_indexed_instanced != nullptr)
      UnpatchSlot(vtable, kD12GraphicsDrawIndexedInstanced,
                  e.orig_draw_indexed_instanced);
    if (e.orig_close != nullptr)
      UnpatchSlot(vtable, kD12GraphicsClose, e.orig_close);
    e = {};
    graphics->Release();
    return false;
  }
  MemoryBarrier();
  InterlockedExchange(&g_d12_command_list_count, index + 1);
  LogF("m3a d12 command-list hooked vtable=%p\n",
       static_cast<void*>(vtable));
  graphics->Release();
  return true;
}

void STDMETHODCALLTYPE HookD12ExecuteCommandLists(
    ID3D12CommandQueue* self, UINT count,
    ID3D12CommandList* const* lists) {
  D12QueueEntry* e = D12QueueFor(*reinterpret_cast<void***>(self));
  D12QueueExecuteFn orig =
      e != nullptr ? reinterpret_cast<D12QueueExecuteFn>(e->orig_execute)
                    : nullptr;
  if (lists != nullptr) {
    for (UINT i = 0; i < count; ++i) HookD12CommandListVtable(lists[i]);
  }
  InterlockedIncrement64(&g_d12_execs);
  InterlockedAdd64(&g_d12_lists, static_cast<LONGLONG>(count));
  if (orig != nullptr) orig(self, count, lists);
}

void HookD12Queue(IUnknown* unknown) {
  if (unknown == nullptr) return;
  ID3D12CommandQueue* queue = nullptr;
  if (FAILED(unknown->QueryInterface(
          __uuidof(ID3D12CommandQueue),
          reinterpret_cast<void**>(&queue))) ||
      queue == nullptr) {
    return;
  }
  void** vtable = *reinterpret_cast<void***>(queue);
  if (D12QueueFor(vtable) == nullptr) {
    const LONG index = InterlockedCompareExchange(&g_d12_queue_count, 0, 0);
    if (index < 4) {
      D12QueueEntry& e = g_d12_queues[index];
      e.vtable = vtable;
      if (PatchSlot(vtable, kD12QueueExecuteCommandLists,
                    reinterpret_cast<void*>(&HookD12ExecuteCommandLists),
                    &e.orig_execute)) {
        MemoryBarrier();
        InterlockedExchange(&g_d12_queue_count, index + 1);
        LogF("m3a d12 queue hooked execute slot=%zu\n",
             kD12QueueExecuteCommandLists);
      } else {
        e = {};
      }
    }
  }
  ID3D12Device* device = nullptr;
  if (SUCCEEDED(queue->GetDevice(__uuidof(ID3D12Device),
                                 reinterpret_cast<void**>(&device))) &&
      device != nullptr) {
    HookD12Device(device);
    device->Release();
  }
  queue->Release();
}

HRESULT STDMETHODCALLTYPE HookD12CreateCommandQueue(
    ID3D12Device* self, const D3D12_COMMAND_QUEUE_DESC* desc, REFIID riid,
    void** out) {
  D12DeviceEntry* e = D12DeviceFor(*reinterpret_cast<void***>(self));
  D12CreateCommandQueueFn orig =
      e != nullptr
          ? reinterpret_cast<D12CreateCommandQueueFn>(e->orig_create_queue)
          : nullptr;
  if (orig == nullptr) return E_FAIL;
  const HRESULT hr = orig(self, desc, riid, out);
  if (SUCCEEDED(hr) && out != nullptr && *out != nullptr) {
    IUnknown* unknown = static_cast<IUnknown*>(*out);
    HookD12Queue(unknown);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE HookD12CreateCommandList(
    ID3D12Device* self, UINT node_mask, D3D12_COMMAND_LIST_TYPE type,
    ID3D12CommandAllocator* allocator, ID3D12PipelineState* state,
    REFIID riid, void** out) {
  D12DeviceEntry* e = D12DeviceFor(*reinterpret_cast<void***>(self));
  D12CreateCommandListFn orig =
      e != nullptr
          ? reinterpret_cast<D12CreateCommandListFn>(e->orig_create_list)
          : nullptr;
  if (orig == nullptr) return E_UNEXPECTED;
  const HRESULT hr =
      orig(self, node_mask, type, allocator, state, riid, out);
  if (SUCCEEDED(hr) && out != nullptr && *out != nullptr) {
    IUnknown* unknown = static_cast<IUnknown*>(*out);
    ID3D12CommandList* list = nullptr;
    if (SUCCEEDED(unknown->QueryInterface(
            __uuidof(ID3D12CommandList),
            reinterpret_cast<void**>(&list))) &&
        list != nullptr) {
      HookD12CommandListVtable(list);
      list->Release();
    }
  }
  return hr;
}

void HookD12Device(ID3D12Device* device) {
  if (device == nullptr) return;
  void** vtable = *reinterpret_cast<void***>(device);
  if (D12DeviceFor(vtable) != nullptr) return;
  const LONG index = InterlockedCompareExchange(&g_d12_device_count, 0, 0);
  if (index >= static_cast<LONG>(kMaxDeviceVtables)) return;
  void* orig_queue = nullptr;
  void* orig_list = nullptr;
  const bool queue_ok = PatchSlot(
      vtable, kD12DeviceCreateCommandQueue,
      reinterpret_cast<void*>(&HookD12CreateCommandQueue), &orig_queue);
  const bool list_ok =
      queue_ok && PatchSlot(vtable, kD12DeviceCreateCommandList,
                            reinterpret_cast<void*>(&HookD12CreateCommandList),
                            &orig_list);
  if (!list_ok) {
    if (orig_queue != nullptr)
      UnpatchSlot(vtable, kD12DeviceCreateCommandQueue, orig_queue);
    return;
  }
  D12DeviceEntry& entry = g_d12_devices[index];
  entry.vtable = vtable;
  entry.orig_create_queue = orig_queue;
  entry.orig_create_list = orig_list;
  MemoryBarrier();
  InterlockedExchange(&g_d12_device_count, index + 1);
  InterlockedIncrement64(&g_d12_device_hooks);
  LogF("m3a d12 device hooked queue/list slots=%zu/%zu vtable=%p\n",
       kD12DeviceCreateCommandQueue, kD12DeviceCreateCommandList,
       static_cast<void*>(vtable));
}

void HookSwapchainVtable(void** vtable) {
  if (vtable == nullptr) return;
  for (LONG i = 0; i < g_vtable_count && i < kMaxVtables; ++i) {
    if (g_vtables[i].vtable == vtable) return;
  }
  if (g_vtable_count >= kMaxVtables) return;
  VtableEntry& e = g_vtables[g_vtable_count];
  e.vtable = vtable;
  void* orig_p = nullptr;
  void* orig_r = nullptr;
  extern HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain*, UINT, UINT);
  extern HRESULT STDMETHODCALLTYPE HookResize(IDXGISwapChain*, UINT, UINT,
                                              UINT, DXGI_FORMAT, UINT);
  if (!PatchSlot(vtable, kPresentIndex, reinterpret_cast<void*>(&HookPresent),
                 &orig_p) ||
      !PatchSlot(vtable, kResizeBuffersIndex,
                 reinterpret_cast<void*>(&HookResize), &orig_r)) {
    if (orig_p != nullptr) UnpatchSlot(vtable, kPresentIndex, orig_p);
    e.vtable = nullptr;
    return;
  }
  e.orig_present = orig_p;
  e.orig_resize = orig_r;
  MemoryBarrier();
  InterlockedExchange(&g_vtable_count, g_vtable_count + 1);
}

void HookContextFromSwapchain(IDXGISwapChain* self) {
  if (self == nullptr) return;
  ID3D11Device* device = nullptr;
  if (FAILED(self->GetDevice(__uuidof(ID3D11Device),
                             reinterpret_cast<void**>(&device))) ||
      device == nullptr) {
    return;
  }
  HookDeviceVtable(device);
  HookDevice1Vtable(device);
  ID3D11DeviceContext* ctx = nullptr;
  device->GetImmediateContext(&ctx);
  device->Release();
  if (ctx == nullptr) return;
  HookContextInterfaces(ctx);
  ctx->Release();  // Vtable pointer stays valid; no ref retained.
}

void ReadPhaseFile() {
  wchar_t temp[MAX_PATH];
  wchar_t path[MAX_PATH];
  if (GetTempPathW(MAX_PATH, temp) == 0) return;
  if (FAILED(StringCchPrintfW(path, MAX_PATH, L"%smecvr_m3a_phase.txt",
                              temp))) {
    return;
  }
  char buf[32] = {};
  DWORD read = 0;
  HANDLE f =
      CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  if (ReadFile(f, buf, sizeof(buf) - 1, &read, nullptr) && read > 0) {
    buf[read] = '\0';
    for (DWORD i = 0; i < read; ++i) {
      if (buf[i] == '\r' || buf[i] == '\n' || buf[i] == ' ' || buf[i] == '\t') {
        buf[i] = '\0';
        break;
      }
    }
    if (buf[0] != '\0' && strcmp(buf, g_phase) != 0) {
      StringCchCopyA(g_phase, 32, buf);
      g_dumps_this_phase = 0;  // Fresh dump budget per phase.
      g_execl_this_window = 0;
      LogF("m3a phase=%s present=%llu\n", g_phase, g_present_idx);
    }
  }
  CloseHandle(f);
}

double TicksToUs(std::int64_t ticks) {
  if (g_qpc_freq <= 0) return 0.0;
  return static_cast<double>(ticks) * 1000000.0 /
         static_cast<double>(g_qpc_freq);
}

// Top main-camera candidates by (matrix windows, updates). Offline
// review does the real classification; this seeds it.
void TopCandidates(char* out, std::size_t out_len) {
  struct Scored {
    UINT id = 0;
    UINT mat = 0;
    std::uint64_t updates = 0;
  };
  Scored top[8];
  for (std::size_t s = 0; s < kCbTableSize; ++s) {
    const CbRecord& r = g_cbtable[s];
    if (r.buf == nullptr || r.mat_windows == 0) continue;
    if (r.vs_mask == 0) continue;  // Main camera must feed the VS stage.
    if (r.size > kBigBufferBytes) continue;  // Megabuffers excluded.
    Scored cand{r.id, r.mat_windows, r.updates};
    for (int i = 0; i < 8; ++i) {
      if (cand.mat > top[i].mat ||
          (cand.mat == top[i].mat && cand.updates > top[i].updates)) {
        for (int j = 7; j > i; --j) top[j] = top[j - 1];
        top[i] = cand;
        break;
      }
    }
  }
  std::size_t pos = 0;
  for (int i = 0; i < 8 && pos + 1 < out_len; ++i) {
    if (top[i].id == 0) break;
    char one[32];
    _snprintf_s(one, sizeof(one), _TRUNCATE, "%s%u", i == 0 ? "" : ",", top[i].id);
    size_t left = 0;
    if (SUCCEEDED(StringCchLengthA(one, 32, &left)) && pos + left < out_len) {
      memcpy(out + pos, one, left + 1);
      pos += left;
    }
  }
  if (pos == 0 && out_len > 0) {
    out[0] = '-';
    if (out_len > 1) out[1] = '\0';
  }
}

void DumpDrawCorrelation(const char* tag) {
  if (!g_palette_discovery_enabled) return;
  const LONG count = std::min<LONG>(
      InterlockedExchange(&g_draw_correlation_count, 0),
      kMaxDrawCorrelationSamples);
  LogF("%s draw-correlation samples=%ld\n", tag, count);
  for (LONG i = 0; i < count; ++i) {
    const auto& s = g_draw_correlation[i];
    LogF("%s draw-correlation i=%ld draw=%llu present=%llu kind=%u count=%u "
         "start=%u base=%d vs=%p ps=%p vb=%p ib=%p stride=%u offset=%u "
         "fmt=%u vs_srv0=%p ps_srv0=%p\n",
         tag, i, static_cast<unsigned long long>(s.draw),
         static_cast<unsigned long long>(s.present), s.kind, s.count, s.start,
         s.base, static_cast<void*>(s.vs), static_cast<void*>(s.ps),
         static_cast<void*>(s.vb), static_cast<void*>(s.ib), s.stride,
         s.offset, static_cast<unsigned>(s.index_format),
         static_cast<void*>(s.vs_srv0), static_cast<void*>(s.ps_srv0));
  }
}

void DumpStatus(const char* tag, std::int64_t now_ns, std::int64_t first_ns) {
  const double secs =
      first_ns > 0 ? static_cast<double>(now_ns - first_ns) / 1e9 : 0.0;
  const double rate =
      secs > 0.0 ? static_cast<double>(g_present_idx) / secs : 0.0;
  std::int64_t total_ticks = 0;
  std::int64_t max_ticks = 0;
  const Ovhd* all[] = {&g_ovh_setcb, &g_ovh_shader, &g_ovh_update, &g_ovh_map,
                       &g_ovh_unmap, &g_ovh_om,    &g_ovh_rs,     &g_ovh_present,
                       &g_ovh_execl};
  for (const Ovhd* o : all) {
    total_ticks += o->ticks;
    if (o->max_ticks > max_ticks) max_ticks = o->max_ticks;
  }
  const double mean_us =
      g_present_idx > 0 ? TicksToUs(total_ticks) / g_present_idx : 0.0;
  char main_ids[128] = {};
  TopCandidates(main_ids, sizeof(main_ids));
  LONGLONG camera_overrides = 0;
  LONGLONG native_pose_attempts = 0;
  LONGLONG native_pose_applied = 0;
  LONGLONG native_pose_rejected = 0;
#ifdef MECVR_M3B
  camera_overrides = g_camera_overrides;
  native_pose_attempts = g_native_pose_write_attempts;
  native_pose_applied = g_native_pose_write_applied;
  native_pose_rejected = g_native_pose_write_rejected;
#endif
  LogF("%s presents=%llu draws_last=%lld draw_idx=%lld cbs=%lld cands=%lld caps=%lld "
       "drops=%llu ovfl=%lld ovh_us=%.1f max_us=%.1f rate=%.1f state_fail=%lld "
       "dxgi_pc=%u dxgi_last=%u main=%s umatch=%lld execl=%lld exdrop=%lld "
       "d12dev=%lld d12q=%ld d12exec=%lld d12lists=%lld d12draws=%lld "
       "clear=%lld flush=%lld finish=%lld ik=%lld ik_valid=%lld body=%lld palettes=%lld "
       "camera_overrides=%lld overlay=%lld native_pose_attempts=%lld "
       "native_pose_applied=%lld native_pose_rejected=%lld "
       "phase=%s\n",
       tag, g_present_idx, g_draws_last_frame, g_draw_idx, g_cb_observed,
       g_candidates,
       g_captures, g_drops, g_cb_overflow, mean_us, TicksToUs(max_ticks),
       rate, g_state_fail, g_fstats.PresentCount, g_last_present_count,
       main_ids, g_unmap_matched, g_execl_total, g_execl_drops,
       g_d12_device_hooks, g_d12_queue_count, g_d12_execs, g_d12_lists,
       g_d12_draws, g_clear_states, g_flushes, g_finish_lists, g_ik_frames,
       g_ik_valid_frames, g_body_overlay_frames, g_palette_candidates,
       camera_overrides, g_body_overlay_frames, native_pose_attempts,
       native_pose_applied, native_pose_rejected, g_phase);
  const char* names[] = {"setcb", "shader", "update", "map", "unmap",
                         "om",    "rs",     "present", "execl"};
  for (int i = 0; i < 9; ++i) {
    const Ovhd* o = all[i];
    const double mean = o->calls > 0
                            ? TicksToUs(o->ticks) / o->calls
                            : 0.0;
    LogF("%s ovh %s calls=%lld mean_us=%.2f max_us=%.1f\n", tag, names[i],
         o->calls, mean, TicksToUs(o->max_ticks));
  }
  DumpDrawCorrelation(tag);
}

void DumpRegistry() {
  LogF("m3a registry begin cbs=%lld\n", g_cb_observed);
  for (std::size_t s = 0; s < kCbTableSize; ++s) {
    const CbRecord& r = g_cbtable[s];
    if (r.buf == nullptr) continue;
    LogF("m3a reg id=%u size=%u vs=%08x ps=%08x gs=%08x cs=%08x binds=%llu "
         "upd=%llu maps=%llu fpch=%llu matw=%u pal=%u:%u:%u:%u "
         "first=%llu last=%llu "
         "rt=%ux%u vp=%.0fx%.0f draw=%llu phase=%s\n",
         r.id, r.size, r.vs_mask, r.ps_mask, r.gs_mask, r.cs_mask, r.binds,
         r.updates, r.maps, r.fp_changes, r.mat_windows, r.palette_offset,
         r.palette_stride, r.palette_matrices, r.palette_layout, r.first_present,
         r.last_present, r.rt_w, r.rt_h, r.vp_w, r.vp_h, r.last_draw_idx,
         r.last_phase);
  }
  LogF("m3a registry end\n");
}

std::int64_t g_first_present_ns = 0;
std::int64_t g_last_dump_ns = 0;

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* self, UINT sync,
                                      UINT flags) {
  // Overhead measures OUR work only (never the forwarded Present).
  std::uint64_t work = 0;
  std::uint64_t t0 = QpcNow();
  HookSwapchainVtable(*reinterpret_cast<void***>(self));
  HookContextFromSwapchain(self);
#ifdef MECVR_M3B
  // Capture is intentionally performed below, after the optional mod-owned
  // body overlay has been drawn into the game's current render target.
#endif
  const VtableEntry* entry = nullptr;
  const LONG c = g_vtable_count;
  void** vtable = *reinterpret_cast<void***>(self);
  for (LONG i = 0; i < c && i < kMaxVtables; ++i) {
    if (g_vtables[i].vtable == vtable) {
      entry = &g_vtables[i];
      break;
    }
  }
  PresentFn orig =
      entry != nullptr ? reinterpret_cast<PresentFn>(entry->orig_present)
                       : nullptr;
  // State spot-check (subset) around our own observation below.
  ID3D12Device* d12_device = nullptr;
  if (SUCCEEDED(self->GetDevice(__uuidof(ID3D12Device),
                                reinterpret_cast<void**>(&d12_device))) &&
      d12_device != nullptr) {
    HookD12Device(d12_device);
    d12_device->Release();
  }
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  if (SUCCEEDED(self->GetDevice(__uuidof(ID3D11Device),
                                reinterpret_cast<void**>(&device))) &&
      device != nullptr) {
    device->GetImmediateContext(&ctx);
    device->Release();
  }
  ID3D11RenderTargetView* check_rtv = nullptr;
  ID3D11DepthStencilView* check_dsv = nullptr;
  if (ctx != nullptr) ctx->OMGetRenderTargets(1, &check_rtv, &check_dsv);
#ifdef MECVR_M3B
  const bool temporal_stereo =
      !g_preserve_runtime_pacing &&
      g_stereo_enabled.load(std::memory_order_acquire) &&
      g_stereo_eye_valid.load(std::memory_order_acquire) &&
      g_stereo_pair_epoch.load(std::memory_order_acquire) != 0 &&
      g_stereo_pair_pose.load(std::memory_order_acquire) != 0;
  const std::uint32_t stereo_eye =
      g_stereo_eye.load(std::memory_order_acquire) & 1u;
  // The procedural overlay is rendered in the game's current view, so give
  // each temporal eye its half-IPD horizontal separation. This keeps the
  // mod-owned arms, hands, and weapon silhouettes from becoming a flat card
  // when the capture worker assembles the stereo pair.
  constexpr float kOverlayHalfIpdMeters = 0.032f;
  const float body_eye_offset =
      temporal_stereo ? (stereo_eye == 0 ? -kOverlayHalfIpdMeters
                                         : kOverlayHalfIpdMeters)
                      : 0.0f;
  if (g_body_overlay_enabled && ctx != nullptr && check_rtv != nullptr) {
    mecvr::ik::HumanoidPoseFrame body_pose;
    if (g_body_pose_mailbox.latest(&body_pose)) {
      if (g_body_overlay.render(ctx, check_rtv, body_pose, body_eye_offset))
        InterlockedIncrement64(&g_body_overlay_frames);
    }
  }
  // Capture the game's final desktop image after the optional overlay and
  // before forwarding Present. The XR worker consumes this asynchronously.
  if (auto* capture =
          g_live_capture.load(std::memory_order_acquire); capture != nullptr) {
    // Prefer the nonblocking shared-GPU path. While its registration is still
    // opening (or when capability checks fail), retain the established CPU
    // transport as a safe fallback.
    if (capture->captureGpu(self)) {
      // Published directly to the XR worker; no CPU readback this Present.
    } else if (temporal_stereo) {
      const std::uint64_t epoch =
          g_stereo_pair_epoch.load(std::memory_order_acquire);
      const std::uint64_t pose_sequence =
          g_stereo_pair_pose.load(std::memory_order_acquire);
      const std::uint32_t eye = stereo_eye;
      capture->captureStereo(self, eye, epoch, pose_sequence);
      if (eye == 0) {
        g_stereo_eye.store(1, std::memory_order_release);
      } else {
        const std::uint64_t next_epoch =
            g_xr_epoch.load(std::memory_order_acquire);
        const std::uint64_t next_pose =
            g_xr_pose_sequence.load(std::memory_order_acquire);
        if (next_epoch != 0 && next_pose != 0) {
          g_stereo_pair_epoch.store(next_epoch, std::memory_order_release);
          g_stereo_pair_pose.store(next_pose, std::memory_order_release);
        }
        g_stereo_eye.store(0, std::memory_order_release);
      }
    } else {
      capture->capture(self);
    }
  }
#endif
  work += QpcNow() - t0;  // Pre-orig work done; orig excluded below.
  const HRESULT hr = orig != nullptr ? orig(self, sync, flags) : E_UNEXPECTED;
  t0 = QpcNow();
  bool preserved = true;
  if (ctx != nullptr) {
    ID3D11RenderTargetView* after_rtv = nullptr;
    ID3D11DepthStencilView* after_dsv = nullptr;
    ctx->OMGetRenderTargets(1, &after_rtv, &after_dsv);
    preserved = (after_rtv == check_rtv && after_dsv == check_dsv);
    if (after_rtv != nullptr) after_rtv->Release();
    if (after_dsv != nullptr) after_dsv->Release();
  }
  if (check_rtv != nullptr) check_rtv->Release();
  if (check_dsv != nullptr) check_dsv->Release();
  if (ctx != nullptr) ctx->Release();
  if (!preserved) InterlockedIncrement64(&g_state_fail);

  if (g_armed) {
    ++g_present_idx;
    InterlockedIncrement64(&g_presents);
    g_draws_last_frame = g_draws;
    g_draws = 0;
    // Epoch-token candidates: documented DXGI frame counters.
    DXGI_FRAME_STATISTICS fs{};
    if (SUCCEEDED(self->GetFrameStatistics(&fs))) g_fstats = fs;
    UINT last_pc = 0;
    if (SUCCEEDED(self->GetLastPresentCount(&last_pc))) {
      g_last_present_count = last_pc;
    }
    if (g_present_idx % kPhaseRecheckPresents == 0) ReadPhaseFile();
    // Rolling dump budget: 256 per 600 presents (~4 s at 150 fps) so
    // long phases keep yielding fresh content, not just the opening.
    if (g_present_idx % 600 == 0) {
      g_dumps_this_phase = 0;
      g_execl_this_window = 0;
    }
    const std::int64_t now = SteadyNs();
    if (g_first_present_ns == 0) {
      g_first_present_ns = now;
      g_last_dump_ns = now;
    }
    if (now - g_last_dump_ns > 10000000000LL) {
      DumpStatus("m3a status", now, g_first_present_ns);
      g_last_dump_ns = now;
    }
    if (g_shutdown != nullptr &&
        WaitForSingleObject(g_shutdown, 0) == WAIT_OBJECT_0) {
      extern void BeginDetach(const char* reason);
      BeginDetach("shutdown-event");
    }
  }
  OvhdAdd(g_ovh_present, work + (QpcNow() - t0));
  return hr;
}

HRESULT STDMETHODCALLTYPE HookResize(IDXGISwapChain* self, UINT count,
                                     UINT width, UINT height, DXGI_FORMAT fmt,
                                     UINT flags) {
  void** vtable = *reinterpret_cast<void***>(self);
  const VtableEntry* entry = nullptr;
  const LONG c = g_vtable_count;
  for (LONG i = 0; i < c && i < kMaxVtables; ++i) {
    if (g_vtables[i].vtable == vtable) {
      entry = &g_vtables[i];
      break;
    }
  }
  ResizeBuffersFn orig =
      entry != nullptr
          ? reinterpret_cast<ResizeBuffersFn>(entry->orig_resize)
          : nullptr;
  const HRESULT hr = orig != nullptr
                         ? orig(self, count, width, height, fmt, flags)
                         : E_UNEXPECTED;
  if (g_armed) {
    LogF("m3a resize count=%u %ux%u fmt=%d present=%llu\n", count, width,
         height, static_cast<int>(fmt), g_present_idx);
  }
  return hr;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam,
                             LPARAM lparam) {
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// Factory hooks: catch late swapchains the game creates after us.
constexpr std::size_t kCreateSwapChainIndex = 10;
constexpr std::size_t kCreateSwapChainForHwndIndex = 15;

using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);

void* g_factory_vtables[kMaxVtables] = {};
void* g_factory_orig[kMaxVtables] = {};
void* g_factory_for_hwnd[kMaxVtables] = {};
volatile LONG g_factory_count = 0;

HRESULT STDMETHODCALLTYPE HookCreateSwapChain(IDXGIFactory* self,
                                              IUnknown* device,
                                              DXGI_SWAP_CHAIN_DESC* desc,
                                              IDXGISwapChain** out) {
  for (LONG i = 0; i < g_factory_count; ++i) {
    if (g_factory_vtables[i] == *reinterpret_cast<void***>(self) &&
        g_factory_orig[i] != nullptr) {
      HookD12Queue(device);
      const HRESULT hr =
          reinterpret_cast<CreateSwapChainFn>(g_factory_orig[i])(self, device,
                                                                 desc, out);
      if (SUCCEEDED(hr) && out != nullptr && *out != nullptr) {
        HookSwapchainVtable(*reinterpret_cast<void***>(*out));
        HookContextFromSwapchain(*out);
      }
      return hr;
    }
  }
  return E_UNEXPECTED;
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChainForHwnd(
    IDXGIFactory2* self, IUnknown* device, HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* output,
    IDXGISwapChain1** out) {
  for (LONG i = 0; i < g_factory_count; ++i) {
    if (g_factory_vtables[i] == *reinterpret_cast<void***>(self) &&
        g_factory_for_hwnd[i] != nullptr) {
      HookD12Queue(device);
      const HRESULT hr = reinterpret_cast<CreateSwapChainForHwndFn>(
          g_factory_for_hwnd[i])(self, device, hwnd, desc, fs, output, out);
      if (SUCCEEDED(hr) && out != nullptr && *out != nullptr) {
        HookSwapchainVtable(*reinterpret_cast<void***>(*out));
        HookContextFromSwapchain(*out);
      }
      return hr;
    }
  }
  return E_UNEXPECTED;
}

void HookFactoryVtable(IDXGIFactory* factory) {
  void** vtable = *reinterpret_cast<void***>(factory);
  for (LONG i = 0; i < g_factory_count; ++i) {
    if (g_factory_vtables[i] == vtable) return;
  }
  if (g_factory_count >= kMaxVtables) return;
  void* orig = nullptr;
  if (!PatchSlot(vtable, kCreateSwapChainIndex,
                 reinterpret_cast<void*>(&HookCreateSwapChain), &orig)) {
    return;
  }
  g_factory_vtables[g_factory_count] = vtable;
  g_factory_orig[g_factory_count] = orig;
  g_factory_for_hwnd[g_factory_count] = nullptr;
  IDXGIFactory2* as_f2 = nullptr;
  if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory2),
                                        reinterpret_cast<void**>(&as_f2))) &&
      as_f2 != nullptr) {
    void* orig_fh = nullptr;
    if (PatchSlot(vtable, kCreateSwapChainForHwndIndex,
                  reinterpret_cast<void*>(&HookCreateSwapChainForHwnd),
                  &orig_fh)) {
      g_factory_for_hwnd[g_factory_count] = orig_fh;
    }
    as_f2->Release();
  }
  MemoryBarrier();
  InterlockedExchange(&g_factory_count, g_factory_count + 1);
}

// T3B technique: throwaway own-device swapchains of several flavors so
// their (per-class shared) vtables get hooked — which simultaneously
// observes the game's pre-existing swapchains of the same classes.
void HookKnownClasses(ID3D11Device* device, HWND hwnd) {
  if (device == nullptr || hwnd == nullptr) return;
  HookDeviceVtable(device);
  HookDevice1Vtable(device);
  IDXGIDevice* dxgi_device = nullptr;
  IDXGIAdapter* adapter = nullptr;
  IDXGIFactory* factory = nullptr;
  if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice),
                                    reinterpret_cast<void**>(&dxgi_device))) ||
      dxgi_device == nullptr) {
    return;
  }
  HRESULT hr = dxgi_device->GetAdapter(&adapter);
  dxgi_device->Release();
  if (FAILED(hr) || adapter == nullptr) return;
  hr = adapter->GetParent(__uuidof(IDXGIFactory),
                          reinterpret_cast<void**>(&factory));
  adapter->Release();
  if (FAILED(hr) || factory == nullptr) return;
  HookFactoryVtable(factory);

  DXGI_SWAP_CHAIN_DESC d{};
  d.BufferCount = 2;
  d.BufferDesc.Width = 64;
  d.BufferDesc.Height = 64;
  d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  d.OutputWindow = hwnd;
  d.SampleDesc.Count = 1;
  d.Windowed = TRUE;
  d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  IDXGISwapChain* sc = nullptr;
  if (SUCCEEDED(factory->CreateSwapChain(device, &d, &sc)) && sc != nullptr) {
    HookSwapchainVtable(*reinterpret_cast<void***>(sc));
    HookContextFromSwapchain(sc);
    sc->Release();
  }
  IDXGIFactory2* f2 = nullptr;
  if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory2),
                                        reinterpret_cast<void**>(&f2))) &&
      f2 != nullptr) {
    DXGI_SWAP_CHAIN_DESC1 d1{};
    d1.Width = 64;
    d1.Height = 64;
    d1.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d1.BufferCount = 2;
    d1.SampleDesc.Count = 1;
    d1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    IDXGISwapChain1* s1 = nullptr;
    if (SUCCEEDED(f2->CreateSwapChainForHwnd(device, hwnd, &d1, nullptr,
                                             nullptr, &s1)) &&
        s1 != nullptr) {
      HookSwapchainVtable(*reinterpret_cast<void***>(s1));
      HookContextFromSwapchain(s1);
      s1->Release();
    }
    d1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* s2 = nullptr;
    if (SUCCEEDED(f2->CreateSwapChainForHwnd(device, hwnd, &d1, nullptr,
                                             nullptr, &s2)) &&
        s2 != nullptr) {
      HookSwapchainVtable(*reinterpret_cast<void***>(s2));
      HookContextFromSwapchain(s2);
      s2->Release();
    }
    f2->Release();
  }
  // Own immediate context: same class as the game's -> hook it now.
  ID3D11DeviceContext* ctx = nullptr;
  device->GetImmediateContext(&ctx);
  if (ctx != nullptr) {
    HookContextInterfaces(ctx);
    ctx->Release();
  }
  factory->Release();
}

#ifdef MECVR_M3B
unsigned __stdcall PoseWorkerProc(void*) {
  mecvr::openxr::RealOpenXRBackend backend;
  if (!backend.startup()) {
    LogF("m3b camera: XR startup unavailable: %s\n",
         backend.diagnostics().failure_reason.c_str());
    return 0;
  }
  {
    const auto diagnostics = backend.diagnostics();
    LogF("m3b camera: XR started runtime=%s version=%u.%u.%u stage=%s "
         "body_tracking=%s refresh=%.2f\n",
         diagnostics.runtime_name.c_str(), diagnostics.runtime_version_major,
         diagnostics.runtime_version_minor, diagnostics.runtime_version_patch,
         diagnostics.stage_available ? "yes" : "no",
         backend.bodyTracking().active ? "active" : "procedural",
         static_cast<double>(backend.displayFrequencyHz()));
  }
  std::unique_ptr<mecvr::input::GameInputSynth> input_synth;
  if (g_input_enabled) {
    mecvr::input::ComfortConfig comfort;
    mecvr::input::ParkourKeyConfig parkour_keys;
    const auto read_scan = [](const char* name, std::uint16_t fallback) {
      char value[16] = {};
      const DWORD length =
          GetEnvironmentVariableA(name, value, sizeof(value));
      if (length == 0 || length >= sizeof(value)) return fallback;
      char* end = nullptr;
      const unsigned long parsed = std::strtoul(value, &end, 0);
      return end != value && *end == '\0' && parsed > 0 && parsed <= 0xff
                 ? static_cast<std::uint16_t>(parsed)
                 : fallback;
    };
    parkour_keys.vault_scan = read_scan("MECVR_PARKOUR_VAULT_SCAN",
                                        parkour_keys.vault_scan);
    parkour_keys.climb_scan = read_scan("MECVR_PARKOUR_CLIMB_SCAN",
                                        parkour_keys.climb_scan);
    parkour_keys.slide_scan = read_scan("MECVR_PARKOUR_SLIDE_SCAN",
                                        parkour_keys.slide_scan);
    char turn_mode[16] = {};
    const DWORD turn_len = GetEnvironmentVariableA(
        "MECVR_TURN_MODE", turn_mode, sizeof(turn_mode));
    if (turn_len > 0 && _stricmp(turn_mode, "snap") == 0) {
      comfort.turn_mode = mecvr::input::TurnMode::kSnap;
    }
    input_synth = std::make_unique<mecvr::input::GameInputSynth>(g_pid,
                                                                  comfort,
                                                                  parkour_keys);
    LogF("m3b input: foreground-gated keyboard/mouse bridge enabled turn=%s\n",
         comfort.turn_mode == mecvr::input::TurnMode::kSnap ? "snap"
                                                            : "smooth");
  }
  mecvr::openxr::FrameMailbox mailbox(2);
  const bool stereo_on = g_stereo_enabled.load(std::memory_order_acquire);
  mecvr::openxr::StereoMailbox stereo_mailbox(2);
  mecvr::render::SharedGpuMailbox gpu_mailbox;
  mecvr::render::LiveCapture capture(&mailbox,
                                     stereo_on ? &stereo_mailbox : nullptr,
                                     &gpu_mailbox);
  g_live_capture.store(&capture, std::memory_order_release);

  std::uint64_t sequence = 0;
  std::uint64_t space_generation = 0;
  mecvr::input::RecenterLatch recenter_latch;
  mecvr::ik::FullBodyAnimator body_animator;
  mecvr::ik::MotionClip motion_clip;
  char motion_clip_path[512] = {};
  const DWORD motion_clip_len = GetEnvironmentVariableA(
      "MECVR_RECORD_MOTION_CLIP", motion_clip_path,
      static_cast<DWORD>(sizeof(motion_clip_path)));
  const bool record_motion_clip = motion_clip_len > 0 &&
                                  motion_clip_len < sizeof(motion_clip_path);
  if (record_motion_clip)
    LogF("m3b motion clip recording enabled path=%s max_frames=%zu\n",
         motion_clip_path, static_cast<std::size_t>(1800));
  char playback_path[512] = {};
  const DWORD playback_len = GetEnvironmentVariableA(
      "MECVR_PLAY_MOTION_CLIP", playback_path,
      static_cast<DWORD>(sizeof(playback_path)));
  std::unique_ptr<mecvr::ik::MotionClip> playback_clip;
  double playback_elapsed_seconds = 0.0;
  if (playback_len > 0 && playback_len < sizeof(playback_path)) {
    auto loaded = std::make_unique<mecvr::ik::MotionClip>();
    if (mecvr::ik::MotionClip::load(playback_path, loaded.get())) {
      LogF("m3b motion clip playback enabled path=%s frames=%zu duration=%.3f\n",
           playback_path, loaded->size(), loaded->durationSeconds());
      playback_clip = std::move(loaded);
    } else {
      LogF("m3b motion clip playback rejected path=%s\n", playback_path);
    }
  }
  mecvr::camera::Vec3 previous_head{};
  bool previous_head_valid = false;
  mecvr::ik::HeightCalibrator height_calibrator;
  std::uint64_t last_xr_diagnostic_frame = 0;
  const auto observe_frame = [&](const mecvr::openxr::FrameTiming& timing,
                                 const mecvr::openxr::LocatedViews& located) {
    g_xr_epoch.store(timing.frame_index, std::memory_order_release);
    const auto left_state =
        backend.controllerState(mecvr::openxr::Hand::kLeft);
    const auto right_state =
        backend.controllerState(mecvr::openxr::Hand::kRight);
    if (timing.frame_index >= last_xr_diagnostic_frame + 120) {
      last_xr_diagnostic_frame = timing.frame_index;
      const auto diagnostics = backend.diagnostics();
      const auto tracked_body = backend.bodyTracking();
      LogF("m3b xr status frames=%llu session=%s focus=%s views=%u "
           "left=%s right=%s body=%s mask=%08x confidence=%.3f\n",
           static_cast<unsigned long long>(diagnostics.frames_pumped),
           diagnostics.session_state.c_str(),
           diagnostics.focus_received ? "yes" : "no", located.views.size(),
           left_state.pose_valid ? "valid" : "invalid",
           right_state.pose_valid ? "valid" : "invalid",
           tracked_body.active ? "active" : "procedural",
           tracked_body.valid_mask, static_cast<double>(tracked_body.confidence));
      LogF("m3b xr pose head=(%.3f,%.3f,%.3f) left=(%.3f,%.3f,%.3f) "
           "right=(%.3f,%.3f,%.3f)\n",
           static_cast<double>(located.views.size() == 2
                                   ? 0.5 * (located.views[0].pose.position.x +
                                            located.views[1].pose.position.x)
                                   : 0.0),
           static_cast<double>(located.views.size() == 2
                                   ? 0.5 * (located.views[0].pose.position.y +
                                            located.views[1].pose.position.y)
                                   : 0.0),
           static_cast<double>(located.views.size() == 2
                                   ? 0.5 * (located.views[0].pose.position.z +
                                            located.views[1].pose.position.z)
                                   : 0.0),
           static_cast<double>(left_state.grip_pose.position.x),
           static_cast<double>(left_state.grip_pose.position.y),
           static_cast<double>(left_state.grip_pose.position.z),
           static_cast<double>(right_state.grip_pose.position.x),
           static_cast<double>(right_state.grip_pose.position.y),
           static_cast<double>(right_state.grip_pose.position.z));
    }
    mecvr::ik::MotionHand motion_left;
    motion_left.pose.valid = left_state.pose_valid;
    motion_left.pose.position = {left_state.grip_pose.position.x,
                                 left_state.grip_pose.position.y,
                                 left_state.grip_pose.position.z};
    motion_left.pose.orientation = {left_state.grip_pose.orientation.x,
                                   left_state.grip_pose.orientation.y,
                                   left_state.grip_pose.orientation.z,
                                   left_state.grip_pose.orientation.w};
    motion_left.trigger = left_state.trigger_value;
    motion_left.grip = left_state.squeeze_value;
    motion_left.palm_open = left_state.squeeze_value < 0.20f;
    mecvr::ik::MotionHand motion_right;
    motion_right.pose.valid = right_state.pose_valid;
    motion_right.pose.position = {right_state.grip_pose.position.x,
                                  right_state.grip_pose.position.y,
                                  right_state.grip_pose.position.z};
    motion_right.pose.orientation = {right_state.grip_pose.orientation.x,
                                    right_state.grip_pose.orientation.y,
                                    right_state.grip_pose.orientation.z,
                                    right_state.grip_pose.orientation.w};
    motion_right.trigger = right_state.trigger_value;
    motion_right.grip = right_state.squeeze_value;
    motion_right.palm_open = right_state.squeeze_value < 0.20f;
    const bool has_stereo_views = located.views.size() >= 2;
    const bool has_mono_view = !located.views.empty();
    mecvr::ik::FullBodyInput body_input;
    body_input.sequence = timing.frame_index;
    body_input.sample_time_ns = located.sample_time_ns;
    body_input.head.valid = located.sample_time_ns != 0 && has_mono_view;
    if (has_stereo_views) {
      body_input.head.position = {
          0.5 * (located.views[0].pose.position.x +
                 located.views[1].pose.position.x),
          0.5 * (located.views[0].pose.position.y +
                 located.views[1].pose.position.y),
          0.5 * (located.views[0].pose.position.z +
                 located.views[1].pose.position.z)};
    } else if (has_mono_view) {
      body_input.head.position = {located.views[0].pose.position.x,
                                  located.views[0].pose.position.y,
                                  located.views[0].pose.position.z};
    }
    if (has_mono_view) {
      body_input.head.orientation = {located.views[0].pose.orientation.x,
                                     located.views[0].pose.orientation.y,
                                     located.views[0].pose.orientation.z,
                                     located.views[0].pose.orientation.w};
    }
    body_input.left_hand = motion_left;
    body_input.right_hand = motion_right;
    const mecvr::openxr::BodyTrackingSnapshot tracked_body =
        backend.bodyTracking();
    body_input.has_tracked_body = tracked_body.active;
    if (tracked_body.active) {
      for (std::size_t i = 0; i < tracked_body.joints.size(); ++i) {
        if ((tracked_body.valid_mask & (1u << i)) == 0) continue;
        const auto& source = tracked_body.joints[i];
        auto& target = body_input.tracked_joints[i];
        target.position = {source.position.x, source.position.y,
                           source.position.z};
        target.orientation = {source.orientation.x, source.orientation.y,
                              source.orientation.z, source.orientation.w};
        target.valid = true;
      }
    }
    double calibrated_floor_y = 0.0;
    if (!height_calibrator.update(body_input.head.position.y,
                                  &calibrated_floor_y)) {
      calibrated_floor_y = body_input.head.position.y - 1.70;
    }
    // Keep X/Z body anchoring responsive while freezing Y at the calibrated
    // standing floor. Recomputing Y from the current head would erase the
    // physical-crouch signal before FullBodyAnimator sees it.
    body_input.floor_origin = {body_input.head.position.x, calibrated_floor_y,
                               body_input.head.position.z};
    const bool physically_crouched = mecvr::ik::IsPhysicallyCrouched(
        body_input.head.position.y, calibrated_floor_y);
    body_input.delta_seconds =
        timing.predicted_display_period_ns > 0
            ? static_cast<double>(timing.predicted_display_period_ns) / 1.0e9
            : 1.0 / 90.0;
    if (previous_head_valid && body_input.delta_seconds > 0.0) {
      const double inverse_dt = 1.0 / body_input.delta_seconds;
      body_input.velocity = {
          (body_input.head.position.x - previous_head.x) * inverse_dt,
          (body_input.head.position.y - previous_head.y) * inverse_dt,
          (body_input.head.position.z - previous_head.z) * inverse_dt};
    }
    previous_head = body_input.head.position;
    previous_head_valid = body_input.head.valid;
    const bool both_squeezed =
        (left_state.buttons & mecvr::openxr::kButtonSqueeze) != 0 &&
        (right_state.buttons & mecvr::openxr::kButtonSqueeze) != 0;
    body_input.climbing =
        both_squeezed && motion_left.pose.valid && motion_right.pose.valid &&
        motion_left.pose.position.y > body_input.head.position.y - 0.35 &&
        motion_right.pose.position.y > body_input.head.position.y - 0.35;
    body_input.grounded =
        !(body_input.velocity.y > 0.9 && motion_left.pose.valid &&
          motion_right.pose.valid &&
          motion_left.pose.position.y > body_input.head.position.y &&
          motion_right.pose.position.y > body_input.head.position.y);
    mecvr::input::PoseState left_hand_pose;
    left_hand_pose.valid = motion_left.pose.valid;
    left_hand_pose.quality = 1.0f;
    left_hand_pose.position[0] =
        static_cast<float>(motion_left.pose.position.x);
    left_hand_pose.position[1] =
        static_cast<float>(motion_left.pose.position.y);
    left_hand_pose.position[2] =
        static_cast<float>(motion_left.pose.position.z);
    mecvr::input::PoseState right_hand_pose;
    right_hand_pose.valid = motion_right.pose.valid;
    right_hand_pose.quality = 1.0f;
    right_hand_pose.position[0] =
        static_cast<float>(motion_right.pose.position.x);
    right_hand_pose.position[1] =
        static_cast<float>(motion_right.pose.position.y);
    right_hand_pose.position[2] =
        static_cast<float>(motion_right.pose.position.z);
    const bool hands_raised_for_jump = mecvr::input::HandsRaisedForJump(
        body_input.head.position.y, left_hand_pose, right_hand_pose);
    mecvr::ik::ParkourIntentInput parkour_input;
    parkour_input.head_position = body_input.head.position;
    parkour_input.floor_origin = body_input.floor_origin;
    parkour_input.velocity = body_input.velocity;
    parkour_input.left_hand = motion_left;
    parkour_input.right_hand = motion_right;
    parkour_input.grounded = body_input.grounded;
    const auto parkour = mecvr::ik::ClassifyParkourIntents(parkour_input);
    body_input.climbing = parkour.climbing;
    body_input.sliding = parkour.sliding;
    body_input.vaulting = parkour.vaulting;
    body_input.wall_running = parkour.wall_running;
    const auto body_frame = body_animator.update(body_input);
    auto solved_frame = body_frame;
    if (playback_clip != nullptr && playback_clip->durationSeconds() > 0.0) {
      const double duration = playback_clip->durationSeconds();
      playback_elapsed_seconds = std::fmod(
          playback_elapsed_seconds + body_input.delta_seconds, duration);
      mecvr::ik::HumanoidPoseFrame replayed;
      if (playback_clip->sample(playback_elapsed_seconds, &replayed)) {
        replayed.sequence = timing.frame_index;
        replayed.sample_time_ns = located.sample_time_ns;
        solved_frame = replayed;
      }
    }
    InterlockedIncrement64(&g_ik_frames);
    if (solved_frame.valid) {
      g_body_pose_mailbox.publish(solved_frame);
      InterlockedIncrement64(&g_ik_valid_frames);
      if (record_motion_clip) (void)motion_clip.append(solved_frame);
    }
    if (recenter_latch.update(
            (left_state.buttons & mecvr::openxr::kButtonMenu) != 0,
            (right_state.buttons & mecvr::openxr::kButtonMenu) != 0)) {
      backend.recenter();
      height_calibrator.reset();
      previous_head_valid = false;
      ++space_generation;
      LogF("m3b recenter requested generation=%llu\n",
           static_cast<unsigned long long>(space_generation));
    }
    if (located.sample_time_ns != 0 && has_mono_view) {
      mecvr::camera::XRFramePoseSnapshot snapshot;
      snapshot.sequence = ++sequence;
      snapshot.predicted_display_time_ns = timing.predicted_display_time_ns;
      snapshot.predicted_display_period_ns = timing.predicted_display_period_ns;
      snapshot.position_valid = body_input.head.valid;
      snapshot.orientation_valid = body_input.head.valid;
      snapshot.space_generation = space_generation;
      snapshot.head = located.views[0].pose;
      // OpenXR returns eye poses here; use their midpoint for the game
      // camera anchor so temporal stereo applies symmetric -IPD/+IPD
      // translation instead of treating the left eye as the head origin.
      if (has_stereo_views) {
        snapshot.head.position.x =
            0.5f * (located.views[0].pose.position.x +
                    located.views[1].pose.position.x);
        snapshot.head.position.y =
            0.5f * (located.views[0].pose.position.y +
                    located.views[1].pose.position.y);
        snapshot.head.position.z =
            0.5f * (located.views[0].pose.position.z +
                    located.views[1].pose.position.z);
      }
      snapshot.views[0] = located.views[0];
      snapshot.views[1] = has_stereo_views ? located.views[1] : located.views[0];
      snapshot.publish_time_ns = SteadyNs();
      g_pose_mailbox.publish(snapshot);
      g_xr_pose_sequence.store(snapshot.sequence, std::memory_order_release);
      if (stereo_on &&
          g_stereo_pair_epoch.load(std::memory_order_acquire) == 0) {
        g_stereo_pair_epoch.store(timing.frame_index,
                                  std::memory_order_release);
        g_stereo_pair_pose.store(snapshot.sequence,
                                 std::memory_order_release);
        g_stereo_eye.store(0, std::memory_order_release);
        g_stereo_eye_valid.store(true, std::memory_order_release);
      }
    }
    if (input_synth) {
      input_synth->update(
          static_cast<std::uint64_t>(SteadyNs() / 1000000),
          left_state, right_state,
          g_physical_crouch_input_enabled && physically_crouched,
          body_input.grounded && g_physical_jump_enabled &&
              hands_raised_for_jump,
          g_parkour_input_enabled ? parkour
                                   : mecvr::ik::ParkourIntents{});
    }
  };
  mecvr::openxr::XrFrameWorker worker(backend, mailbox,
                                      stereo_on ? &stereo_mailbox : nullptr,
                                      observe_frame, &gpu_mailbox,
                                      [&](mecvr::render::SharedCaptureRegistration* r) {
                                        return capture.sharedRegistration(r);
                                      },
                                      [&](bool ready) {
                                        capture.setGpuConsumerReady(ready);
                                      });
  std::uint64_t worker_chunks = 0;
  while (g_shutdown != nullptr &&
         WaitForSingleObject(g_shutdown, 0) != WAIT_OBJECT_0) {
    worker.run(180);  // bounded chunk; shutdown is checked between chunks.
    if (++worker_chunks % 5 == 0) {
      const auto stats = worker.stats();
      const auto diagnostics = backend.diagnostics();
      LogF("m3b transport gpu_submit=%llu gpu_fallback=%llu gpu_reg_fail=%llu "
           "gpu_timeout=%llu cpu_submit=%llu upload_fail=%llu active=%s\n",
           static_cast<unsigned long long>(stats.gpu_submitted),
           static_cast<unsigned long long>(stats.gpu_fallback),
           static_cast<unsigned long long>(stats.gpu_registration_failed),
           static_cast<unsigned long long>(diagnostics.gpu_acquire_timeout),
           static_cast<unsigned long long>(stats.submitted_new),
           static_cast<unsigned long long>(stats.upload_failed),
           diagnostics.gpu_transport_active ? "yes" : "no");
    }
  }
  worker.requestStop();
  if (input_synth) input_synth->releaseAll();
  if (record_motion_clip && !motion_clip.empty()) {
    LogF("m3b motion clip save path=%s frames=%zu result=%s\n",
         motion_clip_path, motion_clip.size(),
         motion_clip.save(motion_clip_path) ? "ok" : "failed");
  }
  g_live_capture.store(nullptr, std::memory_order_release);
  g_xr_epoch.store(0, std::memory_order_release);
  g_xr_pose_sequence.store(0, std::memory_order_release);
  g_stereo_pair_epoch.store(0, std::memory_order_release);
  g_stereo_pair_pose.store(0, std::memory_order_release);
  g_stereo_eye_valid.store(false, std::memory_order_release);
  backend.shutdown();
  return 0;
}
#endif

HANDLE g_worker_thread = nullptr;

unsigned __stdcall WorkerProc(void*) {
  WNDCLASSW wc{};
  wc.lpfnWndProc = &WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"MecvrM3aHidden";
  HWND hwnd = nullptr;
  if (RegisterClassW(&wc) != 0) {
    hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 64, 64, nullptr,
                           nullptr, wc.hInstance, nullptr);
  }
  ID3D11Device* own_device = nullptr;
  if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                  0, nullptr, 0, D3D11_SDK_VERSION,
                                  &own_device, nullptr, nullptr)) &&
      own_device != nullptr) {
    HookKnownClasses(own_device, hwnd);
    own_device->Release();
  } else {
    LogF("m3a WARN: own D3D11 device failed; bootstrap hooks missing\n");
  }
  if (hwnd != nullptr) DestroyWindow(hwnd);
  // Park until shutdown (keeps lifetime explicit; hooks persist).
  if (g_shutdown != nullptr) WaitForSingleObject(g_shutdown, INFINITE);
  return 0;
}

void BeginDetach(const char* reason) {
  if (InterlockedCompareExchange(&g_detaching, 1, 0) != 0) return;
  InterlockedExchange(&g_armed, 0);
  const std::int64_t now = SteadyNs();
  DumpStatus("m3a-final", now, g_first_present_ns);
  DumpRegistry();
  for (LONG i = 0; i < g_device_count && i < kMaxDeviceVtables; ++i) {
    DeviceEntry& e = g_devices[i];
    if (e.vtable == nullptr) continue;
    if (e.orig_create_deferred != nullptr)
      UnpatchSlot(e.vtable, kCreateDeferredContext,
                  e.orig_create_deferred);
    e = {};
  }
  for (LONG i = 0; i < g_device1_count && i < kMaxDeviceVtables; ++i) {
    Device1Entry& e = g_devices1[i];
    if (e.vtable == nullptr) continue;
    if (e.orig_get_immediate != nullptr)
      UnpatchSlot(e.vtable, kGetImmediateContext1, e.orig_get_immediate);
    if (e.orig_create_deferred != nullptr)
      UnpatchSlot(e.vtable, kCreateDeferredContext1,
                  e.orig_create_deferred);
    e = {};
  }
  for (LONG i = 0; i < g_d12_command_list_count && i < 8; ++i) {
    D12CommandListEntry& e = g_d12_command_lists[i];
    if (e.vtable == nullptr) continue;
    if (e.orig_draw_instanced != nullptr)
      UnpatchSlot(e.vtable, kD12GraphicsDrawInstanced,
                  e.orig_draw_instanced);
    if (e.orig_draw_indexed_instanced != nullptr)
      UnpatchSlot(e.vtable, kD12GraphicsDrawIndexedInstanced,
                  e.orig_draw_indexed_instanced);
    if (e.orig_close != nullptr)
      UnpatchSlot(e.vtable, kD12GraphicsClose, e.orig_close);
    e = {};
  }
  for (LONG i = 0; i < g_d12_queue_count && i < 4; ++i) {
    D12QueueEntry& e = g_d12_queues[i];
    if (e.vtable == nullptr) continue;
    if (e.orig_execute != nullptr)
      UnpatchSlot(e.vtable, kD12QueueExecuteCommandLists,
                  e.orig_execute);
    e = {};
  }
  for (LONG i = 0; i < g_d12_device_count && i < kMaxDeviceVtables; ++i) {
    D12DeviceEntry& e = g_d12_devices[i];
    if (e.vtable == nullptr) continue;
    if (e.orig_create_queue != nullptr)
      UnpatchSlot(e.vtable, kD12DeviceCreateCommandQueue,
                  e.orig_create_queue);
    if (e.orig_create_list != nullptr)
      UnpatchSlot(e.vtable, kD12DeviceCreateCommandList,
                  e.orig_create_list);
    e = {};
  }
  for (LONG i = 0; i < g_ctx_count && i < kMaxCtxVtables; ++i) {
    if (g_ctx[i].vtable == nullptr) continue;
    for (int h = 0; h < kH_Count; ++h) {
      if (g_ctx[i].orig[h] != nullptr) {
        UnpatchSlot(g_ctx[i].vtable, kCtxSlots[h], g_ctx[i].orig[h]);
        g_ctx[i].orig[h] = nullptr;
      }
    }
    g_ctx[i].vtable = nullptr;
  }
  for (LONG i = 0; i < g_vtable_count && i < kMaxVtables; ++i) {
    if (g_vtables[i].vtable == nullptr) continue;
    if (g_vtables[i].orig_present != nullptr) {
      UnpatchSlot(g_vtables[i].vtable, kPresentIndex,
                  g_vtables[i].orig_present);
      g_vtables[i].orig_present = nullptr;
    }
    if (g_vtables[i].orig_resize != nullptr) {
      UnpatchSlot(g_vtables[i].vtable, kResizeBuffersIndex,
                  g_vtables[i].orig_resize);
      g_vtables[i].orig_resize = nullptr;
    }
    g_vtables[i].vtable = nullptr;
  }
  for (LONG i = 0; i < g_factory_count && i < kMaxVtables; ++i) {
    if (g_factory_vtables[i] == nullptr) continue;
    void** vt = static_cast<void**>(g_factory_vtables[i]);
    if (g_factory_orig[i] != nullptr) {
      UnpatchSlot(vt, kCreateSwapChainIndex, g_factory_orig[i]);
      g_factory_orig[i] = nullptr;
    }
    if (g_factory_for_hwnd[i] != nullptr) {
      UnpatchSlot(vt, kCreateSwapChainForHwndIndex, g_factory_for_hwnd[i]);
      g_factory_for_hwnd[i] = nullptr;
    }
    g_factory_vtables[i] = nullptr;
  }
  if (g_worker_thread != nullptr) {
    WaitForSingleObject(g_worker_thread, 2000);
    CloseHandle(g_worker_thread);
    g_worker_thread = nullptr;
  }
#ifdef MECVR_M3B
  if (g_pose_thread != nullptr) {
    WaitForSingleObject(g_pose_thread, 2000);
    CloseHandle(g_pose_thread);
    g_pose_thread = nullptr;
  }
#endif
  LogF("m3a detach (%s) presents=%llu\n", reason, g_present_idx);
  if (g_log != INVALID_HANDLE_VALUE) {
    CloseHandle(g_log);
    g_log = INVALID_HANDLE_VALUE;
  }
  if (g_shutdown != nullptr) {
    CloseHandle(g_shutdown);
    g_shutdown = nullptr;
  }
  if (g_lock_ready) {
    DeleteCriticalSection(&g_lock);
    DeleteCriticalSection(&g_cb_lock);
    g_lock_ready = false;
  }
}

}  // namespace m3a_probe

using namespace m3a_probe;

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
  (void)module;
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(module);
    g_pid = GetCurrentProcessId();
    LARGE_INTEGER freq{};
    if (QueryPerformanceFrequency(&freq)) {
      g_qpc_freq = static_cast<std::int64_t>(freq.QuadPart);
    }
    wchar_t temp[MAX_PATH];
    wchar_t path[MAX_PATH];
    if (GetTempPathW(MAX_PATH, temp) == 0) return FALSE;
    if (FAILED(StringCchPrintfW(path, MAX_PATH, L"%smecvr_m3a_%lu.log", temp,
                                g_pid))) {
      return FALSE;
    }
    g_log = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_log == INVALID_HANDLE_VALUE) return FALSE;
    wchar_t evname[128];
    if (FAILED(StringCchPrintfW(evname, 128, L"Local\\MECVR_M3A_SHUTDOWN_%lu",
                                g_pid))) {
      return FALSE;
    }
    g_shutdown = CreateEventW(nullptr, TRUE, FALSE, evname);
    if (g_shutdown == nullptr) return FALSE;
    InitializeCriticalSection(&g_lock);
    InitializeCriticalSection(&g_cb_lock);
    g_lock_ready = true;
    LogF("m3a attached pid=%lu mode=%s\n", g_pid,
#ifdef MECVR_M3B
         "M3B-CAMERA-OVERRIDE"
#else
         "OBSERVE-CB-INTEL"
#endif
    );
    char palette_discovery[8] = {};
    g_palette_discovery_enabled =
        GetEnvironmentVariableA("MECVR_DISCOVER_PALETTES", palette_discovery,
                                sizeof(palette_discovery)) > 0 &&
         (palette_discovery[0] == '1' || palette_discovery[0] == 'y' ||
         palette_discovery[0] == 'Y');
    char performance_mode[24] = {};
    const DWORD performance_len = GetEnvironmentVariableA(
        "MECVR_PERFORMANCE_MODE", performance_mode,
        sizeof(performance_mode));
    const bool performance_profile =
        performance_len > 0 && _stricmp(performance_mode, "performance") == 0;
    if (performance_profile) g_palette_discovery_enabled = false;
    LogF("m3a palette discovery: %s\n",
         g_palette_discovery_enabled ? "enabled" : "disabled");
#ifdef MECVR_M3B
    char enable[8] = {};
    g_camera_enabled = GetEnvironmentVariableA(
                           "MECVR_ENABLE_CAMERA", enable, sizeof(enable)) >
                       0 &&
                        (enable[0] == '1' || enable[0] == 'y' ||
                         enable[0] == 'Y');
    char stereo[8] = {};
    g_stereo_enabled.store(
        GetEnvironmentVariableA("MECVR_ENABLE_STEREO", stereo,
                                sizeof(stereo)) > 0 &&
            (stereo[0] == '1' || stereo[0] == 'y' || stereo[0] == 'Y'),
        std::memory_order_release);
    LogF("m3b stereo: %s\n",
         g_stereo_enabled.load(std::memory_order_acquire)
             ? "experimental temporal producer enabled"
             : "disabled");
    char preserve_pacing[8] = {};
    g_preserve_runtime_pacing =
        GetEnvironmentVariableA("MECVR_PRESERVE_RUNTIME_PACING",
                                preserve_pacing, sizeof(preserve_pacing)) == 0 ||
        (preserve_pacing[0] != '0' && preserve_pacing[0] != 'n' &&
         preserve_pacing[0] != 'N');
    LogF("m3b runtime pacing/AFR preservation: %s\n",
         g_preserve_runtime_pacing ? "enabled (temporal stereo suppressed)"
                                    : "disabled");
    char native_map_path[512] = {};
    const DWORD native_map_len = GetEnvironmentVariableA(
        "MECVR_NATIVE_BONE_MAP", native_map_path,
        static_cast<DWORD>(sizeof(native_map_path)));
    if (native_map_len > 0 && native_map_len < sizeof(native_map_path)) {
      g_native_bone_map_loaded = mecvr::ik::LoadNativeBoneMap(
          native_map_path, &g_native_bone_map);
      LogF("m3b native bone contract: %s (%s)\n",
           g_native_bone_map_loaded ? "loaded" : "rejected", native_map_path);
    } else {
      LogF("m3b native bone contract: disabled (MECVR_NATIVE_BONE_MAP unset)\n");
    }
    char body_overlay[8] = {};
    g_body_overlay_enabled =
        GetEnvironmentVariableA("MECVR_ENABLE_BODY_OVERLAY", body_overlay,
                                sizeof(body_overlay)) > 0 &&
         (body_overlay[0] == '1' || body_overlay[0] == 'y' ||
         body_overlay[0] == 'Y');
    if (performance_profile) g_body_overlay_enabled = false;
    LogF("m3b performance profile: %s\n",
         performance_profile ? "performance" :
         (performance_len > 0 ? performance_mode : "balanced"));
    LogF("m3b body overlay: %s\n",
         g_body_overlay_enabled ? "enabled" : "disabled");
    char physical_jump[8] = {};
    const DWORD physical_jump_len = GetEnvironmentVariableA(
        "MECVR_ENABLE_PHYSICAL_JUMP", physical_jump, sizeof(physical_jump));
    g_physical_jump_enabled =
        physical_jump_len == 0 ||
        (physical_jump[0] != '0' && physical_jump[0] != 'n' &&
         physical_jump[0] != 'N');
    LogF("m3b physical jump gesture: %s\n",
         g_physical_jump_enabled ? "enabled" : "disabled");
    char physical_crouch_input[8] = {};
    const DWORD physical_crouch_input_len = GetEnvironmentVariableA(
        "MECVR_ENABLE_PHYSICAL_CROUCH_INPUT", physical_crouch_input,
        sizeof(physical_crouch_input));
    g_physical_crouch_input_enabled =
        physical_crouch_input_len == 0 ||
        (physical_crouch_input[0] != '0' && physical_crouch_input[0] != 'n' &&
         physical_crouch_input[0] != 'N');
    LogF("m3b physical crouch input: %s\n",
         g_physical_crouch_input_enabled ? "enabled" : "disabled");
    char parkour_input[8] = {};
    const DWORD parkour_input_len = GetEnvironmentVariableA(
        "MECVR_ENABLE_PARKOUR_INPUT", parkour_input, sizeof(parkour_input));
    g_parkour_input_enabled =
        parkour_input_len > 0 &&
        (parkour_input[0] == '1' || parkour_input[0] == 'y' ||
         parkour_input[0] == 'Y');
    LogF("m3b parkour gameplay bridge: %s\n",
         g_parkour_input_enabled ? "enabled (vault=Space climb=E slide=Ctrl)"
                                  : "disabled");
    char units[32] = {};
    if (GetEnvironmentVariableA("MECVR_UNITS_PER_METER", units,
                                sizeof(units)) > 0) {
      char* end = nullptr;
      const double parsed = std::strtod(units, &end);
      if (end != units && *end == '\0' && parsed > 0.0 && parsed <= 10000.0) {
        g_camera_units_per_meter = parsed;
      }
    }
    if (g_camera_enabled) {
      char input[8] = {};
      g_input_enabled = GetEnvironmentVariableA(
                            "MECVR_ENABLE_INPUT", input, sizeof(input)) >
                        0 &&
                        (input[0] == '1' || input[0] == 'y' ||
                         input[0] == 'Y');
      g_pose_thread = reinterpret_cast<HANDLE>(
          _beginthreadex(nullptr, 0, &PoseWorkerProc, nullptr, 0, nullptr));
      if (g_pose_thread == nullptr) {
        g_camera_enabled = false;
        LogF("m3b camera: pose worker failed; override disabled\n");
      }
      LogF("m3b camera: rotation=on translation=%s\n",
           g_camera_units_per_meter > 0.0 ? "calibrated" : "disabled");
      LogF("m3b input: %s\n", g_input_enabled ? "enabled" : "disabled");
    }
#endif
    g_worker_thread = reinterpret_cast<HANDLE>(
        _beginthreadex(nullptr, 0, &WorkerProc, nullptr, 0, nullptr));
    if (g_worker_thread == nullptr) {
      BeginDetach("worker-start-failed");
      return FALSE;
    }
  } else if (reason == DLL_PROCESS_DETACH) {
    BeginDetach("process-detach");
  }
  return TRUE;
}

extern "C" {

__declspec(dllexport) void MecvrM3aShutdown(void) {
  if (m3a_probe::g_shutdown != nullptr) SetEvent(m3a_probe::g_shutdown);
}

}  // extern "C"
