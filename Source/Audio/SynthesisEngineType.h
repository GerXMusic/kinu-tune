#pragma once

#include "../JuceHeader.h"

#include <atomic>

/**
 * Engines available for resynthesising edited regions.
 */
enum class SynthesisEngineType
{
    Vocoder = 0,   // PC-NSF-HiFiGAN neural vocoder (mel + F0 -> waveform)
    Psola,         // Classic - time-domain PSOLA (pitch-synchronous overlap-add)
    Pitchcore      // Our own classic engine, M2 step 1 (D-089, D-096)
};

/**
 * Default engine for a fresh install/instance with no saved preference.
 *
 * - macOS: the neural vocoder, which Core ML always takes off the CPU.
 * - Linux: classic PSOLA, since the vocoder only runs on CPU there (no
 *   CUDA/DirectML/CoreML execution provider).
 * - Windows: the vocoder only when there is real GPU headroom for it. A
 *   machine with no hardware adapter, or whose only adapter is an integrated
 *   GPU sharing system memory, starts on classic PSOLA instead - DirectML on an
 *   iGPU is barely faster than the CPU for this model, and every edit would
 *   render slowly. A lone adapter whose name contains "Intel" also starts on
 *   classic, regardless of its reported memory architecture. Other machines
 *   with a discrete card (including iGPU + dGPU laptops) keep the vocoder.
 *
 * The Windows check touches DXGI/D3D12, so the result is computed once and
 * cached for the life of the process.
 */
SynthesisEngineType defaultSynthesisEngineType();

/**
 * Apply hardware policy when restoring an engine preference. On Windows a
 * single Intel adapter takes precedence over the saved engine. Explicit engine
 * changes during the session remain available.
 */
SynthesisEngineType synthesisEngineTypeForSavedPreference(SynthesisEngineType type);

/**
 * One engine setting shared by every EditorController that renders for the
 * same plug-in instance.
 *
 * The plug-in keeps several controllers (the composite analysis, the region on
 * the canvas, one per modification rendered in the background) and binds the
 * editor to just one of them. A change made in the editor used to reach that
 * one controller while the others went on rendering with the default engine.
 * Controllers that share a preference read and write this single value
 * (EditorController::shareSynthesisEngine), so there is nothing to keep in
 * step and nothing a new controller can miss.
 */
struct SynthesisEnginePreference
{
    std::atomic<SynthesisEngineType> value{defaultSynthesisEngineType()};
};

/**
 * Convert SynthesisEngineType to string for display/storage.
 */
inline const char* synthesisEngineTypeToString(SynthesisEngineType type)
{
    switch (type)
    {
        case SynthesisEngineType::Psola:   return "PSOLA";
        case SynthesisEngineType::Pitchcore: return "Pitchcore";
        case SynthesisEngineType::Vocoder: return "Vocoder";
        default:                           return "Vocoder";
    }
}

/**
 * Convert string to SynthesisEngineType.
 */
inline SynthesisEngineType stringToSynthesisEngineType(const juce::String& str)
{
    if (str == "PSOLA")
        return SynthesisEngineType::Psola;
    if (str == "Pitchcore")
        return SynthesisEngineType::Pitchcore;
    if (str == "Vocoder")
        return SynthesisEngineType::Vocoder;
    return defaultSynthesisEngineType();
}
