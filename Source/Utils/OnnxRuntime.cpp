#include "OnnxRuntime.h"
#include "Localization.h"

#ifdef HAVE_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>

#include <atomic>
#include <mutex>

#include "AppLogger.h"
#include "OnnxRuntimeLoader.h"

#if JUCE_MAC
#include <dlfcn.h>
#endif

namespace OnnxRuntime
{
namespace
{
std::once_flag initialisationFlag;
bool initialised = false;
juce::String initialisationError;

// Set by tests to exercise the unavailable path; see the header.
std::atomic<bool> forcedUnavailable{false};
juce::String forcedReason;

#if JUCE_MAC
const OrtApiBase *getApiBase()
{
  Dl_info imageInfo{};
  if (dladdr(reinterpret_cast<const void *>(&getApiBase), &imageInfo) == 0 ||
      imageInfo.dli_fname == nullptr)
  {
    initialisationError = "Could not locate the Kinu Tune plug-in bundle";
    return nullptr;
  }

  const auto plugInBinary = juce::File(imageInfo.dli_fname);
  const auto runtimePath = plugInBinary.getParentDirectory()
                               .getParentDirectory()
                               .getChildFile("Frameworks")
                               .getChildFile("libonnxruntime.dylib");

  void *runtime = dlopen(runtimePath.getFullPathName().toRawUTF8(),
                         RTLD_NOW | RTLD_LOCAL);
  if (runtime == nullptr)
  {
    initialisationError = "Could not load Kinu Tune's ONNX Runtime: " +
                          juce::String(dlerror());
    return nullptr;
  }

  using GetApiBaseFunction = const OrtApiBase *(*)();
  auto *getApiBase = reinterpret_cast<GetApiBaseFunction>(
      dlsym(runtime, "OrtGetApiBase"));
  if (getApiBase == nullptr)
  {
    initialisationError = "Kinu Tune's ONNX Runtime has no OrtGetApiBase symbol";
    return nullptr;
  }

  return getApiBase();
}
#elif JUCE_WINDOWS
// Resolved from the onnxruntime.dll next to the plug-in, never through the
// delay-load import: that would fall back to the older copy in System32.
const OrtApiBase *getApiBase()
{
  if (OnnxRuntimeLoader::ensureLoadedFromLocalDirectory() == nullptr)
  {
    initialisationError = "Could not load Kinu Tune's ONNX Runtime: " +
                          juce::String(OnnxRuntimeLoader::getLoadFailure());
    return nullptr;
  }

  using GetApiBaseFunction = const OrtApiBase *(ORT_API_CALL *)();
  auto *getApiBase = reinterpret_cast<GetApiBaseFunction>(
      OnnxRuntimeLoader::findExport("OrtGetApiBase"));
  if (getApiBase == nullptr)
  {
    initialisationError = "Kinu Tune's ONNX Runtime has no OrtGetApiBase symbol";
    return nullptr;
  }

  return getApiBase();
}
#else
const OrtApiBase *getApiBase() { return OrtGetApiBase(); }
#endif
} // namespace

bool initialise(juce::String *errorMessage)
{
  std::call_once(initialisationFlag, [] {
    const auto *apiBase = getApiBase();
    const auto *api = apiBase == nullptr ? nullptr : apiBase->GetApi(ORT_API_VERSION);

    if (api == nullptr)
    {
      if (initialisationError.isEmpty())
        initialisationError = "Kinu Tune's ONNX Runtime does not support API version " +
                              juce::String(ORT_API_VERSION);

      // The release build writes no log file; on Windows this still reaches
      // the debugger output, which CI reads (tools/windows/vst3-instantiate).
      const auto line = "Kinu Tune: ONNX Runtime is unavailable, analysis is off: " +
                        initialisationError;
      juce::Logger::writeToLog(line);
      LOG(line);
      return;
    }

    Ort::InitApi(api);
    initialised = true;
  });

  if (forcedUnavailable.load())
  {
    if (errorMessage != nullptr)
      *errorMessage = forcedReason;
    return false;
  }

  if (!initialised && errorMessage != nullptr)
    *errorMessage = initialisationError;

  return initialised;
}

bool isAvailable() { return initialise(nullptr); }

juce::String getUnavailableReason()
{
  juce::String reason;
  initialise(&reason);
  return reason;
}

juce::String getUnavailableMessage()
{
  auto message = TR("error.onnx_runtime_unavailable");
#if JUCE_WINDOWS
  message << "\n\n" << TR("error.install_vc_redist") << "\n"
          << "https://aka.ms/vc14/vc_redist.x64.exe";
#endif
  message << "\n\n" << TR("error.details") << " " << getUnavailableReason();
  return message;
}

void forceUnavailableForTesting(const juce::String &reason)
{
  forcedReason = reason;
  forcedUnavailable.store(reason.isNotEmpty());
}

bool useLinkedRuntimeForTesting()
{
#if JUCE_WINDOWS
  return initialise();
#else
  std::call_once(initialisationFlag, [] {
    if (const auto *api = OrtGetApiBase()->GetApi(ORT_API_VERSION))
    {
      Ort::InitApi(api);
      initialised = true;
    }
  });
  return initialised;
#endif
}
} // namespace OnnxRuntime
#else
// Built without ONNX Runtime: there is nothing to initialise or guard.
namespace OnnxRuntime
{
bool initialise(juce::String *) { return true; }
bool isAvailable() { return true; }
juce::String getUnavailableReason() { return {}; }
juce::String getUnavailableMessage() { return TR("error.onnx_runtime_unavailable"); }
void forceUnavailableForTesting(const juce::String &) {}
bool useLinkedRuntimeForTesting() { return true; }
} // namespace OnnxRuntime
#endif
