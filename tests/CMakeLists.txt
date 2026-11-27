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
    CommitSeamTests.cpp
    D197BlockDirtyTests.cpp
    FormantRetirementTests.cpp
    FormantShiftTests.cpp
    HarmonyIntervalTests.cpp
    HostSyncTempoTests.cpp
    # D-310: el tempo map del host (quarters <-> segundos, compases).
    HostTempoMapTests.cpp
    # R3: la grilla y la regla siguen el mapa del host (compases reales).
    TempoGridTests.cpp
    # R4: el snap del ciclo pasa por el mapa (quarters) y el BPM del cursor
    # solo se guarda cuando no hay mapa.
    SnapTempoMapTests.cpp
    ProjectTempoPersistenceTests.cpp
    # D-311/D-312: la guia MIDI (coordenadas, emparejamiento, importacion y
    # persistencia).
    MidiGuideTests.cpp
    MidiGuideImportTests.cpp
    MidiGuideApplyTests.cpp    NoteEngineTests.cpp
    # R1: la guia solo edita el item activo (ventanas fuente/host).
    MidiGuideRegionScopeTests.cpp
    # R2: los fantasmas viven en tiempo host y se dibujan con el offset actual.
    MidiGuideGhostTests.cpp
    # R5b (D-356): la guia es del documento; el store publicado apunta a las
    # guias reales y sobrevive al cambio de item.
    MidiGuideStoreTests.cpp
    # R5d (D-358): el store vive en el documento (no en el editor) y su clave
    # de pista es estable al recrearse la RegionSequence; sobrevive al cierre
    # del editor y una clave desconocida importa, no vacia.
    MidiGuideStoreLifetimeTests.cpp
    # R7 (D-360): la guia sobrevive a cerrar y reabrir el proyecto por un
    # sidecar JSON atomico (el archivo ARA no es de fiar, D-346).
    MidiGuideStorePersistenceTests.cpp
    # R11b (D-365): la clave del sidecar es la huella de las fuentes (nombre
    # ARA) + nombre de pista; reapertura y migracion del legacy `doc:`.
    MidiGuideStoreIdentityTests.cpp
    # R11d (D-367): la clave de pista es el NOMBRE solo; mover, anadir o quitar
    # items no la cambia, y la clave `src:...|name:` de R11b se migra una vez.
    MidiGuideStoreTrackKeyTests.cpp
    # R11f (D-370): una ruta de sidecar por pista (la vista del documento no
    # nombra el fichero); cadenas de cambios de vista, dos vistas a la vez,
    # consolidacion de varios vivos y guardado a mitad de cadena.
    MidiGuideStoreViewChainTests.cpp
    # R8 (D-361): el plan de aplicacion por pista (SCOPE=A) resuelve que item y
    # nota recibe cada nota de guia, con motivos de skip, sin escribir nada.
    MidiGuideTrackPlanTests.cpp
    # R9 (D-362): el plan se aplica por pista (SCOPE=A) por el camino real del
    # editor, como UN paso de undo del documento; lo que se salta se cuenta.
    MidiGuideTrackApplyTests.cpp
    # R10 (D-363): identidad estable (sin posicion), sync por hash de contenido
    # y mtime+size como camino rapido; guias huerfanas y aviso de cambio.
    MidiGuideSyncTests.cpp
    # N1 (D-340): el indice de notas del documento (5 items, 2 fuentes) apunta a
    # las notas reales, no inventa items sin project y dibuja lo mismo que cuenta.
    NoteIndexTests.cpp
    # N2 (D-340): la seleccion del documento (marquesina, shift-clic, seleccionar
    # todo en pista, dedupe de ventanas hermanas y poda al desaparecer un project).
    NoteSelectionTests.cpp
    # N3 (D-343): la edicion en lote, una operacion sobre la seleccion aplicada
    # en la toma de cada nota, igual que nota a nota.
    BatchEditTests.cpp
    # N4 (D-344): un gesto en lote es UN paso de undo del documento (undo/redo
    # exactos, vecinas del cuantizar incluidas, y el mensaje de la UI).
    BatchUndoTests.cpp
    # N5 (D-345): el render de un gesto en lote, una peticion por item afectado
    # (ni una por nota ni por ventana), sin items mudos y sin tocar los demas.
    BatchRenderTests.cpp
    # N9 (D-349): la seleccion tiene un solo dueno (DocumentNoteSelection) y
    # Note::selected es vista derivada; aqui se pina que toda operacion deja las
    # banderas coincidiendo con el conjunto.
    DocumentSelectionSingleSourceTests.cpp
    NoteGainCurveTests.cpp
    NonAraLifecycleTests.cpp
    OriginalWaveformRestoreTests.cpp
    PitchCurveRoundTripTests.cpp
    PitchCurveHandleTests.cpp
    PitchCurveTransitionTests.cpp
    D205StraightenContinuityTests.cpp
    TransitionJoinTests.cpp
    TimingQuantizeTests.cpp
    TimingAudioTests.cpp
    NoteMuteTests.cpp
    NoteDeleteTests.cpp
    # D-302: borde izquierdo del silencio por energia (ataque/consonante).
    NoteDeleteAttackTests.cpp
    NoteResetTests.cpp
    # D-258: motor por bloque; D-260: bypass con el original; T7: predicado.
    BlockEngineTests.cpp
    BypassOriginalTests.cpp
    NoteEditPredicateTests.cpp
    # Fase 1 de la edicion: S1/S2/S3/S6 en audio contra el render completo.
    EditSymptomAudioTests.cpp    # R4 (2026-11-21): union de notas con consonante no vocalizada.
    NoteBoundaryHoleTests.cpp
    # D-215: D212BoundaryTransitionTests deshabilitado. Depende de
    # BasePitchCurve::limitBoundarySlope, revertido en c0957836 por el
    # aplanado audible en D-214; el criterio de 1 c/ms queda descartado.
    PitchDrawingTests.cpp
    PitchDriftTests.cpp
    PluginStateArchiveTests.cpp
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

# R3: la grilla comparte la iteracion con el renderer (TempoGridView.cpp vive
# en la capa UI), asi que el test enlaza tambien pitchnet_ui.
target_link_libraries(TempoGridTests PRIVATE pitchnet_ui)

# R4: la regla de persistencia del tempo vive en Source/UI (pitchnet_ui).
target_link_libraries(ProjectTempoPersistenceTests PRIVATE pitchnet_ui)

# N4: el gesto del inspector y el aviso no modal son del editor, asi que el test
# maneja el PianoRollComponent real (pitchnet_ui).
target_link_libraries(BatchUndoTests PRIVATE pitchnet_ui)

# R9 (D-362): el camino real es MainComponent::applyMidiGuide -> plan + undo
# compuesto + render por item, asi que el test enlaza pitchnet_ui.
target_link_libraries(MidiGuideTrackApplyTests PRIVATE pitchnet_ui)

# N5: las peticiones de render salen del camino real del editor (piano roll,
# undo y destinos de la pista) y ademas se renderiza en audio: pitchnet_ui.
target_link_libraries(BatchRenderTests PRIVATE pitchnet_ui)

# TimingAudioTests (M4 v1): mide S1/S2 en audio con FCPE y los motores reales.
target_compile_definitions(TimingAudioTests PRIVATE
    TIMING_TEST_MODELS_DIR="${CMAKE_SOURCE_DIR}/Resources/models")
if(WIN32 AND ONNXRUNTIME_DLLS)
    foreach(DLL ${ONNXRUNTIME_DLLS})
        add_custom_command(TARGET TimingAudioTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DLL}" "$<TARGET_FILE_DIR:TimingAudioTests>")
    endforeach()
endif()

# P1b (render al Play): deferred dirty blocks and the mark-only triggers.
# P1b fase 3: el borde de Play del host.
add_executable(P1bHostPlayTests P1bHostPlayTests.cpp)
target_link_libraries(P1bHostPlayTests PRIVATE pitchnet_core)
target_compile_features(P1bHostPlayTests PRIVATE cxx_std_17)
target_compile_definitions(P1bHostPlayTests PRIVATE
    P1B_SOURCE_DIR="${CMAKE_SOURCE_DIR}/Source")
add_test(NAME P1b_hostplay COMMAND P1bHostPlayTests)

# P1b fase 4: flush forzado antes de guardar.
add_executable(P1bSaveTests P1bSaveTests.cpp)
target_link_libraries(P1bSaveTests PRIVATE pitchnet_core)
target_compile_features(P1bSaveTests PRIVATE cxx_std_17)
target_compile_definitions(P1bSaveTests PRIVATE
    P1B_SOURCE_DIR="${CMAKE_SOURCE_DIR}/Source")
add_test(NAME P1b_save COMMAND P1bSaveTests)

# P1b fase 5: al abrir se usa el render guardado, sin resintetizar.
add_executable(P1bOpenTests P1bOpenTests.cpp)
target_link_libraries(P1bOpenTests PRIVATE pitchnet_core)
target_compile_features(P1bOpenTests PRIVATE cxx_std_17)
target_compile_definitions(P1bOpenTests PRIVATE
    P1B_SOURCE_DIR="${CMAKE_SOURCE_DIR}/Source")
add_test(NAME P1b_open COMMAND P1bOpenTests)

# P1b fase 6: barrido de render desde la cola de bloques pendientes.
add_executable(P1bSweepTests P1bSweepTests.cpp)
target_link_libraries(P1bSweepTests PRIVATE pitchnet_core)
target_compile_features(P1bSweepTests PRIVATE cxx_std_17)
target_compile_definitions(P1bSweepTests PRIVATE
    P1B_SOURCE_DIR="${CMAKE_SOURCE_DIR}/Source")
add_test(NAME P1b_sweep COMMAND P1bSweepTests)

# Two groups, same executable: "deferred" is the bare queue behaviour, "wiring"
# is the invariant over MainComponent.cpp and the undo history size.
add_executable(P1bRenderAtPlayTests P1bRenderAtPlayTests.cpp)
target_link_libraries(P1bRenderAtPlayTests PRIVATE pitchnet_core)
target_compile_features(P1bRenderAtPlayTests PRIVATE cxx_std_17)
target_compile_definitions(P1bRenderAtPlayTests PRIVATE
    P1B_SOURCE_DIR="${CMAKE_SOURCE_DIR}/Source")
add_test(NAME P1b_deferred COMMAND P1bRenderAtPlayTests deferred)
add_test(NAME P1b_wiring COMMAND P1bRenderAtPlayTests wiring)

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

# N8 (D-348): el hover ya no activa la region; el inspector habla del
# documento (indice/seleccion), no del project que el puntero dejo activo.
add_executable(TrackHoverRegionTests TrackHoverRegionTests.cpp)
target_link_libraries(TrackHoverRegionTests PRIVATE pitchnet_ui)
target_compile_features(TrackHoverRegionTests PRIVATE cxx_std_17)
add_test(NAME TrackHoverRegionTests COMMAND TrackHoverRegionTests)

# N10 (D-350): una sola copia de las notas y un solo camino de dibujo. El preview
# presta el Project vivo; indice, dibujo y hit-test leen los mismos Note*.
add_executable(RegionPreviewSingleSourceTests RegionPreviewSingleSourceTests.cpp)
target_link_libraries(RegionPreviewSingleSourceTests PRIVATE pitchnet_ui)
target_compile_features(RegionPreviewSingleSourceTests PRIVATE cxx_std_17)
add_test(NAME RegionPreviewSingleSourceTests COMMAND RegionPreviewSingleSourceTests)

# N12 (D-354): todos los items de la pista se pintan igual (un solo estilo, sin
# rama por item activo), las fronteras se deduplican y el clip fijado es la
# excepcion documentada.
add_executable(RegionUniformLooksTests RegionUniformLooksTests.cpp)
target_link_libraries(RegionUniformLooksTests PRIVATE pitchnet_ui)
target_compile_features(RegionUniformLooksTests PRIVATE cxx_std_17)
add_test(NAME RegionUniformLooksTests COMMAND RegionUniformLooksTests)

# V1 (guia MIDI): instrumentacion de la visibilidad de los fantasmas. Mide
# ghost_count / in_view / visible / painted_last / pixeles cian y DOCUMENTA el
# comportamiento actual; V2 lo actualiza a painted == in_view.
add_executable(MidiGuideVisibilityTests MidiGuideVisibilityTests.cpp)
target_link_libraries(MidiGuideVisibilityTests PRIVATE pitchnet_ui)
target_compile_features(MidiGuideVisibilityTests PRIVATE cxx_std_17)
add_test(NAME MidiGuideVisibilityTests COMMAND MidiGuideVisibilityTests)

# N12 (D-354): la tira de overview es estado de vista; cambiar de item no la
# toca, el toggle del usuario si, y el campo del JSON sigue por compatibilidad.
add_executable(OverviewIndependenceTests OverviewIndependenceTests.cpp)
target_link_libraries(OverviewIndependenceTests PRIVATE pitchnet_ui)
target_compile_features(OverviewIndependenceTests PRIVATE cxx_std_17)
add_test(NAME OverviewIndependenceTests COMMAND OverviewIndependenceTests)

# N8 (D-348): conteo y texto del objetivo a nivel de documento (caso real
# 262+29, ventanas 0/7/77/70/110) y valores mixtos.
add_executable(DocumentInspectorTests DocumentInspectorTests.cpp)
target_link_libraries(DocumentInspectorTests PRIVATE pitchnet_ui)
target_compile_features(DocumentInspectorTests PRIVATE cxx_std_17)
add_test(NAME DocumentInspectorTests COMMAND DocumentInspectorTests)

add_executable(AuditionOutputTests AuditionOutputTests.cpp)
target_link_libraries(AuditionOutputTests PRIVATE pitchnet_ui)
target_compile_features(AuditionOutputTests PRIVATE cxx_std_17)
add_test(NAME AuditionOutputTests COMMAND AuditionOutputTests)

add_executable(AuditionHandoffTests AuditionHandoffTests.cpp)
target_link_libraries(AuditionHandoffTests PRIVATE pitchnet_ui)
target_compile_features(AuditionHandoffTests PRIVATE cxx_std_17)
add_test(NAME AuditionHandoffTests COMMAND AuditionHandoffTests)

# S4d (D-254): el cierre del render del plugin limpia por revision, no todo.
add_executable(EditRenderCloseTests EditRenderCloseTests.cpp)
target_link_libraries(EditRenderCloseTests PRIVATE pitchnet_ui)
target_compile_features(EditRenderCloseTests PRIVATE cxx_std_17)
add_test(NAME EditRenderCloseTests COMMAND EditRenderCloseTests)

# D-265 (T1): el criterio de envolvente en la frontera, con su segundo gesto
# (nota del borde de la frase) como entrada propia de ctest.
add_test(NAME EditSymptom_sbedge COMMAND EditSymptomAudioTests sb-edge)

# D-285 (2.2/2.3): cortar no afina y unir es un solo paso; medido en cents sobre
# la curva compuesta, con control sin cortar.
add_executable(SplitPitchNeutralityTests SplitPitchNeutralityTests.cpp)
target_link_libraries(SplitPitchNeutralityTests PRIVATE pitchnet_ui)
target_compile_features(SplitPitchNeutralityTests PRIVATE cxx_std_17)
add_test(NAME SplitPitchNeutralityTests COMMAND SplitPitchNeutralityTests)

# D-285/3: la curva sigue a las notas después de cada gesto de altura (arrastre,
# selección, flechas, doble clic, inspector, cortar/unir).
add_executable(CurveFollowsNotesTests CurveFollowsNotesTests.cpp)
target_link_libraries(CurveFollowsNotesTests PRIVATE pitchnet_ui)
target_compile_features(CurveFollowsNotesTests PRIVATE cxx_std_17)
add_test(NAME CurveFollowsNotesTests COMMAND CurveFollowsNotesTests)

# D-306: inclinar no mueve el cuerpo de la nota; medido con el controlador real
# (PitchToolHandles + MouseEvent) y con la base reconstruida.
add_executable(TiltCenterTests TiltCenterTests.cpp)
target_link_libraries(TiltCenterTests PRIVATE pitchnet_ui)
target_compile_features(TiltCenterTests PRIVATE cxx_std_17)
add_test(NAME TiltCenterTests COMMAND TiltCenterTests)

# D-308: tres capas de inclinacion independientes (general + un fino por
# mitad), con curva compuesta, undo/redo, JSON y corte de nota inclinada.
add_executable(TiltIndependenceTests TiltIndependenceTests.cpp)
target_link_libraries(TiltIndependenceTests PRIVATE pitchnet_ui)
target_compile_features(TiltIndependenceTests PRIVATE cxx_std_17)
add_test(NAME TiltIndependenceTests COMMAND TiltIndependenceTests)

# D-266 (T6): estado unico derivado del render (tabla de transiciones).
add_executable(RenderStateTests RenderStateTests.cpp)
target_link_libraries(RenderStateTests PRIVATE pitchnet_core)
target_compile_features(RenderStateTests PRIVATE cxx_std_17)
add_test(NAME RenderStateTests COMMAND RenderStateTests)

# D-267 (T3/T4): boton de bypass y aviso de motor no modal en el rail.
add_executable(EnginePanelTests EnginePanelTests.cpp)
target_link_libraries(EnginePanelTests PRIVATE pitchnet_ui)
target_compile_features(EnginePanelTests PRIVATE cxx_std_17)
add_test(NAME EnginePanelTests COMMAND EnginePanelTests)

# D-309: colores de motor por bloque visual, bloque activo, cache, barrido de
# borde y presupuesto de pintado.
add_executable(EngineBlockViewTests EngineBlockViewTests.cpp)
target_link_libraries(EngineBlockViewTests PRIVATE pitchnet_ui)
target_compile_features(EngineBlockViewTests PRIVATE cxx_std_17)
add_test(NAME EngineBlockViewTests COMMAND EngineBlockViewTests)

# Tarea A/B (2026-11-22): el reset borra la transicion (D-225) y la cola de la
# ultima nota vuelve a la fuente. Usa el camino de produccion del reset.
add_executable(NoteTransitionResetTests NoteTransitionResetTests.cpp)
target_link_libraries(NoteTransitionResetTests PRIVATE pitchnet_ui)
target_compile_features(NoteTransitionResetTests PRIVATE cxx_std_17)
add_test(NAME NoteTransitionResetTests COMMAND NoteTransitionResetTests)

# Tareas 1-3 (2026-11-22): cajon alineado, bypass en la barra superior y curva
# de fuente en la vista de bypass.
add_executable(BypassUiTests BypassUiTests.cpp)
target_link_libraries(BypassUiTests PRIVATE pitchnet_ui)
target_compile_features(BypassUiTests PRIVATE cxx_std_17)
add_test(NAME BypassUiTests COMMAND BypassUiTests)

add_executable(OnnxRuntimeAvailabilityTests OnnxRuntimeAvailabilityTests.cpp)
target_link_libraries(OnnxRuntimeAvailabilityTests PRIVATE pitchnet_ui)
target_compile_features(OnnxRuntimeAvailabilityTests PRIVATE cxx_std_17)
add_test(NAME OnnxRuntimeAvailabilityTests COMMAND OnnxRuntimeAvailabilityTests)

# Loads the shipped models and checks analysis and synthesis with numbers. The
# CI Windows job builds and runs only this target (the other tests stay on
# macOS), because nothing else runs a model on Windows. The test takes the
# models folder as its argument; GAME is downloaded into it at configure time.
add_executable(ModelInferenceTests ModelInferenceTests.cpp)
target_link_libraries(ModelInferenceTests PRIVATE pitchnet_core)
target_compile_features(ModelInferenceTests PRIVATE cxx_std_17)
add_test(NAME ModelInferenceTests
         COMMAND ModelInferenceTests "${CMAKE_SOURCE_DIR}/Resources/models")
if(WIN32 AND ONNXRUNTIME_DLLS)
    # Like the plug-in, the test loads onnxruntime.dll from its own folder.
    foreach(DLL ${ONNXRUNTIME_DLLS})
        add_custom_command(TARGET ModelInferenceTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DLL}" "$<TARGET_FILE_DIR:ModelInferenceTests>")
    endforeach()
endif()

# Calls that must go through one entry point (see SourceGuard.cmake).
foreach(guard_rule IN ITEMS pitch-writes note-selection region-copies midi-guides audition-output render-controllers)
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

