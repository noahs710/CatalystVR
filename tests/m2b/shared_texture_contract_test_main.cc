#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

template <typename T>
void SafeRelease(T*& value) {
  if (value != nullptr) {
    value->Release();
    value = nullptr;
  }
}

void PrintHr(const char* operation, HRESULT hr) {
  std::printf("%s failed (HRESULT=0x%08lX)\n", operation,
              static_cast<unsigned long>(hr));
}

// These HRESULTs mean that the installed D3D/DXGI implementation does not
// expose the requested NT-handle sharing capability. E_INVALIDARG and all
// synchronization/data errors deliberately remain failures: they can indicate
// a broken descriptor or protocol rather than an unavailable capability.
bool IsSharingCapabilityUnavailable(HRESULT hr) {
  return hr == DXGI_ERROR_UNSUPPORTED || hr == E_NOTIMPL ||
         hr == E_NOINTERFACE;
}

int Skip(const char* operation, HRESULT hr) {
  std::printf(
      "SHARED_TEXTURE_CONTRACT_TEST: SKIP - WARP cross-device NT sharing "
      "unsupported at %s (HRESULT=0x%08lX)\n",
      operation, static_cast<unsigned long>(hr));
  return 0;
}

int Fail(const char* message) {
  std::printf("SHARED_TEXTURE_CONTRACT_TEST: FAIL - %s\n", message);
  return 1;
}

bool SameLuid(const LUID& a, const LUID& b) {
  return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

HRESULT GetAdapterLuid(ID3D11Device* device, LUID* luid) {
  if (device == nullptr || luid == nullptr) return E_POINTER;

  IDXGIDevice* dxgi_device = nullptr;
  IDXGIAdapter* adapter = nullptr;
  HRESULT hr = device->QueryInterface(__uuidof(IDXGIDevice),
                                      reinterpret_cast<void**>(&dxgi_device));
  if (SUCCEEDED(hr)) hr = dxgi_device->GetAdapter(&adapter);
  if (SUCCEEDED(hr)) {
    DXGI_ADAPTER_DESC desc{};
    hr = adapter->GetDesc(&desc);
    if (SUCCEEDED(hr)) *luid = desc.AdapterLuid;
  }
  SafeRelease(adapter);
  SafeRelease(dxgi_device);
  return hr;
}

}  // namespace

int main() {
  constexpr UINT kWidth = 8;
  constexpr UINT kHeight = 4;
  constexpr UINT kBytesPerPixel = 4;
  constexpr UINT kRowBytes = kWidth * kBytesPerPixel;

  int result = 1;
  HANDLE shared_handle = nullptr;
  ID3D11Device* producer_device = nullptr;
  ID3D11DeviceContext* producer_context = nullptr;
  ID3D11Device* consumer_device = nullptr;
  ID3D11DeviceContext* consumer_context = nullptr;
  ID3D11Device1* consumer_device1 = nullptr;
  ID3D11Texture2D* upload = nullptr;
  ID3D11Texture2D* shared_producer = nullptr;
  ID3D11Texture2D* shared_consumer = nullptr;
  ID3D11Texture2D* readback = nullptr;
  IDXGIResource1* shared_resource = nullptr;
  IDXGIKeyedMutex* producer_mutex = nullptr;
  IDXGIKeyedMutex* consumer_mutex = nullptr;
  bool producer_owned = false;
  bool consumer_owned = false;

  const auto finish = [&]() {
    if (consumer_owned && consumer_mutex != nullptr)
      (void)consumer_mutex->ReleaseSync(0);
    if (producer_owned && producer_mutex != nullptr)
      (void)producer_mutex->ReleaseSync(1);
    SafeRelease(consumer_mutex);
    SafeRelease(producer_mutex);
    SafeRelease(shared_resource);
    SafeRelease(readback);
    SafeRelease(shared_consumer);
    SafeRelease(shared_producer);
    SafeRelease(upload);
    SafeRelease(consumer_device1);
    SafeRelease(consumer_context);
    SafeRelease(consumer_device);
    SafeRelease(producer_context);
    SafeRelease(producer_device);
    if (shared_handle != nullptr) {
      CloseHandle(shared_handle);
      shared_handle = nullptr;
    }
    return result;
  };

  HRESULT hr = D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
      D3D11_SDK_VERSION, &producer_device, nullptr, &producer_context);
  if (FAILED(hr)) {
    PrintHr("producer D3D11CreateDevice(WARP)", hr);
    result = Fail("producer WARP device creation failed");
    return finish();
  }
  hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                         D3D11_SDK_VERSION, &consumer_device, nullptr,
                         &consumer_context);
  if (FAILED(hr)) {
    PrintHr("consumer D3D11CreateDevice(WARP)", hr);
    result = Fail("consumer WARP device creation failed");
    return finish();
  }

  LUID producer_luid{};
  LUID consumer_luid{};
  hr = GetAdapterLuid(producer_device, &producer_luid);
  if (FAILED(hr)) {
    PrintHr("producer adapter LUID query", hr);
    result = Fail("could not query producer adapter LUID");
    return finish();
  }
  hr = GetAdapterLuid(consumer_device, &consumer_luid);
  if (FAILED(hr)) {
    PrintHr("consumer adapter LUID query", hr);
    result = Fail("could not query consumer adapter LUID");
    return finish();
  }
  if (!SameLuid(producer_luid, consumer_luid)) {
    std::printf("producer LUID=%08lX:%08lX consumer LUID=%08lX:%08lX\n",
                static_cast<unsigned long>(producer_luid.HighPart),
                static_cast<unsigned long>(producer_luid.LowPart),
                static_cast<unsigned long>(consumer_luid.HighPart),
                static_cast<unsigned long>(consumer_luid.LowPart));
    result = Fail("the two WARP devices report incompatible adapter LUIDs");
    return finish();
  }

  hr = consumer_device->QueryInterface(
      __uuidof(ID3D11Device1), reinterpret_cast<void**>(&consumer_device1));
  if (FAILED(hr)) {
    if (IsSharingCapabilityUnavailable(hr)) {
      result = Skip("ID3D11Device1 query", hr);
      return finish();
    }
    PrintHr("ID3D11Device1 query", hr);
    result = Fail("consumer device interface query failed");
    return finish();
  }

  D3D11_TEXTURE2D_DESC shared_desc{};
  shared_desc.Width = kWidth;
  shared_desc.Height = kHeight;
  shared_desc.MipLevels = 1;
  shared_desc.ArraySize = 1;
  shared_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  shared_desc.SampleDesc.Count = 1;
  shared_desc.Usage = D3D11_USAGE_DEFAULT;
  shared_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  shared_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                          D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
  hr = producer_device->CreateTexture2D(&shared_desc, nullptr,
                                        &shared_producer);
  if (FAILED(hr)) {
    if (IsSharingCapabilityUnavailable(hr)) {
      result = Skip("CreateTexture2D(shared NT handle)", hr);
      return finish();
    }
    PrintHr("CreateTexture2D(shared NT handle)", hr);
    result = Fail("shared texture creation failed");
    return finish();
  }

  hr = shared_producer->QueryInterface(
      __uuidof(IDXGIResource1), reinterpret_cast<void**>(&shared_resource));
  if (FAILED(hr)) {
    if (IsSharingCapabilityUnavailable(hr)) {
      result = Skip("IDXGIResource1 query", hr);
      return finish();
    }
    PrintHr("IDXGIResource1 query", hr);
    result = Fail("shared texture does not expose IDXGIResource1");
    return finish();
  }
  hr = shared_resource->CreateSharedHandle(
      nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
      &shared_handle);
  if (FAILED(hr)) {
    if (IsSharingCapabilityUnavailable(hr)) {
      result = Skip("IDXGIResource1::CreateSharedHandle", hr);
      return finish();
    }
    PrintHr("IDXGIResource1::CreateSharedHandle", hr);
    result = Fail("NT shared-handle creation failed");
    return finish();
  }
  hr = consumer_device1->OpenSharedResource1(
      shared_handle, __uuidof(ID3D11Texture2D),
      reinterpret_cast<void**>(&shared_consumer));
  if (FAILED(hr)) {
    if (IsSharingCapabilityUnavailable(hr)) {
      result = Skip("ID3D11Device1::OpenSharedResource1", hr);
      return finish();
    }
    PrintHr("ID3D11Device1::OpenSharedResource1", hr);
    result = Fail("consumer could not open the NT shared texture");
    return finish();
  }

  hr = shared_producer->QueryInterface(
      __uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&producer_mutex));
  if (FAILED(hr)) {
    PrintHr("producer IDXGIKeyedMutex query", hr);
    result = Fail("producer shared texture has no keyed mutex");
    return finish();
  }
  hr = shared_consumer->QueryInterface(
      __uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&consumer_mutex));
  if (FAILED(hr)) {
    PrintHr("consumer IDXGIKeyedMutex query", hr);
    result = Fail("consumer shared texture has no keyed mutex");
    return finish();
  }

  std::uint8_t expected[kHeight][kRowBytes]{};
  for (UINT y = 0; y < kHeight; ++y) {
    for (UINT x = 0; x < kWidth; ++x) {
      const UINT p = x * kBytesPerPixel;
      expected[y][p + 0] = static_cast<std::uint8_t>(17u + x * 19u + y * 3u);
      expected[y][p + 1] = static_cast<std::uint8_t>(31u + x * 5u + y * 23u);
      expected[y][p + 2] = static_cast<std::uint8_t>(47u + x * 11u + y * 7u);
      expected[y][p + 3] = static_cast<std::uint8_t>(255u - x - y);
    }
  }

  D3D11_TEXTURE2D_DESC upload_desc = shared_desc;
  upload_desc.BindFlags = 0;
  upload_desc.MiscFlags = 0;
  D3D11_SUBRESOURCE_DATA initial{};
  initial.pSysMem = expected;
  initial.SysMemPitch = kRowBytes;
  initial.SysMemSlicePitch = sizeof(expected);
  hr = producer_device->CreateTexture2D(&upload_desc, &initial, &upload);
  if (FAILED(hr)) {
    PrintHr("producer upload texture creation", hr);
    result = Fail("could not create producer upload texture");
    return finish();
  }

  D3D11_TEXTURE2D_DESC staging_desc = shared_desc;
  staging_desc.Usage = D3D11_USAGE_STAGING;
  staging_desc.BindFlags = 0;
  staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  staging_desc.MiscFlags = 0;
  hr = consumer_device->CreateTexture2D(&staging_desc, nullptr, &readback);
  if (FAILED(hr)) {
    PrintHr("consumer staging texture creation", hr);
    result = Fail("could not create consumer readback texture");
    return finish();
  }

  hr = producer_mutex->AcquireSync(0, 0);  // Nonblocking producer acquire.
  if (FAILED(hr)) {
    PrintHr("producer AcquireSync(key=0, timeout=0)", hr);
    result = Fail("producer failed to acquire the initial mutex key");
    return finish();
  }
  producer_owned = true;
  producer_context->CopyResource(shared_producer, upload);
  hr = producer_mutex->ReleaseSync(1);
  if (FAILED(hr)) {
    PrintHr("producer ReleaseSync(key=1)", hr);
    result = Fail("producer failed to hand off key 1");
    return finish();
  }
  producer_owned = false;

  hr = consumer_mutex->AcquireSync(1, 0);  // Nonblocking consumer acquire.
  if (FAILED(hr)) {
    PrintHr("consumer AcquireSync(key=1, timeout=0)", hr);
    result = Fail("consumer failed the nonblocking producer handoff");
    return finish();
  }
  consumer_owned = true;
  consumer_context->CopyResource(readback, shared_consumer);
  hr = consumer_mutex->ReleaseSync(0);
  if (FAILED(hr)) {
    PrintHr("consumer ReleaseSync(key=0)", hr);
    result = Fail("consumer failed to return key 0");
    return finish();
  }
  consumer_owned = false;

  D3D11_MAPPED_SUBRESOURCE mapped{};
  hr = consumer_context->Map(readback, 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(hr)) {
    PrintHr("consumer staging Map", hr);
    result = Fail("consumer verification readback failed");
    return finish();
  }
  bool pixels_match = mapped.RowPitch >= kRowBytes;
  for (UINT y = 0; pixels_match && y < kHeight; ++y) {
    const auto* actual = static_cast<const std::uint8_t*>(mapped.pData) +
                         static_cast<size_t>(y) * mapped.RowPitch;
    pixels_match = std::memcmp(actual, expected[y], kRowBytes) == 0;
  }
  consumer_context->Unmap(readback, 0);
  if (!pixels_match) {
    result = Fail("consumer readback does not match the producer pixel pattern");
    return finish();
  }

  std::printf("adapter LUID=%08lX:%08lX; %ux%u RGBA transfer verified\n",
              static_cast<unsigned long>(producer_luid.HighPart),
              static_cast<unsigned long>(producer_luid.LowPart), kWidth,
              kHeight);
  std::printf("SHARED_TEXTURE_CONTRACT_TEST: PASS\n");
  result = 0;
  return finish();
}
