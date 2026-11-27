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

// ---- R11b (D-365) / R11d (D-367) / R11f (D-370): sidecar migration ------
//
// Before R11b the track key started with "doc:" and carried the address of
// the ARADocument, so every session wrote a NEW file under a NEW key; before
// R11d it started with "src:" and carried the fingerprint of the track's live
// items, so moving or adding one item did the same. R11d/R11e wrote the
// canonical "name:X" key at a path that still hashed the document view, which
// is what R11f removed. Those files are the user's guides; they are not
// deleted. A file is adopted when its track key names the same track (the name
// after "name:", or the same ordinal when the host never sent a name), it is
// written under the canonical path and the adopted file is renamed to
// "<file>.bak" so the old bytes stay for the user.

// Name of the track a key belongs to. Accepts the canonical form ("name:X")
// and the two forms older builds wrote ("doc:<address>|name:X",
// "src:<fingerprint>|name:X"). Empty for a key that carries no name.
juce::String namePartOfTrackKey(const juce::String &trackKey)
{
    const juce::String prefix("name:");
    if (trackKey.startsWith(prefix))
        return trackKey.substring(prefix.length());

    const juce::String marker("|name:");
    const int at = trackKey.lastIndexOf(marker);
    if (at < 0)
        return {};
    return trackKey.substring(at + marker.length());
}

juce::String ordinalPartOfTrackKey(const juce::String &trackKey)
{
    const juce::String prefix("ordinal:");
    if (trackKey.startsWith(prefix))
        return trackKey.substring(prefix.length());

    const juce::String marker("|ordinal:");
    const int at = trackKey.lastIndexOf(marker);
    if (at < 0)
        return {};
    return trackKey.substring(at + marker.length());
}

bool keyNamesSameTrack(const juce::String &candidateKey,
                       const juce::String &wantedKey)
{
    const auto wantedName = namePartOfTrackKey(wantedKey);
    if (wantedName.isNotEmpty())
        return namePartOfTrackKey(candidateKey) == wantedName;

    const auto wantedOrdinal = ordinalPartOfTrackKey(wantedKey);
    if (wantedOrdinal.isNotEmpty())
        return namePartOfTrackKey(candidateKey).isEmpty() &&
               ordinalPartOfTrackKey(candidateKey) == wantedOrdinal;

    // Every key trackKeyForTrack() produces carries a name or an ordinal, and so
    // did every legacy form. A key with neither matches only itself: without
    // this, two arbitrary keys would look like "the same track" and a save
    // could retire another track's file.
    return candidateKey == wantedKey;
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
    // The document VIEW of the sidecar (TrackIdentity.h). R11f (D-370) removed
    // it from the file name: it is written inside the file as "which view
    // wrote it last" and never chooses a path. It is fed the stable source
    // identities - the host's source names - not the persistent IDs: measured
    // in R11, the IDs changed on every reopen.
    return stableSourceFingerprint(std::move(sourceIds));
}

juce::File fileFor(const juce::File &directory, const juce::String &trackKey)
{
    // R11f (D-370): the track key alone names the file. The document view used
    // to be hashed in here, so every set of source names produced another path
    // and the guide jumped files on a view change (R12: second change -> 0/0).
    if (directory == juce::File() || trackKey.isEmpty())
        return {};
    return directory.getChildFile("guide-" + fnv1aHex(trackKey) +
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

juce::File MidiGuidePersistence::fileFor(const juce::String &trackKey) const
{
    return midiGuideDisk::fileFor(getDirectory(), trackKey);
}

bool MidiGuidePersistence::writeGuides(const juce::String &documentViewKey,
                                       const juce::String &trackKey,
                                       const std::vector<GuideSource> &guides,
                                       juce::String *error) const
{
    const auto target = fileFor(trackKey);
    if (target == juce::File())
    {
        if (error != nullptr)
            *error = "MIDI guide sidecar: empty track key";
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
    // Informational only (D-370): which view of the document wrote the file
    // last. load() never rejects a file because of it.
    json << "  \"projectKey\": " << quoted(documentViewKey) << ",\n";
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

// R11b (D-365) / R11d (D-367) / R11f (D-370): one live file per track. After
// the new file has been written, every OTHER readable file that names the same
// track is renamed to "<file>.bak": this covers the "doc:" and "src:...|name:"
// key forms of older builds AND the canonical files R11d/R11e left at their old
// per-view paths, which is the consolidation that leaves exactly one path per
// track. The user's bytes stay on disk; nothing is deleted.
void MidiGuidePersistence::retireOtherFilesOfTrack(const juce::String &trackKey,
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
        if (!keyNamesSameTrack(parsed.trackKey, trackKey))
            continue;
        file.moveFileTo(backupPathFor(file));
    }
}

bool MidiGuidePersistence::save(const juce::String &documentViewKey,
                                const juce::String &trackKey,
                                const std::vector<GuideSource> &guides,
                                juce::String *error)
{
    if (!writeGuides(documentViewKey, trackKey, guides, error))
        return false;
    retireOtherFilesOfTrack(trackKey, fileFor(trackKey));
    return true;
}

MidiGuideLoadResult MidiGuidePersistence::load(const juce::String &documentViewKey,
                                               const juce::String &trackKey) const
{
    MidiGuideLoadResult result;
    const auto target = fileFor(trackKey);
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
        // R11f (D-370): the recorded document view is NOT validated. The file
        // is the track's, every view of the document reads it, and the view
        // that wrote it last is only context. The track key still is: a file of
        // another track copied over this one keeps saying so.
        if (parsed.trackKey != trackKey)
            return reject("another track");

        // No normalise() here on purpose: the file is the truth and a round
        // trip must return the very same notes, order included. The importer
        // normalises before it ever reaches the store.
        result.status = MidiGuideLoadResult::Status::loaded;
        result.guides = parsed.guides;
        return result;
    }

    // R11b (D-365) / R11d (D-367) / R11f (D-370): no file at this track's path.
    // Adopt every readable file that names the same track - the "doc:" files of
    // R11, the "src:...|name:" files of R11b/R11c and the canonical files of
    // R11d/R11e that live at the old per-view paths - merge them by guide id,
    // write the single canonical file and retire each adopted one to .bak. With
    // one path per track there is no "which view is this" question left, so
    // every candidate is the same track's history. This runs only when the path
    // has no file, so it can never overwrite a guide the track already owns.
    struct Candidate
    {
        juce::File file;
        std::vector<GuideSource> guides;
    };
    std::vector<Candidate> candidates;
    for (const auto &file : guideFilesIn(getDirectory()))
    {
        if (file == target)
            continue;
        const auto parsed = parseGuideFile(file);
        if (!parsed.ok)
            continue;
        if (!keyNamesSameTrack(parsed.trackKey, trackKey))
            continue;
        candidates.push_back({file, parsed.guides});
    }

    if (candidates.empty())
        return result; // Status::none: the caller migrates from the Project cache

    std::vector<GuideSource> adopted;
    for (const auto &candidate : candidates)
        mergeGuides(adopted, candidate.guides);

    juce::String error;
    if (!writeGuides(documentViewKey, trackKey, adopted, &error))
    {
        result.status = MidiGuideLoadResult::Status::rejected;
        result.message = "MIDI guide legacy migration failed: " + error;
        return result;
    }
    for (const auto &candidate : candidates)
        candidate.file.moveFileTo(backupPathFor(candidate.file));

    result.status = MidiGuideLoadResult::Status::loaded;
    result.guides = std::move(adopted);
    return result;
}
