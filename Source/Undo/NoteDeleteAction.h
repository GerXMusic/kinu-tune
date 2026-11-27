#pragma once

// M5 v2 / D-299: eliminar notas.
//
// La nota no se saca del vector: queda como lapida (deleted + rest) para que su
// bloque D-129 se re-renderice y el silencio sobreviva a guardar/reabrir. La UI
// la oculta por la misma via que un rest. La accion guarda tambien el estado de
// seleccion para que undo devuelva la nota seleccionada, como el gesto que la
// elimino.

#include "UndoableAction.h"
#include "../Models/DocumentNoteSelection.h"
#include "../Models/Project.h"

#include <algorithm>
#include <utility>
#include <vector>

class NoteDeleteAction : public UndoableAction
{
public:
    struct Entry
    {
        Note* note = nullptr;
        bool beforeDeleted = false;
        bool beforeRest = false;
        bool beforeSelected = false;
        bool afterDeleted = true;
        bool afterRest = true;
        bool afterSelected = false;
    };

    NoteDeleteAction(Project& project, std::vector<Entry> entries,
                     juce::String name = "Delete Notes")
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
            entry.note->setDeleted(toAfter ? entry.afterDeleted : entry.beforeDeleted);
            entry.note->setRest(toAfter ? entry.afterRest : entry.beforeRest);
            // N9 (D-349): undo gives the note back selected through the owner
            // of the selection, not by writing the flag here.
            DocumentNoteSelection::setNoteSelected(
                project, *entry.note,
                toAfter ? entry.afterSelected : entry.beforeSelected);
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
