// Kinu Tune - Copyright (C) 2026 Keinoaza
// Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.

// Who owns the audition output and what the others are told when it changes
// hands (AuditionOutput), with recording stand-ins for the host callbacks.

#include "../Source/UI/Main/AuditionOutput.h"
#include "TestAssert.h"
#include <iostream>
#include <string>
#include <vector>

using Source = AuditionOutput::Source;

namespace
{
struct Recorder
{
    std::vector<std::string> calls;
    void add(const std::string &call) { calls.push_back(call); }
    bool has(const std::string &call) const
    {
        for (const auto &c : calls)
            if (c == call)
                return true;
        return false;
    }
};

void watch(AuditionOutput &output, Recorder &log)
{
    output.setOnTakenOver(Source::DraggedSegment, [&log](bool stop)
                          { log.add(stop ? "drag:stop" : "drag:replaced"); });
    output.setOnTakenOver(Source::PianoKey, [&log](bool stop)
                          { log.add(stop ? "key:stop" : "key:replaced"); });
    output.setOnTakenOver(Source::RegionPreview, [&log](bool stop)
                          { log.add(stop ? "region:stop" : "region:replaced"); });
}

void usePluginBackends(AuditionOutput &output, Recorder &log)
{
    output.setEngine([] { return static_cast<AudioEngine *>(nullptr); },
                     [] { return true; });
    output.setStartHostPreview([&log](const Project &, int start, int end)
                               { log.add("host:preview " + std::to_string(start) + "-" +
                                         std::to_string(end)); });
    output.setStopHostPreview([&log] { log.add("host:preview stop"); });
    output.setPlayHostLoop([&log](const juce::AudioBuffer<float> &, double, bool whole)
                           { log.add(whole ? "host:loop whole" : "host:loop seamless"); });
    output.setStopHostLoop([&log] { log.add("host:loop stop"); });
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;
    juce::AudioBuffer<float> buffer(1, 64);
    buffer.clear();
    Project project;

    // Claiming takes the output from its owner. A loop replaces a loop; a
    // source that does not play a loop (the region preview) stops it.
    {
        AuditionOutput output;
        Recorder log;
        watch(output, log);
        CHECK(output.owner() == Source::None);
        output.claim(Source::DraggedSegment);
        CHECK(output.owner() == Source::DraggedSegment);
        CHECK(log.calls.empty());
        output.claim(Source::DraggedSegment); // same source: nothing to hand over
        CHECK(log.calls.empty());
        output.claim(Source::PianoKey);
        CHECK(output.owner() == Source::PianoKey);
        CHECK(log.calls.size() == 1 && log.has("drag:replaced"));
        output.claim(Source::DraggedSegment);
        CHECK(log.has("key:replaced"));
        output.claim(Source::RegionPreview);
        CHECK(log.has("drag:stop"));
        output.claim(Source::PianoKey);
        CHECK(log.has("region:stop"));
        output.claim(Source::RegionPreview);
        CHECK(log.has("key:stop"));
    }

    // Transport commands silence the dragged segment and the piano key; the
    // region preview is finished by pause() and stop() themselves.
    {
        AuditionOutput output;
        Recorder log;
        watch(output, log);
        output.claim(Source::DraggedSegment);
        output.yieldToTransport();
        CHECK(log.has("drag:stop"));
        CHECK(output.owner() == Source::None);

        output.claim(Source::PianoKey);
        output.yieldToTransport();
        CHECK(log.has("key:stop"));
        CHECK(output.owner() == Source::None);

        log.calls.clear();
        output.claim(Source::RegionPreview);
        output.yieldToTransport();
        CHECK(log.calls.empty());
        CHECK(output.owner() == Source::RegionPreview);

        log.calls.clear();
        output.release(Source::RegionPreview);
        output.yieldToTransport(); // nobody owns it: nothing happens
        CHECK(log.calls.empty());
    }

    // release() only clears the source that owns the output.
    {
        AuditionOutput output;
        output.claim(Source::PianoKey);
        output.release(Source::DraggedSegment);
        CHECK(output.owner() == Source::PianoKey);
        output.release(Source::PianoKey);
        CHECK(output.owner() == Source::None);
    }

    // A handler may release or claim again while it is told (the editor's
    // handlers call release() from abandon/finish).
    {
        AuditionOutput output;
        Recorder log;
        output.setOnTakenOver(Source::DraggedSegment, [&](bool)
                              {
                                  output.release(Source::DraggedSegment);
                                  log.add("drag:told");
                              });
        output.claim(Source::DraggedSegment);
        output.claim(Source::PianoKey);
        CHECK(log.has("drag:told"));
        CHECK(output.owner() == Source::PianoKey);
    }

    // Plug-in: every sound path claims first, then calls the host.
    {
        AuditionOutput output;
        Recorder log;
        watch(output, log);
        usePluginBackends(output, log);

        output.startHostPreview(Source::DraggedSegment, project, 10, 20);
        CHECK(output.owner() == Source::DraggedSegment);
        CHECK(log.has("host:preview 10-20"));
        CHECK(output.playLoop(Source::DraggedSegment, buffer, 44100.0, true));
        CHECK(log.has("host:loop whole"));

        log.calls.clear();
        output.startHostPreview(Source::PianoKey, project, 0, 1);
        CHECK(log.calls.size() == 2);
        CHECK(log.calls[0] == "drag:replaced"); // handed over before the host call
        CHECK(log.calls[1] == "host:preview 0-1");
        CHECK(output.playLoop(Source::PianoKey, buffer, 44100.0, false));
        CHECK(log.has("host:loop seamless"));

        output.stopLoop();
        CHECK(log.has("host:loop stop"));
        output.stopHostPreview();
        CHECK(log.has("host:preview stop"));
    }

    // Standalone: the loop goes to the AudioEngine, seamless for the key tone
    // and repeated from the start for the dragged segment.
    {
        AudioEngine engine;
        AuditionOutput output;
        output.setEngine([&engine] { return &engine; }, [] { return false; });
        CHECK(output.playLoop(Source::PianoKey, buffer, 44100.0, false));
        CHECK(engine.isAuditioning() && engine.isAuditionSeamless());
        CHECK(output.playLoop(Source::DraggedSegment, buffer, 44100.0, true));
        CHECK(engine.isAuditioning() && !engine.isAuditionSeamless());
        output.stopLoop();
        CHECK(!engine.isAuditioning());
    }

    // Standalone without an engine: nothing sounds and nothing crashes.
    {
        AuditionOutput output;
        output.setEngine([] { return static_cast<AudioEngine *>(nullptr); },
                         [] { return false; });
        CHECK(!output.playLoop(Source::PianoKey, buffer, 44100.0, false));
        output.stopLoop();
    }

    // No backends at all (before the plug-in editor wires them).
    {
        AuditionOutput output;
        output.startHostPreview(Source::RegionPreview, project, 0, 10);
        output.stopHostPreview();
        CHECK(!output.playLoop(Source::DraggedSegment, buffer, 44100.0, true));
        output.stopLoop();
    }

    std::cout << "AuditionOutputTests passed\n";
    return 0;
}
