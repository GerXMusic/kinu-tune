<!-- TuneX - Copyright (C) 2026 Keinoaza
Part of TuneX, a modified version of PitchNet. Licensed under the GNU
Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md. -->

# Arquitectura de TuneX

Mapa para quien llega nuevo: qué caja hace qué, por dónde viaja el audio y dónde
buscar cuando algo falla. Las rutas son relativas a `Source/` salvo que se diga
otra cosa. Estado: rama `refactor/modularidad`, commit `eba02bf` (R2, 2026-11-28).
Cuando R3/R4 muevan código, este mapa se actualiza en el mismo commit: si un
archivo citado aquí ya no existe, el mapa está viejo.

## Las ocho cajas

| Caja | Qué hace (una frase) | Dónde vive hoy |
|---|---|---|
| `UI` | ventana, piano roll, inspector, diálogos y todo lo que se pinta | `UI/` |
| `App/Interfaces` | contratos entre el plugin y la UI (`IMainView`, factory); hoy no tiene carpeta propia | `UI/IMainView.h`, `UI/MainViewFactory.{h,cpp}` |
| `Plugin` | host VST3/ARA: estado, captura, regiones, scheduler de publicación | `Plugin/` |
| `Audio` | motor, síntesis, IO, tiempo real y mapa de tempo del host | `Audio/` |
| `Undo` | acciones reversibles sobre `Models` | `Undo/` |
| `Models` | `Project`, `Note`, `RenderPlan`, serialización y guías MIDI | `Models/` |
| `Utils/Core` | cadenas, archivos, logging, plataforma, curvas de pitch | `Utils/` |
| `pitchcore` | render matemático puro (F0 → segmentar → PSOLA/espectral/neuronal → WAV) | `codigo/pitchcore/`, sin JUCE |

## Dos flujos que hay que tener en la cabeza

**Audio (host → altavoz):**
`host` → `PitchNetDocumentController` (`Plugin/ARADocumentController.cpp`) →
`Project` de la región (`PitchNetAudioProcessor::araRegions`) → `Note` →
`renderPlan::build` (`Models/RenderPlan.cpp`) → `IncrementalSynthesizer`
(`Audio/Synthesis/`) → búfer publicado → `host`.

**Edición (gesto → render):**
`UI` (piano roll / inspector) → `Undo` (`Undo/*Action.h`) → `Project` →
`MainComponent::markRenderDirty()` (`UI/MainComponent.cpp:5054`) →
`DeferredRenderQueue` → `serviceEditRender()` / cadena de flush
(`MainComponent.cpp:5085` y `5277`) → `IncrementalSynthesizer` → publish.

## Dónde buscar si falla…

| Síntoma | Primer sitio |
|---|---|
| El diálogo «Acerca de» | `UI/Dialogs/AboutDialog.{h,cpp}` (`AboutContent`, los avisos de terceros salen de `BinaryData::THIRD_PARTY_NOTICES_txt`; `showAboutDialog()`); `MainComponent::showAboutPopup()` solo delega; se abre desde el menú en `UI/MainComponent.cpp` |
| El undo de una nota | `Undo/NoteActions.h`, `Undo/PitchUndoManager.h`; tras un cambio de historial se llama `MainComponent::afterHistoryChange` y `PianoRollComponent::dropTransientNotePointers()` |
| El audio de una nota muda | se decide en `Models/Note.h:193` (`isMuted()`), `Models/Project.cpp:173-208` (`shouldResetNeutralNoteToOriginal`, `noteHasAudioRelevantEdit`) y se escribe por `Models/DocumentNoteBatch.cpp:287` (`setMuted`) |
| El corte / la unión | geometría en `UI/PianoRoll/NoteSplitter.cpp` (`splitNote`, `mergeNotes`); gesto en `UI/PianoRoll/States/SplitHandler.cpp` y `PianoRollComponent.cpp`; undo en `Undo/InspectorEditAction.h` / `NoteActions.h` |
| La inclinación (tilt) | datos en `Models/Note.h` (`tilt`, `tiltFineLeft`, `tiltFineRight`); matemática en `Utils/PitchToolOperations.cpp:10` (`tiltDeltaPitch`) y `Utils/PitchCurveProcessor.cpp:82` (`collectNoteSegments`); UI en `UI/VariAudioInspector.cpp:329-335` y `UI/PianoRoll/PitchToolHandles.h` |
| El tempo del host | `Audio/Engine/HostTempoMap.{h,cpp}` y `Audio/Engine/HostSyncService.cpp`; en ARA lo lee `Plugin/ARATempoMapReader.{h,cpp}` |
| La guía MIDI | `Models/MidiGuide.h`, `DocumentMidiGuides.h`, `MidiGuidePersistence.cpp`, `MidiGuideTrackApply.cpp`; panel en `UI/MidiGuidePanel.cpp`; importación en `Audio/IO/MidiGuideImporter.cpp` |
| El estado al guardar/reabrir | `Models/ProjectSerializer.cpp` (`toBinaryArchive`/`fromBinaryArchive`, `FORMAT_VERSION`); estado del plugin en `Plugin/PluginProcessor.cpp:2288` (`getStateInformation`) y `:2341` (`setStateInformation`) |
| El análisis ARA | `Audio/EditorController.cpp:777` (`analyzeAudio`) y `:1142` (`analyzeAudioAsync`); ciclo ARA en `Plugin/ARADocumentController.cpp` |
| La captura no-ARA | `Plugin/NonAraCaptureController.{h,cpp}` (`processBlock`, `:43`) |

## Las cinco preguntas del lector nuevo

1. **«Acerca de»**: `MainComponent::showAboutPopup()` (`UI/MainComponent.cpp`) delega
   en `showAboutDialog()` (`UI/Dialogs/AboutDialog.h`); el contenido está en
   `AboutContent` (`UI/Dialogs/AboutDialog.cpp`) y los avisos de terceros son
   `BinaryData::THIRD_PARTY_NOTICES_txt`.
2. **Nota muteada**: `Note::isMuted()` (`Models/Note.h:193`); el render lo trata
   como edición audible en `Project::noteHasAudioRelevantEdit`
   (`Models/Project.cpp:196`).
3. **Scheduler que no drena**: el estado está en `UI/Main/RenderState.h` y la cola
   en `UI/Main/DeferredRenderQueue.h`; el audio se degrada en el callback ARA
   (`Plugin/ARADocumentController.cpp:962-988`): se oye el último audio publicado y,
   si nunca hubo, silencio (`:975-983`).
4. **Estado ARA de una región**: `PitchNetAudioProcessor::araRegions`
   (`Plugin/PluginProcessor.h:476-491`), serializado por
   `serializeAraRegionProject` (`Plugin/PluginProcessor.cpp:3179`), que llama
   `ProjectSerializer::toBinaryArchive` en modo `hostBackedARA`.
5. **Curva de ratio de pitch**: `IncrementalSynthesizer::buildPitchRatioCurve`
   (`Audio/Synthesis/IncrementalSynthesizer.cpp:1142`), con `interpolateF0`
   (`:684`) e `interpolateMelFrame` (`:515`).

## Reglas de dependencia

```
UI → App → Plugin / Audio / Undo → Models → Utils/Core
pitchcore: STL y nada más (sin JUCE, sin Models del plugin)
```

- `UI` puede incluir `App`, `Audio`, `Models`, `Undo`, `Utils`; **no** incluye
  internos de `Plugin` ni `pitchcore` directamente.
- `Plugin` puede incluir `Audio`, `Models`, `Undo`, `Utils`, `App`; **no** widgets
  de `UI` ni `pitchcore` directamente.
- `Audio` puede incluir `Models`, `Utils` y `pitchcore`; **no** `UI` ni `Plugin`.
- `Undo` solo toca `Models` y `Utils`; **no** `UI`, `Audio` ni `Plugin`.
- `Models` solo depende de `Utils/Core`; **no** `Audio`, `UI` ni `Plugin`.
- `Utils/Core` no conoce nada del dominio.
- Si un archivo nuevo necesita romper esta tabla, primero se escribe la excepción
  en `dev-rules.md` / `dev-rules.es.md` y se justifica; no se descubre después.

## Mapa de archivos

Los diez que hay que conocer:

| Archivo | Por qué |
|---|---|
| `Main.cpp` | entrada de la app de escritorio; crea la ventana y el `MainComponent` |
| `UI/MainComponent.{h,cpp}` | cáscara del editor: comandos, proyecto activo, scheduler de la UI |
| `UI/PianoRollComponent.{h,cpp}` | piano roll: dibujo, ratón, herramientas y estados |
| `Plugin/PluginProcessor.{h,cpp}` | `AudioProcessor` VST3/ARA, estado, regiones, captura |
| `Plugin/ARADocumentController.{h,cpp}` | documento ARA: ciclo de vida, callback de audio, publicación |
| `Models/Project.{h,cpp}` | raíz del modelo: notas, región, sucio, undo de estado |
| `Models/Note.h` | la nota: pitch, tiempo, mute, tilt, motor |
| `Models/RenderPlan.{h,cpp}` | plan inmutable de un render (bloques + revisión) |
| `Audio/EditorController.{h,cpp}` | backend del editor: análisis, síntesis, publicación |
| `Audio/Synthesis/IncrementalSynthesizer.cpp` | render por bloques y curva de ratio |
| `Undo/NoteActions.h` | acciones reversibles de nota (pitch, mute, borrar) |

El resto, por carpeta:

- `UI/PianoRoll/` — render del piano roll (`NoteRenderer`, `PitchCurveRenderer`,
  `TimelineRenderer`, `WaveformBackgroundRenderer`), estados del ratón
  (`States/`), herramientas (`PitchToolController`), scroll/zoom, overview.
- `UI/Main/` — piezas de `MainComponent` ya extraídas: `AuditionOutput`,
  `DeferredRenderQueue`, `RenderState`, `SettingsManager`, `MenuHandler`,
  `ProjectIO`, `ExportHelper`, `ExportController`, `EditorTooltip`.
- `UI/Components/`, `UI/Workspace/`, `UI/Dialogs/` — widgets, paneles y diálogos.
- `Plugin/` — además del processor y el documento ARA: `NonAraCaptureController`,
  `ARATempoMapReader`, `RenderControllers.h`, `AraDiagnostics.h`.
- `Audio/Synthesis/` — `IncrementalSynthesizer`, `PsolaSynthesizer`,
  `PitchcoreSynthesizer`, `EngineBlocks`, formantes.
- `Audio/Engine/` — tempo del host, transporte del plugin, cuantización.
- `Audio/IO/` — importación/exportación de audio y MIDI.
- `Models/` — `Note`, `Project`, `RenderPlan`, serialización, guías MIDI y lotes
  de documento (`Document*`).
- `Undo/` — una acción por familia: nota, mute, borrar, drag, timing, pitch
  drawing, inspector.
- `Utils/` — pitch (`NotePitch`, `PitchCurveProcessor`, `PitchToolOperations`),
  escala/armonía, ONNX, plataforma, localización y `Utils/UI/`.
