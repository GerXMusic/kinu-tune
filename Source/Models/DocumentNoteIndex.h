// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#pragma once

#include "Note.h"
#include "Project.h"

#include <memory>
#include <utility>
#include <vector>

/**
 * Document-level index of the notes of one track (N1, decision D-340).
 *
 * This is the single, read-only enumeration of every note the track's items
 * hold. It does NOT copy notes and owns no edits: every reference points at a
 * Note inside its modification's Project, which stays the only owner. The UI
 * draws track mode from this index; the index is republished whenever a
 * modification revision, the active region or the canvas edit serial changes.
 *
 * Axes are explicit, as in MidiGuideRegionScope:
 *   source  : modification (project) seconds
 *   host    : timeline seconds = source + offsetSeconds
 */
struct DocumentNoteRef
{
    int regionIndex = -1;       // index into DocumentNoteIndex::regions
    Project* project = nullptr; // not owned; the modification's Project
    Note* note = nullptr;       // inside project->getNotes(); never copied
    int firstFrame = 0;
    int lastFrame = 0;
};

struct DocumentNoteRegion
{
    juce::String key;             // region selector (which window)
    juce::String name;
    juce::String modificationKey; // whose edits these are
    juce::String sourceKey;       // audio source (file) this item comes from
    double hostStartSeconds = 0.0;
    double hostEndSeconds = 0.0;
    // The window of the modification this item shows. Taken from ARA's
    // start/end in modification samples, not from the host duration: a
    // stretched or trimmed clip has one and the other disagree.
    double sourceStartSeconds = 0.0;
    double sourceEndSeconds = 0.0;
    double offsetSeconds = 0.0; // host - source
    Project* project = nullptr; // not owned; nullptr when not analysed
    bool analyzed = false;
    bool pinned = false; // another track pinned in the list (drawn, not edited)
    int projectNotes = 0;
    // Notes the host stored for the modification (archive), -1 when unknown.
    // Published so the measured saved-vs-live difference can be closed without
    // re-measuring by hand (N0 carried 262 vs 246).
    int savedNotes = -1;
    int notesInWindow = 0;
    // One entry per note of the window, pointing at the real Note. A note seen
    // by two sibling windows of the same modification appears in both regions
    // but only once in DocumentNoteIndex::notes.
    std::vector<DocumentNoteRef> notes;
};

struct DocumentNoteSourceSummary
{
    juce::String sourceKey;
    int regions = 0;
    int projectNotes = 0;
    int visibleNotes = 0;
    int outsideNotes = 0;
    bool analyzed = false;
};

struct DocumentNoteIndex
{
    juce::String trackKey;
    std::vector<DocumentNoteRegion> regions;
    // Every note visible in at least one window, deduplicated (one note of a
    // modification counts once however many sibling windows show it).
    std::vector<DocumentNoteRef> notes;
    // Notes of a modification's Project that no window of this track shows
    // (measured: 5 of 246). Published, never silently dropped.
    std::vector<DocumentNoteRef> outsideNotes;
    std::vector<DocumentNoteSourceSummary> sources;
    int totalNotes = 0;   // deduplicated visible notes
    int projectNotes = 0; // deduplicated notes stored in the projects
    int regionCount() const { return static_cast<int>(regions.size()); }
    bool empty() const { return regions.empty(); }
};

using DocumentNoteIndexPtr = std::shared_ptr<const DocumentNoteIndex>;

/** What the plug-in knows about one region when it builds the index. */
struct DocumentNoteRegionSpec
{
    juce::String key;
    juce::String name;
    juce::String modificationKey;
    juce::String sourceKey;
    double hostStartSeconds = 0.0;
    double hostEndSeconds = 0.0;
    double sourceStartSeconds = 0.0;
    double sourceEndSeconds = 0.0;
    Project* project = nullptr;
    bool pinned = false;
    int savedNotes = -1;
};

/** Analysis-frame window of one region, the one the drawing and the hit-test
    clip by. */
struct DocumentNoteWindowFrames
{
    int firstFrame = 0;
    int lastFrame = 0;
    bool valid = false;
};

/** Frame window of [sourceStart, sourceEnd) clamped to a project of frameCount
    analysis frames. N10 (D-350): the drawing and the hit-test clip by these
    frames, so the index and what is drawn always cover the same notes. */
DocumentNoteWindowFrames documentNoteWindowFrames(double sourceStartSeconds,
                                                  double sourceEndSeconds,
                                                  int frameCount);

/** True when the note's analysis span overlaps the frame window. */
bool documentNoteBelongsToWindow(const Note& note,
                                 const DocumentNoteWindowFrames& window);

/** Visible rectangle of one indexed note on the host timeline, clipped to its
    region. These are the note shapes track mode draws for the region. */
struct DocumentNoteShape
{
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    float midi = 0.0f;
};

/** Visible rectangle of one indexed note on the host timeline, clipped to its
    region. False when the note draws nothing there (a rest, a note with no
    pointer, or one lying wholly outside the window). N10 (D-350): the overview
    draws through this, so no note geometry is copied into a preview. */
bool documentNoteShape(const DocumentNoteRegion& region,
                       const DocumentNoteRef& ref,
                       DocumentNoteShape& shape);

std::vector<DocumentNoteShape>
documentNoteShapes(const DocumentNoteRegion& region);

/** Builds the index. Pure: reads each spec's Project, never writes it. */
DocumentNoteIndexPtr
buildDocumentNoteIndex(const juce::String& trackKey,
                       const std::vector<DocumentNoteRegionSpec>& specs);
