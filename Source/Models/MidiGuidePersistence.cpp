// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#include "MidiGuidePersistence.h"

#include "../Utils/PlatformPaths.h"
#include "TrackIdentity.h"

#include <algorithm>
#include <cmath>

namespace
{
// 17 significant digits round-trip every finite double, and the JSON parser
// JUCE uses (`readDoubleValue`, 17+1 digits) reads them back exactly. Same
// number of digits as ProjectSerializer's own precision budget, so a guide
// written here and one written by the host archive agree bit for bit.
juce::String number(double value) { return juce::String(value, 17); }

juce::String quoted(const juce::String &text)
{
    return juce::JSON::toString(juce::var(text), true);
}

juce::String guideJson(const GuideSource &guide)
{
    juce::String out;
    out << "    {\n";
    out << "      \"id\": " << quoted(juce::String(guide.id)) << ",\n";
    out << "      \"label\": " << quoted(juce::String(guide.label)) << ",\n";
    out << "      \"kind\": " << quoted(juce::String(guide.kind)) << ",\n";
    out << "      \"importedAt\": " << quoted(juce::String(guide.importedAt))
        << ",\n";
    out << "      \"notes\": [";
    for (std::size_t i = 0; i < guide.notes.size(); ++i)
    {
        const auto &note = guide.notes[i];
        out << (i == 0 ? "\n" : ",\n");
        out << "        [" << number(note.startQuarter) << ", "
            << number(note.endQuarter) << ", "
            << number(static_cast<double>(note.pitch)) << ", "
            << number(static_cast<double>(note.velocity)) << "]";
    }
    if (!guide.notes.empty())
        out << "\n      ";
    out << "]\n";
    out << "    }";
    return out;
}

bool notesAreFinite(const GuideSource &guide)
{
    for (const auto &note : guide.notes)
        if (!juce::juce_isfinite(note.startQuarter) ||
            !juce::juce_isfinite(note.endQuarter) ||
            !juce::juce_isfinite(static_cast<double>(note.pitch)) ||
            !juce::juce_isfinite(static_cast<double>(note.velocity)))
            return false;
    return true;
}

// FNV-1a 64, the same hash the ARA diagnostics use (AraDiagnostics.h). This
// JUCE copy carries no crypto module, and a 64-bit hash is ample for the
// handful of guide files one user has: it only has to name a file, the keys
// inside the file are what validate it.
juce::String fnv1aHex(const juce::String &text)
{
    juce::uint64 hash = 1469598103934665603ull;
    const auto utf8 = text.toUTF8();
    if (const char *p = utf8.getAddress())
        for (; *p != '\0'; ++p)
        {
            hash ^= static_cast<unsigned char>(*p);
            hash *= 1099511628211ull;
        }
    return juce::String::toHexString(static_cast<juce::int64>(hash))
        .toLowerCase();
}

struct ParsedGuideFile
{
    bool ok = false;
    juce::String why;
    int format = 0;
    juce::String projectKey;
    juce::String trackKey;
    std::vector<GuideSource> guides;
};

// One reader for the file, used by load() and by the legacy migration. It
// validates the CONTENT only; whose file it is (which project, which track) is
// the caller's decision, because the migration accepts a legacy track key.
ParsedGuideFile parseGuideFile(const juce::File &file)
{
    ParsedGuideFile parsed;

    juce::var rootVar;
    const auto parsedOk =
        juce::JSON::parse(file.loadFileAsString(), rootVar).wasOk();
    if (!parsedOk || !rootVar.isObject())
    {
        parsed.why = "not valid JSON";
        return parsed;
    }

    auto *root = rootVar.getDynamicObject();
    if (root == nullptr)
    {
        parsed.why = "not an object";
        return parsed;
    }

    parsed.format = static_cast<int>(root->getProperty("format"));
    parsed.projectKey = root->getProperty("projectKey").toString();
    parsed.trackKey = root->getProperty("trackKey").toString();
    if (parsed.format != midiGuideDisk::kFormatVersion)
    {
        parsed.why = "format " + juce::String(parsed.format);
        return parsed;
    }

    auto *guides = root->getProperty("guides").getArray();
    if (guides == nullptr)
    {
        parsed.why = "no guides list";
        return parsed;
    }

    parsed.guides.reserve(static_cast<std::size_t>(guides->size()));
    for (const auto &guideVar : *guides)
    {
        if (!guideVar.isObject())
        {
            parsed.why = "guide is not an object";
            return parsed;
        }
        auto *guideObj = guideVar.getDynamicObject();
        if (guideObj == nullptr)
        {
            parsed.why = "guide is not an object";
            return parsed;
        }

        GuideSource guide;
        guide.id = guideObj->getProperty("id").toString().toStdString();
        guide.label = guideObj->getProperty("label").toString().toStdString();
        guide.kind = guideObj->getProperty("kind").toString().toStdString();
        guide.importedAt =
            guideObj->getProperty("importedAt").toString().toStdString();
        if (guide.id.empty())
        {
            parsed.why = "guide without id";
            return parsed;
        }

        auto *notes = guideObj->getProperty("notes").getArray();
        if (notes == nullptr)
        {
            parsed.why = "guide without notes";
            return parsed;
        }

        guide.notes.reserve(static_cast<std::size_t>(notes->size()));
        for (const auto &entryVar : *notes)
        {
            auto *values = entryVar.getArray();
            if (values == nullptr || values->size() < 3)
            {
                parsed.why = "malformed note";
                return parsed;
            }
            GuideNote note;
            note.startQuarter = static_cast<double>((*values)[0]);
            note.endQuarter = static_cast<double>((*values)[1]);
            note.pitch = static_cast<float>(
                static_cast<double>((*values)[2]));
            note.velocity =
                values->size() > 3
                    ? static_cast<float>(static_cast<double>((*values)[3]))
                    : 0.0f;
            if (!juce::juce_isfinite(note.startQuarter) ||
                !juce::juce_isfinite(note.endQuarter) ||
                !juce::juce_isfinite(static_cast<double>(note.pitch)))
            {
                parsed.why = "non-finite note";
                return parsed;
            }
            guide.notes.push_back(note);
        }
        parsed.guides.push_back(std::move(guide));
    }

    parsed.ok = true;
    return parsed;
}

// ---- R11b (D-365): legacy sidecar migration --------------------------
//
// Before R11b the track key started with "doc:" and carried the address of
// the ARADocument, so every session wrote a NEW file under a NEW key. Those
// files are the user's guides; they are not deleted. A legacy file is adopted
// when its track key names the same track (the name after "|name:", or the
// same ordinal when the host never sent a name), it is rewritten under the new
// key and the legacy file is renamed to "<file>.bak" so exactly one live file
// remains and the old bytes are still there for the user.

constexpr const char *kLegacyKeyPrefix = "doc:";

juce::String namePartOfTrackKey(const juce::String &trackKey)
{
    const juce::String marker("|name:");
    const int at = trackKey.lastIndexOf(marker);
    if (at < 0)
        return {};
    return trackKey.substring(at + marker.length());
}

juce::String ordinalPartOfTrackKey(const juce::String &trackKey)
{
    const juce::String marker("|ordinal:");
    const int at = trackKey.lastIndexOf(marker);
    if (at < 0)
        return {};
    return trackKey.substring(at + marker.length());
}

bool legacyKeyIsSameTrack(const juce::String &legacyKey,
                          const juce::String &wantedKey)
{
    if (!legacyKey.startsWith(kLegacyKeyPrefix))
        return false;
    const auto wantedName = namePartOfTrackKey(wantedKey);
    if (wantedName.isNotEmpty())
        return namePartOfTrackKey(legacyKey) == wantedName;
    return namePartOfTrackKey(legacyKey).isEmpty() &&
           ordinalPartOfTrackKey(legacyKey) ==
               ordinalPartOfTrackKey(wantedKey);
}

// Merges by guide id: adopting two legacy files of the same track (the normal
// case after R11, one per session) must not duplicate the same guide. The
// newest importedAt wins; with no timestamp the longer copy wins.
void mergeGuides(std::vector<GuideSource> &into,
                 const std::vector<GuideSource> &from)
{
    for (const auto &guide : from)
    {
        auto it = std::find_if(into.begin(), into.end(),
                               [&guide](const GuideSource &existing) {
                                   return existing.id == guide.id;
                               });
        if (it == into.end())
        {
            into.push_back(guide);
            continue;
        }
        if (guide.importedAt > it->importedAt ||
            (guide.importedAt == it->importedAt &&
             guide.notes.size() > it->notes.size()))
            *it = guide;
    }
}

std::vector<juce::File> guideFilesIn(const juce::File &directory)
{
    std::vector<juce::File> files;
    if (!directory.isDirectory())
        return files;
    for (const auto &entry :
         directory.findChildFiles(juce::File::findFiles, false, "guide-*.json"))
        if (entry.existsAsFile())
            files.push_back(entry);

    // Deterministic order: the merge must not depend on how the filesystem
    // returned the entries.
    std::sort(files.begin(), files.end(),
              [](const juce::File &a, const juce::File &b) {
                  return a.getFileName() < b.getFileName();
              });
    return files;
}

juce::File backupPathFor(const juce::File &file)
{
    const auto base = file.getFileName();
    auto candidate = file.getSiblingFile(base + ".bak");
    for (int i = 1; candidate.exists(); ++i)
        candidate = file.getSiblingFile(base + "." + juce::String(i) + ".bak");
    return candidate;
}
} // namespace

namespace midiGuideDisk
{
juce::File defaultDirectory()
{
    return PlatformPaths::getConfigDirectory()
        .getChildFile("guides")
        .getChildFile("projects");
}

juce::String projectKeyFromSources(std::vector<juce::String> sourceIds)
{
    // Same fingerprint as the track key's first half (TrackIdentity.h). It is
    // fed the stable source identities - the host's source names - not the
    // persistent IDs: measured in R11, the IDs changed on every reopen.
    return stableSourceFingerprint(std::move(sourceIds));
}

juce::File fileFor(const juce::File &directory, const juce::String &projectKey,
                   const juce::String &trackKey)
{
    if (directory == juce::File() || projectKey.isEmpty() || trackKey.isEmpty())
        return {};
    return directory.getChildFile("guide-" +
                                 fnv1aHex(projectKey + "|" + trackKey) +
                                 kFileExtension);
}
} // namespace midiGuideDisk

MidiGuidePersistence::MidiGuidePersistence(juce::File directoryIn)
    : directory(std::move(directoryIn))
{
}

juce::File MidiGuidePersistence::getDirectory() const
{
    return directory == juce::File() ? midiGuideDisk::defaultDirectory()
                                     : directory;
}

juce::File MidiGuidePersistence::fileFor(const juce::String &projectKey,
                                         const juce::String &trackKey) const
{
    return midiGuideDisk::fileFor(getDirectory(), projectKey, trackKey);
}

bool MidiGuidePersistence::writeGuides(const juce::String &projectKey,
                                       const juce::String &trackKey,
                                       const std::vector<GuideSource> &guides,
                                       juce::String *error) const
{
    const auto target = fileFor(projectKey, trackKey);
    if (target == juce::File())
    {
        if (error != nullptr)
            *error = "MIDI guide sidecar: empty document or track key";
        return false;
    }

    for (const auto &guide : guides)
        if (!notesAreFinite(guide))
        {
            if (error != nullptr)
                *error = "MIDI guide sidecar: non-finite note, not written";
            return false;
        }

    juce::String json;
    json << "{\n";
    json << "  \"format\": " << midiGuideDisk::kFormatVersion << ",\n";
    json << "  \"projectKey\": " << quoted(projectKey) << ",\n";
    json << "  \"trackKey\": " << quoted(trackKey) << ",\n";
    json << "  \"guides\": [";
    for (std::size_t i = 0; i < guides.size(); ++i)
    {
        json << (i == 0 ? "\n" : ",\n");
        json << guideJson(guides[i]);
    }
    if (!guides.empty())
        json << "\n  ";
    json << "]\n}\n";

    const auto parent = target.getParentDirectory();
    if (!parent.createDirectory())
    {
        if (error != nullptr)
            *error = "MIDI guide sidecar: cannot create " + parent.getFullPathName();
        return false;
    }

    // Atomic by construction: the visible file is only ever the result of
    // renaming a fully written temporary sibling over it.
    juce::TemporaryFile temporary(target);
    bool wrote = false;
    {
        juce::FileOutputStream out(temporary.getFile());
        if (out.openedOk())
        {
            wrote = out.write(json.toRawUTF8(),
                              static_cast<std::size_t>(json.getNumBytesAsUTF8()));
            out.flush();
            wrote = wrote && !out.getStatus().failed();
        }
    }
    if (!wrote)
    {
        if (error != nullptr)
            *error = "MIDI guide sidecar: write failed (" +
                     target.getFileName() + ")";
        temporary.deleteTemporaryFile();
        return false;
    }

    if (!temporary.overwriteTargetFileWithTemporary())
    {
        if (error != nullptr)
            *error = "MIDI guide sidecar: cannot replace " +
                     target.getFullPathName();
        temporary.deleteTemporaryFile();
        return false;
    }
    return true;
}

// R11b (D-365): one live file per track. After the new key has been written,
// every legacy sidecar of the same track is renamed to "<file>.bak": the user's
// bytes stay on disk and no session can leave a second live key behind.
void MidiGuidePersistence::retireLegacyFiles(const juce::String &trackKey,
                                             const juce::File &keep) const
{
    const auto directory = getDirectory();
    for (const auto &file : guideFilesIn(directory))
    {
        if (file == keep)
            continue;
        const auto parsed = parseGuideFile(file);
        if (!parsed.ok)
            continue; // not ours: never touch a file we cannot read
        if (!legacyKeyIsSameTrack(parsed.trackKey, trackKey))
            continue;
        file.moveFileTo(backupPathFor(file));
    }
}

bool MidiGuidePersistence::save(const juce::String &projectKey,
                                const juce::String &trackKey,
                                const std::vector<GuideSource> &guides,
                                juce::String *error)
{
    if (!writeGuides(projectKey, trackKey, guides, error))
        return false;
    retireLegacyFiles(trackKey, fileFor(projectKey, trackKey));
    return true;
}

MidiGuideLoadResult MidiGuidePersistence::load(const juce::String &projectKey,
                                               const juce::String &trackKey) const
{
    MidiGuideLoadResult result;
    const auto target = fileFor(projectKey, trackKey);
    if (target == juce::File())
        return result;

    auto reject = [&result, &target](const juce::String &why) {
        result.status = MidiGuideLoadResult::Status::rejected;
        result.guides.clear();
        result.message = "MIDI guide file rejected (" + why + "): " +
                         target.getFileName();
        return result;
    };

    if (target.existsAsFile())
    {
        const auto parsed = parseGuideFile(target);
        if (!parsed.ok)
            return reject(parsed.why);
        if (parsed.projectKey != projectKey)
            return reject("another project");
        if (parsed.trackKey != trackKey)
            return reject("another track");

        // No normalise() here on purpose: the file is the truth and a round
        // trip must return the very same notes, order included. The importer
        // normalises before it ever reaches the store.
        result.status = MidiGuideLoadResult::Status::loaded;
        result.guides = parsed.guides;
        return result;
    }

    // R11b (D-365): no file for the new key. Adopt the legacy "doc:" sidecars
    // of this same track, rewrite them under the stable key and retire the old
    // files. This is the one-time migration of the guides written by the
    // builds up to R11; it runs only when the new key has no file, so it can
    // never overwrite a guide the stable key already owns.
    std::vector<GuideSource> adopted;
    bool adoptedAny = false;
    for (const auto &file : guideFilesIn(getDirectory()))
    {
        if (file == target)
            continue;
        const auto parsed = parseGuideFile(file);
        if (!parsed.ok)
            continue;
        if (!legacyKeyIsSameTrack(parsed.trackKey, trackKey))
            continue;
        mergeGuides(adopted, parsed.guides);
        adoptedAny = true;
    }

    if (!adoptedAny)
        return result; // Status::none: the caller migrates from the Project cache

    juce::String error;
    if (!writeGuides(projectKey, trackKey, adopted, &error))
    {
        result.status = MidiGuideLoadResult::Status::rejected;
        result.message = "MIDI guide legacy migration failed: " + error;
        return result;
    }
    retireLegacyFiles(trackKey, target);

    result.status = MidiGuideLoadResult::Status::loaded;
    result.guides = std::move(adopted);
    return result;
}
