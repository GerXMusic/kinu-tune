// Kinu Tune - Copyright (C) 2026 Keinoaza
// Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
//
// Loads a Windows VST3 the way a host does on insert: load the DLL, create the
// audio component (this runs the AudioProcessor constructor), initialise it,
// set up processing and activate it, then tear it all down again.
//
// It exists to reproduce a host that runs on an older Visual C++ runtime. This
// program links the C++ runtime statically, so it never loads MSVCP140.dll
// itself; when the plug-in imports MSVCP140.dll, Windows looks in this
// program's folder first. Copying an old msvcp140.dll next to this program
// therefore gives the plug-in exactly what a DAW that ships an old app-local
// runtime gives it. Such a DAW has loaded its runtime long before any plug-in,
// so runtime DLLs found next to this program are loaded first, too. On a
// machine without any Visual C++ runtime this program still runs, which
// pluginval cannot: it uses the shared runtime and is itself built with a
// toolset that needs MSVCP140 14.40 or later.
//
// After the run, onnxruntime.dll must have come from the plug-in's own folder.
// Windows ships an older onnxruntime.dll in System32, and the delay-load
// helper falls back to it when the bundled one cannot be loaded.
//
// Usage: vst3_instantiate <path to the DLL inside Contents\x86_64-win>
// Exit codes: 0 = every stage passed, 1 = usage, 2 = the DLL or its factory
// could not be loaded, 3 = a stage crashed or failed.

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"

using namespace Steinberg;

namespace
{
const TUID componentIid = INLINE_UID (0xE831FF31, 0xF2D54301, 0x928EBBEE, 0x25697802);
const TUID audioProcessorIid = INLINE_UID (0x42043F99, 0xB7DA453C, 0xA569E79D, 0x9AAEC33D);

using GetFactoryProc = IPluginFactory* (PLUGIN_API*) ();
using InitDllProc = bool (PLUGIN_API*) ();

struct Context
{
    IPluginFactory* factory = nullptr;
    char classId[sizeof (TUID)] {};
    Vst::IComponent* component = nullptr;
    Vst::IAudioProcessor* processor = nullptr;
    bool ok = true;
};

void fail (Context& c, const char* what, tresult result)
{
    std::printf ("  %s returned %d\n", what, static_cast<int> (result));
    c.ok = false;
}

void createComponent (Context& c)
{
    void* object = nullptr;
    const auto result = c.factory->createInstance (c.classId, componentIid, &object);
    if (result != kResultOk || object == nullptr)
        return fail (c, "createInstance(IComponent)", result);

    c.component = static_cast<Vst::IComponent*> (object);
}

void initialiseComponent (Context& c)
{
    if (const auto result = c.component->initialize (nullptr); result != kResultOk)
        fail (c, "IComponent::initialize", result);
}

void activate (Context& c)
{
    void* object = nullptr;
    if (const auto result = c.component->queryInterface (audioProcessorIid, &object);
        result != kResultOk || object == nullptr)
        return fail (c, "queryInterface(IAudioProcessor)", result);

    c.processor = static_cast<Vst::IAudioProcessor*> (object);

    Vst::ProcessSetup setup {};
    setup.processMode = Vst::kRealtime;
    setup.symbolicSampleSize = Vst::kSample32;
    setup.maxSamplesPerBlock = 512;
    setup.sampleRate = 48000.0;

    if (const auto result = c.processor->setupProcessing (setup); result != kResultOk)
        return fail (c, "IAudioProcessor::setupProcessing", result);

    if (const auto result = c.component->setActive (true); result != kResultOk)
        return fail (c, "IComponent::setActive(true)", result);

    c.component->setActive (false);
}

void tearDown (Context& c)
{
    if (c.processor != nullptr)
        c.processor->release();

    c.component->terminate();
    c.component->release();
    c.component = nullptr;
}

// Structured exception handling cannot share a function with C++ objects that
// need unwinding, so each stage runs through this plain wrapper.
bool runStage (const char* name, void (*stage) (Context&), Context& c)
{
    std::printf ("stage: %s\n", name);
    std::fflush (stdout);

    __try
    {
        stage (c);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        std::printf ("  CRASHED with exception 0x%08lX\n", GetExceptionCode());
        std::fflush (stdout);
        return false;
    }

    return c.ok;
}

const wchar_t* const runtimeDlls[] = { L"vcruntime140.dll", L"vcruntime140_1.dll",
                                       L"msvcp140.dll", L"msvcp140_1.dll" };

std::wstring loadedPath (const wchar_t* name)
{
    wchar_t path[MAX_PATH] {};
    if (auto* module = GetModuleHandleW (name))
        GetModuleFileNameW (module, path, MAX_PATH);

    return path;
}

std::wstring directoryOf (const std::wstring& path)
{
    const auto slash = path.find_last_of (L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr (0, slash);
}

void printLoadedRuntime()
{
    for (const auto* name : runtimeDlls)
    {
        const auto path = loadedPath (name);
        std::printf ("  %ls: %ls\n", name, path.empty() ? L"(not loaded)" : path.c_str());
    }

    const auto path = loadedPath (L"onnxruntime.dll");
    std::printf ("  onnxruntime.dll: %ls\n", path.empty() ? L"(not loaded)" : path.c_str());
}

// Loads the runtime DLLs that sit next to this program, as a DAW with an
// app-local runtime has done by the time it loads a plug-in.
void preloadLocalRuntime()
{
    wchar_t self[MAX_PATH] {};
    GetModuleFileNameW (nullptr, self, MAX_PATH);
    const auto folder = directoryOf (self);

    for (const auto* name : runtimeDlls)
    {
        const auto path = folder + L"\\" + name;
        if (GetFileAttributesW (path.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            const bool loaded = LoadLibraryW (path.c_str()) != nullptr;
            std::printf ("preloaded %ls: %s\n", path.c_str(), loaded ? "ok" : "FAILED");
        }
    }
}
} // namespace

int wmain (int argc, wchar_t** argv)
{
    if (argc != 2)
    {
        std::printf ("usage: vst3_instantiate <plug-in DLL>\n");
        return 1;
    }

    preloadLocalRuntime();

    std::printf ("loading %ls\n", argv[1]);
    auto* module = LoadLibraryW (argv[1]);
    if (module == nullptr)
    {
        std::printf ("  LoadLibrary failed with error %lu\n", GetLastError());
        return 2;
    }

    printLoadedRuntime();

    if (auto* initDll = reinterpret_cast<InitDllProc> (GetProcAddress (module, "InitDll")))
        initDll();

    auto* getFactory = reinterpret_cast<GetFactoryProc> (GetProcAddress (module, "GetPluginFactory"));
    Context c;
    c.factory = getFactory != nullptr ? getFactory() : nullptr;
    if (c.factory == nullptr)
    {
        std::printf ("  no plug-in factory\n");
        return 2;
    }

    bool found = false;
    for (int32 i = 0; i < c.factory->countClasses(); ++i)
    {
        PClassInfo info {};
        if (c.factory->getClassInfo (i, &info) == kResultOk &&
            std::strcmp (info.category, kVstAudioEffectClass) == 0)
        {
            std::printf ("audio component: %s\n", info.name);
            std::memcpy (c.classId, info.cid, sizeof (TUID));
            found = true;
            break;
        }
    }

    if (! found)
    {
        std::printf ("  the factory has no audio component\n");
        return 2;
    }

    bool passed = runStage ("create the audio component", createComponent, c)
               && runStage ("initialise", initialiseComponent, c)
               && runStage ("set up processing and activate", activate, c)
               && runStage ("tear down", tearDown, c);

    std::printf ("runtime after the run:\n");
    printLoadedRuntime();

    const auto onnxRuntimeFolder = directoryOf (loadedPath (L"onnxruntime.dll"));
    if (_wcsicmp (onnxRuntimeFolder.c_str(), directoryOf (argv[1]).c_str()) != 0)
    {
        std::printf ("  onnxruntime.dll was not loaded from the plug-in's folder\n");
        passed = false;
    }
    std::printf (passed ? "RESULT: PASS\n" : "RESULT: FAIL\n");
    return passed ? 0 : 3;
}
