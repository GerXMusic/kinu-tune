# Kinu Tune - Copyright (C) 2026 Keinoaza
# Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
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
# audition-output:  the engine's audition loop is sounded only through
#                   Source/UI/Main/AuditionOutput.h, so a new kind of audition
#                   takes the output over from the previous one.
#
# The check reads one line at a time. A call through a member pointer, or one
# split across lines, is not caught; dev-rules.md asks not to write them.

if(NOT SOURCE_DIR OR NOT RULE)
    message(FATAL_ERROR "SourceGuard: pass -DSOURCE_DIR=<repo>/Source -DRULE=<rule>")
endif()

if(RULE STREQUAL "pitch-writes")
    set(scan_dir "${SOURCE_DIR}")
    set(allowed "${SOURCE_DIR}/Models/Note.h" "${SOURCE_DIR}/Utils/NotePitch.h")
    set(pattern "(setMidiNote|setLastNonMacroMidiNote|setMidiNoteFromPitchCorrection)[ \t]*\\(")
    set(hint "Use NotePitch::applyEdit / restore / applyCorrection / setCorrectionSource / setSungPitch instead (see Source/Utils/NotePitch.h).")
elseif(RULE STREQUAL "audition-output")
    set(scan_dir "${SOURCE_DIR}")
    set(allowed "${SOURCE_DIR}/UI/Main/AuditionOutput.h"
                "${SOURCE_DIR}/Audio/AudioEngine.h"
                "${SOURCE_DIR}/Audio/AudioEngine.cpp")
    set(pattern "(beginAuditionLoop|endAuditionLoop)[ \t]*\\(")
    set(hint "Use AuditionOutput::playLoop / stopLoop instead (see Source/UI/Main/AuditionOutput.h).")
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
