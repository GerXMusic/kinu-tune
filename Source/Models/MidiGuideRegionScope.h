// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#pragma once

#include "MidiGuide.h"
#include "Project.h"

#include <vector>

/**
 * Region scope of a MIDI guide (R1 of the guide-integration fix, D-313).
 *
 * A guide belongs to the whole track, but "Apply" only edits the active item:
 * the project this instance holds. The active item's source window is the ARA
 * playback region stored in AudioData::playbackRegionRanges; the host window is
 * that same interval on the timeline (offset = projectToTimeline(0)).
 *
 * Axes, kept explicit so nothing is compared across axes by accident:
 *   guide slot  : timeline seconds (quarters -> HostTempoMap)
 *   audio note  : project seconds (frames -> seconds); timeline = project + offset
 */
struct MidiGuideScopeWindows
{
    double sourceStartSeconds = 0.0;
    double sourceEndSeconds = 0.0;
    double offsetSeconds = 0.0; // project seconds -> timeline seconds
    bool valid = false;

    double hostStartSeconds() const { return sourceStartSeconds + offsetSeconds; }
    double hostEndSeconds() const { return sourceEndSeconds + offsetSeconds; }
};

/** Active-item windows, or an invalid set (no filtering) outside ARA. */
MidiGuideScopeWindows midiGuideScopeWindowsFor(const Project& project,
                                               double offsetSeconds);

/** Read-only counters, the same ones the local channel publishes. */
struct MidiGuideScopeReport
{
    MidiGuideScopeWindows windows;
    int guideNotes = 0;          // every guide note the guide source carries
    int guideNotesInRegion = 0;  // slots overlapping the host window
    int matchedInRegion = 0;     // matches once both windows are applied
    int matchedOutsideRegion = 0; // matches that only exist without the windows
    int changesInRegion = 0;     // in-region matches that really move the pitch
    double guideHostStartSeconds = 0.0;
    double guideHostEndSeconds = 0.0;
    bool hasGuideSpan = false;
};

/** Everything Apply needs, computed once so the debug channel sees the same. */
struct MidiGuidePlannedChange
{
    int noteIndex = -1;     // index into Project::getNotes()
    double targetPitch = 60.0;
};

struct MidiGuideScopedPlan
{
    MidiGuideScopeReport report;
    std::vector<MidiGuideMatcher::GuideSlot> slots; // in region, timeline axis
    std::vector<MidiGuideMatcher::AudioNote> audio; // in region, timeline axis
    std::vector<MidiGuidePlannedChange> changes;    // one per matched note
};

/**
 * Maps the guide to the timeline, keeps only what overlaps the active item and
 * matches. Pure: the project is read, never written (Apply does the writing).
 * With invalid windows nothing is filtered, which keeps standalone behaviour.
 */
MidiGuideScopedPlan
planMidiGuideScope(const Project& project, const GuideSource& guide,
                   const HostTempoMap& tempo,
                   const MidiGuideScopeWindows& windows, int sampleRate,
                   int hopSize);
