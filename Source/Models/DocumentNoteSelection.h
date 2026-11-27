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
 * Division of labour with Note::selected (N9, D-349): this set is the ONLY
 * owner of the selection. Note::selected is a derived view, written from this
 * file and nowhere else (SourceGuard_note-selection enforces it): applyToNotes()
 * derives every flag of every live project from the set, and every other entry
 * point here (setNoteSelected, selectAllOfProject, clearProject,
 * inheritSelectionToNewNote) ends in that same writer. refresh() is the inverse
 * edge: when a path that cannot see this set
 * (an item click in clip mode, a delete, an undo step) moved the flags, it
 * re-adopts them so the two never drift. A note no window of the track shows
 * cannot enter the set through a marquee or "select all".
 *
 * Clip mode publishes no index (N1): there is no document yet, so the item's
 * own flags are its truth and the writers here write them directly. Switching
 * between both modes does not clear the selection: the flags hold it while the
 * set is not published, and the next refresh() re-adopts it.
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
    DocumentNoteSelection() = default;
    // Detaches the projects it adopted, so no Project keeps a pointer to a set
    // that is going away.
    ~DocumentNoteSelection();

    DocumentNoteSelection(const DocumentNoteSelection&) = delete;
    DocumentNoteSelection& operator=(const DocumentNoteSelection&) = delete;

    // Re-adopts the live Note::selected flags, restricted to what the index's
    // windows show, and drops anything whose Project/Note no longer exists (an
    // item deleted or a project reloaded). Null means "no document published"
    // (clip mode): the set is emptied, every adopted project is detached and no
    // flag is touched, so the item keeps its own selection.
    void refresh(const DocumentNoteIndex* index);

    // Same, owning a copy of the shared publication so the index the set derives
    // its region keys from cannot be destroyed under it.
    void refresh(const DocumentNoteIndexPtr& index)
    {
        refresh(index.get());
        if (index != nullptr)
            index_ = index;
    }

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

    // The item's own "select all" and "deselect all" (Project::selectAllNotes /
    // Project::deselectAllNotes end here). With a published document they are
    // set changes: only the notes a window of that item shows can enter, and
    // every flag is derived again.
    static void selectAllOfProject(Project& project, bool includeRests);
    static void clearProject(Project& project);

    // One note, from a click, a "select this note first" of a context menu, a
    // delete or an undo/redo step. This is the only single-note entry point: it
    // mutates the set when the project has a published document and writes the
    // item's own flag when it does not.
    static void setNoteSelected(Project& project, Note& note, bool selected);

    // A structural edit (a split) gave the project a new note. addNote() may
    // have moved every Note of the project, so the pointers this set kept are
    // not trustworthy any more: they are dropped and the flags re-adopted. The
    // new half inherits the flag of the half it comes from, through here (the
    // index that names the region keys is republished right after).
    static void inheritSelectionToNewNote(Project& project, Note& created,
                                          bool selected);

    // The selection needs no window copy to be drawn: N10 (D-350) removed the
    // copies MainViewRegionPreview used to hold, so the notes the canvas draws
    // are these same Notes and carry the flag applyToNotes() derives.

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
    // THE single writer of Note::selected (D-349).
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
    // Region key of the note when the published index lists it, empty when it
    // does not (a note a split just created).
    void addEntry(Project& project, Note& note);
    void dropEntriesOfProject(const Project& project);
    void adoptProjectFlags(Project& project);
    bool noteIsPublished(const Project& project, const Note& note) const;
    void attachProjects(const std::vector<Project*>& projects);
    void detachProjects();

    std::vector<DocumentNoteSelectionEntry> entries_;
    std::vector<Project*> projects_;
    // The index the set was last derived from. Kept so a single note can be
    // added with its region keys; the Notes themselves are never copied.
    DocumentNoteIndexPtr index_;
    // Projects whose noteSelection points here, so they can be detached.
    std::vector<Project*> adopted_;
    std::uint64_t revision_ = 0;
};
