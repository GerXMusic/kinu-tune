// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#include "DocumentNoteIndex.h"

#include "../Utils/Constants.h"

#include <algorithm>
#include <map>
#include <set>

namespace
{
using NoteKey = std::pair<const Project*, const Note*>;

struct SourceTally
{
    DocumentNoteSourceSummary summary;
    std::set<NoteKey> projectNotes;
    std::set<NoteKey> visibleNotes;
    std::set<NoteKey> outsideNotes;
};

} // namespace

DocumentNoteWindowFrames documentNoteWindowFrames(double sourceStartSeconds,
                                                  double sourceEndSeconds,
                                                  int frameCount)
{
    DocumentNoteWindowFrames window;
    if (frameCount <= 0 || sourceEndSeconds <= sourceStartSeconds)
        return window;

    // Frame window of the item: floor at the start, one extra frame past the
    // end, both clamped to what the project stores. N10 (D-350) removed the
    // window Project, so these are the frames the drawing and the hit-test
    // clip by: the index and the drawing cover the same notes by construction.
    const double framesPerSecond =
        static_cast<double>(SAMPLE_RATE) / static_cast<double>(HOP_SIZE);
    const int firstFrame =
        juce::jlimit(0, frameCount,
                     static_cast<int>(std::floor(sourceStartSeconds * framesPerSecond)));
    const int lastFrame =
        juce::jlimit(firstFrame, frameCount,
                     static_cast<int>(std::ceil(sourceEndSeconds * framesPerSecond)) + 1);
    window.firstFrame = firstFrame;
    window.lastFrame = lastFrame;
    window.valid = lastFrame > firstFrame;
    return window;
}

bool documentNoteBelongsToWindow(const Note& note,
                                 const DocumentNoteWindowFrames& window)
{
    if (!window.valid)
        return false;
    return note.getEndFrame() > window.firstFrame &&
           note.getStartFrame() < window.lastFrame;
}

bool documentNoteShape(const DocumentNoteRegion& region,
                       const DocumentNoteRef& ref,
                       DocumentNoteShape& shape)
{
    if (!region.analyzed || ref.note == nullptr || ref.note->isRest())
        return false;

    const auto& note = *ref.note;
    const double shift = region.hostStartSeconds - region.sourceStartSeconds;
    // Same clipping the region preview has always applied: a note is drawn
    // only over the part of it the item shows.
    const double start =
        std::max(region.hostStartSeconds,
                 static_cast<double>(framesToSeconds(note.getVisualStartFrame())) + shift);
    const double end =
        std::min(region.hostEndSeconds,
                 static_cast<double>(framesToSeconds(note.getVisualEndFrame())) + shift);
    if (end <= start)
        return false;
    shape.startSeconds = start;
    shape.endSeconds = end;
    // N10 (D-350): every height goes through NotePitch's accessor.
    shape.midi = note.getAdjustedMidiNote();
    return true;
}

std::vector<DocumentNoteShape>
documentNoteShapes(const DocumentNoteRegion& region)
{
    std::vector<DocumentNoteShape> shapes;
    if (!region.analyzed)
        return shapes;

    shapes.reserve(region.notes.size());
    for (const auto& ref : region.notes)
    {
        DocumentNoteShape shape;
        if (documentNoteShape(region, ref, shape))
            shapes.push_back(shape);
    }
    return shapes;
}

DocumentNoteIndexPtr
buildDocumentNoteIndex(const juce::String& trackKey,
                       const std::vector<DocumentNoteRegionSpec>& specs)
{
    auto index = std::make_shared<DocumentNoteIndex>();
    index->trackKey = trackKey;
    index->regions.reserve(specs.size());

    std::map<juce::String, SourceTally> sources;
    std::set<NoteKey> visible;
    std::set<NoteKey> allProjectNotes;
    // Notes covered by some window of each project; whatever is left over is
    // the only thing reported as outside.
    std::map<const Project*, std::set<const Note*>> covered;

    for (const auto& spec : specs)
    {
        DocumentNoteRegion region;
        region.key = spec.key;
        region.name = spec.name;
        region.modificationKey = spec.modificationKey;
        region.sourceKey = spec.sourceKey;
        region.hostStartSeconds = spec.hostStartSeconds;
        region.hostEndSeconds = spec.hostEndSeconds;
        region.sourceStartSeconds = spec.sourceStartSeconds;
        region.sourceEndSeconds = spec.sourceEndSeconds;
        region.offsetSeconds = spec.hostStartSeconds - spec.sourceStartSeconds;
        region.project = spec.project;
        region.pinned = spec.pinned;
        region.savedNotes = spec.savedNotes;

        auto& tally = sources[region.sourceKey];
        tally.summary.sourceKey = region.sourceKey;
        ++tally.summary.regions;

        const int regionIndex = static_cast<int>(index->regions.size());

        if (spec.project != nullptr)
        {
            auto& notes = spec.project->getNotes();
            region.analyzed = true;
            region.projectNotes = static_cast<int>(notes.size());
            tally.summary.analyzed = true;
            for (const auto& note : notes)
            {
                tally.projectNotes.insert({spec.project, &note});
                allProjectNotes.insert({spec.project, &note});
            }

            const int frameCount =
                static_cast<int>(spec.project->getAudioData().f0.size());
            const auto window = documentNoteWindowFrames(
                region.sourceStartSeconds, region.sourceEndSeconds, frameCount);
            if (window.valid)
            {
                for (auto& note : notes)
                {
                    if (note.isRest() || !documentNoteBelongsToWindow(note, window))
                        continue;
                    DocumentNoteRef ref;
                    ref.regionIndex = regionIndex;
                    ref.project = spec.project;
                    ref.note = &note;
                    ref.firstFrame = note.getStartFrame();
                    ref.lastFrame = note.getEndFrame();
                    region.notes.push_back(ref);
                    covered[spec.project].insert(&note);
                    tally.visibleNotes.insert({spec.project, &note});
                    if (visible.insert({spec.project, &note}).second)
                        index->notes.push_back(ref);
                }
            }
        }

        region.notesInWindow = static_cast<int>(region.notes.size());
        index->regions.push_back(std::move(region));
    }

    // Notes a modification stores that no window of this track shows. They are
    // published, never dropped and never invented.
    {
        std::set<NoteKey> outside;
        std::set<const Project*> walked;
        for (const auto& spec : specs)
        {
            if (spec.project == nullptr || !walked.insert(spec.project).second)
                continue;
            const auto cov = covered.find(spec.project);
            for (auto& note : spec.project->getNotes())
            {
                if (note.isRest())
                    continue;
                if (cov != covered.end() && cov->second.count(&note) > 0)
                    continue;
                if (!outside.insert({spec.project, &note}).second)
                    continue;
                DocumentNoteRef ref;
                ref.regionIndex = -1;
                ref.project = spec.project;
                ref.note = &note;
                ref.firstFrame = note.getStartFrame();
                ref.lastFrame = note.getEndFrame();
                index->outsideNotes.push_back(ref);
            }
        }
    }

    for (auto& entry : sources)
    {
        auto& tally = entry.second;
        tally.summary.projectNotes = static_cast<int>(tally.projectNotes.size());
        tally.summary.visibleNotes = static_cast<int>(tally.visibleNotes.size());
        for (const auto& ref : index->outsideNotes)
            if (tally.projectNotes.count({ref.project, ref.note}) > 0)
                tally.outsideNotes.insert({ref.project, ref.note});
        tally.summary.outsideNotes = static_cast<int>(tally.outsideNotes.size());
        index->sources.push_back(tally.summary);
    }

    index->projectNotes = static_cast<int>(allProjectNotes.size());
    index->totalNotes = static_cast<int>(index->notes.size());
    return index;
}
