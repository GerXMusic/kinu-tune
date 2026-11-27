// N8 (document-scope UI, D-348): the inspector speaks in document terms.
//
// The measured case of D-346 is reproduced here: five items, two sources,
// 262 + 29 notes in the Projects, windows of 0 / 7 / 77 / 70 / 110 and 264
// visible notes. The objective is the document selection or, with nothing
// selected, every visible note of the published index: never the project the
// pointer happened to activate. The target text says how many segments are in
// how many items, and a value the target notes disagree on shows the mixed
// placeholder instead of one note's value.
#include "TestAssert.h"

#include "../Source/Models/DocumentNoteIndex.h"
#include "../Source/Models/DocumentNoteSelection.h"
#include "../Source/Models/Project.h"
#include "../Source/UI/PianoRollComponent.h"
#include "../Source/UI/VariAudioInspector.h"
#include "../Source/Utils/Localization.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

namespace
{
constexpr int kSampleRate = 44100;
constexpr int kHopSize = 512;

int frameFor(double seconds)
{
    return static_cast<int>(
        std::llround(seconds * kSampleRate / static_cast<double>(kHopSize)));
}

Note makeNote(double startSeconds, double endSeconds, float midi)
{
    return Note(frameFor(startSeconds), frameFor(endSeconds), midi);
}

void setLength(Project& project, double seconds)
{
    project.getAudioData().sampleRate = kSampleRate;
    project.getAudioData().f0.assign(
        static_cast<size_t>(frameFor(seconds)), 0.0f);
}

// `count` notes spread inside [start, end), each 90% of its slot long.
void addNotes(Project& project, double start, double end, int count, float midi)
{
    const double span = end - start - 0.2;
    const double step = span / static_cast<double>(count);
    for (int i = 0; i < count; ++i)
    {
        const double noteStart = start + 0.1 + step * i;
        project.getNotes().push_back(
            makeNote(noteStart, noteStart + step * 0.9, midi));
    }
}

DocumentNoteRegionSpec makeSpec(const juce::String& key,
                                const juce::String& modification,
                                const juce::String& source, double hostStart,
                                double hostEnd, double sourceStart,
                                double sourceEnd, Project* project)
{
    DocumentNoteRegionSpec spec;
    spec.key = key;
    spec.name = key;
    spec.modificationKey = modification;
    spec.sourceKey = source;
    spec.hostStartSeconds = hostStart;
    spec.hostEndSeconds = hostEnd;
    spec.sourceStartSeconds = sourceStart;
    spec.sourceEndSeconds = sourceEnd;
    spec.project = project;
    return spec;
}

// D-346: 262 notes in Miu_stems_v2.wav (windows 0/77/70/110 plus 5 outside),
// 29 in Miu_stems_v2-001.wav (7 inside its window, 22 outside).
struct RealCase
{
    Project stems;
    Project stems001;
    std::vector<DocumentNoteRegionSpec> specs;

    RealCase()
    {
        setLength(stems, 116.5);
        addNotes(stems, 10.737, 35.157, 77, 60.0f);
        addNotes(stems, 42.749, 68.930, 70, 62.0f);
        addNotes(stems, 70.198, 115.787, 110, 64.0f);
        for (int i = 0; i < 5; ++i)
            stems.getNotes().push_back(
                makeNote(7.9 + 0.5 * i, 8.2 + 0.5 * i, 55.0f + i));

        setLength(stems001, 12.376);
        addNotes(stems001, 0.0, 3.5, 7, 61.0f);
        addNotes(stems001, 4.0, 12.3, 22, 63.0f);

        specs.push_back(makeSpec("it0", "modA", "Miu_stems_v2.wav", 5.818,
                                 12.924, 0.0, 7.106, &stems));
        specs.push_back(makeSpec("it1", "modB", "Miu_stems_v2-001.wav", 12.914,
                                 16.574, 0.0, 3.660, &stems001));
        specs.push_back(makeSpec("it2", "modA", "Miu_stems_v2.wav", 16.555,
                                 40.975, 10.737, 35.157, &stems));
        specs.push_back(makeSpec("it3", "modA", "Miu_stems_v2.wav", 48.567,
                                 74.748, 42.749, 68.930, &stems));
        specs.push_back(makeSpec("it4", "modA", "Miu_stems_v2.wav", 76.016,
                                 121.605, 70.198, 115.787, &stems));
    }
};

std::vector<std::pair<Project*, Note*>>
pairsOfRegions(const DocumentNoteIndex& index,
               const std::vector<juce::String>& keys)
{
    std::vector<std::pair<Project*, Note*>> pairs;
    for (const auto& region : index.regions)
        if (std::find(keys.begin(), keys.end(), region.key) != keys.end())
            for (const auto& ref : region.notes)
                if (ref.note != nullptr)
                    pairs.push_back({ref.project, ref.note});
    return pairs;
}

bool containsNote(const Project& project, const Note* note)
{
    for (const auto& candidate : project.getNotes())
        if (&candidate == note)
            return true;
    return false;
}

juce::Label* findLabelWithText(juce::Component& root, const juce::String& text)
{
    for (auto* child : root.getChildren())
    {
        if (auto* label = dynamic_cast<juce::Label*>(child))
            if (label->getText() == text)
                return label;
        if (auto* found = findLabelWithText(*child, text))
            return found;
    }
    return nullptr;
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    Localization::getInstance().setLanguage("en");

    RealCase real;
    const auto index = buildDocumentNoteIndex("track", real.specs);
    CHECK(index != nullptr);
    CHECK(index->regionCount() == 5);
    CHECK(index->totalNotes == 264);
    CHECK(index->projectNotes == 291);
    CHECK(static_cast<int>(index->outsideNotes.size()) == 27);
    CHECK(real.stems.getNotes().size() == 262);
    CHECK(real.stems001.getNotes().size() == 29);

    auto selection = std::make_shared<DocumentNoteSelection>();
    PianoRollComponent roll;
    roll.setSize(900, 500);
    roll.setProject(&real.stems);
    roll.setTrackViewMode(true);
    roll.setDocumentNoteContext(index, selection);

    // --- The objective is the document, not the hovered / active item ------
    CHECK(roll.getInspectorTargetCount() == 264);
    CHECK(roll.getInspectorTargetItemCount() == 4); // it0 has no visible note
    CHECK(!roll.inspectorTargetsAreSelection());
    {
        const auto targets = roll.getInspectorTargets();
        CHECK(targets.size() == 264);
        bool allReal = true;
        for (const auto* note : targets)
            allReal = allReal && (containsNote(real.stems, note) ||
                                  containsNote(real.stems001, note));
        CHECK(allReal); // pointers into the real Projects, no note copies
    }
    // Switching the active item does not change the document objective (this
    // is the symptom the hover used to trigger: 29 vs 262).
    roll.setProject(&real.stems001);
    CHECK(roll.getInspectorTargetCount() == 264);
    CHECK(roll.getInspectorTargetItemCount() == 4);
    roll.setProject(&real.stems);

    // --- Target text: {n} segments, never "in {m} items" --------------------
    VariAudioInspector inspector;
    inspector.getCurrentProject = [&roll] { return roll.getProject(); };
    inspector.getTargetCount = [&roll]
    {
        return std::pair<int, bool>{roll.getInspectorTargetCount(),
                                    roll.inspectorTargetsAreSelection()};
    };
    inspector.getParamValue = [&roll](VariAudioInspector::Param param)
    {
        return roll.getInspectorValue(
            static_cast<PianoRollComponent::InspectorParam>(param));
    };
    inspector.getParamValueMixed = [&roll](VariAudioInspector::Param param)
    {
        return roll.inspectorValueIsMixed(
            static_cast<PianoRollComponent::InspectorParam>(param));
    };
    inspector.refresh();
    CHECK(findLabelWithText(inspector, "Target: 264 segment(s)") != nullptr);
    CHECK(TR("inspector.target_document").contains("{n}"));
    CHECK(!TR("inspector.target_document").contains("{m}"));
    CHECK(TR("inspector.mixed").isNotEmpty());

    // --- A selection of one item keeps the same wording --------------------
    auto setSelection = [&](const std::vector<juce::String>& keys)
    {
        const auto pairs = pairsOfRegions(*index, keys);
        CHECK(selection->setFromPairs(pairs, *index));
        CHECK(roll.getInspectorTargetCount() == static_cast<int>(pairs.size()));
    };
    setSelection({"it1"});
    CHECK(roll.getInspectorTargetCount() == 7);
    CHECK(roll.getInspectorTargetItemCount() == 1);
    CHECK(roll.inspectorTargetsAreSelection());
    inspector.refresh();
    CHECK(findLabelWithText(inspector, "Target: 7 segment(s)") != nullptr);

    // --- A selection across two items says the same, with no item count -----
    setSelection({"it1", "it2"});
    CHECK(roll.getInspectorTargetCount() == 84);
    CHECK(roll.getInspectorTargetItemCount() == 2);
    CHECK(roll.inspectorTargetsAreSelection());
    inspector.refresh();
    CHECK(findLabelWithText(inspector, "Target: 84 segment(s)") != nullptr);
    CHECK(findLabelWithText(inspector, "Target: 84 segment(s) in 2 item(s)") ==
          nullptr);

    // --- Mixed values: the row says "—", not the last item's value ---------
    using Param = PianoRollComponent::InspectorParam;
    CHECK(!roll.inspectorValueIsMixed(Param::Tilt));
    const auto entryNotes = selection->entries();
    CHECK(entryNotes.size() == 84);
    const auto firstTilt = roll.getInspectorValue(Param::Tilt);
    CHECK(firstTilt.has_value());
    Note* firstNote = entryNotes.front().note;
    Note* lastNote = entryNotes.back().note;
    CHECK(firstNote != nullptr && lastNote != nullptr && firstNote != lastNote);
    lastNote->setTilt(1.0f);
    CHECK(roll.inspectorValueIsMixed(Param::Tilt));
    const auto shownTilt = roll.getInspectorValue(Param::Tilt);
    CHECK(shownTilt.has_value());
    CHECK(std::abs(*shownTilt - *firstTilt) <= 1.0e-4f); // first of the document
    inspector.refresh();
    CHECK(inspector.isRowMixed(VariAudioInspector::Param::Tilt));
    lastNote->setTilt(firstNote->getTilt());
    CHECK(!roll.inspectorValueIsMixed(Param::Tilt));
    inspector.refresh();
    CHECK(!inspector.isRowMixed(VariAudioInspector::Param::Tilt));

    std::cout << "Document inspector tests passed\n";
    return 0;
}
