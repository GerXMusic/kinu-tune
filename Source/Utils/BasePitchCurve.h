#pragma once

#include <vector>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/**
 * Generates a smoothed base pitch curve from note MIDI values.
 *
 * Algorithm: Step function smoothing with cosine-windowed convolution
 * Reference: ds-editor-lite BasePitchCurve (based on OpenSVIP Ace plugin)
 *
 * The algorithm:
 * 1. Creates a step function where each note has a constant semitone value
 * 2. At note boundaries, switches at the midpoint between notes
 * 3. Applies cosine-windowed convolution (81-point kernel, ~80 ms window)
 * 4. Results in a smooth base pitch curve that preserves note transitions
 */
class BasePitchCurve
{
public:
    struct NoteSegment {
        int startFrame;
        int endFrame;
        float midiNote;
        // target - original, in semitones. Zero for an unmodified note and for
        // callers that only need the plain base curve. D-182 uses it to find
        // the boundaries that need a glide.
        float shiftSemitones = 0.0f;
    };

    // Accessors for incremental preview utilities.
    static constexpr int kernelSize() { return KERNEL_SIZE; }
    static constexpr double smoothWindowSec() { return SMOOTH_WINDOW; }
    static const std::vector<double>& getCosineKernel();

    // Generate smoothed base pitch for a single note
    // Returns base pitch in MIDI note values for each frame
    static std::vector<float> generateForNote(int startFrame, int endFrame, float midiNote, int totalFrames);

    // Generate smoothed base pitch for multiple notes
    static std::vector<float> generateForNotes(const std::vector<NoteSegment>& notes, int totalFrames);

    // Calculate delta pitch (actual F0 in MIDI - base pitch)
    static std::vector<float> calculateDeltaPitch(const std::vector<float>& f0Values,
                                                   const std::vector<float>& basePitch,
                                                   int startFrame);

    // Apply base pitch change while preserving delta pitch
    // Returns new F0 values in Hz
    static std::vector<float> applyBasePitchChange(const std::vector<float>& deltaPitch,
                                                    float newBaseMidi,
                                                    int numFrames);

private:
    static constexpr int KERNEL_SIZE = 81;  // ~80ms total window at 1000Hz sampling
    static constexpr double SMOOTH_WINDOW = 0.08;  // 80ms total window for faster transitions

    static std::vector<double> createCosineKernel();
    static std::vector<float> generateSmoothed(const std::vector<NoteSegment>& notes,
                                                int totalFrames);
};
