# Kinu Tune - Copyright (C) 2026 Keinoaza
# Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
# Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#
# Removes absolute rpaths from a macOS binary and fails if one is left.
#
# Linking against the ONNX Runtime library in the build tree records that
# folder (an absolute path of the build machine, including the user name) as
# an rpath. Shipped bundles find the library in Contents/Frameworks through
# @loader_path / @executable_path, so absolute rpaths are only a leak.
#
#   cmake -DBINARY=<bundle>/Contents/MacOS/<name> -P StripBuildRpaths.cmake

if(NOT BINARY OR NOT EXISTS "${BINARY}")
    message(FATAL_ERROR "StripBuildRpaths: BINARY not found: '${BINARY}'")
endif()

function(read_rpaths binary out_var)
    execute_process(COMMAND otool -l "${binary}"
                    OUTPUT_VARIABLE load_commands
                    RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "StripBuildRpaths: otool failed on ${binary}")
    endif()
    string(REGEX MATCHALL "cmd LC_RPATH\n[^\n]*\n[ \t]*path [^\n]+ \\(offset [0-9]+\\)"
           entries "${load_commands}")
    set(paths "")
    foreach(entry IN LISTS entries)
        string(REGEX REPLACE ".*path ([^\n]+) \\(offset [0-9]+\\)$" "\\1" path "${entry}")
        list(APPEND paths "${path}")
    endforeach()
    set(${out_var} "${paths}" PARENT_SCOPE)
endfunction()

read_rpaths("${BINARY}" rpaths)
foreach(path IN LISTS rpaths)
    if(path MATCHES "^/")
        execute_process(COMMAND install_name_tool -delete_rpath "${path}" "${BINARY}"
                        RESULT_VARIABLE result ERROR_VARIABLE error)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "StripBuildRpaths: could not delete rpath ${path}: ${error}")
        endif()
    endif()
endforeach()

read_rpaths("${BINARY}" rpaths)
foreach(path IN LISTS rpaths)
    if(path MATCHES "^/")
        message(FATAL_ERROR "StripBuildRpaths: absolute rpath left in ${BINARY}: ${path}")
    endif()
endforeach()
