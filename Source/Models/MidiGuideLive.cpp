// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#include "MidiGuideLive.h"

#include "../Audio/IO/MidiGuideImporter.h"

#include <algorithm>
#include <cstring>

namespace midiGuideLive
{
namespace
{
constexpr juce::uint64 kFnvOffset = 1469598103934665603ull;
constexpr juce::uint64 kFnvPrime = 1099511628211ull;

void mix(juce::uint64& hash, const void* data, std::size_t bytes)
{
    const auto* raw = static_cast<const juce::uint8*>(data);
    for (std::size_t i = 0; i < bytes; ++i)
    {
        hash ^= raw[i];
        hash *= kFnvPrime;
    }
}
} // namespace

juce::String contentHash(const GuideSource& guide)
{
    juce::uint64 hash = kFnvOffset;
    const auto count = static_cast<juce::uint64>(guide.notes.size());
    mix(hash, &count, sizeof(count));
    for (const auto& note : guide.notes)
    {
        // Field by field, bit exact: a moved note changes the hash even when
        // the list still has the same length.
        mix(hash, &note.startQuarter, sizeof(note.startQuarter));
        mix(hash, &note.endQuarter, sizeof(note.endQuarter));
        mix(hash, &note.pitch, sizeof(note.pitch));
        mix(hash, &note.velocity, sizeof(note.velocity));
    }
    return juce::String::formatted("%016llx",
                                   static_cast<unsigned long long>(hash));
}

juce::String stableSourceId(const juce::String& trackGuid,
                            const juce::String& takeKey)
{
    juce::String take = takeKey.trim();
    if (take.isEmpty())
        take = "midi";
    // A colon would add a fourth field and make the id look like the old
    // positional one (isLegacyReaperSourceId). The ReaScript does the same.
    take = take.replaceCharacter(':', '_');
    return "reaper:" + trackGuid.trim() + ":" + take;
}

bool isLegacyReaperSourceId(const juce::String& sourceId)
{
    if (!sourceId.startsWith("reaper:"))
        return false;

    const int trackEnd = sourceId.indexOfChar(':');
    if (trackEnd < 0)
        return false;
    const int third = sourceId.indexOfChar(trackEnd + 1, ':');
    if (third < 0)
        return false; // only two fields: not a ReaScript live id

    juce::String rest = sourceId.substring(third + 1);
    const int fieldEnd = rest.indexOfChar(':');
    if (fieldEnd <= 0)
        return false; // no take field after it: not the old shape

    const juce::String maybePosition = rest.substring(0, fieldEnd);
    if (!maybePosition.containsOnly("0123456789.-"))
        return false;
    return maybePosition.containsAnyOf("0123456789");
}

Status statusFor(const GuideSource& guide, const HostTempoMap& tempo,
                 const juce::String& lastAppliedHash)
{
    Status status;
    status.notes = static_cast<int>(guide.notes.size());
    status.updatedAt = juce::String(guide.importedAt);
    status.hash = contentHash(guide);
    status.changedSinceApply =
        lastAppliedHash.isNotEmpty() && lastAppliedHash != status.hash;

    if (!guide.notes.empty())
    {
        double first = guide.notes.front().startQuarter;
        double last = guide.notes.front().endQuarter;
        for (const auto& note : guide.notes)
        {
            first = std::min(first, note.startQuarter);
            last = std::max(last, note.endQuarter);
        }
        status.hostStartSeconds = tempo.timeForQuarter(first);
        status.hostEndSeconds = tempo.timeForQuarter(last);
    }
    return status;
}

LiveDirectoryScanner::Result
LiveDirectoryScanner::scan(const juce::File& directory)
{
    Result result;
    if (!directory.isDirectory())
    {
        // No folder is not "no guides": the caller keeps what the sidecar
        // loaded (D-363).
        cache.clear();
        return result;
    }
    result.directoryExists = true;

    std::vector<juce::File> files;
    for (const auto& entry :
         juce::RangedDirectoryIterator(directory, false, "guide_*.json"))
        if (entry.getFile().existsAsFile())
            files.push_back(entry.getFile());

    // Deterministic order: if two files ever share an id, the last name wins,
    // not whatever the file system returned.
    std::sort(files.begin(), files.end(),
              [](const juce::File& a, const juce::File& b)
              {
                  return a.getFileName() < b.getFileName();
              });

    std::map<juce::String, int> indexById;
    std::vector<juce::String> seenPaths;
    seenPaths.reserve(files.size());

    for (const auto& file : files)
    {
        const juce::String path = file.getFullPathName();
        seenPaths.push_back(path);
        const juce::int64 modified =
            file.getLastModificationTime().toMilliseconds();
        const juce::int64 size = file.getSize();

        auto found = cache.find(path);
        if (found != cache.end() && found->second.modified == modified &&
            found->second.size == size)
        {
            ++result.reused;
        }
        else
        {
            GuideSource source;
            juce::String error;
            if (!MidiGuideImporter::importJsonFile(file, source, error))
            {
                ++result.rejected;
                cache.erase(path);
                continue;
            }
            Cached cached;
            cached.modified = modified;
            cached.size = size;
            cached.source = std::move(source);
            cache[path] = std::move(cached);
            ++result.reRead;
            found = cache.find(path);
        }

        const GuideSource& source = found->second.source;
        if (isLegacyReaperSourceId(juce::String(source.id)))
        {
            // A pre-R10 file (position inside the id). Ignored, not deleted:
            // the file is the user's, but it must not become a second guide.
            ++result.ignored;
            continue;
        }

        const auto insert = indexById.emplace(source.id,
                                              static_cast<int>(result.guides.size()));
        if (!insert.second)
            result.guides[static_cast<std::size_t>(insert.first->second)] =
                source; // one guide per identity
        else
            result.guides.push_back(source);
    }

    // Drop cached sources whose file is gone, so the memory of the scanner
    // does not grow with every re-run.
    for (auto it = cache.begin(); it != cache.end();)
    {
        if (std::find(seenPaths.begin(), seenPaths.end(), it->first) ==
            seenPaths.end())
            it = cache.erase(it);
        else
            ++it;
    }

    return result;
}
} // namespace midiGuideLive
