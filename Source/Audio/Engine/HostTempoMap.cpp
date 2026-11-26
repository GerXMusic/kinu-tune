// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#include "HostTempoMap.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{
constexpr double kQuarterEpsilon = 1.0e-9;

bool isFinitePositive(double v) { return std::isfinite(v) && v > 0.0; }

bool isWholeMultiple(double value, double step)
{
    if (!(step > 0.0))
        return false;
    const double ratio = value / step;
    const double nearest = std::floor(ratio + 0.5);
    return std::abs(ratio - nearest) < 1.0e-7;
}
} // namespace

HostTempoMap HostTempoMap::fromEntries(std::vector<TempoEntry> entries,
                                       std::vector<BarSignature> signatures)
{
    HostTempoMap map;
    map.tempoEntries = std::move(entries);
    map.barSignatures = std::move(signatures);

    // ARA says sync points come ordered, but a host that lies must not break
    // the timeline: sort and validate.
    std::sort(map.tempoEntries.begin(), map.tempoEntries.end(),
              [](const TempoEntry& a, const TempoEntry& b) {
                  return a.quarter < b.quarter;
              });

    map.valid = map.tempoEntries.size() >= 2;
    for (size_t i = 0; map.valid && i < map.tempoEntries.size(); ++i)
    {
        const auto& e = map.tempoEntries[i];
        if (!std::isfinite(e.timeSeconds) || !std::isfinite(e.quarter))
            map.valid = false;
        if (i > 0)
        {
            const auto& prev = map.tempoEntries[i - 1];
            const double dq = e.quarter - prev.quarter;
            const double dt = e.timeSeconds - prev.timeSeconds;
            // Both axes must advance: a vertical or horizontal segment has no
            // usable tempo.
            if (dq <= kQuarterEpsilon || dt <= 0.0)
                map.valid = false;
        }
    }

    if (map.barSignatures.empty())
    {
        BarSignature fallback;
        fallback.quarter = map.tempoEntries.empty()
                               ? 0.0
                               : map.tempoEntries.front().quarter;
        map.barSignatures.push_back(fallback);
    }

    std::sort(map.barSignatures.begin(), map.barSignatures.end(),
              [](const BarSignature& a, const BarSignature& b) {
                  return a.quarter < b.quarter;
              });

    for (const auto& sig : map.barSignatures)
    {
        if (!std::isfinite(sig.quarter) || sig.numerator <= 0 ||
            sig.denominator <= 0)
            map.valid = false;
    }

    return map;
}

HostTempoMap HostTempoMap::constant(double bpm, int numerator, int denominator)
{
    if (!isFinitePositive(bpm))
        bpm = 120.0;
    if (numerator <= 0)
        numerator = 4;
    if (denominator <= 0)
        denominator = 4;

    std::vector<TempoEntry> entries;
    entries.push_back({0.0, 0.0});
    entries.push_back({60.0 / bpm, 1.0});

    std::vector<BarSignature> sigs;
    sigs.push_back({0.0, numerator, denominator});

    return fromEntries(std::move(entries), std::move(sigs));
}

int HostTempoMap::segmentForQuarter(double quarter) const
{
    // Index i means "segment from entry i to entry i+1", clamped.
    const int last = static_cast<int>(tempoEntries.size()) - 2;
    if (last <= 0)
        return 0;

    if (quarter <= tempoEntries.front().quarter)
        return 0;
    if (quarter >= tempoEntries.back().quarter)
        return last;

    const auto it = std::upper_bound(
        tempoEntries.begin(), tempoEntries.end(), quarter,
        [](double q, const TempoEntry& e) { return q < e.quarter; });
    int index = static_cast<int>(it - tempoEntries.begin()) - 1;
    return std::clamp(index, 0, last);
}

int HostTempoMap::segmentForTime(double timeSeconds) const
{
    const int last = static_cast<int>(tempoEntries.size()) - 2;
    if (last <= 0)
        return 0;

    if (timeSeconds <= tempoEntries.front().timeSeconds)
        return 0;
    if (timeSeconds >= tempoEntries.back().timeSeconds)
        return last;

    const auto it = std::upper_bound(
        tempoEntries.begin(), tempoEntries.end(), timeSeconds,
        [](double t, const TempoEntry& e) { return t < e.timeSeconds; });
    int index = static_cast<int>(it - tempoEntries.begin()) - 1;
    return std::clamp(index, 0, last);
}

double HostTempoMap::slopeSecondsPerQuarter(int segmentIndex) const
{
    const auto& a = tempoEntries[static_cast<size_t>(segmentIndex)];
    const auto& b = tempoEntries[static_cast<size_t>(segmentIndex) + 1];
    const double dq = b.quarter - a.quarter;
    if (dq <= 0.0)
        return 0.0;
    return (b.timeSeconds - a.timeSeconds) / dq;
}

double HostTempoMap::timeForQuarter(double quarter) const
{
    if (!valid)
        return quarter * 0.5; // 120 BPM: the documented fallback

    const int segment = segmentForQuarter(quarter);
    const auto& a = tempoEntries[static_cast<size_t>(segment)];
    return a.timeSeconds + (quarter - a.quarter) * slopeSecondsPerQuarter(segment);
}

double HostTempoMap::quarterForTime(double timeSeconds) const
{
    if (!valid)
        return timeSeconds * 2.0; // 120 BPM

    const int segment = segmentForTime(timeSeconds);
    const auto& a = tempoEntries[static_cast<size_t>(segment)];
    const double slope = slopeSecondsPerQuarter(segment);
    if (slope <= 0.0)
        return a.quarter;
    return a.quarter + (timeSeconds - a.timeSeconds) / slope;
}

double HostTempoMap::snapTimeToGrid(double timeSeconds, double gridQuarterNotes) const
{
    if (!valid || !(gridQuarterNotes > 0.0))
        return timeSeconds;

    const double quarter = quarterForTime(timeSeconds);
    const double snappedQuarter =
        std::round(quarter / gridQuarterNotes) * gridQuarterNotes;
    return timeForQuarter(snappedQuarter);
}

double HostTempoMap::bpmAtTime(double timeSeconds) const
{
    if (!valid)
        return 120.0;
    return bpmAtQuarter(quarterForTime(timeSeconds));
}

double HostTempoMap::bpmAtQuarter(double quarter) const
{
    if (!valid)
        return 120.0;
    const double slope = secondsPerQuarterAtQuarter(quarter);
    if (slope <= 0.0)
        return 0.0;
    return 60.0 / slope;
}

double HostTempoMap::secondsPerQuarterAtQuarter(double quarter) const
{
    if (!valid)
        return 0.5;
    return slopeSecondsPerQuarter(segmentForQuarter(quarter));
}

double HostTempoMap::minSecondsPerQuarter(double qStart, double qEnd) const
{
    if (!valid)
        return 0.5;
    if (qEnd < qStart)
        std::swap(qStart, qEnd);

    double smallest = secondsPerQuarterAtQuarter(qStart);
    const double endSlope = secondsPerQuarterAtQuarter(qEnd);
    smallest = std::min(smallest, endSlope);

    // Every segment that intersects the range contributes its own slope, which
    // is constant inside the segment.
    for (size_t i = 0; i + 1 < tempoEntries.size(); ++i)
    {
        const double segStart = tempoEntries[i].quarter;
        const double segEnd = tempoEntries[i + 1].quarter;
        if (segEnd < qStart || segStart > qEnd)
            continue;
        smallest = std::min(smallest, slopeSecondsPerQuarter(static_cast<int>(i)));
    }
    return smallest;
}

const HostTempoMap::BarSignature&
HostTempoMap::signatureAtQuarter(double quarter) const
{
    const auto it = std::upper_bound(
        barSignatures.begin(), barSignatures.end(), quarter,
        [](double q, const BarSignature& s) { return q < s.quarter; });
    if (it == barSignatures.begin())
        return barSignatures.front();
    return *(it - 1);
}

double HostTempoMap::barQuartersFor(const BarSignature& signature) const
{
    if (signature.denominator <= 0)
        return 4.0;
    return static_cast<double>(signature.numerator) * 4.0 /
           static_cast<double>(signature.denominator);
}

double HostTempoMap::beatQuartersFor(const BarSignature& signature) const
{
    if (signature.denominator <= 0)
        return 1.0;
    return 4.0 / static_cast<double>(signature.denominator);
}

double HostTempoMap::barStartQuarter(int barIndex) const
{
    if (barSignatures.empty())
        return 0.0;

    // Walk forward from the first signature; bar 0 starts there. A negative
    // bar index walks backwards with the first signature's length.
    double start = barSignatures.front().quarter;
    if (barIndex >= 0)
    {
        for (int bar = 0; bar < barIndex; ++bar)
        {
            const auto& sig = signatureAtQuarter(start + 1.0e-9);
            start += barQuartersFor(sig);
        }
        return start;
    }

    const double firstLength = barQuartersFor(barSignatures.front());
    return start + static_cast<double>(barIndex) * firstLength;
}

int HostTempoMap::barIndexAtQuarter(double quarter) const
{
    double start = barSignatures.front().quarter;
    if (quarter < start)
    {
        const double firstLength = barQuartersFor(barSignatures.front());
        if (firstLength <= 0.0)
            return 0;
        return static_cast<int>(std::floor((quarter - start) / firstLength));
    }

    int index = 0;
    // Signatures are few (a handful per project), so a linear walk is fine and
    // avoids an allocation.
    while (true)
    {
        const auto& sig = signatureAtQuarter(start + 1.0e-9);
        const double length = barQuartersFor(sig);
        if (length <= 0.0)
            return index;
        if (quarter < start + length - 1.0e-9)
            return index;
        start += length;
        ++index;
        if (index > 1000000)
            return index;
    }
}

int HostTempoMap::forEachBar(
    double qStart, double qEnd,
    const std::function<void(const BarInfo&)>& callback) const
{
    if (!valid || barSignatures.empty() || !callback)
        return 0;
    if (qEnd < qStart)
        std::swap(qStart, qEnd);

    // Start on the bar that contains qStart (barStartQuarter/barIndexAtQuarter
    // already handle bars before bar 1 with the first signature's length).
    int index = barIndexAtQuarter(qStart);
    double start = barStartQuarter(index);

    int visited = 0;
    constexpr int kMaxBars = 200000;
    while (start <= qEnd + kQuarterEpsilon && visited < kMaxBars)
    {
        const auto& signature = signatureAtQuarter(start + kQuarterEpsilon);
        const double length = barQuartersFor(signature);
        if (!(length > 0.0))
            break;

        BarInfo info;
        info.startQuarter = start;
        info.endQuarter = start + length;
        info.barIndex = index;
        info.barNumber = index + 1;
        info.numerator = signature.numerator;
        info.denominator = signature.denominator;

        if (info.endQuarter > qStart + kQuarterEpsilon)
            callback(info);
        ++visited;

        start += length;
        ++index;
    }
    return visited;
}

int HostTempoMap::forEachGridLine(
    double qStart, double qEnd, double gridQuarters,
    const std::function<void(const GridLineInfo&)>& callback) const
{
    if (!valid || !callback || !isFinitePositive(gridQuarters))
        return 0;

    int count = 0;
    constexpr int kMaxLines = 200000;
    forEachBar(qStart, qEnd, [&](const BarInfo& bar) {
        const double barLength = bar.endQuarter - bar.startQuarter;
        if (!(barLength > 0.0))
            return;

        BarSignature signature;
        signature.quarter = bar.startQuarter;
        signature.numerator = bar.numerator;
        signature.denominator = bar.denominator;
        const double beatQuarters = beatQuartersFor(signature);

        for (int k = 0; count < kMaxLines; ++k)
        {
            const double offset = static_cast<double>(k) * gridQuarters;
            const bool isBarLine = (k == 0);
            if (!isBarLine && offset >= barLength - kQuarterEpsilon)
                break;

            const double quarter = bar.startQuarter + offset;
            if (quarter > qEnd + kQuarterEpsilon)
                break;

            GridLineInfo line;
            line.quarter = quarter;
            line.barIndex = bar.barIndex;
            line.barNumber = bar.barNumber;
            if (isBarLine)
            {
                line.beatInBar = 0;
                line.kind = GridLineKind::Bar;
            }
            else if (isWholeMultiple(offset, beatQuarters))
            {
                line.beatInBar =
                    static_cast<int>(std::floor(offset / beatQuarters + 0.5));
                line.kind = GridLineKind::Beat;
            }
            else
            {
                line.beatInBar =
                    static_cast<int>(std::floor(offset / beatQuarters));
                line.kind = GridLineKind::Subdivision;
            }

            callback(line);
            ++count;
        }
    });
    return count;
}

bool HostTempoMap::isConstant() const
{
    if (!valid)
        return false;
    const double first = slopeSecondsPerQuarter(0);
    for (size_t i = 0; i + 1 < tempoEntries.size(); ++i)
    {
        const double slope = slopeSecondsPerQuarter(static_cast<int>(i));
        if (std::abs(slope - first) > 1.0e-9)
            return false;
    }
    return true;
}

bool HostTempoMap::equals(const HostTempoMap& other) const
{
    if (valid != other.valid)
        return false;
    if (!valid)
        return true;
    if (tempoEntries.size() != other.tempoEntries.size())
        return false;
    if (barSignatures.size() != other.barSignatures.size())
        return false;

    for (size_t i = 0; i < tempoEntries.size(); ++i)
    {
        if (std::abs(tempoEntries[i].quarter - other.tempoEntries[i].quarter) >
                1.0e-9 ||
            std::abs(tempoEntries[i].timeSeconds -
                     other.tempoEntries[i].timeSeconds) > 1.0e-9)
            return false;
    }
    for (size_t i = 0; i < barSignatures.size(); ++i)
    {
        if (std::abs(barSignatures[i].quarter -
                     other.barSignatures[i].quarter) > 1.0e-9 ||
            barSignatures[i].numerator != other.barSignatures[i].numerator ||
            barSignatures[i].denominator != other.barSignatures[i].denominator)
            return false;
    }
    return true;
}

std::string HostTempoMap::describe() const
{
    char buffer[256];
    if (!valid)
        return "entries=0 invalid";

    const auto& first = tempoEntries.front();
    const auto& last = tempoEntries.back();
    double minBpm = 1.0e9;
    double maxBpm = 0.0;
    for (size_t i = 0; i + 1 < tempoEntries.size(); ++i)
    {
        const double slope = slopeSecondsPerQuarter(static_cast<int>(i));
        if (slope <= 0.0)
            continue;
        const double bpm = 60.0 / slope;
        minBpm = std::min(minBpm, bpm);
        maxBpm = std::max(maxBpm, bpm);
    }
    std::snprintf(buffer, sizeof(buffer),
                  "entries=%d sigs=%d span=%.4f..%.4fs quarters=%.4f..%.4f "
                  "bpm=%.3f..%.3f",
                  static_cast<int>(tempoEntries.size()),
                  static_cast<int>(barSignatures.size()), first.timeSeconds,
                  last.timeSeconds, first.quarter, last.quarter, minBpm,
                  maxBpm);
    return buffer;
}
