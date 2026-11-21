#pragma once

// M5 v1 / D-245: silenciar o reactivar notas.
//
// Cambia solo el flag `muted` de las notas; la ganancia elegida por el usuario
// (volumeDb) se conserva. El re-render pasa por el commit por bloque (D-239) y
// la ganancia 0 la aplica NoteGainCurve con su fade, asi que el corte no clica.

#include "UndoableAction.h"
#include "../Models/Project.h"
#include "../Utils/PitchCurveProcessor.h"

#include <utility>
#include <vector>

class NoteMuteAction : public UndoableAction
{
public:
    struct Entry
    {
        Note* note = nullptr;
        bool before = false;
        bool after = false;
    };

    NoteMuteAction(Project& project, std::vector<Entry> entries,
                   juce::String name = "Mute Notes")
        : project(project), entries(std::move(entries)), name(std::move(name)) {}

    void undo() override { apply(/*toAfter=*/false); }
    void redo() override { apply(/*toAfter=*/true); }
    bool requiresAudioResynthesis() const override { return true; }
    juce::String getName() const override { return name; }

private:
    void apply(bool toAfter)
    {
        int dirtyStart = entries.empty() ? 0 : entries.front().note->getStartFrame();
        int dirtyEnd = dirtyStart;
        for (auto& entry : entries)
        {
            if (entry.note == nullptr)
                continue;
            entry.note->setMuted(toAfter ? entry.after : entry.before);
            entry.note->markDirty();
            entry.note->markSynthDirty();
            dirtyStart = std::min(dirtyStart, entry.note->getStartFrame());
            dirtyEnd = std::max(dirtyEnd, entry.note->getEndFrame());
        }
        if (dirtyEnd > dirtyStart)
        {
            const int frameCount = project.getAudioData().getNumFrames();
            project.setF0DirtyRange(std::max(0, dirtyStart - 24),
                                    std::min(frameCount, dirtyEnd + 24));
        }
        project.setModified(true);
    }

    Project& project;
    std::vector<Entry> entries;
    juce::String name;
};
