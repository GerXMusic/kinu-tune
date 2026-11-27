// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#include "DocumentMidiGuides.h"

#include <algorithm>

namespace
{
// Equality of the two note lists that decides whether a live guide has to
// replace the one in the store. Cheap and exact: the importer writes the same
// values on every poll, so only a real change replaces.
bool sameNotes(const GuideSource& a, const GuideSource& b)
{
    if (a.notes.size() != b.notes.size())
        return false;
    for (std::size_t i = 0; i < a.notes.size(); ++i)
    {
        const auto& left = a.notes[i];
        const auto& right = b.notes[i];
        if (left.startQuarter != right.startQuarter ||
            left.endQuarter != right.endQuarter || left.pitch != right.pitch ||
            left.velocity != right.velocity)
            return false;
    }
    return true;
}

// The compat cache only needs to know whether the Project already carries the
// document's guides: writing identical values would mark the Project modified
// for nothing.
bool cacheMatches(const std::vector<GuideSource>& cache,
                  const std::vector<GuideSource>& wanted)
{
    if (cache.size() != wanted.size())
        return false;
    for (std::size_t i = 0; i < cache.size(); ++i)
    {
        if (cache[i].id != wanted[i].id || cache[i].label != wanted[i].label ||
            cache[i].kind != wanted[i].kind ||
            cache[i].notes.size() != wanted[i].notes.size())
            return false;
    }
    return true;
}
} // namespace

int DocumentMidiGuides::indexOf(const std::string& guideId) const
{
    for (std::size_t i = 0; i < guides.size(); ++i)
        if (guides[i].id == juce::String(guideId))
            return static_cast<int>(i);
    return -1;
}

const GuideSource* DocumentMidiGuides::guideAt(int index) const
{
    if (index < 0 || index >= guideCount())
        return nullptr;
    return guides[static_cast<std::size_t>(index)].guide.get();
}

bool DocumentMidiGuideStore::bindTrack(const juce::String& newTrackKey)
{
    if (newTrackKey.isEmpty())
        return false; // no document published (clip mode): keep the track's guides
    if (newTrackKey == trackKey)
        return false;

    // R5d (D-358): a different key is not automatically "a different track".
    // The key is stable now (Models/TrackIdentity.h), so the same track keeps
    // it across a RegionSequence recreation; when it really changed, the
    // previous list must not follow (another track is another history). The
    // document's Projects DO follow: they are the only place a cache can come
    // from, so an unknown key is imported from them instead of left empty.
    const bool hadDocument = trackKey.isNotEmpty();
    const auto documentProjects = observedProjects;
    trackKey = newTrackKey;
    entries.clear();
    importedProjects.clear();
    observedProjects.clear();
    markDirty();
    publish();

    for (const auto* project : documentProjects)
        if (project != nullptr)
            importFromProject(*project);
    publish();

    return hadDocument;
}

void DocumentMidiGuideStore::observeProjects(std::vector<const Project*> projects)
{
    std::vector<const Project*> unique;
    for (const auto* project : projects)
        if (project != nullptr &&
            std::find(unique.begin(), unique.end(), project) == unique.end())
            unique.push_back(project);
    if (unique.empty())
        return; // a publication with no regions is not "every Project died"
    observedProjects = std::move(unique);
}

bool DocumentMidiGuideStore::releaseProject(const Project* project)
{
    if (project == nullptr)
        return false;

    observedProjects.erase(
        std::remove(observedProjects.begin(), observedProjects.end(), project),
        observedProjects.end());
    importedProjects.erase(
        std::remove(importedProjects.begin(), importedProjects.end(), project),
        importedProjects.end());

    // Item changes release nothing, so the guides survive them (the R5 bug was
    // exactly the opposite). Only when the last Project of the document is
    // gone is the document gone: the store releases the guides it adopted.
    if (!observedProjects.empty() || entries.empty())
        return false;

    clear();
    return true;
}

bool DocumentMidiGuideStore::importFromProject(const Project& project)
{
    if (std::find(importedProjects.begin(), importedProjects.end(), &project) !=
        importedProjects.end())
        return false;
    importedProjects.push_back(&project);

    bool adopted = false;
    for (const auto& legacy : project.getMidiGuides())
    {
        // The store is the truth: a guide already known by id (a live import of
        // the same source) is not duplicated by the legacy copy.
        if (findById(legacy.id) != nullptr)
            continue;
        addOrReplace(legacy, "legacy");
        adopted = true;
    }
    return adopted;
}

bool DocumentMidiGuideStore::syncLive(
    const std::vector<GuideSource>& liveGuides)
{
    bool changed = false;
    for (const auto& live : liveGuides)
    {
        const auto* existing = findById(live.id);
        if (existing == nullptr)
        {
            addOrReplace(live, "live");
            changed = true;
        }
        else if (!sameNotes(*existing, live))
        {
            addOrReplace(live, "live");
            changed = true;
        }
    }
    return changed;
}

std::pair<int, bool> DocumentMidiGuideStore::addOrReplace(
    GuideSource guide, const juce::String& origin)
{
    if (guide.id.empty())
        guide.id = "guide:" + std::to_string(entries.size());

    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        if (entries[i].guide->id == guide.id)
        {
            auto stored = std::make_shared<GuideSource>(std::move(guide));
            entries[i].guide = std::move(stored);
            entries[i].origin = origin;
            markDirty();
            return {static_cast<int>(i), true};
        }
    }

    auto stored = std::make_shared<GuideSource>(std::move(guide));
    entries.push_back(Entry{std::move(stored), origin});
    markDirty();
    return {static_cast<int>(entries.size()) - 1, true};
}

int DocumentMidiGuideStore::upsert(GuideSource guide,
                                   const juce::String& origin)
{
    return addOrReplace(std::move(guide), origin).first;
}

bool DocumentMidiGuideStore::removeAt(int index)
{
    if (index < 0 || index >= size())
        return false;
    entries.erase(entries.begin() + index);
    markDirty();
    return true;
}

void DocumentMidiGuideStore::clear()
{
    entries.clear();
    observedProjects.clear();
    importedProjects.clear();
    markDirty();
    publish();
}

bool DocumentMidiGuideStore::writeCompatCache(Project& project) const
{
    std::vector<GuideSource> wanted;
    wanted.reserve(entries.size());
    for (const auto& entry : entries)
        wanted.push_back(*entry.guide);

    if (cacheMatches(project.getMidiGuides(), wanted))
        return false;

    project.setMidiGuides(std::move(wanted));
    return true;
}

bool DocumentMidiGuideStore::writeCompatCache(
    std::vector<Project*>& projects) const
{
    bool changed = false;
    std::vector<Project*> written;
    for (auto* project : projects)
    {
        if (project == nullptr ||
            std::find(written.begin(), written.end(), project) != written.end())
            continue;
        written.push_back(project);
        changed = writeCompatCache(*project) || changed;
    }
    return changed;
}

const DocumentMidiGuidesPtr& DocumentMidiGuideStore::publish()
{
    if (published != nullptr && !dirty)
        return published;

    auto next = std::make_shared<DocumentMidiGuides>();
    next->trackKey = trackKey;
    next->guides.reserve(entries.size());
    for (const auto& entry : entries)
    {
        DocumentMidiGuideRef ref;
        ref.guide = entry.guide;
        ref.id = juce::String(entry.guide->id);
        ref.label = juce::String(entry.guide->label);
        ref.notes = static_cast<int>(entry.guide->notes.size());
        ref.origin = entry.origin;
        next->totalNotes += ref.notes;
        next->guides.push_back(std::move(ref));
    }
    published = std::move(next);
    dirty = false;
    return published;
}

const GuideSource* DocumentMidiGuideStore::at(int index) const
{
    if (index < 0 || index >= size())
        return nullptr;
    return entries[static_cast<std::size_t>(index)].guide.get();
}

const GuideSource* DocumentMidiGuideStore::findById(
    const std::string& guideId) const
{
    for (const auto& entry : entries)
        if (entry.guide->id == guideId)
            return entry.guide.get();
    return nullptr;
}
