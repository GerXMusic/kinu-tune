#include "OnnxRuntimeLoader.h"

#if defined(_WIN32) && defined(HAVE_ONNXRUNTIME)

#include <windows.h>
#include <cstring>
#include <delayimp.h>
#include <onnxruntime_c_api.h>
#include <mutex>
#include <string>

namespace
{
    HMODULE loadedOnnxRuntime = nullptr;
    std::string loadFailure;

    HMODULE getCurrentModule()
    {
        HMODULE module = nullptr;
        const auto flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;

        if (!GetModuleHandleExW(flags,
                                reinterpret_cast<LPCWSTR>(&getCurrentModule),
                                &module))
        {
            return nullptr;
        }

        return module;
    }

    std::wstring getCurrentModuleDirectory()
    {
        HMODULE module = getCurrentModule();
        if (module == nullptr)
            return {};

        std::wstring path(MAX_PATH, L'\0');
        for (;;)
        {
            const auto length = GetModuleFileNameW(module, path.data(),
                                                   static_cast<DWORD>(path.size()));
            if (length == 0)
                return {};

            if (length < path.size() - 1)
            {
                path.resize(length);
                break;
            }

            path.resize(path.size() * 2);
        }

        const auto slash = path.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
            return {};

        path.resize(slash);
        return path;
    }

    HMODULE loadOnnxRuntimeFromLocalDirectory()
    {
        const auto moduleDirectory = getCurrentModuleDirectory();
        if (moduleDirectory.empty())
        {
            loadFailure = "could not locate the plug-in's own folder";
            return nullptr;
        }

        const auto dllPath = moduleDirectory + L"\\onnxruntime.dll";
        if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            loadFailure = "onnxruntime.dll is missing from the plug-in's folder";
            return nullptr;
        }

        // Error 126 here usually means a DLL that onnxruntime.dll needs is
        // missing: the Microsoft Visual C++ Redistributable.
        const auto handle = LoadLibraryW(dllPath.c_str());
        if (handle == nullptr)
            loadFailure = "LoadLibrary(onnxruntime.dll) failed with Windows error " +
                          std::to_string(GetLastError());

        return handle;
    }

    // Stands in for OrtGetApiBase when our onnxruntime.dll could not be
    // loaded: every API version is reported as unsupported.
    const OrtApi *ORT_API_CALL unavailableGetApi(uint32_t) NO_EXCEPTION { return nullptr; }
    const char *ORT_API_CALL unavailableGetVersionString() NO_EXCEPTION { return "unavailable"; }
    const OrtApiBase unavailableApiBase{ unavailableGetApi, unavailableGetVersionString };
    const OrtApiBase *ORT_API_CALL unavailableOrtGetApiBase() NO_EXCEPTION { return &unavailableApiBase; }

    // OnnxRuntime::initialise resolves OrtGetApiBase itself, so normally no
    // delay-loaded call reaches this. If one does, it must not end up in
    // the System32 copy: the delay-load helper only searches for the DLL
    // when this hook returns nullptr, so a failed load hands it this
    // plug-in's own module instead, where the lookup of OrtGetApiBase then
    // fails and delayFailureHook supplies the stand-in.
    FARPROC WINAPI delayLoadHook(unsigned dliNotify, PDelayLoadInfo delayInfo)
    {
        if (dliNotify == dliNotePreLoadLibrary &&
            delayInfo != nullptr &&
            delayInfo->szDll != nullptr &&
            _stricmp(delayInfo->szDll, "onnxruntime.dll") == 0)
        {
            auto *module = static_cast<HMODULE>(OnnxRuntimeLoader::ensureLoadedFromLocalDirectory());
            return reinterpret_cast<FARPROC>(module != nullptr ? module : getCurrentModule());
        }

        return nullptr;
    }

    FARPROC WINAPI delayFailureHook(unsigned dliNotify, PDelayLoadInfo delayInfo)
    {
        if (dliNotify == dliFailGetProc &&
            delayInfo != nullptr &&
            delayInfo->szDll != nullptr &&
            _stricmp(delayInfo->szDll, "onnxruntime.dll") == 0 &&
            delayInfo->dlp.fImportByName &&
            std::strcmp(delayInfo->dlp.szProcName, "OrtGetApiBase") == 0)
        {
            return reinterpret_cast<FARPROC>(&unavailableOrtGetApiBase);
        }

        return nullptr;
    }
}

extern "C" const PfnDliHook __pfnDliNotifyHook2 = delayLoadHook;
extern "C" const PfnDliHook __pfnDliFailureHook2 = delayFailureHook;

namespace OnnxRuntimeLoader
{
    void *ensureLoadedFromLocalDirectory()
    {
        static std::once_flag once;
        std::call_once(once, []()
        {
            loadedOnnxRuntime = loadOnnxRuntimeFromLocalDirectory();
        });

        return loadedOnnxRuntime;
    }

    std::string getLoadFailure()
    {
        ensureLoadedFromLocalDirectory();
        return loadedOnnxRuntime != nullptr ? std::string() : loadFailure;
    }

    void *findExport(const char *name)
    {
        auto *module = static_cast<HMODULE>(ensureLoadedFromLocalDirectory());
        return module != nullptr ? reinterpret_cast<void *>(GetProcAddress(module, name))
                                 : nullptr;
    }
}

#else

namespace OnnxRuntimeLoader
{
    void *ensureLoadedFromLocalDirectory() { return nullptr; }
    std::string getLoadFailure() { return {}; }
    void *findExport(const char *) { return nullptr; }
}

#endif
