// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#include "ARATempoMapReader.h"

#if JucePlugin_Enable_ARA

#include <cmath>
#include <vector>

namespace
{
// ARA 2 says a valid tempo map has at least two entries and one of them in
// quarter 0 (plan/tempo-map-ara-grilla.md §3). Fewer than two is a constant
// tempo the host never spelled out; we refuse it so the caller uses the cursor
// BPM fallback instead of inventing a map.
bool looksUsable(const std::vector<HostTempoMap::TempoEntry>& entries)
{
    return entries.size() >= 2;
}
} // namespace

juce::String ARATempoMapReader::Probe::toString() const
{
    return juce::String("tempo-map probe: grade=") + grade +
           " contentGrade=" + juce::String(contentGrade) +
           " contexts=" + juce::String(musicalContexts) +
           " entries=" + juce::String(tempoEntries) +
           " bars=" + juce::String(barSignatures) +
           " first=" + juce::String(firstTimeSeconds, 4) + "s/q" +
           juce::String(firstQuarter, 4) +
           " last=" + juce::String(lastTimeSeconds, 4) + "s/q" +
           juce::String(lastQuarter, 4);
}

juce::ARAMusicalContext*
ARATempoMapReader::chooseContext(juce::ARAPlaybackRegion* region)
{
    if (region != nullptr)
    {
        if (auto* sequence = region->getRegionSequence())
        {
            if (auto* context = sequence->getMusicalContext())
                return context;

            if (auto* document = sequence->getDocument())
            {
                const auto& contexts = document->getMusicalContexts();
                if (contexts.size() == 1)
                    return contexts.front();
            }
        }
        return nullptr;
    }
    return nullptr;
}

std::shared_ptr<const HostTempoMap>
ARATempoMapReader::readFromContext(juce::ARAMusicalContext* context,
                                   Probe* probe)
{
    if (probe != nullptr)
        *probe = Probe{};

    if (context == nullptr)
    {
        if (probe != nullptr)
            probe->grade = "no-context";
        return nullptr;
    }

    if (probe != nullptr)
    {
        if (auto* document = context->getDocument())
            probe->musicalContexts =
                static_cast<int>(document->getMusicalContexts().size());
    }

    std::vector<HostTempoMap::TempoEntry> tempoEntries;
    std::vector<HostTempoMap::BarSignature> barSignatures;

    {
        const ARA::PlugIn::HostContentReader<ARA::kARAContentTypeTempoEntries>
            tempoReader(context);
        if (!tempoReader)
        {
            if (probe != nullptr)
                probe->grade = "no-reader";
            return nullptr;
        }

        if (probe != nullptr)
            probe->contentGrade = static_cast<int>(tempoReader.getGrade());

        const auto count = tempoReader.getEventCount();
        for (ARA::ARAInt32 i = 0; i < count; ++i)
        {
            const auto* event = tempoReader.getDataPtrForEvent(i);
            if (event == nullptr)
                continue;
            HostTempoMap::TempoEntry entry;
            entry.timeSeconds = event->timePosition;
            entry.quarter = event->quarterPosition;
            tempoEntries.push_back(entry);
        }
    }

    {
        const ARA::PlugIn::HostContentReader<ARA::kARAContentTypeBarSignatures>
            barReader(context);
        if (barReader)
        {
            const auto count = barReader.getEventCount();
            for (ARA::ARAInt32 i = 0; i < count; ++i)
            {
                const auto* event = barReader.getDataPtrForEvent(i);
                if (event == nullptr)
                    continue;
                HostTempoMap::BarSignature signature;
                signature.quarter = event->position;
                signature.numerator = event->numerator;
                signature.denominator = event->denominator;
                barSignatures.push_back(signature);
            }
        }
    }

    if (probe != nullptr)
    {
        probe->tempoEntries = static_cast<int>(tempoEntries.size());
        probe->barSignatures = static_cast<int>(barSignatures.size());
        if (!tempoEntries.empty())
        {
            probe->firstTimeSeconds = tempoEntries.front().timeSeconds;
            probe->firstQuarter = tempoEntries.front().quarter;
            probe->lastTimeSeconds = tempoEntries.back().timeSeconds;
            probe->lastQuarter = tempoEntries.back().quarter;
        }
    }

    if (!looksUsable(tempoEntries))
    {
        if (probe != nullptr)
            probe->grade = "constant";
        return nullptr;
    }

    auto map = std::make_shared<HostTempoMap>(
        HostTempoMap::fromEntries(std::move(tempoEntries),
                                  std::move(barSignatures)));
    if (!map->isValid())
    {
        if (probe != nullptr)
            probe->grade = "invalid";
        return nullptr;
    }

    if (probe != nullptr)
        probe->grade = map->isConstant() ? "constant" : "real-map";

    return map;
}

std::shared_ptr<const HostTempoMap>
ARATempoMapReader::readForRegion(juce::ARAPlaybackRegion* region, Probe* probe)
{
    auto* context = chooseContext(region);
    auto map = readFromContext(context, probe);
    if (probe != nullptr && map == nullptr)
    {
        if (auto* sequence = region != nullptr ? region->getRegionSequence() : nullptr)
            if (auto* document = sequence->getDocument())
                probe->musicalContexts =
                    static_cast<int>(document->getMusicalContexts().size());
    }
    return map;
}

#endif
