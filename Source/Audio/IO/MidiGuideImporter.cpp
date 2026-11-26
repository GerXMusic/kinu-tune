// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#include "MidiGuideImporter.h"

#include "../../Utils/Constants.h"
#include "../../Utils/SHA256Utils.h"

MidiGuideImporter::Result
MidiGuideImporter::importMidiFile(const juce::File &file)
{
    Result result;
    if (!file.existsAsFile())
    {
        result.error = "the file does not exist";
        return result;
    }

    juce::FileInputStream stream(file);
    if (!stream.openedOk())
    {
        result.error = "cannot open the file";
        return result;
    }

    juce::MidiFile midi;
    if (!midi.readFrom(stream))
    {
        result.error = "not a readable MIDI file";
        return result;
    }

    // Content hash: the same exported file keeps the same identity.
    const juce::String sourceId = "file:" + SHA256Utils::fileSHA256(file);
    return fromMidiFile(midi, file.getFileNameWithoutExtension(), sourceId);
}

MidiGuideImporter::Result
MidiGuideImporter::fromMidiFile(const juce::MidiFile &midi,
                                const juce::String &label,
                                const juce::String &sourceId)
{
    Result result;

    const short timeFormat = midi.getTimeFormat();
    if (timeFormat <= 0)
    {
        result.error =
            "SMPTE-timestamped MIDI needs a tempo map; export it as PPQ";
        return result;
    }

    const double ticksPerQuarter = static_cast<double>(timeFormat);

    GuideSource source;
    source.id = sourceId.toStdString();
    source.label = label.toStdString();
    source.kind = "file";
    source.importedAt =
        juce::Time::getCurrentTime().toISO8601(true).toStdString();

    for (int trackIndex = 0; trackIndex < midi.getNumTracks(); ++trackIndex)
    {
        const auto *track = midi.getTrack(trackIndex);
        if (track == nullptr)
            continue;

        for (int eventIndex = 0; eventIndex < track->getNumEvents();
             ++eventIndex)
        {
            const auto *event = track->getEventPointer(eventIndex);
            if (event == nullptr)
                continue;

            const auto &message = event->message;
            // Note-on with velocity 0 is a note-off in disguise; skip it (the
            // matching note-off closes the note below).
            if (!message.isNoteOn(true))
                continue;

            GuideNote note;
            note.startQuarter = event->message.getTimeStamp() / ticksPerQuarter;
            note.pitch = static_cast<float>(message.getNoteNumber());
            note.velocity = static_cast<float>(message.getVelocity()) / 127.0f;

            // Find the matching note-off in the same track. MidiMessage pairs
            // are not guaranteed to be adjacent, so scan forward.
            for (int offIndex = eventIndex + 1;
                 offIndex < track->getNumEvents(); ++offIndex)
            {
                const auto *off = track->getEventPointer(offIndex);
                if (off == nullptr)
                    continue;
                if (off->message.isNoteOff(false) &&
                    off->message.getNoteNumber() == message.getNoteNumber() &&
                    off->message.getChannel() == message.getChannel())
                {
                    note.endQuarter =
                        off->message.getTimeStamp() / ticksPerQuarter;
                    break;
                }
            }

            // Unclosed notes (a truncated export) get one beat, never a
            // negative length.
            if (note.endQuarter <= note.startQuarter)
                note.endQuarter = note.startQuarter + 1.0;

            if (note.startQuarter < 0.0 || note.pitch < MIN_MIDI_NOTE ||
                note.pitch > MAX_MIDI_NOTE)
            {
                ++result.droppedNotes;
                continue;
            }

            if (static_cast<int>(source.notes.size()) >= kMaxNotes)
            {
                result.droppedNotes += 1;
                continue;
            }
            source.notes.push_back(note);
        }
    }

    if (source.notes.empty())
    {
        result.error = "the MIDI file has no notes";
        return result;
    }

    source.normalise();
    result.ok = true;
    result.source = std::move(source);
    return result;
}

bool MidiGuideImporter::fromJson(const juce::var &json, GuideSource &destination,
                                 juce::String &error)
{
    error.clear();
    const auto *object = json.getDynamicObject();
    if (object == nullptr)
    {
        error = "the guide is not a JSON object";
        return false;
    }

    const int schema = static_cast<int>(object->getProperty("schema"));
    if (schema != kSchemaVersion)
    {
        error = "unsupported guide schema " + juce::String(schema);
        return false;
    }

    GuideSource source;
    source.id = object->getProperty("source_id").toString().toStdString();
    source.label = object->getProperty("label").toString().toStdString();
    source.kind = object->getProperty("kind").toString().toStdString();
    source.importedAt =
        object->getProperty("created_utc").toString().toStdString();
    if (source.kind.empty())
        source.kind = "file";
    if (source.id.empty())
        source.id = "live:" + source.label;

    const auto *notes = object->getProperty("notes").getArray();
    if (notes == nullptr)
    {
        error = "the guide has no notes array";
        return false;
    }
    if (notes->size() > kMaxNotes)
    {
        error = "the guide has more than " + juce::String(kMaxNotes) +
                " notes";
        return false;
    }

    int dropped = 0;
    for (const auto &entry : *notes)
    {
        const auto *values = entry.getArray();
        if (values == nullptr || values->size() < 3)
        {
            ++dropped;
            continue;
        }

        GuideNote note;
        note.startQuarter = static_cast<double>(values->getUnchecked(0));
        note.endQuarter = static_cast<double>(values->getUnchecked(1));
        note.pitch = static_cast<float>(static_cast<double>(
            values->getUnchecked(2)));
        note.velocity = values->size() > 3
                            ? static_cast<float>(static_cast<double>(
                                  values->getUnchecked(3)))
                            : 0.0f;

        const bool finite = std::isfinite(note.startQuarter) &&
                            std::isfinite(note.endQuarter) &&
                            std::isfinite(note.pitch);
        if (!finite || note.endQuarter <= note.startQuarter ||
            note.pitch < MIN_MIDI_NOTE || note.pitch > MAX_MIDI_NOTE)
        {
            ++dropped;
            continue;
        }
        note.velocity = juce::jlimit(0.0f, 1.0f, note.velocity);
        source.notes.push_back(note);
    }

    if (source.notes.empty())
    {
        error = "the guide has no valid notes";
        return false;
    }

    source.normalise();
    destination = std::move(source);
    return true;
}

bool MidiGuideImporter::importJsonFile(const juce::File &file,
                                       GuideSource &destination,
                                       juce::String &error)
{
    error.clear();
    if (!file.existsAsFile())
    {
        error = "the guide file does not exist";
        return false;
    }
    if (file.getSize() > kMaxJsonBytes)
    {
        error = "the guide file is larger than 8 MB";
        return false;
    }

    juce::var json;
    const auto parseResult = juce::JSON::parse(file.loadFileAsString(), json);
    if (parseResult.failed())
    {
        error = "invalid JSON: " + parseResult.getErrorMessage();
        return false;
    }
    return fromJson(json, destination, error);
}

std::vector<GuideSource>
MidiGuideImporter::readLiveGuides(const juce::File &liveDirectory,
                                  int *rejectedFiles)
{
    std::vector<GuideSource> guides;
    if (rejectedFiles != nullptr)
        *rejectedFiles = 0;

    if (!liveDirectory.isDirectory())
        return guides;

    for (const auto &entry :
         juce::RangedDirectoryIterator(liveDirectory, false, "guide_*.json"))
    {
        const auto file = entry.getFile();
        if (!file.existsAsFile())
            continue;

        GuideSource source;
        juce::String error;
        if (importJsonFile(file, source, error))
            guides.push_back(std::move(source));
        else if (rejectedFiles != nullptr)
            ++*rejectedFiles;
    }
    return guides;
}
