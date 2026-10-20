#pragma once

#include "../JuceHeader.h"

/**
 * Initialises the ONNX Runtime C++ API for this plug-in, from the runtime
 * bundled with it.
 *
 * On macOS a host may already have a different ONNX Runtime loaded, and on
 * Windows the system ships an older onnxruntime.dll in System32. Both are
 * avoided by resolving OrtGetApiBase from the bundled copy explicitly
 * (ORT_API_MANUAL_INIT is defined on every platform).
 *
 * When the bundled runtime cannot be loaded - on Windows usually because the
 * Microsoft Visual C++ Redistributable is missing - nothing that uses ONNX
 * Runtime may run: the API pointer stays null and any Ort:: call would crash.
 * Every place that creates an Ort::Env or otherwise calls into ONNX Runtime
 * checks isAvailable() first, and the editor shows getUnavailableMessage().
 */
namespace OnnxRuntime
{
bool initialise(juce::String *errorMessage = nullptr);

/** True once the bundled runtime is loaded and initialised. */
bool isAvailable();

/** Why the runtime is unavailable, in English for logs (empty if available). */
juce::String getUnavailableReason();

/** Localised text for the user: what is off, how to fix it, and the reason. */
juce::String getUnavailableMessage();

/** Shows getUnavailableMessage() in a warning box, at most once per process
    (an ARA host analyses every clip, which would otherwise stack one box per
    clip). Safe to call from any thread. */
void showUnavailableMessageOnce();

/** Tests only: makes isAvailable() report false with this reason; an empty
    reason restores the real state. */
void forceUnavailableForTesting(const juce::String &reason);

/** Tests only: a test program is not inside a plug-in bundle, so it takes the
    API from the ONNX Runtime it links instead. Call before anything else
    touches ONNX Runtime. On Windows this is the same as initialise(). */
bool useLinkedRuntimeForTesting();
}
