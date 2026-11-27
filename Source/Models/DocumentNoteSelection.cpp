// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#include "DocumentNoteSelection.h"

#include <algorithm>
#include <set>

namespace
{
using NoteKey = std::pair<const Project*, const Note*>;
}

std::vector<Project*> DocumentNoteSelection::projectsOf(
    const DocumentNoteIndex& index)
{
    std::vector<Project*> projects;
    for (const auto& region : index.regions)
    {
        if (region.project == nullptr)
            continue;
        if (std::find(projects.begin(), projects.end(), region.project) ==
            projects.end())
            projects.push_back(region.project);
    }
    return projects;
}

DocumentNoteSelectionEntry DocumentNoteSelection::entryFrom(
    const DocumentNoteIndex& index, const DocumentNoteRef& ref)
{
    DocumentNoteSelectionEntry entry;
    entry.project = ref.project;
    entry.note = ref.note;
    if (ref.regionIndex >= 0 && ref.regionIndex < index.regionCount())
    {
        const auto& region = index.regions[static_cast<size_t>(ref.regionIndex)];
        entry.regionKey = region.key;
        entry.modificationKey = region.modificationKey;
        entry.sourceKey = region.sourceKey;
    }
    return entry;
}

int DocumentNoteSelection::findIndex(const Project* project,
                                     const Note* note) const
{
    for (size_t i = 0; i < entries_.size(); ++i)
        if (entries_[i].project == project && entries_[i].note == note)
            return static_cast<int>(i);
    return -1;
}

bool DocumentNoteSelection::replace(
    std::vector<DocumentNoteSelectionEntry> next,
    std::vector<Project*> projects)
{
    bool changed = next.size() != entries_.size();
    if (!changed)
        for (size_t i = 0; i < next.size(); ++i)
            if (next[i].project != entries_[i].project ||
                next[i].note != entries_[i].note)
            {
                changed = true;
                break;
            }
    entries_ = std::move(next);
    projects_ = std::move(projects);
    if (changed)
        ++revision_;
    return changed;
}

void DocumentNoteSelection::refresh(const DocumentNoteIndex* index)
{
    if (index == nullptr)
    {
        clear();
        return;
    }

    std::vector<DocumentNoteSelectionEntry> next;
    std::set<NoteKey> seen;
    next.reserve(static_cast<size_t>(index->totalNotes));
    for (const auto& region : index->regions)
        for (const auto& ref : region.notes)
        {
            if (ref.note == nullptr || ref.project == nullptr)
                continue;
            // A rest is a hole in the melody and a deleted note is a tombstone:
            // neither is selectable (same rule as Project::selectAllNotes).
            if (ref.note->isRest() || ref.note->isDeleted())
                continue;
            if (!ref.note->isSelected())
                continue;
            // The first window that shows the note owns its entry, so sibling
            // windows of one take cannot select it twice.
            if (!seen.insert({ref.project, ref.note}).second)
                continue;
            next.push_back(entryFrom(*index, ref));
        }

    replace(std::move(next), projectsOf(*index));

    // Flags the set cannot represent (a note no window shows, a tombstone, an
    // item-mode "select all" that reached the invisible notes) are derived away
    // here so the published selection and Note::selected never disagree.
    int flagged = 0;
    for (auto* project : projects_)
        for (const auto& note : project->getNotes())
            if (note.isSelected() && !note.isRest())
                ++flagged;
    if (flagged != size())
        applyToNotes();
}

void DocumentNoteSelection::clear()
{
    // The flags are not touched here: the caller releases them through
    // applyToNotes() (the empty-canvas click does), and publishing no index at
    // all (clip mode) must not deselect the active project's notes.
    if (entries_.empty())
        return;
    entries_.clear();
    ++revision_;
}

bool DocumentNoteSelection::selectAll(const DocumentNoteIndex& index)
{
    std::vector<DocumentNoteSelectionEntry> next;
    next.reserve(static_cast<size_t>(index.totalNotes));
    for (const auto& ref : index.notes)
    {
        if (ref.note == nullptr || ref.note->isRest() || ref.note->isDeleted())
            continue;
        next.push_back(entryFrom(index, ref));
    }
    const bool changed = replace(std::move(next), projectsOf(index));
    if (changed)
        applyToNotes();
    return changed;
}

bool DocumentNoteSelection::setFromPairs(
    const std::vector<std::pair<Project*, Note*>>& pairs,
    const DocumentNoteIndex& index)
{
    std::set<NoteKey> wanted;
    for (const auto& pair : pairs)
        if (pair.first != nullptr && pair.second != nullptr)
            wanted.insert({pair.first, pair.second});

    std::vector<DocumentNoteSelectionEntry> next;
    next.reserve(wanted.size());
    // Walk the index, not the pairs: order is the document's, the primary
    // region is resolved and anything no window shows is left out.
    for (const auto& ref : index.notes)
    {
        if (ref.note == nullptr)
            continue;
        if (wanted.count({ref.project, ref.note}) == 0)
            continue;
        next.push_back(entryFrom(index, ref));
    }
    const bool changed = replace(std::move(next), projectsOf(index));
    if (changed)
        applyToNotes();
    return changed;
}

bool DocumentNoteSelection::toggle(Project* project, Note* note,
                                   const DocumentNoteIndex& index)
{
    // Adopt whatever a legacy path (a plain click, a split) did to the flags
    // first, so a toggle is always relative to what the user sees.
    refresh(&index);
    if (project == nullptr || note == nullptr)
        return false;

    const int at = findIndex(project, note);
    if (at >= 0)
    {
        entries_.erase(entries_.begin() + at);
        ++revision_;
        applyToNotes();
        return true;
    }

    for (const auto& ref : index.notes)
        if (ref.project == project && ref.note == note)
        {
            entries_.push_back(entryFrom(index, ref));
            ++revision_;
            applyToNotes();
            return true;
        }
    return false; // not shown by any window of the track: not selectable
}

bool DocumentNoteSelection::contains(const Project* project,
                                     const Note* note) const
{
    return findIndex(project, note) >= 0;
}

int DocumentNoteSelection::countForRegion(const juce::String& regionKey) const
{
    int count = 0;
    for (const auto& entry : entries_)
        if (entry.regionKey == regionKey)
            ++count;
    return count;
}

int DocumentNoteSelection::countForSource(const juce::String& sourceKey) const
{
    int count = 0;
    for (const auto& entry : entries_)
        if (entry.sourceKey == sourceKey)
            ++count;
    return count;
}

int DocumentNoteSelection::countForModification(
    const juce::String& modificationKey) const
{
    int count = 0;
    for (const auto& entry : entries_)
        if (entry.modificationKey == modificationKey)
            ++count;
    return count;
}

void DocumentNoteSelection::applyToNotes() const
{
    // Pointer identity is enough: every Note is one object of one project.
    std::set<const Note*> selected;
    for (const auto& entry : entries_)
        selected.insert(entry.note);
    for (auto* project : projects_)
        for (auto& note : project->getNotes())
            note.setSelected(!note.isRest() && !note.isDeleted() &&
                             selected.count(&note) > 0);
}
