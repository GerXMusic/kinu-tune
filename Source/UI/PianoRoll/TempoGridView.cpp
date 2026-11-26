#include "TempoGridView.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr double kEpsilon = 1.0e-9;
constexpr int kMaxBarStride = 1024;

double clampedQuarter(const HostTempoMap &map, double hostSeconds)
{
    const double quarter = map.quarterForTime(hostSeconds);
    return std::isfinite(quarter) ? quarter : 0.0;
}

void appendLine(std::vector<TempoGridLine> &lines, const HostTempoMap &map,
                const HostTempoMap::GridLineInfo &info)
{
    TempoGridLine line;
    line.quarter = info.quarter;
    line.hostSeconds = map.timeForQuarter(info.quarter);
    line.barIndex = info.barIndex;
    line.barNumber = info.barNumber;
    line.beatInBar = info.beatInBar;
    line.kind = info.kind;
    lines.push_back(line);
}
} // namespace

double minBarQuartersInRange(const HostTempoMap &map, double qStart, double qEnd)
{
    if (!map.isValid())
        return 0.0;
    if (qEnd < qStart)
        std::swap(qStart, qEnd);

    double smallest = 0.0;
    map.forEachBar(qStart, qEnd, [&](const HostTempoMap::BarInfo &bar) {
        const double length = bar.endQuarter - bar.startQuarter;
        if (!(length > 0.0))
            return;
        if (!(smallest > 0.0) || length < smallest)
            smallest = length;
    });
    if (smallest > 0.0)
        return smallest;

    const auto &signature = map.signatureAtQuarter(qStart);
    return map.barQuartersFor(signature);
}

double minBeatQuartersInRange(const HostTempoMap &map, double qStart, double qEnd)
{
    if (!map.isValid())
        return 0.0;
    if (qEnd < qStart)
        std::swap(qStart, qEnd);

    double smallest = 0.0;
    map.forEachBar(qStart, qEnd, [&](const HostTempoMap::BarInfo &bar) {
        HostTempoMap::BarSignature signature;
        signature.quarter = bar.startQuarter;
        signature.numerator = bar.numerator;
        signature.denominator = bar.denominator;
        const double beat = map.beatQuartersFor(signature);
        if (!(beat > 0.0))
            return;
        if (!(smallest > 0.0) || beat < smallest)
            smallest = beat;
    });
    if (smallest > 0.0)
        return smallest;

    const auto &signature = map.signatureAtQuarter(qStart);
    return map.beatQuartersFor(signature);
}

TempoGridPlan planTempoGrid(const HostTempoMap &map, double hostStart,
                            double hostEnd, double gridQuarterNotes,
                            double pixelsPerSecond, double minPixelsPerLine)
{
    TempoGridPlan plan;
    plan.level = TempoGridLevel::Beat;
    if (!map.isValid() || !(pixelsPerSecond > 0.0) ||
        !(gridQuarterNotes > 0.0))
        return plan;

    if (hostEnd < hostStart)
        std::swap(hostStart, hostEnd);

    const double qStart = clampedQuarter(map, hostStart);
    const double qEnd = clampedQuarter(map, hostEnd);

    // The fastest section decides: a slow section would otherwise hide lines
    // in the dense one.
    const double fastest = map.minSecondsPerQuarter(qStart, qEnd);
    if (!(fastest > 0.0))
        return plan;

    const auto fits = [&](double quarters) {
        return quarters * fastest * pixelsPerSecond >= minPixelsPerLine;
    };

    if (fits(gridQuarterNotes))
    {
        plan.level = TempoGridLevel::Subdivision;
        return plan;
    }

    if (fits(minBeatQuartersInRange(map, qStart, qEnd)))
    {
        plan.level = TempoGridLevel::Beat;
        return plan;
    }

    const double bar = minBarQuartersInRange(map, qStart, qEnd);
    int stride = 1;
    while (stride < kMaxBarStride && !fits(bar * static_cast<double>(stride)))
        stride *= 2;

    plan.level = TempoGridLevel::Bar;
    plan.barStride = stride;
    return plan;
}

TempoGridPlan planTempoGridBars(const HostTempoMap &map, double hostStart,
                                double hostEnd, double pixelsPerSecond,
                                double minPixelsPerLine)
{
    TempoGridPlan plan;
    plan.level = TempoGridLevel::Bar;
    if (!map.isValid() || !(pixelsPerSecond > 0.0))
        return plan;

    if (hostEnd < hostStart)
        std::swap(hostStart, hostEnd);

    const double qStart = clampedQuarter(map, hostStart);
    const double qEnd = clampedQuarter(map, hostEnd);
    const double fastest = map.minSecondsPerQuarter(qStart, qEnd);
    const double bar = minBarQuartersInRange(map, qStart, qEnd);
    if (!(fastest > 0.0) || !(bar > 0.0))
        return plan;

    int stride = 1;
    while (stride < kMaxBarStride &&
           bar * static_cast<double>(stride) * fastest * pixelsPerSecond <
               minPixelsPerLine)
        stride *= 2;

    plan.barStride = stride;
    return plan;
}

std::vector<TempoGridLine> tempoGridLines(const HostTempoMap &map,
                                          double hostStart, double hostEnd,
                                          double gridQuarterNotes,
                                          const TempoGridPlan &plan)
{
    std::vector<TempoGridLine> lines;
    if (!map.isValid())
        return lines;
    if (hostEnd < hostStart)
        std::swap(hostStart, hostEnd);

    const double qStart = clampedQuarter(map, hostStart);
    const double qEnd = clampedQuarter(map, hostEnd);

    if (plan.level == TempoGridLevel::Subdivision)
    {
        if (!(gridQuarterNotes > 0.0))
            return lines;
        map.forEachGridLine(qStart, qEnd, gridQuarterNotes,
                            [&](const HostTempoMap::GridLineInfo &info) {
                                appendLine(lines, map, info);
                            });
        return lines;
    }

    if (plan.level == TempoGridLevel::Beat)
    {
        // Beats come from each bar's own signature, so 7/8 has seven eighth
        // notes and 3/4 three quarters.
        map.forEachBar(qStart, qEnd, [&](const HostTempoMap::BarInfo &bar) {
            lines.push_back({map.timeForQuarter(bar.startQuarter),
                             bar.startQuarter, bar.barIndex, bar.barNumber, 0,
                             HostTempoMap::GridLineKind::Bar});

            HostTempoMap::BarSignature signature;
            signature.quarter = bar.startQuarter;
            signature.numerator = bar.numerator;
            signature.denominator = bar.denominator;
            const double beat = map.beatQuartersFor(signature);
            if (!(beat > 0.0))
                return;

            const double length = bar.endQuarter - bar.startQuarter;
            for (int k = 1; k < bar.numerator; ++k)
            {
                const double offset = static_cast<double>(k) * beat;
                if (offset >= length - kEpsilon)
                    break;
                const double quarter = bar.startQuarter + offset;
                lines.push_back({map.timeForQuarter(quarter), quarter,
                                 bar.barIndex, bar.barNumber, k,
                                 HostTempoMap::GridLineKind::Beat});
            }
        });
        return lines;
    }

    const int stride = std::max(1, plan.barStride);
    map.forEachBar(qStart, qEnd, [&](const HostTempoMap::BarInfo &bar) {
        if (stride > 1 && (bar.barIndex % stride) != 0)
            return;
        lines.push_back({map.timeForQuarter(bar.startQuarter), bar.startQuarter,
                         bar.barIndex, bar.barNumber, 0,
                         HostTempoMap::GridLineKind::Bar});
    });
    return lines;
}
