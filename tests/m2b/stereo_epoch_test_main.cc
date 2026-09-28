#include <cstdint>
#include <iostream>
#include <memory>

#include "openxr/stereo_mailbox.h"
#include "render/stereo_frame.h"

namespace {

std::shared_ptr<mecvr::render::StereoFrame> Make(std::uint64_t epoch,
                                                  std::uint64_t pose,
                                                  std::uint8_t left,
                                                  std::uint8_t right) {
  auto frame = std::make_shared<mecvr::render::StereoFrame>();
  frame->epoch = epoch;
  frame->pose_sequence = pose;
  frame->capture_time_ns = 100;
  for (int eye = 0; eye < 2; ++eye) {
    frame->width[eye] = 2;
    frame->height[eye] = 1;
    frame->pixels_rgba[eye] = {eye == 0 ? left : right, 0, 0, 255,
                               eye == 0 ? left : right, 0, 0, 255};
  }
  return frame;
}

bool Check(bool value, const char* label) {
  if (!value) std::cerr << "FAIL: " << label << "\n";
  return value;
}

}  // namespace

int main() {
  using mecvr::render::M6EpochGate;
  using mecvr::render::M6EpochToken;
  using mecvr::render::SameStereoEpoch;
  using mecvr::render::StereoFrameFreshForDisplay;
  using mecvr::render::StereoFrameValid;
  auto frame = Make(7, 7, 10, 20);
  bool ok = true;
  ok &= Check(StereoFrameValid(*frame), "valid stereo frame");
  ok &= Check(SameStereoEpoch(*frame, 7, 7), "same epoch accepted");
  ok &= Check(!SameStereoEpoch(*frame, 8, 7), "cross epoch rejected");
  ok &= Check(!SameStereoEpoch(*frame, 7, 8), "cross pose rejected");
  ok &= Check(frame->pixels_rgba[0] != frame->pixels_rgba[1],
              "independent eye pixels preserved");
  ok &= Check(StereoFrameFreshForDisplay(*frame, 100),
              "fresh pair accepted at predicted display time");
  ok &= Check(StereoFrameFreshForDisplay(*frame, 250000100),
              "pair remains valid across several XR ticks");
  ok &= Check(!StereoFrameFreshForDisplay(*frame, 250000101),
              "old pair rejected by freshness window");
  ok &= Check(StereoFrameFreshForDisplay(*frame, 50),
              "slightly future pair accepted for clock jitter");
  ok &= Check(!StereoFrameFreshForDisplay(*frame, 0),
              "missing display timestamp rejected");

  auto malformed = Make(7, 7, 10, 20);
  malformed->pixels_rgba[1].pop_back();
  ok &= Check(!StereoFrameValid(*malformed),
              "malformed eye payload rejected");

  auto empty = Make(7, 7, 10, 20);
  empty->width[0] = 0;
  ok &= Check(!StereoFrameValid(*empty),
              "zero-sized eye rejected");

  auto mismatched_dimensions = Make(7, 7, 10, 20);
  mismatched_dimensions->width[1] = 4;
  mismatched_dimensions->pixels_rgba[1].resize(16);
  ok &= Check(!StereoFrameValid(*mismatched_dimensions),
              "mismatched eye dimensions rejected");

  mecvr::openxr::StereoMailbox mailbox(1);
  ok &= Check(mailbox.tryPublish(frame), "publish valid frame");
  ok &= Check(mailbox.consumeNewest() != nullptr, "consume newest frame");
  ok &= Check(!mailbox.tryPublish(Make(0, 1, 1, 2)),
              "invalid epoch rejected at mailbox");

  const M6EpochToken token{11, 12, 0x1234, 13, 14, 15};
  M6EpochGate gate;
  ok &= Check(gate.beginPair(token), "M6 pair begins with immutable token");
  ok &= Check(gate.commitLeft(token), "M6 left eye commits");
  ok &= Check(gate.beginRight(token), "M6 right eye begins after left");
  ok &= Check(gate.commitRight(token), "M6 right eye commits");
  ok &= Check(gate.canSubmit(), "M6 same-epoch pair may submit");

  auto RejectsMutation = [&](M6EpochToken changed, const char* label) {
    M6EpochGate changed_gate;
    const bool rejected = changed_gate.beginPair(token) &&
                          changed_gate.commitLeft(token) &&
                          !changed_gate.beginRight(changed) &&
                          !changed_gate.canSubmit();
    ok &= Check(rejected, label);
  };
  auto changed = token;
  ++changed.simulation_generation;
  RejectsMutation(changed, "M6 simulation mutation rejected");
  changed = token; ++changed.scene_generation;
  RejectsMutation(changed, "M6 scene mutation rejected");
  changed = token; ++changed.prepared_scene_identity;
  RejectsMutation(changed, "M6 prepared scene mutation rejected");
  changed = token; ++changed.pose_sequence;
  RejectsMutation(changed, "M6 pose mutation rejected");
  changed = token; ++changed.predicted_display_time;
  RejectsMutation(changed, "M6 display-time mutation rejected");
  changed = token; ++changed.space_generation;
  RejectsMutation(changed, "M6 space mutation rejected");

  M6EpochGate out_of_order;
  ok &= Check(!out_of_order.commitLeft(token),
              "M6 out-of-order eye commit rejected");
  M6EpochGate history_changed;
  ok &= Check(history_changed.beginPair(token) &&
                  !history_changed.commitLeft(token, true) &&
                  !history_changed.canSubmit(),
              "M6 temporal-history mutation rejected");
  std::cout << (ok ? "STEREO_EPOCH_TEST: PASS\n" : "STEREO_EPOCH_TEST: FAIL\n");
  return ok ? 0 : 1;
}
