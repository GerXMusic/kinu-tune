# Reglas de desarrollo de TuneX (promesas de diseño)

Traducción al español de `dev-rules.md` (japonés). El original manda si hay
discrepancia; esto es la versión legible para el usuario. No cambia código.

Este es el archivo que se lee antes de cambiar código. Solo escribe las promesas
que, si se rompen, rompen otra parte. Los pasos de compilación, los registros de
verificación y el historial están fuera del repositorio, en `verify-map.md`.

Cuando se descubre una promesa nueva, se añade aquí en el mismo commit que el
cambio. Cada promesa dice: qué hay que respetar, cuál es la única entrada
obligatoria y con qué test se detecta si se rompe.

## 1. Solo NotePitch escribe el pitch de una nota

- El pitch de la nota (`Note::midiNote`) y la referencia de «Corregir
  afinación» (`Note::lastNonMacroMidiNote`, la nota cantada) solo se escriben con
  las funciones de `Source/Utils/NotePitch.h`:
  - mover con una edición (arrastre, flechas, armonía, imán, inclinación, smart
    controls): `NotePitch::applyEdit` (el valor para el undo es `sourceAfterEdit`);
  - devolver un valor registrado (undo, redo, cancelar, cargar, cortar y unir):
    `NotePitch::restore`;
  - la corrección en sí (corregir afinación, centro de pitch):
    `NotePitch::applyCorrection` y `setCorrectionSource`;
  - convertir en nota cantada nueva (dibujar pitch): `NotePitch::setSungPitch`.
- Las acciones de undo (`Source/Undo/`) están hechas para que el valor de
  referencia no se pueda omitir. Al añadir una acción nueva, recibe siempre la
  referencia anterior y la posterior.
- Si se rompe: el test `SourceGuard_pitch-writes` señala la línea que escribe
  directamente. Un error al manejar la referencia lo detecta
  `VariAudioInspectorTests`.
- Esta comprobación lee línea a línea, así que no ve llamadas por puntero a
  función miembro ni llamadas partidas en varias líneas. No se escriben así.

## 2. Solo AuditionOutput hace sonar algo que no sea la reproducción

- La escucha durante el arrastre, la del teclado y la de un rango tienen una sola
  salida (en el plugin, la ruta de escucha del host; en la app suelta, el bucle de
  escucha de AudioEngine). Solo se hace sonar y se para con `startHostPreview` /
  `playLoop` / `stopLoop` / `stopHostPreview` de
  `Source/UI/Main/AuditionOutput.h`.
- Al empezar a sonar se le quita la salida al dueño anterior (`claim`; al anterior
  se le llama lo registrado con `setOnTakenOver`). El dueño nuevo que también
  haga bucle sustituye el bucle anterior; si no (escucha de un rango), para el
  anterior. Reproducir, pausar y parar llaman antes a `yieldToTransport`.
- Al añadir un tipo de escucha nuevo, se añade antes de `AuditionOutput::Source`
  `Count` y, si hace bucle, también a `playsLoop`. El tratamiento de «me quitaron
  la salida» se registra al construir MainComponent.
- Las cuatro llamadas al host (inicio y fin de la escucha de rango, reproducción y
  parada del bucle) solo existen dentro de AuditionOutput. MainComponent no las
  llama directamente.
- Si se rompe: el test `SourceGuard_audition-output` señala la línea que llama al
  bucle de escucha desde fuera de AuditionOutput y AudioEngine (lee línea a línea,
  con la misma limitación que §1). Un error de traspaso lo detectan
  `AuditionOutputTests` (incluye la ruta de la app suelta) y `AuditionHandoffTests`
  (con el MainComponent real).

## 3. No se guarda el proyecto: se pregunta cada vez

- Hay más de una decena de caminos por los que cambia el proyecto (abrir archivo,
  restaurar una instantánea, ARA…). Ningún componente de la pantalla guarda un
  puntero al proyecto: se pregunta cada vez con `getCurrentProject` o se engancha
  a `parameterPanel.onProjectBound`.
- Si se rompe: no hay test automático. Se nota porque la tonalidad sale «-» o
  aparece el valor de otro evento.

## 4. Los punteros a notas se tiran en cada edición

- Después de undo, redo, unir o cortar, la lista de notas se reconstruye. Los
  punteros a la nota bajo el ratón o a sus manijas se tiran con
  `PianoRollComponent::dropTransientNotePointers()` (se llama desde
  `MainComponent::afterHistoryChange`).
- Si se rompe: se toca una nota que ya no existe y la app cae.

## 5. Cómo se leen la tonalidad y la escala

- La tonalidad de la armonía y de la escala del editor se lee con
  `Project::getPreferredScaleMode()` / `getScaleRootNote()`. `getScaleMode()` es
  el ajuste del imán al arrastrar y puede devolver cromático.
- La única entrada del usuario para cambiar la tonalidad es la escala del editor
  en el inspector izquierdo (`ParameterPanel` solo escribe al ajustar una raíz sin
  configurar).

## 6. Releer la pantalla del inspector

- El inspector relee la pantalla 8 veces por segundo. Al releer un desplegable se
  pasa por `VariAudioInspector::showMenuValue` (no se toca mientras el desplegable
  está abierto ni cuando el valor es distinto del último que escribió el propio
  inspector). Llamar directamente a `setSelectedId` hace desaparecer el valor
  elegido.

## 7. Escribir código que también compile en Windows

- No se lee una `constexpr` local dentro de una lambda sin captura `[]` (el
  compilador de Windows, MSVC, no lo acepta). Se usa `[&]`.
- Si se rompe: cae `windows-build` en la comprobación automática (CI) de GitHub.
- El plugin de Windows enlaza estáticamente la biblioteca de ejecución de C++
  (runtime de MSVC) (`CMAKE_MSVC_RUNTIME_LIBRARY` en `CMakeLists.txt`; se ajusta
  antes de crear los objetivos). Con el runtime compartido, el DAW usa un
  `msvcp140.dll` viejo de su propia carpeta y un `std::mutex` creado con un
  compilador nuevo cae en el primer bloqueo (aparece en la lista, pero cae al
  insertarlo). El DLL de ONNX Runtime sigue usando el runtime compartido, porque
  Microsoft lo prepara para funcionar también con runtimes viejos.
- Si se rompe: en CI cae `tools/windows/check-vst3.ps1` dentro de `windows-build`
  (el plugin declara que carga `msvcp140.dll` / `vcruntime140.dll`, o no se puede
  crear con el runtime de Visual C++ 14.29). El flujo de release de Windows
  tampoco adjunta el zip si no pasa esa misma comprobación.

## 8. Si cambia un texto, se reconstruye el binario

- `THIRD_PARTY_NOTICES.txt` y `Resources/lang/*.json` van incrustados en el
  plugin. Si cambian, se reconstruyen las versiones de Mac y Windows y se
  sustituyen los artefactos de distribución.

## 9. No dejar rutas de la Mac de compilación en el binario distribuido

- Al enlazar, la ubicación de ONNX Runtime dentro de la carpeta de compilación
  (ruta absoluta con el nombre de usuario) queda registrada como rpath (dónde se
  buscan las bibliotecas). El bundle de Mac borra los rpath absolutos con
  `cmake/StripBuildRpaths.cmake` y falla la compilación si queda alguno.
- Al añadir un objetivo nuevo para Mac (bundle), se añade el mismo tratamiento en
  POST_BUILD.
- Cómo se comprueba: `otool -l <binario> | grep -A2 LC_RPATH` no debe mostrar
  ninguna ruta que empiece por `/`. `strings` no muestra el rpath; se comprueba
  así.

## 10. La configuración del motor de síntesis se comparte una sola vez dentro del plugin

- Aunque el plugin (`Source/Plugin`) tenga varios `EditorController`, todos
  comparten un único `SynthesisEnginePreference` con el ajuste del motor de
  síntesis (resíntesis IA / clásico). Se crean siempre por
  `makeSharedEngineController` de `Source/Plugin/RenderControllers.h` (en el lado
  de proceso, `PitchNetAudioProcessor::makeRenderController`). No se escribe
  directamente `std::make_unique<EditorController>(false)`.
- El motor se lee y se escribe solo con `EditorController::getSynthesisEngineType`
  / `setSynthesisEngineType`. Mientras es compartido se lee y escribe el valor
  compartido, no una variable miembro. El valor que se pasa a
  `IncrementalSynthesizer` justo antes de sintetizar también sale de ese getter.
- Al cambiar de motor (los radios del panel derecho) se guarda el ajuste y
  después se llama a `MainComponent::rerenderEditedNotes`. La lista de notas cuya
  edición llega al audio la decide `Project::markEditedNotesDirty`; no se recorre
  `markDirty` a mano.
- El inicio y el fin de la síntesis pasan por `MainComponent::showRenderStatus`
  (la línea de estado del panel derecho). Al añadir un camino que empiece una
  síntesis, se llama en el inicio y en el fin.
- Si se rompe: el test `SourceGuard_render-controllers` señala la línea de
  `Source/Plugin` que escribe `std::make_unique<EditorController>(false)` (fuera
  de `RenderControllers.h`). Esta comprobación solo ve la forma en que se pasa el
  literal `false` a `make_unique`. `new EditorController(false)`, pasar una
  variable, `(true)` (el error de crear dentro del plugin uno con dispositivo de
  audio) o partir `(` y `false` en dos líneas pasan la comprobación, así que no se
  escriben así. El traspaso del valor compartido lo detecta
  `SynthesisEngineShareTests`. Olvidar la línea de estado no tiene test
  automático: se nota porque se queda en «sintetizando» o muestra lo anterior.

## 11. Cómo se avanza un cambio

- Al arreglar un fallo, primero se añade un test que falle antes del arreglo.
- Antes del commit pasan todos los tests (`ctest --test-dir build`) y pluginval
  (severidad 5). Los tests que enlazan `pitchnet_ui` se compilan enteros antes de
  ejecutarlos si se tocó la UI.
- Al enviar a GitHub se comprueba que pasa la comprobación automática (CI).
- Al cambiar la carga de modelos, la inferencia o la síntesis (FCPE, GAME, modelo
  de voz, PSOLA) se comprueba que pasa `tests/ModelInferenceTests.cpp`. Es el
  único test de modelos que corre también en el job de Windows de CI, y es la
  única prueba de que análisis y síntesis funcionan en Windows. Al añadir un
  modelo o motor de síntesis se añade también su etapa a ese test.
- Los archivos de código nuevos llevan al principio las tres líneas de copyright
  y licencia de TuneX (el mismo texto que los archivos nuevos existentes).
- Cuando llega una URL de GitHub para implementar o portar, al añadir una
  dependencia nueva o al traer código de un fork de terceros (p. ej.
  Rowryu/tunex), primero se hace la comprobación de seguridad de §14 y se informa
  el veredicto de tres niveles antes de mover nada.

## 12. ONNX Runtime solo se llama si se pudo cargar

- ONNX Runtime (el motor de inferencia de IA) se carga solo desde el que va
  incluido en el plugin, con `OnnxRuntime::initialise` indicando la ubicación
  (macOS: `Frameworks/libonnxruntime.dylib`; Windows: `onnxruntime.dll` en la
  misma carpeta que el plugin). `ORT_API_MANUAL_INIT` está definido en todas las
  plataformas. Nunca se cambia al `onnxruntime.dll` viejo de System32 de Windows.
  El plugin está hecho para no tener ni una sola importación de
  `onnxruntime.dll`, y por eso aparece LNK4199 (no hay destino de carga
  diferida) al enlazar en Windows. Aun así, por si ocurre una carga diferida, el
  hook de `OnnxRuntimeLoader.cpp` impide que la carga auxiliar busque el DLL (si
  lo busca, lee el viejo de System32).
- Cuando no se puede cargar (en Windows casi siempre porque falta el paquete
  redistribuible de Visual C++), los punteros de la API quedan vacíos. Llamar a
  un solo `Ort::` en ese estado hace caer la app. Donde se crea `Ort::Env` y donde
  se llama a ONNX Runtime (`Ort::GetAvailableProviders`, etc.) se comprueba antes
  `OnnxRuntime::isAvailable()` (hoy son 6 sitios: FCPE, RMVPE, GAME, síntesis de
  voz, lista de GPU y pantalla de ajustes). La entrada del análisis
  `EditorController::analyzeAudio` y el primer editor que se abre muestran cómo
  arreglarlo con `OnnxRuntime::showUnavailableMessageOnce()` (una vez por
  proceso; el host ARA analiza por clip, y mostrarlo cada vez apilaría varios
  avisos). El motivo queda en el registro (en Windows, en la salida del depurador).
- Al añadir un sitio nuevo que llame a ONNX Runtime, se comprueba antes igual y se
  añade esa entrada a `tests/OnnxRuntimeAvailabilityTests.cpp`.
- Si se rompe: cae `OnnxRuntimeAvailabilityTests` (vacía la API y pasa por cada
  entrada, así que falla donde se olvidó comprobar; la parte de DirectML de la
  lista de GPU solo pasa en la versión DirectML de Windows, no en los tests de
  macOS). En Windows cae la comprobación de CI de `tools/windows/check-vst3.ps1`
  en el contenedor sin runtime.

## 13. El instalador se construye desde el contenido del zip

- El `.pkg` de Mac (`packaging/macos/build-pkg.sh`) y el instalador de Windows
  (`packaging/windows/TuneX.iss`, `.github/workflows/windows-installer.yml`) se
  construyen desde el zip de distribución descomprimido. No se recompila solo para
  el instalador. Por eso se puede decir que el zip y el instalador tienen el mismo
  contenido.
- Mac instala en el home del usuario (el mismo sitio que la instalación manual).
  No pide contraseña de administrador y sobrescribe lo instalado a mano. Si se
  instala en `/Library`, quedan dos copias junto a la de `~/Library` manual.
- En el `.pkg` de Mac, si hay un bundle con el mismo identificador en otro sitio,
  el instalador lo pone allí (relocation). `build-pkg.sh` desactiva
  `BundleIsRelocatable`. Al añadir componentes se pasa por la misma función.
- macOS pone a los archivos creados en esta Mac un atributo que no se puede
  borrar (`com.apple.provenance`), y `pkgbuild` lo mete en el contenido como
  archivos «._». `build-pkg.sh` rehace el contenido con cpio y mkbom para
  quitarlos, y falla si queda alguno en el contenido final.
- El `AppId` de Windows no se cambia. Si se cambia, la versión nueva no encuentra
  la vieja y quedan dos instaladas.
- El `.pkg` de Mac incluye también los documentos de licencia (LICENSE,
  THIRD_PARTY_NOTICES.txt, MODIFICATIONS.md, licenses) (en la misma carpeta que
  los modelos). El zip y el instalador de Windows también los llevan.
- El instalador de Windows pone los modelos en `%PROGRAMDATA%`. El plugin, si
  `%APPDATA%\Keinoaza\TuneX\models` existe «como carpeta», lee solo de ahí aunque
  falten archivos (no vuelve a `%PROGRAMDATA%` archivo por archivo). El README
  pide a quien instala con el instalador que borre la carpeta de `%APPDATA%`. Al
  sacar una versión que cambia modelos, hay que cuidar este orden. El instalador
  no vacía la carpeta de modelos (para no borrar el `rmvpe.onnx` que pone el
  usuario).
- Si se rompe: en Windows cae `tools/windows/check-installer.ps1` de
  `windows-installer.yml` (instala en un contenedor limpio, crea el plugin y lo
  borra). Pero no comprueba hasta cargar los modelos de verdad. En Mac no hay
  comprobación automática. Tras reconstruir, en esta Mac se instala con
  `installer -pkg <pkg> -target CurrentUserHomeDirectory`, se compara con `diff -r`
  contra el contenido del zip y se comprueba que no llevan atributos.

## 14. El código externo se comprueba antes de usarlo y se fija por hash o commit ID

- Qué respetar: el código de repositorios externos, las dependencias y las
  acciones de GitHub Actions pasan por el procedimiento de
  `~/.claude/skills/_shared/external-code-intake.md` (obtener en una carpeta
  vacía → localizar mecanismos que se ejecutan solos → `scan_external_code.sh` →
  fijar dependencias → informar el veredicto de tres niveles) antes de entrar en
  compilación, instalación o tests. Si el veredicto es «con puntos de atención» o
  peor, decide el usuario si se usa.
- Entradas obligatorias: el registro de la comprobación está en `verify-map.md`
  (fecha, commit ID, veredicto, puntos de atención) y la licencia y procedencia en
  `THIRD_PARTY_NOTICES.txt` / `docs/source-ledger.md`.
- El SHA-256 de los archivos que descarga CMake (ONNX Runtime, NuGet de DirectML,
  modelos GAME) tiene su fuente de verdad en la tabla de
  `cmake/DependencyHashes.cmake`, y la descarga se verifica siempre con el valor
  de la tabla. Al subir `ONNXRUNTIME_VERSION`, `DIRECTML_VERSION` o
  `GAME_MODELS_VERSION`, se descarga el archivo nuevo dos veces y se comprueba que
  coincide antes de actualizar la tabla en el mismo commit. Las variantes que no
  están en la tabla (GPU de Linux, la CUDA 13 que no existe) detienen la
  configuración salvo que se pase `-DPITCHNET_ALLOW_UNPINNED_DOWNLOADS=ON`. Al
  sustituir `GAME_MODELS_URL` se pasa también `GAME_MODELS_SHA256`.
- Los `uses:` de GitHub Actions se indican con el commit ID de 40 dígitos y se
  deja el nombre de la versión en un comentario al final de la línea. No se usan
  etiquetas (`@v7`) ni nombres de rama. Al subir de versión, el ID se obtiene con
  `gh api repos/<owner>/<repo>/git/ref/tags/<versión>`.
- Los submódulos (JUCE, ARA_SDK, r8brain) están fijados por commit ID. Al
  avanzarlos se escribe el cambio de `git submodule status` en la descripción del
  commit.
- Si se rompe: si el contenido descargado no coincide con la tabla, la
  configuración se detiene con «hash mismatch». Olvidar actualizar la tabla
  detiene en el mismo sitio. Las etiquetas en las acciones no tienen test
  automático y se encuentran con
  `grep -rn 'uses:' .github/workflows | grep -v '@[0-9a-f]\{40\}'` (las
  referencias locales `./.github/workflows/…` quedan fuera).
