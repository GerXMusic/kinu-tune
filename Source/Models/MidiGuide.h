// TuneX - Copyright (C) 2026 Keinoaza
// Part of TuneX, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#pragma once

#include "../Audio/Engine/HostTempoMap.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

/**
 * MIDI guide: a song-level pitch reference imported from another track
 * (plan/midi-guia-afinacion-hoja-de-ruta.md, D-311).
 *
 * The guide is stored in quarter notes, not seconds, so it follows the host
 * tempo map when that changes; it is materialized in the project state and the
 * render never reads it live (deterministic bounces). Nothing here touches the
 * engine: a guide only provides a target pitch.
 *
 * Coordinates:
 *   guide quarter --(HostTempoMap)--> timeline seconds
 *   timeline seconds --(-timelineDisplayOffset)--> project seconds (the axis
 *   the notes and the waveform are drawn in; see CoordinateMapper)
 */
struct GuideNote
{
    double startQuarter = 0.0;
    double endQuarter = 0.0;
    float pitch = 60.0f;    // MIDI number; fractional allowed (future bend)
    float velocity = 0.0f;  // 0..1, informational only
};

struct GuideSource
{
    std::string id;         // stable: track GUID + item position, file hash…
    std::string label;
    std::string kind;       // "file" | "reascript" | "companion"
    std::string importedAt; // free-form UTC string, informational

    std::vector<GuideNote> notes;

    bool isEmpty() const { return notes.empty(); }

    // Sorted by start, then pitch; drops exact duplicates (same rounded start
    // and pitch, keeping the longest end) so a host that repeats events cannot
    // grow the guide without bound.
    void normalise()
    {
        std::sort(notes.begin(), notes.end(),
                  [](const GuideNote& a, const GuideNote& b) {
                      if (a.startQuarter != b.startQuarter)
                          return a.startQuarter < b.startQuarter;
                      return a.pitch < b.pitch;
                  });

        std::vector<GuideNote> deduped;
        deduped.reserve(notes.size());
        for (const auto& note : notes)
        {
            if (!deduped.empty())
            {
                auto& previous = deduped.back();
                const double sameStart =
                    std::abs(previous.startQuarter - note.startQuarter);
                if (sameStart < 1.0e-4 &&
                    std::abs(previous.pitch - note.pitch) < 1.0e-4)
                {
                    previous.endQuarter =
                        std::max(previous.endQuarter, note.endQuarter);
                    previous.velocity = std::max(previous.velocity, note.velocity);
                    continue;
                }
            }
            deduped.push_back(note);
        }
        notes.swap(deduped);
    }
};

/**
 * Places guide quarters on the project axis. Pure: the caller supplies the
 * tempo map, the region's timeline offset and the frame grid.
 */
struct MidiGuideMapping
{
    const HostTempoMap* tempoMap = nullptr;
    double timelineDisplayOffsetSeconds = 0.0; // region start on the timeline
    int sampleRate = 44100;
    int hopSize = 512;

    double timelineSecondsForQuarter(double quarter) const
    {
        return tempoMap != nullptr ? tempoMap->timeForQuarter(quarter)
                                   : quarter * 0.5;
    }

    double projectSecondsForQuarter(double quarter) const
    {
        return timelineSecondsForQuarter(quarter) - timelineDisplayOffsetSeconds;
    }

    // Frame of the analysis grid (the notes' unit). Rounded, never clamped:
    // out-of-range notes are dropped by the caller, not piled up at 0.
    long long frameForProjectSeconds(double projectSeconds) const
    {
        if (sampleRate <= 0 || hopSize <= 0)
            return 0;
        const double frames = projectSeconds * sampleRate / hopSize;
        return static_cast<long long>(std::llround(frames));
    }

    long long frameForQuarter(double quarter) const
    {
        return frameForProjectSeconds(projectSecondsForQuarter(quarter));
    }
};

/**
 * Deterministic one-to-one matching between audio notes and guide notes.
 *
 * Rules (plan §6.1): temporal overlap > 0, |pitch distance| <= maxPitchDistance,
 * score = overlap / min(durations) - 0.05 * |pitch distance|, accept when
 * score >= minScore, and never reuse a guide note. Ties break by |Δpitch| and
 * then by earlier guide start, so two runs always produce the same list.
 */
class MidiGuideMatcher
{
public:
    struct AudioNote
    {
        int index = 0;
        double startSeconds = 0.0;
        double endSeconds = 0.0;
        double centerMidi = 60.0;
    };

    // A guide note already mapped to the timeline, so the matcher never has to
    // know about quarters, tempo maps or regions.
    struct GuideSlot
    {
        int guideIndex = 0;
        double startSeconds = 0.0;
        double endSeconds = 0.0;
        double pitch = 60.0;
    };

    struct Match
    {
        int audioIndex = -1;
        int guideIndex = -1;
        double score = 0.0;
    };

    static std::vector<Match>
    match(const std::vector<AudioNote>& audioNotes,
          const std::vector<GuideSlot>& guideSlots,
          double maxPitchDistance = 12.0, double minScore = 0.5);
};
