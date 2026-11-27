// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#include "DocumentMidiGuides.h"

#include "MidiGuideLive.h"
#include "MidiGuidePersistence.h"

#include <algorithm>

namespace
{
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
    //
    // R7 (D-360): the sidecar is read first. When it answers (even with an
    // empty list, which is what "the user removed them" looks like) it is the
    // truth and the legacy cache is NOT migrated: that is how a removed guide
    // used to come back from a stale Project copy.
    const bool hadDocument = trackKey.isNotEmpty();
    const auto documentProjects = observedProjects;
    trackKey = newTrackKey;
    entries.clear();
    importedProjects.clear();
    observedProjects.clear();
    guidesFromDisk = false;
    diskAuthoritative = false;
    markDirty();
    publish();

    if (!loadFromDisk())
    {
        bool adopted = false;
        for (const auto* project : documentProjects)
            if (project != nullptr)
                adopted = importFromProject(*project) || adopted;
        // First open of an older project: write the migrated guide so the next
        // open reads the disk and the legacy cache stops being needed.
        if (adopted)
            persistIfPossible();
    }
    publish();

    return hadDocument;
}

void DocumentMidiGuideStore::setDocumentKey(const juce::String& projectKey)
{
    if (projectKey == documentKey)
        return;
    documentKey = projectKey;

    // No document published yet (clip mode, or the host has not created the
    // audio sources): nothing is read or written under an unknown identity.
    if (documentKey.isEmpty() || trackKey.isEmpty())
        return;

    // R11f (D-370): a different document fingerprint is a different VIEW of the
    // same document, not another document. The sidecar has one path per track,
    // so this view reads the very file the previous view wrote: the store
    // ADOPTS it - what is in memory is kept and what the file holds replaces by
    // id - instead of clearing the list and looking for a file that never
    // existed (the second view change of R12 landed on 0/0 exactly because of
    // that clear + new path). An empty list here would also be written next
    // time, retiring the good sidecar to .bak.
    const bool loaded = loadFromDisk();
    if (!loaded)
        importFromObservedProjects();

    // A guide that arrived before the view had a key (bindTrack with no key, a
    // live sync, a Project migration) has not been written yet: materialise it
    // under the track's single path. persistIfPossible refuses an empty list
    // without disk authority, so this can never retire a good file.
    if (!diskAuthoritative && !entries.empty())
        persistIfPossible();

    publish();
}

bool DocumentMidiGuideStore::loadFromDisk()
{
    guidesFromDisk = false;
    diskAuthoritative = false;
    if (persistence == nullptr || documentKey.isEmpty() || trackKey.isEmpty())
        return false;

    const auto result = persistence->load(documentKey, trackKey);
    if (result.status == MidiGuideLoadResult::Status::rejected)
    {
        // A corrupt or foreign sidecar must not break the load: report it and
        // let the caller migrate from the Project cache as before R7.
        persistenceWarning = result.message;
        return false;
    }
    if (!result.usable())
        return false; // no file at all

    for (const auto& guide : result.guides)
        addOrReplace(guide, "document");
    guidesFromDisk = true; // authoritative, empty list included
    diskAuthoritative = true;
    markDirty();
    return true;
}

bool DocumentMidiGuideStore::importFromObservedProjects()
{
    bool adopted = false;
    for (const auto* project : observedProjects)
        if (project != nullptr)
            adopted = importFromProject(*project) || adopted;
    if (adopted)
        persistIfPossible(); // materialize the migration so the next open is disk
    return adopted;
}

bool DocumentMidiGuideStore::persistIfPossible()
{
    if (persistence == nullptr || documentKey.isEmpty() || trackKey.isEmpty())
        return false;
    // Never create an empty sidecar out of nothing: a document save with no
    // guide must not shadow a legacy cache that has not been adopted yet.
    //
    // R11f (D-370): and never replace a good file with an empty list that only
    // means "this view has nothing in memory" - that is what would retire the
    // user's sidecar to .bak. An empty list IS written when the sidecar is
    // already the authority (the user removed the last guide, removeAt), so a
    // removal still sticks.
    if (!diskAuthoritative && entries.empty())
        return false;

    std::vector<GuideSource> guides;
    guides.reserve(entries.size());
    for (const auto& entry : entries)
        guides.push_back(*entry.guide);

    juce::String error;
    if (!persistence->save(documentKey, trackKey, guides, &error))
    {
        if (error.isNotEmpty() && persistenceWarning.isEmpty())
            persistenceWarning = error;
        return false;
    }
    diskAuthoritative = true;
    return true;
}

bool DocumentMidiGuideStore::flushPersistence() { return persistIfPossible(); }

juce::String DocumentMidiGuideStore::takePersistenceWarning()
{
    auto warning = persistenceWarning;
    persistenceWarning.clear();
    return warning;
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

    // R7 (D-360): when the sidecar answered, it is the truth. A Project copy
    // is only the one-time migration source, and importing it next to a valid
    // sidecar is how a removed guide came back.
    if (guidesFromDisk)
        return false;

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

namespace
{
// Entries the live folder owns: the ones the sync has written (origin "live"),
// and the ones a sidecar round trip kept as origin "document" while still
// being a live snapshot (kind "reascript"/"companion"). A .mid import, a
// legacy cache migration or a document-only guide is never removed by a scan.
bool isLiveManagedEntry(const std::shared_ptr<const GuideSource>& guide,
                        const juce::String& origin)
{
    if (origin == "live")
        return true;
    if (origin != "document")
        return false;
    return guide->kind == "reascript" || guide->kind == "companion";
}
} // namespace

bool DocumentMidiGuideStore::syncLive(
    const std::vector<GuideSource>& liveGuides, bool authoritative)
{
    bool changed = false;
    std::vector<std::string> seen;
    seen.reserve(liveGuides.size());
    for (const auto& live : liveGuides)
    {
        // One guide per identity: two files with the same source_id (an old
        // name left behind by an interrupted write) are one guide, and the
        // scanner already picked the deterministic winner.
        if (std::find(seen.begin(), seen.end(), live.id) != seen.end())
            continue;
        seen.push_back(live.id);

        const auto* existing = findById(live.id);
        if (existing == nullptr || midiGuideLive::contentHash(*existing) !=
                                       midiGuideLive::contentHash(live))
        {
            addOrReplace(live, "live");
            changed = true;
        }
    }

    if (authoritative)
    {
        for (int i = size() - 1; i >= 0; --i)
        {
            const auto& entry = entries[static_cast<std::size_t>(i)];
            if (!isLiveManagedEntry(entry.guide, entry.origin))
                continue;
            if (std::find(seen.begin(), seen.end(), entry.guide->id) !=
                seen.end())
                continue;
            // Its file is gone: the guide goes with it (D-363).
            entries.erase(entries.begin() + i);
            markDirty();
            changed = true;
        }
    }

    if (changed)
        persistIfPossible();
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
    const auto added = addOrReplace(std::move(guide), origin);
    persistIfPossible();
    return added.first;
}

bool DocumentMidiGuideStore::removeAt(int index)
{
    if (index < 0 || index >= size())
        return false;
    entries.erase(entries.begin() + index);
    markDirty();
    // Removing the last guide is state too: diskAuthoritative makes this write
    // an empty sidecar instead of silently keeping the old one.
    persistIfPossible();
    return true;
}

void DocumentMidiGuideStore::clear()
{
    // Deliberately does NOT persist: this is also the document-gone path
    // (releaseProject when the last Project dies, i.e. the host closing or
    // unloading the project). Writing here would erase the user's guide the
    // moment the project was closed.
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
