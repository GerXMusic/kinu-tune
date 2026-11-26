// D-311: guide coordinates (quarters -> project seconds -> frames) and the
// deterministic one-to-one matcher between audio notes and guide notes.
#include "TestAssert.h"

#include "../Source/Audio/Engine/HostTempoMap.h"
#include "../Source/Models/MidiGuide.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
bool near(double a, double b, double tolerance = 1.0e-9)
{
    return std::abs(a - b) <= tolerance;
}

MidiGuideMapping mappingFor(const HostTempoMap& map,
                            double timelineDisplayOffset = 0.0)
{
    MidiGuideMapping mapping;
    mapping.tempoMap = &map;
    mapping.timelineDisplayOffsetSeconds = timelineDisplayOffset;
    mapping.sampleRate = 44100;
    mapping.hopSize = 512;
    return mapping;
}

void testMappingConstantTempo()
{
    const HostTempoMap map = HostTempoMap::constant(120.0);
    const auto mapping = mappingFor(map);

    CHECK(near(mapping.timelineSecondsForQuarter(0.0), 0.0));
    CHECK(near(mapping.timelineSecondsForQuarter(8.0), 4.0));
    CHECK(near(mapping.projectSecondsForQuarter(8.0), 4.0));
    CHECK(mapping.frameForQuarter(8.0) ==
          static_cast<long long>(std::llround(4.0 * 44100.0 / 512.0)));
}

void testMappingWithRegionOffset()
{
    // Region starts 6.0 s into the timeline: a guide note at quarter 8 (4.0 s
    // of song time) is at -2.0 s of project time, i.e. before the region.
    const HostTempoMap map = HostTempoMap::constant(120.0);
    const auto mapping = mappingFor(map, 6.0);
    CHECK(near(mapping.timelineSecondsForQuarter(8.0), 4.0));
    CHECK(near(mapping.projectSecondsForQuarter(8.0), -2.0));

    // The caller drops negatives; the mapping itself never clamps.
    const auto frame = mapping.frameForQuarter(8.0);
    CHECK(frame < 0);
}

void testMappingWithTempoMap()
{
    std::vector<HostTempoMap::TempoEntry> entries{{0.0, 0.0}, {2.0, 8.0},
                                                 {4.0, 12.0}};
    std::vector<HostTempoMap::BarSignature> sigs{{0.0, 4, 4}};
    const HostTempoMap map =
        HostTempoMap::fromEntries(std::move(entries), std::move(sigs));

    const auto mapping = mappingFor(map, 1.0);
    CHECK(near(mapping.timelineSecondsForQuarter(10.0), 3.0));
    CHECK(near(mapping.projectSecondsForQuarter(10.0), 2.0));
    CHECK(mapping.frameForQuarter(10.0) ==
          static_cast<long long>(std::llround(2.0 * 44100.0 / 512.0)));
}

void testNormaliseSortsAndDedupes()
{
    GuideSource source;
    source.notes = {{4.0, 5.0, 62.0f, 0.4f},
                    {0.0, 1.0, 60.0f, 0.8f},
                    {0.00005, 1.5, 60.0f, 0.9f}, // same start+pitch: dedupe
                    {2.0, 3.0, 61.0f, 0.5f}};
    source.normalise();

    CHECK(source.notes.size() == 3);
    CHECK(near(source.notes[0].startQuarter, 0.0));
    CHECK(near(source.notes[0].endQuarter, 1.5)); // longest end wins
    CHECK(near(source.notes[0].velocity, 0.9f));  // strongest velocity wins
    CHECK(near(source.notes[1].pitch, 61.0f));
    CHECK(near(source.notes[2].pitch, 62.0f));
}

void testMatcherOneToOne()
{
    // Two audio notes over the same guide note: only one may take it.
    std::vector<MidiGuideMatcher::AudioNote> audio{
        {0, 0.0, 1.0, 60.0}, {1, 0.0, 1.0, 61.0}};
    std::vector<MidiGuideMatcher::GuideSlot> guides{{0, 0.0, 1.0, 60.0}};

    const auto matches = MidiGuideMatcher::match(audio, guides);
    CHECK(matches.size() == 1);
    CHECK(matches.front().audioIndex == 0);
    CHECK(matches.front().guideIndex == 0);
}

void testMatcherRejectsFarPitchAndNoOverlap()
{
    std::vector<MidiGuideMatcher::AudioNote> audio{
        {0, 0.0, 1.0, 60.0}, {1, 4.0, 5.0, 62.0}};
    std::vector<MidiGuideMatcher::GuideSlot> guides{
        {0, 0.0, 1.0, 80.0},   // 20 semitones away
        {1, 8.0, 9.0, 62.0}};  // no temporal overlap

    CHECK(MidiGuideMatcher::match(audio, guides).empty());
}

void testMatcherPrefersOverlapAndIsDeterministic()
{
    std::vector<MidiGuideMatcher::AudioNote> audio{{7, 10.0, 11.0, 60.0}};
    std::vector<MidiGuideMatcher::GuideSlot> guides{
        {0, 10.0, 10.5, 60.0},  // half overlap
        {1, 10.0, 11.0, 60.0},  // full overlap
        {2, 9.0, 12.0, 60.0}};  // full overlap, longer than the note

    const auto first = MidiGuideMatcher::match(audio, guides);
    CHECK(first.size() == 1);
    // Scores: slot 1 = 1.0 - 0, slot 2 = 1.0 - 0 -> tie broken by earlier
    // guide start, so slot 2 (start 9.0) wins.
    CHECK(first.front().guideIndex == 2);

    for (int run = 0; run < 100; ++run)
    {
        const auto again = MidiGuideMatcher::match(audio, guides);
        CHECK(again.size() == 1);
        CHECK(again.front().guideIndex == first.front().guideIndex);
        CHECK(near(again.front().score, first.front().score));
    }
}

void testMatcherUnmatchedNotesAreLeftAlone()
{
    std::vector<MidiGuideMatcher::AudioNote> audio{{0, 0.0, 1.0, 60.0},
                                                   {1, 5.0, 6.0, 64.0}};
    std::vector<MidiGuideMatcher::GuideSlot> guides{{0, 0.0, 1.0, 60.0}};

    const auto matches = MidiGuideMatcher::match(audio, guides);
    CHECK(matches.size() == 1);
    CHECK(matches.front().audioIndex == 0);
    // Note 1 has no guide and must not appear in the result.
    for (const auto& m : matches)
        CHECK(m.audioIndex != 1);
}

void testMatcherChordTakesBestSlot()
{
    // A chord in the guide: the audio note takes the closest pitch, and the
    // other guide notes stay unused.
    std::vector<MidiGuideMatcher::AudioNote> audio{{0, 0.0, 1.0, 64.0}};
    std::vector<MidiGuideMatcher::GuideSlot> guides{{0, 0.0, 1.0, 60.0},
                                                    {1, 0.0, 1.0, 64.0},
                                                    {2, 0.0, 1.0, 67.0}};
    const auto matches = MidiGuideMatcher::match(audio, guides);
    CHECK(matches.size() == 1);
    CHECK(matches.front().guideIndex == 1);
}
} // namespace

int main()
{
    testMappingConstantTempo();
    testMappingWithRegionOffset();
    testMappingWithTempoMap();
    testNormaliseSortsAndDedupes();
    testMatcherOneToOne();
    testMatcherRejectsFarPitchAndNoOverlap();
    testMatcherPrefersOverlapAndIsDeterministic();
    testMatcherUnmatchedNotesAreLeftAlone();
    testMatcherChordTakesBestSlot();

    std::printf("MidiGuideTests: all checks passed\n");
    return 0;
}
