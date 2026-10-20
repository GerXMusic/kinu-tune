#pragma once

#include <string>

namespace OnnxRuntimeLoader
{
    /**
     * Windows: loads onnxruntime.dll from this plug-in's own folder by full
     * path, once, and returns its module handle (nullptr if it could not be
     * loaded). Never searches anywhere else: Windows ships an older
     * onnxruntime.dll in System32 that does not support the API this
     * plug-in needs. Other platforms: does nothing and returns nullptr.
     */
    void *ensureLoadedFromLocalDirectory();

    /** Why the load failed, in English for the log (empty after success). */
    std::string getLoadFailure();

    /** Address of an export of the loaded onnxruntime.dll, or nullptr. */
    void *findExport(const char *name);
}
