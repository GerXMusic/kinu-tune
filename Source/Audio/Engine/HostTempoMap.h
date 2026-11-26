// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#pragma once

#include <functional>
#include <string>
#include <vector>

/**
 * Host tempo map: quarter notes <-> seconds, for the whole timeline.
 *
 * Why this exists (D-310, plan/tempo-map-ara-grilla.md §4.1): everything in the
 * editor used to convert positions with ONE instantaneous BPM
 * (HostSyncService::ppqToSeconds). That is correct only locally: in a project
 * with thirty tempo markers the rest of the timeline is placed wrong. ARA
 * publishes the real map as sync points (kARAContentTypeTempoEntries) and bar
 * signatures; this class is the single place that turns them into a timeline.
 *
 * Deliberately free of JUCE and ARA so it can be unit-tested and so the ARA
 * reader (ARATempoMapReader) is the only code that talks to the host.
 *
 * Model:
 *  - tempoEntries: sorted sync points {timeSeconds, quarter}, at least 2.
 *    Between two entries the tempo is constant; the first and last segments are
 *    extrapolated outwards (spec of ARAContentTempoEntry).
 *  - barSignatures: sorted {quarter, numerator, denominator}; the first one
 *    starts bar 1 and each one holds until the next.
 *  - An invalid map (fewer than 2 entries, non-monotonic, non-finite) refuses
 *    to answer: callers fall back to constant() with the cursor BPM.
 */
class HostTempoMap
{
public:
    struct TempoEntry
    {
        double timeSeconds = 0.0;
        double quarter = 0.0;
    };

    struct BarSignature
    {
        double quarter = 0.0;
        int numerator = 4;
        int denominator = 4;
    };

    HostTempoMap() = default;

    // Builds from ARA-provided data. Entries are sorted by quarter; the result
    // is invalid if the data is not usable.
    static HostTempoMap fromEntries(std::vector<TempoEntry> tempoEntries,
                                    std::vector<BarSignature> barSignatures);

    // One constant tempo: two entries {0,0} and {1, 60/bpm}.
    static HostTempoMap constant(double bpm, int numerator = 4,
                                 int denominator = 4);

    bool isValid() const { return valid; }

    // True when every segment has the same tempo (a constant map).
    bool isConstant() const;

    const std::vector<TempoEntry>& getTempoEntries() const { return tempoEntries; }
    const std::vector<BarSignature>& getBarSignatures() const { return barSignatures; }

    // R3 (D-316): one pass over bars and grid lines, so the grid, the ruler
    // and the local channel all see the same lines.
    enum class GridLineKind
    {
        Bar,
        Beat,
        Subdivision
    };

    struct BarInfo
    {
        double startQuarter = 0.0;
        double endQuarter = 0.0;
        int barIndex = 0;   // 0-based; can be negative before bar 1.
        int barNumber = 1;  // What the ruler shows: barIndex + 1.
        int numerator = 4;
        int denominator = 4;
    };

    struct GridLineInfo
    {
        double quarter = 0.0;
        int barIndex = 0;
        int barNumber = 1;
        int beatInBar = 0; // 0 = bar line.
        GridLineKind kind = GridLineKind::Beat;
    };

    // Bars that overlap [qStart,qEnd], in order, walked from the first
    // signature (bar 1). A range before bar 1 walks backwards with the first
    // bar length. Returns the number of bars visited.
    int forEachBar(double qStart, double qEnd,
                   const std::function<void(const BarInfo&)>& callback) const;

    // Grid lines every `gridQuarters`, anchored to each bar start (never to
    // time zero): line 0 of a bar is the bar line, lines on whole beats are
    // Beat, the rest Subdivision. A grid coarser than the bar stops at the bar
    // line. Returns the number of lines emitted.
    int forEachGridLine(
        double qStart, double qEnd, double gridQuarters,
        const std::function<void(const GridLineInfo&)>& callback) const;

    // Quarter notes -> seconds. Extrapolates beyond the first/last entry.
    double timeForQuarter(double quarter) const;

    // Seconds -> quarter notes. Extrapolates beyond the first/last entry.
    double quarterForTime(double timeSeconds) const;

    // R4 (D-317): snap a host time to the musical grid. The grid is counted in
    // quarter notes from the song start (multiple of gridQuarterNotes), so the
    // result follows the tempo map: 1/4 at 60 BPM is 1 s, at 120 BPM 0.5 s.
    // An invalid map or a non-positive grid returns the input untouched.
    double snapTimeToGrid(double timeSeconds, double gridQuarterNotes) const;

    // Tempo in BPM at a point, from the segment that contains it.
    double bpmAtTime(double timeSeconds) const;
    double bpmAtQuarter(double quarter) const;

    // Seconds per quarter of the segment that contains quarter.
    double secondsPerQuarterAtQuarter(double quarter) const;

    // Smallest seconds-per-quarter inside [qStart,qEnd] (grid thinning needs
    // the fastest section, not the average).
    double minSecondsPerQuarter(double qStart, double qEnd) const;

    // Bar helpers. The first signature starts bar 1, i.e. bar index 0.
    const BarSignature& signatureAtQuarter(double quarter) const;
    double barQuartersFor(const BarSignature& signature) const;
    double beatQuartersFor(const BarSignature& signature) const;

    // 0-based bar index containing quarter; can be negative before the first
    // signature. Number shown to the user = index + 1.
    int barIndexAtQuarter(double quarter) const;
    double barStartQuarter(int barIndex) const;

    // Exact comparison with a tolerance, for "publish only if it changed".
    bool equals(const HostTempoMap& other) const;
    bool operator==(const HostTempoMap& other) const { return equals(other); }
    bool operator!=(const HostTempoMap& other) const { return !equals(other); }

    // Diagnostic one-liner (no JUCE): "entries=48 sigs=3 span=0.0..123.4s
    // quarters=0..236.1 bpm=73..165".
    std::string describe() const;

private:
    // Segment index whose [quarter] range contains `quarter`; clamped to the
    // first/last segment so extrapolation uses its slope.
    int segmentForQuarter(double quarter) const;
    int segmentForTime(double timeSeconds) const;
    double slopeSecondsPerQuarter(int segmentIndex) const;

    std::vector<TempoEntry> tempoEntries;
    std::vector<BarSignature> barSignatures;
    bool valid = false;
};
