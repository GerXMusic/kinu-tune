// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#include "MidiGuidePersistence.h"

#include "../Utils/PlatformPaths.h"

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
    sourceIds.erase(std::remove_if(sourceIds.begin(), sourceIds.end(),
                                   [](const juce::String &id) {
                                       return id.isEmpty();
                                   }),
                    sourceIds.end());
    if (sourceIds.empty())
        return {};

    // The host may hand the sources in any order; the fingerprint must not
    // depend on it.
    std::sort(sourceIds.begin(), sourceIds.end());
    juce::String seed("tunex-ara-document-v1");
    for (const auto &id : sourceIds)
        seed << '\n' << id;
    return fnv1aHex(seed);
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

bool MidiGuidePersistence::save(const juce::String &projectKey,
                                const juce::String &trackKey,
                                const std::vector<GuideSource> &guides,
                                juce::String *error)
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

MidiGuideLoadResult MidiGuidePersistence::load(const juce::String &projectKey,
                                               const juce::String &trackKey) const
{
    MidiGuideLoadResult result;
    const auto file = fileFor(projectKey, trackKey);
    if (file == juce::File() || !file.existsAsFile())
        return result; // Status::none: the caller migrates

    auto reject = [&result, &file](const juce::String &why) {
        result.status = MidiGuideLoadResult::Status::rejected;
        result.guides.clear();
        result.message = "MIDI guide file rejected (" + why + "): " +
                         file.getFileName();
        return result;
    };

    juce::var parsed;
    const auto parsedOk =
        juce::JSON::parse(file.loadFileAsString(), parsed).wasOk();
    if (!parsedOk || !parsed.isObject())
        return reject("not valid JSON");

    auto *root = parsed.getDynamicObject();
    if (root == nullptr)
        return reject("not an object");

    const auto format =
        static_cast<int>(root->getProperty("format"));
    if (format != midiGuideDisk::kFormatVersion)
        return reject("format " + juce::String(format));

    if (root->getProperty("projectKey").toString() != projectKey)
        return reject("another project");
    if (root->getProperty("trackKey").toString() != trackKey)
        return reject("another track");

    auto *guides = root->getProperty("guides").getArray();
    if (guides == nullptr)
        return reject("no guides list");

    std::vector<GuideSource> loaded;
    loaded.reserve(static_cast<std::size_t>(guides->size()));
    for (const auto &guideVar : *guides)
    {
        if (!guideVar.isObject())
            return reject("guide is not an object");
        auto *guideObj = guideVar.getDynamicObject();
        if (guideObj == nullptr)
            return reject("guide is not an object");

        GuideSource guide;
        guide.id = guideObj->getProperty("id").toString().toStdString();
        guide.label = guideObj->getProperty("label").toString().toStdString();
        guide.kind = guideObj->getProperty("kind").toString().toStdString();
        guide.importedAt =
            guideObj->getProperty("importedAt").toString().toStdString();
        if (guide.id.empty())
            return reject("guide without id");

        auto *notes = guideObj->getProperty("notes").getArray();
        if (notes == nullptr)
            return reject("guide without notes");

        guide.notes.reserve(static_cast<std::size_t>(notes->size()));
        for (const auto &entryVar : *notes)
        {
            auto *values = entryVar.getArray();
            if (values == nullptr || values->size() < 3)
                return reject("malformed note");
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
                return reject("non-finite note");
            guide.notes.push_back(note);
        }
        loaded.push_back(std::move(guide));
    }

    // No normalise() here on purpose: the file is the truth and a round trip
    // must return the very same notes, order included. The importer normalises
    // before it ever reaches the store.
    result.status = MidiGuideLoadResult::Status::loaded;
    result.guides = std::move(loaded);
    return result;
}
