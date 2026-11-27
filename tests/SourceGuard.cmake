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
#                   Note objects.
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
    message(STATUS "SourceGuard (region-copies): passed")
    return()
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
