// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#pragma once

#include "MidiGuide.h"
#include "Project.h"

#include <memory>
#include <string>
#include <vector>

class MidiGuidePersistence;

/**
 * Document-level MIDI guides (R5b of the guide-integration fix, D-356).
 *
 * A guide covers the whole track (SCOPE=A), so it cannot live in a Project:
 * a Project is one modification/region, and selecting another item used to
 * leave the plug-in with no guide at all (measured in R5: item 4 -> 111
 * ghosts, items 1 and 2 -> 0). This is the same move the notes made in N1
 * (D-340): one document-level store, one immutable published snapshot.
 *
 * The snapshot POINTS at the canonical GuideSource objects the store owns. It
 * copies no note and no vector: two publications with no change return the
 * SAME shared_ptr, and every reference in it is the very object the store
 * holds (identity, not equality). The store is the owner and the only writer;
 * nothing here knows about the active item.
 *
 * The per-modification copy (Project::getMidiGuides, JSON "midiGuides") stays
 * only as a compatibility cache: it is what older builds and the host archive
 * read, and it is the one-time migration source. It is written from here and
 * never read by the UI (SourceGuard_midi-guides enforces it). Persisting the
 * guide as state of the document is R7 (D-360): the store also writes a sidecar
 * through MidiGuidePersistence while the host archive stays unfixed (D-346).
 * R11f (D-370) pins one sidecar per track name: the document view no longer
 * names the file, so a view change adopts the track's guide instead of looking
 * for a second file. Identity/sync is R10.
 *
 * Axes: the guide is in quarter notes (MidiGuide.h); it becomes host seconds
 * through HostTempoMap, and the canvas turns host into project seconds with the
 * offset it publishes (the same rule as frameForRegion, D-350).
 */
struct DocumentMidiGuideRef
{
    // Canonical guide the store owns; never a copy. Shared so a snapshot stays
    // valid even when a later publication replaces the store's entry.
    std::shared_ptr<const GuideSource> guide;
    juce::String id;
    juce::String label;
    int notes = 0;
    juce::String origin; // "live" | "file" | "legacy"
};

struct DocumentMidiGuides
{
    juce::String trackKey; // the document this snapshot belongs to
    std::vector<DocumentMidiGuideRef> guides;
    int totalNotes = 0;
    int guideCount() const { return static_cast<int>(guides.size()); }
    bool empty() const { return guides.empty(); }
    int indexOf(const std::string& guideId) const;
    const GuideSource* guideAt(int index) const;
};

using DocumentMidiGuidesPtr = std::shared_ptr<const DocumentMidiGuides>;

/**
 * The only owner and writer of the document's guides.
 *
 * Ownership (R5d, D-358): the store belongs to the DOCUMENT controller
 * (PitchNetDocumentController), not to the editor. The editor only borrows it
 * while it is open; closing and reopening the window in the same process keeps
 * the guides, because the store outlives the view. A new document controller is
 * a new document and starts empty.
 *
 * Lifecycle:
 *  - observeProjects() tells the store which Projects the document has (the
 *    plug-in publishes them with the note index). A Project that dies is
 *    released with releaseProject(); when the LAST observed Project dies the
 *    document is gone and the store empties, so nothing refers to a dead
 *    Project. Item changes release nothing, which is exactly the R5 fix.
 *  - bindTrack() scopes the store to one ARA track. The key is STABLE (see
 *    Models/TrackIdentity.h): the same track keeps the same key when the host
 *    recreates its region sequence, so the store is not emptied. An unknown
 *    key adopts the guides the document's Project caches still carry, instead
 *    of leaving the list empty; an empty result is only valid when there is no
 *    guide anywhere.
 *  - importFromProject() adopts a Project's legacy "midiGuides" once.
 */
class DocumentMidiGuideStore
{
public:
    DocumentMidiGuideStore() = default;
    ~DocumentMidiGuideStore() = default;

    DocumentMidiGuideStore(const DocumentMidiGuideStore&) = delete;
    DocumentMidiGuideStore& operator=(const DocumentMidiGuideStore&) = delete;

    // True when the track key changed (another track/document was bound).
    // An empty key means "no document published" and keeps what is there (clip
    // mode must not wipe the track's guides). The same key is a no-op, so a
    // recreated RegionSequence with a stable key does not lose the guides. An
    // unknown key does not keep the previous track's list, but it does import
    // whatever the document's Project caches carry: the store is only empty
    // when there is no guide anywhere (R5d, D-358).
    bool bindTrack(const juce::String& trackKey);

    // The Projects of the current document, deduplicated. Empty is ignored: a
    // publication with no regions is not "every Project died".
    void observeProjects(std::vector<const Project*> projects);

    // Drops the Project from the document. When it was the last one observed,
    // the document is gone: the guides are released and the snapshot rebuilt
    // empty. Returns true when the store was emptied.
    bool releaseProject(const Project* project);

    // One-time migration of the per-modification "midiGuides" copy. A guide
    // already known by id is not duplicated (the store is the truth); the new
    // ones keep their own id. A Project is imported at most once, so removing a
    // guide does not resurrect it from the stale cache. Returns true if it
    // adopted at least one guide.
    bool importFromProject(const Project& project);

    // Live-file merge (syncLiveMidiGuides, R10/D-363): same id replaces when
    // the CONTENT HASH differs (never the note count), a new id is appended.
    // When `authoritative` is true the scan saw the whole live folder, so a
    // live guide whose file disappeared is removed; false (folder missing)
    // keeps what the sidecar loaded. Never touches the UI.
    bool syncLive(const std::vector<GuideSource>& liveGuides,
                  bool authoritative = false);

    // A file import: replaces the guide with that id or appends it. Returns its
    // index, -1 if the guide is empty and had nothing to add.
    int upsert(GuideSource guide, const juce::String& origin = "file");

    bool removeAt(int index);
    void clear();

    // --- Disk persistence (R7 D-360, R11f D-370) --------------------------
    // One sidecar per TRACK KEY, and nothing else names the file (D-370). The
    // store owns the guides, so it is also the only writer: every user mutation
    // persists, and no path outside this class writes the file.
    //
    // A document fingerprint may arrive after bindTrack (sources are created
    // later than the editor in some hosts), which is why setDocumentKey()
    // retries the load itself; it is recorded in the file but is NOT part of
    // its path. A new fingerprint is a new VIEW of the same document, so
    // setDocumentKey ADOPTS the track's file instead of clearing the list: the
    // in-memory guides stay and the file's guides replace by id. An empty key
    // means "no document published", and then nothing is read or written (clip
    // mode must not invent a file).
    void setPersistence(MidiGuidePersistence* persistenceIn) {
        persistence = persistenceIn;
    }
    void setDocumentKey(const juce::String& projectKey);
    const juce::String& getDocumentKey() const { return documentKey; }

    // Writes the guides now. Called on host save so a save is also a flush.
    // Returns false when there is nothing writable (no document key, no track
    // key) or the write failed.
    bool flushPersistence();

    // Rejected or unwritable sidecar: the UI consumes this once and shows it.
    // Empty when the last load/save was fine.
    juce::String takePersistenceWarning();
    bool isFromDisk() const { return guidesFromDisk; }

    // Compatibility cache: writes the document's guides into one live Project
    // so the existing archive/JSON keeps carrying them (R7 owns real
    // persistence). Only called from here, never the read path.
    bool writeCompatCache(Project& project) const;
    bool writeCompatCache(std::vector<Project*>& projects) const;

    // Rebuilds the immutable snapshot when something changed; otherwise returns
    // the same publication. The reference is stable until the next mutation.
    const DocumentMidiGuidesPtr& publish();
    const DocumentMidiGuidesPtr& snapshot() const { return published; }

    int size() const { return static_cast<int>(entries.size()); }
    bool empty() const { return entries.empty(); }
    const GuideSource* at(int index) const;
    const GuideSource* findById(const std::string& guideId) const;
    int countObservedProjects() const {
        return static_cast<int>(observedProjects.size());
    }
    const juce::String& getTrackKey() const { return trackKey; }

private:
    struct Entry
    {
        std::shared_ptr<const GuideSource> guide;
        juce::String origin;
    };

    // Appends or replaces by id; returns the index and whether it changed.
    std::pair<int, bool> addOrReplace(GuideSource guide, const juce::String& origin);
    void markDirty() { dirty = true; }

    // Disk helpers (R7): validate/adopt the sidecar, then migrate the legacy
    // Project cache only when there is no usable sidecar. persistIfPossible()
    // never creates an empty file out of nothing, so a headless project save
    // cannot shadow a cache that was never adopted.
    bool loadFromDisk();
    bool importFromObservedProjects();
    bool persistIfPossible();

    std::vector<Entry> entries;
    std::vector<const Project*> observedProjects;
    std::vector<const Project*> importedProjects;
    juce::String trackKey;
    DocumentMidiGuidesPtr published;
    bool dirty = true;

    MidiGuidePersistence* persistence = nullptr;
    juce::String documentKey;
    // The current entries came from the sidecar, so legacy migration must not
    // resurrect a guide the user removed there.
    bool guidesFromDisk = false;
    // A sidecar exists for (documentKey, trackKey): removing the last guide
    // must be written as an empty file, not skipped.
    bool diskAuthoritative = false;
    juce::String persistenceWarning;
};
