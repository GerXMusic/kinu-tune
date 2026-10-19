// Kinu Tune - Copyright (C) 2026 Keinoaza
// Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#pragma once

#include "../../JuceHeader.h"
#include "../../Audio/AudioEngine.h"
#include "../../Models/Project.h"
#include <array>
#include <functional>

// The only way the editor sounds anything besides normal playback. The dragged
// segment's feedback, the piano key tone and the region preview share one
// output: the host's preview path in the plug-in, the AudioEngine's audition
// loop in the standalone app. Starting a source here takes the output from the
// source that had it, so a new kind of audition cannot leave the previous one
// sounding. The host callbacks live only here (MainComponent forwards its
// setters), and the SourceGuard_audition-output test fails when code outside
// this file and AudioEngine calls the engine's audition loop directly.
// A new kind of audition: add it to Source (before Count) and register what
// it does when taken over in MainComponent's constructor.
class AuditionOutput
{
public:
    enum class Source
    {
        None,
        DraggedSegment,
        PianoKey,
        RegionPreview,
        Count // not a source; keep it last
    };

    // Called on a source when another one or a transport command takes the
    // output. stopSound false: the new source plays its own loop over the same
    // output, which replaces this one's sound. stopSound true: silence it (a
    // transport command, or a new source that does not play a loop).
    using TakenOver = std::function<void(bool stopSound)>;

    void setOnTakenOver(Source source, TakenOver handler)
    {
        if (index(source) < sourceCount)
            handlers[index(source)] = std::move(handler);
    }

    // Plug-in backends (set by the plug-in editor through MainComponent).
    void setStartHostPreview(std::function<void(const Project &, int, int)> start)
    {
        startHostPreviewRange = std::move(start);
    }
    void setStopHostPreview(std::function<void()> stop)
    {
        stopHostPreviewRange = std::move(stop);
    }
    // (buffer, sample rate, wholeSegment): true repeats the whole buffer from
    // its start (drag feedback), false loops it seamlessly (key tone).
    void setPlayHostLoop(std::function<void(const juce::AudioBuffer<float> &, double, bool)> play)
    {
        playHostLoop = std::move(play);
    }
    void setStopHostLoop(std::function<void()> stop) { stopHostLoop = std::move(stop); }
    // Standalone backend and the mode switch (set by MainComponent).
    void setEngine(std::function<AudioEngine *()> engineGetter,
                   std::function<bool()> pluginModeGetter)
    {
        engine = std::move(engineGetter);
        isPluginMode = std::move(pluginModeGetter);
    }

    Source owner() const { return current; }

    // A source is about to sound: the output is taken from its current owner.
    // Its loop is replaced when the new source plays a loop too, and stopped
    // otherwise.
    void claim(Source source)
    {
        if (source == current)
            return;
        const Source previous = current;
        current = source;
        notify(previous, !(playsLoop(previous) && playsLoop(source)));
    }

    // Play, pause and stop: the user's transport choice wins over the dragged
    // segment and the piano key, which go silent. The region preview is a
    // transport preview; pause() and stop() finish it themselves.
    void yieldToTransport()
    {
        const Source previous = current;
        if (previous != Source::DraggedSegment && previous != Source::PianoKey)
            return;
        current = Source::None;
        notify(previous, true);
    }

    // The source stopped by itself (mouse up, the preview ended).
    void release(Source source)
    {
        if (current == source)
            current = Source::None;
    }

    // --- Sound paths. Each one claims the output first.

    // Plug-in: opens the host preview path for a timeline range.
    void startHostPreview(Source source, const Project &project, int startFrame,
                          int endFrame)
    {
        claim(source);
        if (startHostPreviewRange)
            startHostPreviewRange(project, startFrame, endFrame);
    }
    void stopHostPreview()
    {
        if (stopHostPreviewRange)
            stopHostPreviewRange();
    }

    // Loops `buffer`: through the host preview path in the plug-in (after
    // startHostPreview), through the AudioEngine in the standalone app.
    // wholeSegment: repeat it from its start (drag feedback); otherwise loop it
    // seamlessly (key tone). Returns false when there is no output.
    bool playLoop(Source source, const juce::AudioBuffer<float> &buffer,
                  double sampleRate, bool wholeSegment)
    {
        claim(source);
        if (pluginMode())
        {
            if (!playHostLoop)
                return false;
            playHostLoop(buffer, sampleRate, wholeSegment);
            return true;
        }
        if (auto *audioEngine = engine ? engine() : nullptr)
        {
            audioEngine->beginAuditionLoop(buffer, static_cast<int>(sampleRate),
                                           /*seamless*/ !wholeSegment);
            return true;
        }
        return false;
    }

    // Stops the loop (the transport state is the caller's to restore).
    void stopLoop()
    {
        if (pluginMode())
        {
            if (stopHostLoop)
                stopHostLoop();
            return;
        }
        if (auto *audioEngine = engine ? engine() : nullptr)
            audioEngine->endAuditionLoop();
    }

private:
    static constexpr size_t sourceCount = static_cast<size_t>(Source::Count);
    static size_t index(Source source) { return static_cast<size_t>(source); }
    // The dragged segment and the piano key loop a buffer over the output;
    // the region preview plays the timeline through the host preview path.
    static bool playsLoop(Source source)
    {
        return source == Source::DraggedSegment || source == Source::PianoKey;
    }

    bool pluginMode() const { return isPluginMode && isPluginMode(); }

    void notify(Source source, bool stopSound)
    {
        if (source == Source::None || source == Source::Count)
            return;
        if (auto &handler = handlers[index(source)])
            handler(stopSound);
    }

    Source current = Source::None;
    std::array<TakenOver, sourceCount> handlers;
    std::function<void(const Project &, int, int)> startHostPreviewRange;
    std::function<void()> stopHostPreviewRange;
    std::function<void(const juce::AudioBuffer<float> &, double, bool)> playHostLoop;
    std::function<void()> stopHostLoop;
    std::function<AudioEngine *()> engine;
    std::function<bool()> isPluginMode;
};
