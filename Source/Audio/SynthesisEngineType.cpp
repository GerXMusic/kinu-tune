#include "SynthesisEngineType.h"

#ifdef _WIN32
#include <d3d12.h>
#include <dxgi1_2.h>
#include <windows.h>

#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "dxgi.lib")
#endif
#endif

namespace
{
#ifdef _WIN32
/**
 * True when the adapter is an integrated GPU, i.e. D3D12 reports it as a UMA
 * (unified memory) architecture. d3d12.dll is loaded at runtime rather than
 * linked, so a machine without it simply reports "not integrated" and the
 * caller's other checks decide.
 *
 * Returns false via @p ok when the question could not be answered at all
 * (no D3D12 runtime, or the adapter cannot create a device) - an adapter
 * DirectML itself could not use.
 */
bool isIntegratedAdapter(IDXGIAdapter1 *adapter, bool &ok)
{
  ok = false;

  static const auto createDevice = []() -> PFN_D3D12_CREATE_DEVICE
  {
    if (auto *module = LoadLibraryW(L"d3d12.dll"))
      return reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(
          GetProcAddress(module, "D3D12CreateDevice"));
    return nullptr;
  }();

  if (createDevice == nullptr)
    return false;

  ID3D12Device *device = nullptr;
  if (FAILED(createDevice(adapter, D3D_FEATURE_LEVEL_11_0,
                          __uuidof(ID3D12Device),
                          reinterpret_cast<void **>(&device))) ||
      device == nullptr)
    return false;

  D3D12_FEATURE_DATA_ARCHITECTURE arch{};
  arch.NodeIndex = 0;
  const bool gotArch = SUCCEEDED(device->CheckFeatureSupport(
      D3D12_FEATURE_ARCHITECTURE, &arch, sizeof(arch)));
  device->Release();

  if (!gotArch)
    return false;

  ok = true;
  return arch.UMA != FALSE;
}

struct WindowsGpuCapabilities
{
  bool vocoderCapable = false;
  bool singleIntelAdapter = false;
};

// Two adapter objects can expose the SAME physical GPU: a virtual display that
// mirrors the only card (Parsec, remote desktop) makes DXGI list it twice with
// a different LUID. The policy below is about how many GPUs the machine has,
// not how many adapter objects it lists, so adapters with the same identity
// count once. Known limit: two identical real cards also share this identity
// and would count as one.
struct AdapterIdentity
{
  UINT vendor = 0;
  UINT device = 0;
  UINT subsystem = 0;
  juce::String name;
};

bool sameGpu (const AdapterIdentity &a, const AdapterIdentity &b)
{
  return a.vendor == b.vendor && a.device == b.device
         && a.subsystem == b.subsystem && a.name == b.name;
}

WindowsGpuCapabilities detectWindowsGpuCapabilities()
{
  IDXGIFactory1 *factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                reinterpret_cast<void **>(&factory))) ||
      factory == nullptr)
    return {};

  int hardwareAdapters = 0;
  std::vector<AdapterIdentity> gpus;
  bool loneAdapterNeedsClassic = false;
  bool loneAdapterIsIntel = false;

  for (UINT i = 0;; ++i)
  {
    IDXGIAdapter1 *adapter = nullptr;
    const auto hr = factory->EnumAdapters1(i, &adapter);
    if (hr == DXGI_ERROR_NOT_FOUND)
      break;
    if (FAILED(hr) || adapter == nullptr)
      continue;

    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
        (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0)
    {
      const AdapterIdentity identity { desc.VendorId, desc.DeviceId,
                                       desc.SubSysId,
                                       juce::String(desc.Description) };
      bool alreadyCounted = false;
      for (const auto &gpu : gpus)
        if (sameGpu (gpu, identity))
        {
          alreadyCounted = true;
          break;
        }

      if (!alreadyCounted)
      {
        gpus.push_back (identity);
        ++hardwareAdapters;
        if (hardwareAdapters == 1)
        {
          bool ok = false;
          const bool integrated = isIntegratedAdapter(adapter, ok);
          loneAdapterIsIntel =
              juce::String(desc.Description).containsIgnoreCase("Intel");
          // An adapter D3D12 cannot open is no better for the vocoder than an
          // integrated one: DirectML would fall back to the CPU either way.
          // Also prefer Classic for a lone Intel adapter, even if its driver
          // does not report UMA. Match the display name without case sensitivity.
          loneAdapterNeedsClassic =
              !ok || integrated || loneAdapterIsIntel;
        }
      }
    }
    adapter->Release();

    if (hardwareAdapters > 1)
      break;
  }

  factory->Release();

  return {hardwareAdapters > 1 ||
              (hardwareAdapters == 1 && !loneAdapterNeedsClassic),
          hardwareAdapters == 1 && loneAdapterIsIntel};
}

const WindowsGpuCapabilities &getWindowsGpuCapabilities()
{
  static const auto cached = detectWindowsGpuCapabilities();
  return cached;
}
#endif
} // namespace

SynthesisEngineType defaultSynthesisEngineType()
{
#if JUCE_LINUX
  return SynthesisEngineType::Psola;
#elif defined(_WIN32)
  return getWindowsGpuCapabilities().vocoderCapable
             ? SynthesisEngineType::Vocoder
             : SynthesisEngineType::Psola;
#else
  return SynthesisEngineType::Vocoder;
#endif
}

SynthesisEngineType synthesisEngineTypeForSavedPreference(SynthesisEngineType type)
{
#ifdef _WIN32
  if (getWindowsGpuCapabilities().singleIntelAdapter)
    return SynthesisEngineType::Psola;
#endif
  return type;
}
