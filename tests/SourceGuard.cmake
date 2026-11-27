# TuneX - Copyright (C) 2026 Keinoaza
# Part of TuneX, a modified version of PitchNet. Licensed under the GNU
# Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#
# Fails when code calls something that must only be called from one place.
# Each rule names the calls, the files allowed to make them, and the entry
# point everybody else must use instead.
#
#   cmake -DSOURCE_DIR=<repo>/Source -DRULE=<rule> -P SourceGuard.cmake
#
# pitch-writes:     a note's pitch and its Correct Pitch reference are written
#                   only through Source/Utils/NotePitch.h, so the reference
#                   rule cannot be forgotten by a new feature.
# note-selection:   Note::selected is derived from the document selection, so it
#                   is written only in Source/Models/DocumentNoteSelection.cpp
#                   (D-349): one owner, one writer.
# region-copies:    a track-mode preview borrows the live Project and copies no
#                   notes (D-350). Source/UI/RegionPreview.h may not hold a
#                   Project by value nor own one through a smart pointer, so the
#                   index, the drawing and the hit-test keep reading the same
#                   Note objects. N12 (D-354): the note painting in
#                   PianoRollComponent.cpp may not branch on region.active
#                   either; the active item and the others share one style.
# audition-output:  the engine's audition loop is sounded only through
#                   Source/UI/Main/AuditionOutput.h, so a new kind of audition
#                   takes the output over from the previous one.
# render-controllers: the plug-in's EditorControllers are created only through
#                   Source/Plugin/RenderControllers.h, so every one of them
#                   shares the instance's rendering-engine setting.
#
# The check reads one line at a time. A call through a member pointer, or one
# split across lines, is not caught; dev-rules.md asks not to write them.

if(NOT SOURCE_DIR OR NOT RULE)
    message(FATAL_ERROR "SourceGuard: pass -DSOURCE_DIR=<repo>/Source -DRULE=<rule>")
endif()

if("${RULE}" STREQUAL "pitch-writes")
    set(scan_dir "${SOURCE_DIR}")
    set(allowed "${SOURCE_DIR}/Models/Note.h" "${SOURCE_DIR}/Utils/NotePitch.h")
    set(pattern "(setMidiNote|setLastNonMacroMidiNote|setMidiNoteFromPitchCorrection)[ \t]*\\(")
    set(hint "Use NotePitch::applyEdit / restore / applyCorrection / setCorrectionSource / setSungPitch instead (see Source/Utils/NotePitch.h).")
elseif("${RULE}" STREQUAL "note-selection")
    # N9 (D-349): a note's selected flag is a derived view of the document
    # selection, never a second truth. The set lives in
    # Source/Models/DocumentNoteSelection.cpp (applyToNotes is its writer); every
    # other path goes through it: setNoteSelected, selectAll / setFromPairs /
    # toggle, Project::selectAllNotes / deselectAllNotes, and the drawing copies.
    set(scan_dir "${SOURCE_DIR}")
    set(allowed "${SOURCE_DIR}/Models/Note.h"
                "${SOURCE_DIR}/Models/DocumentNoteSelection.cpp")
    set(pattern "setSelected[ \t]*\\(")
    set(hint "Note::selected is derived from the document selection. Use DocumentNoteSelection::setNoteSelected / setFromPairs / toggle / selectAll, or Project::selectAllNotes / deselectAllNotes (see Source/Models/DocumentNoteSelection.h).")
elseif("${RULE}" STREQUAL "region-copies")
    # N10 (D-350): one source of truth. The preview that track mode draws from
    # borrows the modification's live Project; owning one (by value, unique_ptr
    # or shared_ptr) is how the second universe of notes came back before. This
    # rule looks at the one file that defines the preview, because it is about
    # what the preview is allowed to hold.
    set(preview_file "${SOURCE_DIR}/UI/RegionPreview.h")
    if(NOT EXISTS "${preview_file}")
        message(FATAL_ERROR "SourceGuard (region-copies): ${preview_file} not found")
    endif()
    file(STRINGS "${preview_file}" preview_lines)
    set(violations "")
    foreach(line IN LISTS preview_lines)
        string(REGEX REPLACE "//.*$" "" code "${line}")
        # An owning smart pointer to a Project, or a Project stored by value.
        if(code MATCHES "(unique_ptr|shared_ptr)[ \t]*<[ \t]*(const[ \t]+)?Project[ \t]*>" OR
           code MATCHES "(^|[^A-Za-z0-9_*>])Project[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]*[=;{}]")
            string(STRIP "${line}" shown)
            string(APPEND violations "  UI/RegionPreview.h: ${shown}\n")
        endif()
    endforeach()
    if(violations)
        message(FATAL_ERROR
            "SourceGuard (region-copies) found an owned Project in the region preview:\n"
            "${violations}"
            "MainViewRegionPreview must borrow the live Project (Project*), not own a copy; see Source/UI/RegionPreview.h and D-350.")
    endif()
    # N12 (D-354): one style for every item, so the note painting may not
    # branch on which item is active (that branch is how the active item and
    # the others drifted apart: grey gradient, own opacity, own wash). The
    # functions below paint the notes of the track; inside them, reading
    # `region.active` / `item.active` is a violation. The pinned clips of
    # another track ARE the documented exception - they are a dimmed reference,
    # not an item of this document - but even they ask RegionNoteStyle for
    # their values instead of hard-coding them.
    set(paint_file "${SOURCE_DIR}/UI/PianoRollComponent.cpp")
    if(NOT EXISTS "${paint_file}")
        message(FATAL_ERROR "SourceGuard (region-copies): ${paint_file} not found")
    endif()
    set(paint_functions drawInactiveRegionEdits drawNotes drawPinnedTrackNotes)
    file(STRINGS "${paint_file}" paint_lines)
    set(in_paint 0)
    set(current_function "")
    set(paint_violations "")
    foreach(line IN LISTS paint_lines)
        if(line MATCHES "^void PianoRollComponent::([A-Za-z0-9_]+)")
            set(current_function "${CMAKE_MATCH_1}")
            set(in_paint 0)
            foreach(fn IN LISTS paint_functions)
                if("${current_function}" STREQUAL "${fn}")
                    set(in_paint 1)
                endif()
            endforeach()
        elseif(line MATCHES "^}")
            set(in_paint 0)
        endif()
        if(in_paint)
            string(REGEX REPLACE "//.*$" "" code "${line}")
            if(code MATCHES "(region|item|preview)(->|\\.)active")
                string(STRIP "${line}" shown)
                string(APPEND paint_violations "  PianoRollComponent.cpp: ${shown}\n")
            endif()
        endif()
    endforeach()
    if(paint_violations)
        message(FATAL_ERROR
            "SourceGuard (region-copies) found item-dependent note painting:\n"
            "${paint_violations}"
            "Every item of the track is painted with trackRegionNoteStyle(); the paint path must not read region.active (see Source/UI/PianoRoll/RegionNoteStyle.h and D-354).")
    endif()
    # R5b (D-356): the guide ghosts belong to the document as well. The layer
    # may not know about Project at all: bindProject()/the "is this the active
    # item?" question is exactly what dropped 111 ghosts to 0 when the item
    # changed. host seconds + the offset published by the mapper is enough.
    set(ghost_file "${SOURCE_DIR}/Models/MidiGuideGhosts.h")
    if(NOT EXISTS "${ghost_file}")
        message(FATAL_ERROR "SourceGuard (region-copies): ${ghost_file} not found")
    endif()
    file(STRINGS "${ghost_file}" ghost_lines)
    set(ghost_violations "")
    foreach(line IN LISTS ghost_lines)
        string(REGEX REPLACE "//.*$" "" code "${line}")
        if(code MATCHES "bindProject" OR code MATCHES "(^|[^A-Za-z0-9_*>])Project([^A-Za-z0-9_]|$)")
            string(STRIP "${line}" shown)
            string(APPEND ghost_violations "  Models/MidiGuideGhosts.h: ${shown}\n")
        endif()
    endforeach()
    if(ghost_violations)
        message(FATAL_ERROR
            "SourceGuard (region-copies) found an item-bound guide ghost layer:\n"
            "${ghost_violations}"
            "The ghosts are the document's: store host seconds and let the painter apply the offset (see Source/Models/MidiGuideGhosts.h and D-356).")
    endif()
    message(STATUS "SourceGuard (region-copies): passed")
    return()
elseif("${RULE}" STREQUAL "midi-guides")
    # R11b (D-365): the sidecar key may only be derived from data that survives
    # closing and reopening the project. Measured in R11: the address of the
    # ARADocument and the audio source persistent IDs both changed on every
    # reopen, so the key was orphaned and the guide disappeared. The two files
    # that derive the key therefore may not name an address, a pointer, the
    # document object or a persistent ID - and the track key may not fall back
    # to the legacy "doc:" prefix.
    foreach(identity_file IN ITEMS
            "${SOURCE_DIR}/Models/TrackIdentity.cpp"
            "${SOURCE_DIR}/Models/MidiGuidePersistence.cpp")
        if(NOT EXISTS "${identity_file}")
            message(FATAL_ERROR "SourceGuard (midi-guides): ${identity_file} not found")
        endif()
        file(STRINGS "${identity_file}" identity_lines)
        set(identity_violations "")
        foreach(line IN LISTS identity_lines)
            string(REGEX REPLACE "\"([^\"\\\\]|\\\\.)*\"" "\"\"" code "${line}")
            string(REGEX REPLACE "//.*$" "" code "${code}")
            if(code MATCHES "reinterpret_cast|uintptr_t|getDocument[ \t]*\\(|getPersistentID[ \t]*\\(")
                string(STRIP "${line}" shown)
                get_filename_component(identity_name "${identity_file}" NAME)
                string(APPEND identity_violations "  ${identity_name}: ${shown}\n")
            endif()
        endforeach()
        if(identity_violations)
            message(FATAL_ERROR
                "SourceGuard (midi-guides) found a session identity in the guide key:\n"
                "${identity_violations}"
                "The key is the audio-source fingerprint plus the track name (Source/Models/TrackIdentity.h); addresses, pointers and persistent IDs change on reopen (see D-365).")
        endif()
    endforeach()

    # The legacy prefix is READ by the migration only; deriving a new key from
    # it again is the bug this phase fixed.
    file(STRINGS "${SOURCE_DIR}/Models/TrackIdentity.cpp" identity_lines)
    foreach(line IN LISTS identity_lines)
        if(line MATCHES "\"doc:")
            string(STRIP "${line}" shown)
            message(FATAL_ERROR
                "SourceGuard (midi-guides): the track key may not use the legacy 'doc:' prefix:\n"
                "  Models/TrackIdentity.cpp: ${shown}\n"
                "See D-365.")
        endif()
    endforeach()

    # R5b (D-356): the guide is the DOCUMENT's. Project::getMidiGuides is only
    # the compatibility cache (old JSON/host archive) and the one-time migration
    # source; reading or writing it anywhere else is how the guide became a
    # per-item thing again. The store and the JSON serializer are the only
    # files allowed to touch it.
    set(scan_dir "${SOURCE_DIR}")
    set(allowed "${SOURCE_DIR}/Models/Project.h"
                "${SOURCE_DIR}/Models/DocumentMidiGuides.cpp"
                "${SOURCE_DIR}/Models/ProjectSerializer.cpp")
    set(pattern "(get|set)MidiGuides[ \t]*\\(")
    set(hint "The guide belongs to the document. Use DocumentMidiGuideStore (see Source/Models/DocumentMidiGuides.h); the Project copy is only the compatibility cache.")
elseif("${RULE}" STREQUAL "audition-output")
    set(scan_dir "${SOURCE_DIR}")
    set(allowed "${SOURCE_DIR}/UI/Main/AuditionOutput.h"
                "${SOURCE_DIR}/Audio/AudioEngine.h"
                "${SOURCE_DIR}/Audio/AudioEngine.cpp")
    set(pattern "(beginAuditionLoop|endAuditionLoop)[ \t]*\\(")
    set(hint "Use AuditionOutput::playLoop / stopLoop instead (see Source/UI/Main/AuditionOutput.h).")
elseif("${RULE}" STREQUAL "render-controllers")
    set(scan_dir "${SOURCE_DIR}/Plugin")
    set(allowed "${SOURCE_DIR}/Plugin/RenderControllers.h")
    set(pattern "make_unique<EditorController>[ \t]*\\([ \t]*false")
    set(hint "Use makeSharedEngineController / PitchNetAudioProcessor::makeRenderController instead (see Source/Plugin/RenderControllers.h).")
else()
    message(FATAL_ERROR "SourceGuard: unknown rule '${RULE}'")
endif()

file(GLOB_RECURSE files "${scan_dir}/*.cpp" "${scan_dir}/*.h" "${scan_dir}/*.mm")
set(violations "")
foreach(file IN LISTS files)
    if(file IN_LIST allowed)
        continue()
    endif()
    file(STRINGS "${file}" lines REGEX "${pattern}")
    foreach(line IN LISTS lines)
        # Mentions in string literals and comments are fine.
        string(REGEX REPLACE "\"([^\"\\\\]|\\\\.)*\"" "\"\"" code "${line}")
        string(REGEX REPLACE "//.*$" "" code "${code}")
        if(code MATCHES "${pattern}")
            file(RELATIVE_PATH rel "${SOURCE_DIR}" "${file}")
            string(STRIP "${line}" shown)
            string(APPEND violations "  ${rel}: ${shown}\n")
        endif()
    endforeach()
endforeach()

if(violations)
    message(FATAL_ERROR "SourceGuard (${RULE}) found calls outside the allowed files:\n${violations}${hint}")
endif()
message(STATUS "SourceGuard (${RULE}): passed")
