#pragma once

#include "../JuceHeader.h"

/**
 * Centralized command IDs for PitchNet.
 *
 * IDs start at 0x2000 to avoid conflicts with JUCE's StandardApplicationCommandIDs.
 * JUCE reserves 0x0001-0x0FFF for internal use.
 *
 * D-279: the editing commands and their default keys follow Melodyne 5. The
 * tool order is the toolbox order, so the F1..F6 row maps one to one.
 */
namespace CommandIDs
{
    enum
    {
        // File Menu Commands (0x2000-0x200F)
        openFile            = 0x2001,
        saveProject         = 0x2002,
        saveProjectAs       = 0x2006,
        exportAudio         = 0x2003,
        exportMidi          = 0x2004,
        quit                = 0x2005,

        // Edit Menu Commands (0x2010-0x201F)
        undo                = 0x2010,
        redo                = 0x2011,
        selectAll           = 0x2012,

        // View Menu Commands (0x2020-0x202F)
        showDeltaPitch      = 0x2020,
        showBasePitch       = 0x2021,
        showSettings        = 0x2022,
        showAbout           = 0x2023,
        zoomFull            = 0x2024,

        // Transport Commands (0x2030-0x203F)
        playPause           = 0x2030,
        stop                = 0x2031,
        goToStart           = 0x2032,
        goToEnd             = 0x2033,

        // Editing-tool commands, Melodyne order (F1..F6).
        activateMainTool        = 0x2041,
        activatePitchTool       = 0x2042,
        activateFormantTool     = 0x2043,
        activateAmplitudeTool   = 0x2044,
        activateTimingTool      = 0x2045,
        activateSeparationTool  = 0x2046,

        // Melodyne macros (0x2060-0x206F).
        correctPitch        = 0x2060,
        quantizeTime        = 0x2061,
        leveling            = 0x2062,

        // Note commands (0x2070-0x207F).
        cutNotes            = 0x2070,
        copyNotes           = 0x2071,
        pasteNotes          = 0x2072,
        deleteNotes         = 0x2073,
        muteNotes           = 0x2074,
        unmuteNotes         = 0x2075,
        mergeNotes          = 0x2076,

        // Melodyne's Reset Individual Edits submenu.
        resetPitch          = 0x2080,
        resetFormant        = 0x2081,
        resetAmplitude      = 0x2082,
        resetTiming         = 0x2083,
        resetAllEdits       = 0x2084,

        // Kept for source compatibility while the dead menu entry is removed.
        fourierFilter       = 0x2050
    };
}
