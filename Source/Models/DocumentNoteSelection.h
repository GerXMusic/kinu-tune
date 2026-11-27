// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#pragma once

#include "DocumentNoteIndex.h"

#include <cstdint>
#include <utility>
#include <vector>

/**
 * Document-level note selection (N2, decision D-340).
 *
 * One set for the whole track: (region, note) pairs deduplicated by
 * (Project*, Note*), so a note two sibling windows of the same take show counts
 * once. It points at the real Notes the index publishes; it copies nothing and
 * owns no edits, exactly like DocumentNoteIndex.
 *
 * Division of labour with Note::selected: this set is the document truth for
 * the marquee, shift-click and "select all" of track mode, and what
 * notes_debug publishes. Note::selected stays the cheap per-note cache the
 * renderers and the edit handlers read, and is derived from the set by
 * applyToNotes(). refresh() is the inverse edge: when a legacy path (a click in
 * the active item, a split, a merge) moved the flags, it re-adopts them so the
 * two never drift. A note no window of the track shows cannot enter the set.
 */
struct DocumentNoteSelectionEntry
{
    // Lowest-index window that shows the note; per-region counts add up to
    // size() because of it.
    juce::String regionKey;
    juce::String modificationKey; // whose edit this note is
    juce::String sourceKey;       // audio source (file) it comes from
    Project* project = nullptr;   // not owned
    Note* note = nullptr;         // inside project->getNotes(); never copied
};

class DocumentNoteSelection
{
public:
    // Re-adopts the live Note::selected flags, restricted to what the index's
    // windows show, and drops anything whose Project/Note no longer exists (an
    // item deleted or a project reloaded). Null clears the set and keeps the
    // live projects so applyToNotes() can still release the flags.
    void refresh(const DocumentNoteIndex* index);

    // Empties the set; the notes keep being released through applyToNotes().
    void clear();

    // Every visible note of the document (track "select all"). The index is
    // already deduplicated and window-bounded, so nothing invisible enters.
    bool selectAll(const DocumentNoteIndex& index);

    // Replaces the set with these notes. Pairs not shown by any window of the
    // index are ignored: nothing invisible is selectable.
    bool setFromPairs(const std::vector<std::pair<Project*, Note*>>& pairs,
                      const DocumentNoteIndex& index);

    // Marquee/shift-click primitive: adopts the live flags, then adds or
    // removes the note. Returns false when the note is not in the document.
    bool toggle(Project* project, Note* note, const DocumentNoteIndex& index);

    bool contains(const Project* project, const Note* note) const;
    bool isEmpty() const { return entries_.empty(); }
    int size() const { return static_cast<int>(entries_.size()); }

    std::vector<DocumentNoteSelectionEntry> entries() const { return entries_; }
    int countForRegion(const juce::String& regionKey) const;
    int countForSource(const juce::String& sourceKey) const;
    int countForModification(const juce::String& modificationKey) const;

    // Projects the set may touch: the ones the published index points at.
    std::vector<Project*> liveProjects() const { return projects_; }

    // Derives Note::selected: true only for the notes of the set, false for
    // every other note (and for tombstones/rests) of every live project.
    void applyToNotes() const;

    // Bumped by every membership change; lets a view know the set moved.
    std::uint64_t getRevision() const { return revision_; }

private:
    int findIndex(const Project* project, const Note* note) const;
    static std::vector<Project*> projectsOf(const DocumentNoteIndex& index);
    static DocumentNoteSelectionEntry entryFrom(const DocumentNoteIndex& index,
                                                const DocumentNoteRef& ref);
    bool replace(std::vector<DocumentNoteSelectionEntry> next,
                 std::vector<Project*> projects);

    std::vector<DocumentNoteSelectionEntry> entries_;
    std::vector<Project*> projects_;
    std::uint64_t revision_ = 0;
};
