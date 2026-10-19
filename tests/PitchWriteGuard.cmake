# Kinu Tune - Copyright (C) 2026 Keinoaza
# Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
# Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#
# Fails when code outside Note.h and NotePitch.h writes a note's pitch or its
# Correct Pitch reference directly. Every pitch write must go through
# NotePitch (Source/Utils/NotePitch.h) so the reference rule cannot be
# forgotten by a new feature.
#
#   cmake -DSOURCE_DIR=<repo>/Source -P PitchWriteGuard.cmake

if(NOT SOURCE_DIR)
    message(FATAL_ERROR "PitchWriteGuard: pass -DSOURCE_DIR=<repo>/Source")
endif()

set(allowed
    "${SOURCE_DIR}/Models/Note.h"
    "${SOURCE_DIR}/Utils/NotePitch.h")
set(raw_write "(setMidiNote|setLastNonMacroMidiNote|setMidiNoteFromPitchCorrection)[ \t]*\\(")

file(GLOB_RECURSE files "${SOURCE_DIR}/*.cpp" "${SOURCE_DIR}/*.h" "${SOURCE_DIR}/*.mm")
set(violations "")
foreach(file IN LISTS files)
    if(file IN_LIST allowed)
        continue()
    endif()
    file(STRINGS "${file}" lines REGEX "${raw_write}")
    foreach(line IN LISTS lines)
        # Mentions in comments are fine.
        string(REGEX REPLACE "//.*$" "" code "${line}")
        if(code MATCHES "${raw_write}")
            file(RELATIVE_PATH rel "${SOURCE_DIR}" "${file}")
            string(STRIP "${line}" shown)
            string(APPEND violations "  ${rel}: ${shown}\n")
        endif()
    endforeach()
endforeach()

if(violations)
    message(FATAL_ERROR
        "Direct pitch writes outside NotePitch.h:\n${violations}"
        "Use NotePitch::applyEdit / restore / applyCorrection / "
        "setCorrectionSource / setSungPitch instead (see Source/Utils/NotePitch.h).")
endif()
message(STATUS "PitchWriteGuard: no direct pitch writes outside NotePitch.h")
