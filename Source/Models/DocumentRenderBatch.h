// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.

#pragma once

#include "Project.h"

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

/**
 * N5 (D-345): the render requests of one batch gesture, one per item.
 *
 * D-247 says whoever marks the audio dirty has to request its render: a dirty
 * item whose request never leaves the editor is heard MUTE (or, since D-285,
 * with the audio it had before, which is just as wrong for an edit). A batch
 * touches several items, and the naive loop over notes or over windows asks the
 * backend once per note (dozens) or once per window (one take cut in three is
 * three windows of THE SAME Project). The engine renders whole blocks of ONE
 * project, so the unit of a request is the Project, not the note or the window.
 *
 * This queue makes that structural:
 *   - `add()` is idempotent per Project*: two items of one take, or two
 *     operations of the same gesture, leave ONE entry;
 *   - `flush()` sends exactly one request per affected item;
 *   - an item the caller cannot request is reported instead of being dropped in
 *     silence, which is the one way a batch could still leave an item mute.
 *
 * It stores nothing of the project: only the pointer to the item that has to be
 * rendered. The notes stay where they are (one source of truth).
 */
namespace documentRenderBatch
{
// One affected item. `active` is the item whose project is the editor's active
// one: it renders through the editor (markRenderDirty + the canvas path).
struct Request
{
    Project *project = nullptr;
    bool active = false;
};

// What one flush did. `requested` is one entry per item, in gesture order;
// `unreachable` are the items that were affected but had no path to a render.
struct Outcome
{
    std::vector<Project *> requested;
    std::vector<Project *> unreachable;

    bool complete() const { return unreachable.empty(); }
    int count() const { return static_cast<int>(requested.size()); }

    int countFor(const Project *project) const
    {
        return static_cast<int>(
            std::count(requested.begin(), requested.end(), project));
    }

    bool touched(const Project *project) const
    {
        return countFor(project) > 0;
    }
};

// The items of one gesture, deduplicated by Project*.
class Gesture
{
public:
    void add(Project *project, bool active = false)
    {
        if (project == nullptr)
            return;
        for (auto &entry : entries)
            if (entry.project == project)
            {
                entry.active = entry.active || active;
                return;
            }
        entries.push_back({project, active});
    }

    bool empty() const { return entries.empty(); }
    int size() const { return static_cast<int>(entries.size()); }

    bool contains(const Project *project) const
    {
        return std::any_of(entries.begin(), entries.end(),
                           [project](const Request &entry)
                           { return entry.project == project; });
    }

    int countFor(const Project *project) const
    {
        return static_cast<int>(
            std::count_if(entries.begin(), entries.end(),
                          [project](const Request &entry)
                          { return entry.project == project; }));
    }

    const std::vector<Request> &requests() const { return entries; }

    void clear() { entries.clear(); }

    // Empties the gesture and hands its items to the caller.
    std::vector<Request> take()
    {
        auto taken = std::move(entries);
        entries.clear();
        return taken;
    }

private:
    std::vector<Request> entries;
};

/**
 * Sends one request per item. `request` returns true when the item really got
 * its render asked for; false (or an empty function) means the caller has no
 * path for it and the item is reported as unreachable.
 */
inline Outcome flush(const std::vector<Request> &requests,
                     const std::function<bool(const Request &)> &request)
{
    Outcome outcome;
    for (const auto &entry : requests)
    {
        if (entry.project == nullptr)
            continue;
        if (outcome.touched(entry.project))
            continue; // defence in depth: one request per item, ever
        if (request && request(entry))
            outcome.requested.push_back(entry.project);
        else
            outcome.unreachable.push_back(entry.project);
    }
    return outcome;
}
} // namespace documentRenderBatch
