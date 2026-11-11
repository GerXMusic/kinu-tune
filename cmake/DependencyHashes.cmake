# Copyright (C) 2026 Keinoaza
# This file is part of Kinu Tune and is licensed under the GNU Affero General
# Public License, version 3. See LICENSE.
#
# SHA-256 of every archive that CMake downloads while configuring Kinu Tune.
#
# Each value was taken on 2026-10-23 by downloading the archive twice and
# comparing the hashes; the NuGet packages also match the SHA-512 that nuget.org
# publishes for them and the GAME zip matches the digest GitHub shows for the
# release asset (see verify-map.md and docs/source-ledger.md outside the repo).
#
# When a version number changes (ONNXRUNTIME_VERSION, DIRECTML_VERSION,
# GAME_MODELS_VERSION), download the new archive twice, compare, and add the
# value here in the same commit. A download whose hash is not recorded stops the
# configure step unless PITCHNET_ALLOW_UNPINNED_DOWNLOADS is ON.

option(PITCHNET_ALLOW_UNPINNED_DOWNLOADS
    "Download an archive even when cmake/DependencyHashes.cmake has no SHA-256 for it (otherwise configuring stops)"
    OFF)

# Store the hash of KEY in PITCHNET_SHA256_<identifier derived from KEY>.
function(pitchnet_record_sha256 key hash)
    string(MAKE_C_IDENTIFIER "${key}" id)
    string(LENGTH "${hash}" hash_length)
    if(NOT hash_length EQUAL 64)
        message(FATAL_ERROR "cmake/DependencyHashes.cmake: the SHA-256 for '${key}' must be 64 hex characters")
    endif()
    set(PITCHNET_SHA256_${id} "${hash}" PARENT_SCOPE)
endfunction()

# Return in OUT_VAR the SHA-256 recorded for KEY, or "" when there is none.
function(pitchnet_dependency_sha256 out_var key)
    string(MAKE_C_IDENTIFIER "${key}" id)
    if(DEFINED PITCHNET_SHA256_${id})
        set(${out_var} "${PITCHNET_SHA256_${id}}" PARENT_SCOPE)
    else()
        set(${out_var} "" PARENT_SCOPE)
    endif()
endfunction()

# Return in OUT_VAR the two list items "<KEYWORD>;SHA256=<hash>" for KEY, where
# KEYWORD is URL_HASH (FetchContent_Declare) or EXPECTED_HASH (file(DOWNLOAD)).
# Without a recorded hash this stops the configure step, or returns "" when
# PITCHNET_ALLOW_UNPINNED_DOWNLOADS is ON.
function(pitchnet_download_hash_arguments out_var key keyword)
    pitchnet_dependency_sha256(hash "${key}")
    if(hash STREQUAL "")
        if(PITCHNET_ALLOW_UNPINNED_DOWNLOADS)
            message(WARNING
                "No SHA-256 is recorded for '${key}' in cmake/DependencyHashes.cmake; "
                "downloading it unverified because PITCHNET_ALLOW_UNPINNED_DOWNLOADS is ON")
            set(${out_var} "" PARENT_SCOPE)
        else()
            message(FATAL_ERROR
                "No SHA-256 is recorded for '${key}' in cmake/DependencyHashes.cmake. "
                "Download the archive twice, compare the hashes and add the value there, "
                "or configure with -DPITCHNET_ALLOW_UNPINNED_DOWNLOADS=ON to download it unverified.")
        endif()
    else()
        set(${out_var} "${keyword}" "SHA256=${hash}" PARENT_SCOPE)
    endif()
endfunction()

pitchnet_record_sha256("onnxruntime-1.19.2-osx-arm64"      "370c49770e2e1f243e17c7b227bb7f4b3da793b847d02f38016dc0e46c30fbe1") # onnxruntime-osx-arm64-1.19.2.tgz (the macOS release build)
pitchnet_record_sha256("onnxruntime-1.19.2-osx-x86_64"     "6536e36d7ea92e32d53dad7ddd0fdf10be5b62d1dace85a13e1295ff81e9b5d4") # onnxruntime-osx-x86_64-1.19.2.tgz
pitchnet_record_sha256("onnxruntime-1.19.2-osx-universal2" "b0289ddbc32f76e5d385abc7b74cc7c2c51cdf2285b7d118bf9d71206e5aee3a") # onnxruntime-osx-universal2-1.19.2.tgz
pitchnet_record_sha256("onnxruntime-1.19.2-win-x64"        "dc4f841e511977c0a4f02e5066c3d9a58427644010ab4f89b918614a1cd4c2b0") # onnxruntime-win-x64-1.19.2.zip (CI and the Windows release build)
pitchnet_record_sha256("onnxruntime-1.19.2-linux-x64"      "eb00c64e0041f719913c4080e0fed7d9963dc3aa9b54664df6036d8308dbcd33") # onnxruntime-linux-x64-1.19.2.tgz
pitchnet_record_sha256("onnxruntime-directml-nuget-1.19.2" "91b80d881105f4fd3480eaef595378bcb4ebb3362098be2fb0f1f2976d42a699") # Microsoft.ML.OnnxRuntime.DirectML 1.19.2 from nuget.org (USE_DIRECTML=ON)
pitchnet_record_sha256("directml-nuget-1.15.4"             "4e7cb7ddce8cf837a7a75dc029209b520ca0101470fcdf275c1f49736a3615b9") # Microsoft.AI.DirectML 1.15.4 from nuget.org (USE_BUNDLED_DIRECTML_RUNTIME)
pitchnet_record_sha256("game-models-v1.0.3-small-onnx"     "00ba0c64115b6b874d9ea4afd3e6cf822abda2a04e52569233b0a044fd40e4e8") # GAME-1.0.3-small-onnx.zip from github.com/openvpi/GAME
