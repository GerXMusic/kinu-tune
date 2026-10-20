# Kinu Tune - Copyright (C) 2026 Keinoaza
# Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
# Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#
# A minimal VST3 host used by CI to check that the Windows plug-in can be
# created under an older Visual C++ runtime. See main.cpp.
cmake_minimum_required(VERSION 3.22)

# The static runtime is the point of this program: it must not load
# MSVCP140.dll itself, so that the plug-in's MSVCP140.dll comes from the
# folder this program runs in.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

project(Vst3InstantiateCheck CXX)

set(VST3_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../third_party/JUCE/modules/juce_audio_processors_headless/format_types/VST3_SDK"
    CACHE PATH "VST3 SDK headers (the copy inside JUCE)")

add_executable(vst3_instantiate main.cpp)
target_compile_features(vst3_instantiate PRIVATE cxx_std_17)
target_include_directories(vst3_instantiate PRIVATE "${VST3_SDK_DIR}")
target_compile_definitions(vst3_instantiate PRIVATE UNICODE _UNICODE)
