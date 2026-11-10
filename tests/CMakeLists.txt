# Unit tests.
#
# Each test is a standalone executable with its own main(), matching the style
# the files were already written in. They link pitchnet_core, which exposes the
# JUCE modules PUBLIC, so a test gets every include path it needs by linking
# that one target.
#
# Built only when PITCHNET_BUILD_TESTS=ON, so a normal build and CI are
# unaffected:
#
#   cmake -B build -DPITCHNET_BUILD_TESTS=ON
#   cmake --build build
#   ctest --test-dir build --output-on-failure

set(PITCHNET_TEST_SOURCES
    AraRegionMergeTests.cpp
    AraRegionSliceTests.cpp
    AraSelectionPolicyTests.cpp
    AraViewFocusTests.cpp
    AraEnginePolicyTests.cpp
    AraRestoreAnalysisRaceTests.cpp
    CursorFollowPolicyTests.cpp
    MelSpectrogramTests.cpp
    SingleWindowPolicyTests.cpp
    DocumentEngineTests.cpp
    EngineBlocksTests.cpp
    D197BlockDirtyTests.cpp
    FormantRetirementTests.cpp
    FormantShiftTests.cpp
    HarmonyIntervalTests.cpp
    HostSyncTempoTests.cpp
    NoteEngineTests.cpp
    NoteGainCurveTests.cpp
    NonAraLifecycleTests.cpp
    OriginalWaveformRestoreTests.cpp
    PitchCurveRoundTripTests.cpp
    PitchCurveHandleTests.cpp
    PitchCurveTransitionTests.cpp
    D205StraightenContinuityTests.cpp
    D206IntervalTests.cpp
    # D-215: D212BoundaryTransitionTests deshabilitado. Depende de
    # BasePitchCurve::limitBoundarySlope, revertido en c0957836 por el
    # aplanado audible en D-214; el criterio de 1 c/ms queda descartado.
    PitchDrawingTests.cpp
    PitchDriftTests.cpp
    PluginStateArchiveTests.cpp
    ProjectModificationRangeTests.cpp
    SynthesisEngineShareTests.cpp
    TimelineCoordinateTests.cpp
    VisualWaveformEnvelopeTests.cpp)

foreach(test_source IN LISTS PITCHNET_TEST_SOURCES)
    get_filename_component(test_name ${test_source} NAME_WE)

    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE pitchnet_core)
    target_compile_features(${test_name} PRIVATE cxx_std_17)

    # The tests assert on results rather than returning a failure code, so a
    # failed assertion aborts the process and CTest sees a non-zero exit.
    add_test(NAME ${test_name} COMMAND ${test_name})

    # NOTE: assert() compiles to nothing under NDEBUG, which release
    # configurations set - so a test written with bare assert() passes
    # vacuously there. Undefining NDEBUG for the target is not the fix: it
    # changes the compile flags for every JUCE source the target pulls in,
    # forcing a full rebuild of JUCE per test. Tests should use a check macro
    # that does not depend on NDEBUG instead; see TestAssert.h.
endforeach()

# The capture state machine belongs to the plug-in rather than pitchnet_core.
target_sources(NonAraLifecycleTests PRIVATE
    ../Source/Plugin/NonAraCaptureController.cpp)

add_executable(PitchMacroScopeTests PitchMacroScopeTests.cpp)
target_link_libraries(PitchMacroScopeTests PRIVATE pitchnet_core)
target_compile_features(PitchMacroScopeTests PRIVATE cxx_std_17)
add_test(NAME PitchMacroScopeTests COMMAND PitchMacroScopeTests)

add_executable(TrackUndoTests TrackUndoTests.cpp)
target_link_libraries(TrackUndoTests PRIVATE pitchnet_core)
target_compile_features(TrackUndoTests PRIVATE cxx_std_17)
add_test(NAME TrackUndoTests COMMAND TrackUndoTests)

add_executable(TrackGestureTests TrackGestureTests.cpp)
target_link_libraries(TrackGestureTests PRIVATE pitchnet_ui)
target_compile_features(TrackGestureTests PRIVATE cxx_std_17)
add_test(NAME TrackGestureTests COMMAND TrackGestureTests)

add_executable(PitchMacroRenderTests PitchMacroRenderTests.cpp)
target_link_libraries(PitchMacroRenderTests PRIVATE pitchnet_ui)
target_compile_features(PitchMacroRenderTests PRIVATE cxx_std_17)
add_test(NAME PitchMacroRenderTests COMMAND PitchMacroRenderTests)

add_executable(HarmonyApplyTests HarmonyApplyTests.cpp)
target_link_libraries(HarmonyApplyTests PRIVATE pitchnet_ui)
target_compile_features(HarmonyApplyTests PRIVATE cxx_std_17)
add_test(NAME HarmonyApplyTests COMMAND HarmonyApplyTests)

# Not a test: renders UI snapshots for visual checks (no audio device needed).
add_executable(RenderSnapshots RenderSnapshots.cpp)
target_link_libraries(RenderSnapshots PRIVATE pitchnet_ui)
target_compile_features(RenderSnapshots PRIVATE cxx_std_17)

add_executable(VariAudioControlTests VariAudioControlTests.cpp)
target_link_libraries(VariAudioControlTests PRIVATE pitchnet_ui)
target_compile_features(VariAudioControlTests PRIVATE cxx_std_17)
add_test(NAME VariAudioControlTests COMMAND VariAudioControlTests)

add_executable(VariAudioInspectorTests VariAudioInspectorTests.cpp)
target_link_libraries(VariAudioInspectorTests PRIVATE pitchnet_ui)
target_compile_features(VariAudioInspectorTests PRIVATE cxx_std_17)
add_test(NAME VariAudioInspectorTests COMMAND VariAudioInspectorTests)

add_executable(AuditionOutputTests AuditionOutputTests.cpp)
target_link_libraries(AuditionOutputTests PRIVATE pitchnet_ui)
target_compile_features(AuditionOutputTests PRIVATE cxx_std_17)
add_test(NAME AuditionOutputTests COMMAND AuditionOutputTests)

add_executable(AuditionHandoffTests AuditionHandoffTests.cpp)
target_link_libraries(AuditionHandoffTests PRIVATE pitchnet_ui)
target_compile_features(AuditionHandoffTests PRIVATE cxx_std_17)
add_test(NAME AuditionHandoffTests COMMAND AuditionHandoffTests)

add_executable(OnnxRuntimeAvailabilityTests OnnxRuntimeAvailabilityTests.cpp)
target_link_libraries(OnnxRuntimeAvailabilityTests PRIVATE pitchnet_ui)
target_compile_features(OnnxRuntimeAvailabilityTests PRIVATE cxx_std_17)
add_test(NAME OnnxRuntimeAvailabilityTests COMMAND OnnxRuntimeAvailabilityTests)

# Calls that must go through one entry point (see SourceGuard.cmake).
foreach(guard_rule IN ITEMS pitch-writes audition-output render-controllers)
    add_test(NAME SourceGuard_${guard_rule}
             COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR}/Source
                     -DRULE=${guard_rule}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/SourceGuard.cmake)
endforeach()

# D-152 characterization: describes the pre-implementation behaviour. It needs
# the ARA diagnostics definitions, because the hydration flag is diagnostic-only.
if(ARA_AVAILABLE AND PITCHNET_ARA_DIAGNOSTICS)
    add_executable(D152CharacterizationTests D152CharacterizationTests.cpp)
    target_link_libraries(D152CharacterizationTests PRIVATE
        PitchNetPlugin
        pitchnet_ui
        pitchnet_core
        juce::juce_audio_processors
        juce::juce_audio_plugin_client
        juce::juce_audio_utils
        juce::juce_gui_extra
        juce::juce_recommended_config_flags)
    target_compile_definitions(D152CharacterizationTests PRIVATE
        JucePlugin_Enable_ARA=1
        PITCHNET_ARA_DIAGNOSTICS=1)
    target_include_directories(D152CharacterizationTests PRIVATE
        ${ARA_SDK_PATH})
    # The plug-in's VST3 wrapper normally supplies the ARA library from
    # juce_audio_plugin_client_ARA.cpp. This test is not a plug-in wrapper, so
    # compile the same library sources into the test executable.
    target_sources(D152CharacterizationTests PRIVATE
        ${ARA_SDK_PATH}/ARA_Library/PlugIn/ARAPlug.cpp
        ${ARA_SDK_PATH}/ARA_Library/Dispatch/ARAPlugInDispatch.cpp
        ${ARA_SDK_PATH}/ARA_Library/Utilities/ARAPitchInterpretation.cpp
        ${ARA_SDK_PATH}/ARA_Library/Utilities/ARAChannelFormat.cpp)
    target_compile_features(D152CharacterizationTests PRIVATE cxx_std_17)

    foreach(D152_GROUP IN ITEMS bypass saved-state processed-audio
                              concurrent-generations hydration)
        add_test(NAME D152_${D152_GROUP}
                 COMMAND D152CharacterizationTests ${D152_GROUP})
    endforeach()
endif()

