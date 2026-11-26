// D-311: MIDI guide import (PPQ -> quarters, SMPTE rejected), live JSON
// schema 1 validation and project persistence (additive "midiGuides" key).
#include "TestAssert.h"

#include "../Source/Audio/IO/MidiGuideImporter.h"
#include "../Source/Models/Project.h"
#include "../Source/Models/ProjectSerializer.h"

#include <cmath>
#include <cstdio>

namespace
{
bool near(double a, double b, double tolerance = 1.0e-9)
{
    return std::abs(a - b) <= tolerance;
}

juce::MidiFile makeTestMidi()
{
    juce::MidiFile midi;
    midi.setTicksPerQuarterNote(480);

    juce::MidiMessageSequence track;
    track.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0.0);
    track.addEvent(juce::MidiMessage::noteOff(1, 60), 480.0);
    track.addEvent(juce::MidiMessage::noteOn(1, 64, (juce::uint8)80), 960.0);
    track.addEvent(juce::MidiMessage::noteOff(1, 64), 1440.0);
    midi.addTrack(track);
    return midi;
}

void testMidiFileToQuarters()
{
    const auto midi = makeTestMidi();
    const auto result =
        MidiGuideImporter::fromMidiFile(midi, "test", "file:test");
    CHECK(result.ok);
    CHECK(result.source.notes.size() == 2);
    CHECK(near(result.source.notes[0].startQuarter, 0.0));
    CHECK(near(result.source.notes[0].endQuarter, 1.0));
    CHECK(near(result.source.notes[0].pitch, 60.0f));
    CHECK(near(result.source.notes[1].startQuarter, 2.0));
    CHECK(near(result.source.notes[1].endQuarter, 3.0));
    CHECK(near(result.source.notes[1].pitch, 64.0f));
    CHECK(result.source.kind == "file");
}

void testMidiFileThroughDisk()
{
    // Also exercises importMidiFile() (file stream + content hash id).
    auto midi = makeTestMidi();
    const auto file =
        juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("tunex_midi_guide_test.mid");
    {
        juce::FileOutputStream out(file);
        CHECK(out.openedOk());
        CHECK(midi.writeTo(out));
    }

    const auto result = MidiGuideImporter::importMidiFile(file);
    CHECK(result.ok);
    CHECK(result.source.notes.size() == 2);
    CHECK(!result.source.id.empty());
    CHECK(juce::String(result.source.id).startsWith("file:"));

    // Identity is the content hash: importing twice gives the same id.
    const auto again = MidiGuideImporter::importMidiFile(file);
    CHECK(again.ok);
    CHECK(again.source.id == result.source.id);
    file.deleteFile();
}

void testSmpteFileIsRejected()
{
    juce::MidiFile midi;
    // Negative time format = SMPTE (frames/second in the high byte).
    midi.setSmpteTimeFormat(25, 40);
    juce::MidiMessageSequence track;
    track.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0.0);
    track.addEvent(juce::MidiMessage::noteOff(1, 60), 40.0);
    midi.addTrack(track);

    const auto result =
        MidiGuideImporter::fromMidiFile(midi, "smpte", "file:smpte");
    CHECK(!result.ok);
    CHECK(result.error.containsIgnoreCase("SMPTE"));
}

juce::var guideJson()
{
    juce::Array<juce::var> notes;
    juce::Array<juce::var> first;
    first.add(8.0);
    first.add(9.5);
    first.add(60.0);
    first.add(0.8);
    notes.add(juce::var(first));
    juce::Array<juce::var> second;
    second.add(9.5);
    second.add(11.0);
    second.add(62.0);
    second.add(0.7);
    notes.add(juce::var(second));

    auto* object = new juce::DynamicObject();
    object->setProperty("schema", 1);
    object->setProperty("source_id", "reaper:track:item");
    object->setProperty("label", "MIDI guia");
    object->setProperty("kind", "reascript");
    object->setProperty("created_utc", "2026-11-26T20:00:00Z");
    object->setProperty("notes", juce::var(notes));
    return juce::var(object);
}

void testJsonSchema()
{
    GuideSource source;
    juce::String error;
    CHECK(MidiGuideImporter::fromJson(guideJson(), source, error));
    CHECK(error.isEmpty());
    CHECK(source.notes.size() == 2);
    CHECK(source.kind == "reascript");
    CHECK(source.id == "reaper:track:item");
    CHECK(near(source.notes[0].startQuarter, 8.0));
    CHECK(near(source.notes[1].pitch, 62.0f));
    CHECK(near(source.notes[0].velocity, 0.8f));

    // Wrong schema is rejected, not silently upgraded.
    auto wrong = guideJson();
    wrong.getDynamicObject()->setProperty("schema", 2);
    GuideSource reject;
    juce::String wrongError;
    CHECK(!MidiGuideImporter::fromJson(wrong, reject, wrongError));
    CHECK(wrongError.containsIgnoreCase("schema"));

    // Out-of-range pitches are dropped; a guide left with nothing is an error.
    auto bad = guideJson();
    juce::Array<juce::var> outOfRange;
    juce::Array<juce::var> entry;
    entry.add(0.0);
    entry.add(1.0);
    entry.add(200.0);
    outOfRange.add(juce::var(entry));
    bad.getDynamicObject()->setProperty("notes", juce::var(outOfRange));
    GuideSource badSource;
    juce::String badError;
    CHECK(!MidiGuideImporter::fromJson(bad, badSource, badError));
}

void testLiveGuideDirectory()
{
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("tunex_guide_live_test");
    dir.deleteRecursively();
    dir.createDirectory();

    const auto good = dir.getChildFile("guide_a.json");
    good.replaceWithText(juce::JSON::toString(guideJson()));
    const auto broken = dir.getChildFile("guide_b.json");
    broken.replaceWithText("{ this is not json");

    int rejected = 0;
    const auto guides = MidiGuideImporter::readLiveGuides(dir, &rejected);
    CHECK(guides.size() == 1);
    CHECK(rejected == 1);
    CHECK(guides.front().label == "MIDI guia");

    dir.deleteRecursively();
}

void testProjectRoundTrip()
{
    Project project;
    project.setName("guide test");

    GuideSource source;
    source.id = "file:abc";
    source.label = "melody.mid";
    source.kind = "file";
    source.notes = {{4.0, 5.0, 60.0f, 0.8f}, {5.0, 6.5, 62.0f, 0.6f}};
    project.getMidiGuides().push_back(source);

    const auto json = ProjectSerializer::toJson(project);
    Project loaded;
    CHECK(ProjectSerializer::fromJson(loaded, json));
    CHECK(loaded.getMidiGuides().size() == 1);
    CHECK(loaded.getMidiGuides().front().label == "melody.mid");
    CHECK(loaded.getMidiGuides().front().notes.size() == 2);
    CHECK(near(loaded.getMidiGuides().front().notes[1].startQuarter, 5.0));
    CHECK(near(loaded.getMidiGuides().front().notes[1].pitch, 62.0f));

    // A project saved before guides existed loads with an empty vector.
    Project old;
    auto oldJson = ProjectSerializer::toJson(old);
    oldJson.getDynamicObject()->removeProperty("midiGuides");
    Project oldLoaded;
    CHECK(ProjectSerializer::fromJson(oldLoaded, oldJson));
    CHECK(oldLoaded.getMidiGuides().empty());
}
} // namespace

int main()
{
    testMidiFileToQuarters();
    testMidiFileThroughDisk();
    testSmpteFileIsRejected();
    testJsonSchema();
    testLiveGuideDirectory();
    testProjectRoundTrip();

    std::printf("MidiGuideImportTests: all checks passed\n");
    return 0;
}
