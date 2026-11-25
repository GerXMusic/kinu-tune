<div align="center">
  <h1>TuneX</h1>
  <p>Editor de afinación vocal con controles estilo VariAudio (Cubase), como plugin VST3 / ARA 2.<br>
  A vocal pitch editor with VariAudio-style controls, shipped as a VST3 / ARA 2 plug-in.</p>
</div>

## Origen, atribución y licencia

TuneX es un **fork de PitchNet 0.7.2** (Session Loops), que a su vez deriva de
**HachiTune** (KCKT0112 y colaboradores). La cadena real es:

1. **HachiTune** — proyecto original.
2. **PitchNet 0.7.2** — Session Loops, commit `8787321`.
3. **TuneX (fork de Keinoaza)** — 2026-10-19 a 2026-10-22, versiones 0.1.0–0.1.3.
4. **TuneX (GerX)** — desde 2026-11-22: identidad propia, compilación y validación
   en macOS por CI, empaquetado de modelos, migración de la carpeta de modelos y
   cambios de editor y motor.

- Copyright (C) 2026 **GerX** (cambios propios)
- Copyright (C) 2026 **Keinoaza** (fork anterior)
- Copyright (C) 2026 **Session Loops** y colaboradores (PitchNet)
- Copyright de los colaboradores de **HachiTune** (HachiTune)

TuneX es software libre bajo **GNU Affero General Public License v3** (ver
[LICENSE](LICENSE)), **sin garantía de ningún tipo**. Al distribuir copias o
versiones modificadas hay que conservar los avisos de copyright, licencia y
atribución, y ofrecer el código fuente correspondiente, como exige la AGPLv3.
La lista de cambios está en [MODIFICATIONS.md](MODIFICATIONS.md) y las licencias
de componentes y modelos en [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).

TuneX no está afiliado ni respaldado por Session Loops, los colaboradores de
HachiTune, OpenVPI, Steinberg, Celemony ni Apple. Cubase y VariAudio son marcas
de Steinberg Media Technologies GmbH. VST es una marca registrada de Steinberg.
Melodyne y ARA son marcas de Celemony Software GmbH. Logic Pro y Audio Units son
marcas de Apple Inc.

## Qué hace

Editor monofónico de afinación pensado para quien viene de VariAudio: los
segmentos son rectángulos, la curva de tono se edita por nota y el objetivo
principal es sacar armonías rápido duplicando una pista de voz.

- Segmentos con nombre de nota y controles al pasar el ratón: enderezar la curva,
  corregir el tono, unir, inclinar, formantes y volumen.
- Modos de imán de tono (absoluto / relativo / apagado) con modificadores durante
  el arrastre y edición con flechas.
- Inspector izquierdo: herramientas (corregir tono, aplanar curva, deriva de tono,
  desplazar formantes, volumen), escala del editor y armonía.
- Armonía a un clic: tercera, cuarta, quinta, sexta u octava, arriba o abajo; la
  tercera mayor o menor la decide la escala elegida.
- La curva original de una nota editada se queda visible en gris; doble clic
  restablece esa nota.

## Requisitos

- **macOS 13 o superior en Apple silicon (arm64).** La build publicada no corre en
  Macs Intel.
- **Windows 10/11 (x64).** La build de Windows se compila y se valida en CI; en un
  DAW real de Windows la prueba todavía es manual.
- Un host con **ARA 2** (Cubase y similares). Sin ARA funciona como inserto normal
  en modo captura. La entrega de macOS incluye VST3 y Audio Unit. **En Mac el ARA va
  por VST3:** REAPER no soporta ARA en AU (su ARA es VST3), y Logic en Apple Silicon
  nativo ejecuta los AU fuera de proceso y no soporta ARA; para ARA en Logic haría
  falta Rosetta y un AU x86_64, que esta build arm64 no incluye. El AU sirve como
  inserto normal en hosts AU; Logic Pro no está probado.

## Instalación

### Desde el instalador

**macOS** (`TuneX-<versión>-macOS-arm64.pkg`):

1. Abre el paquete descargado.
2. Si macOS dice que no se puede abrir, ve a *Ajustes del Sistema > Privacidad y
   seguridad* y pulsa «Abrir igualmente»; vuelve a abrirlo (la entrega no está
   notarizada).
3. «Continuar» e «Instalar». No hace falta contraseña de administrador. Si no
   quieres el Audio Unit, quítalo en «Personalizar» (en Mac el ARA se usa con el
   VST3; el AU queda para inserto normal en hosts AU).
4. Reinicia el DAW y vuelve a escanear plugins.

Para desinstalar: borra `~/Library/Audio/Plug-Ins/VST3/TuneX.vst3`,
`~/Library/Audio/Plug-Ins/Components/TuneX.component` y
`~/Library/Application Support/GerX/TuneX` (modelos y textos de licencia).

**Windows** (`TuneX-<versión>-Windows-x64-Setup.exe`):

1. Si antes instalaste a mano desde un ZIP, borra primero
   `%APPDATA%\GerX\TuneX` y `%APPDATA%\Keinoaza\TuneX`.
2. Abre el instalador.
3. Si Windows muestra «Windows protegió tu PC», pulsa «Más información» y luego
   «Ejecutar de todas formas» (el instalador no está firmado).
4. Sigue los pasos. También instala el Microsoft Visual C++ Redistributable que
   necesita el motor de IA.
5. Reinicia el DAW y vuelve a escanear plugins.

Para desinstalar: *Configuración > Aplicaciones > TuneX*.

### A mano desde el ZIP

**macOS:**

1. Descomprime `TuneX-<versión>-macOS-arm64.zip`.
2. Copia `TuneX.vst3` a `~/Library/Audio/Plug-Ins/VST3/`. Para el Audio Unit,
   copia `TuneX.component` a `~/Library/Audio/Plug-Ins/Components/` (el AU es
   para inserto normal; el ARA en Mac se usa con el VST3).
3. Crea la carpeta de modelos: en Finder, *Ir > Ir a la carpeta…*, escribe
   `~/Library/Application Support`, crea dentro `GerX` y, dentro, `TuneX`.
4. Mueve la carpeta `models` del ZIP a esa carpeta `TuneX`, de modo que exista
   `~/Library/Application Support/GerX/TuneX/models/fcpe.onnx`.
5. Reinicia el DAW y vuelve a escanear.
6. Si sigue bloqueado, ejecuta esto en Terminal y reinicia el DAW (si no
   instalaste el AU, el aviso de «No such file» en Components es inofensivo):

   ```bash
   xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/TuneX.vst3 ~/Library/Audio/Plug-Ins/Components/TuneX.component
   ```

**Windows:**

1. Copia la carpeta `TuneX.vst3` a `C:\Program Files\Common Files\VST3\`.
2. Crea la carpeta de modelos: escribe `%APPDATA%` en la barra de direcciones del
   Explorador, crea dentro `GerX` y, dentro, `TuneX`.
3. Mueve la carpeta `models` del ZIP a esa carpeta `TuneX`, de modo que exista
   `%APPDATA%\GerX\TuneX\models\fcpe.onnx`.
4. Reinicia el DAW y vuelve a escanear.

Si el plugin cae al insertarlo o no empieza a analizar, instala el
[Microsoft Visual C++ Redistributable x64](https://aka.ms/vc14/vc_redist.x64.exe)
y reinicia el DAW: lo necesita ONNX Runtime. Si falta, el plugin lo avisa y
desactiva el análisis.

> **Nota de migración.** Hasta 2026-11-22 la carpeta de modelos era
> `…\Keinoaza\TuneX`. Las builds nuevas buscan primero en `…\GerX\TuneX` y, si ahí
> no hay modelos, siguen leyendo la carpeta antigua. No hace falta mover nada para
> que siga funcionando, pero lo recomendable es moverla a `GerX` y borrar la
> antigua.

## Armonías en Cubase (flujo rápido)

1. Duplica la pista de voz y selecciona el evento de audio de la copia.
2. *Audio > Extensiones > TuneX* para analizar.
3. Elige la tonalidad de la canción en la escala del editor (izquierda).
4. En la sección de armonía pulsa, por ejemplo, «3.ª arriba»: sin selección actúa
   sobre todas las notas; con selección, solo sobre las seleccionadas.

## Atajos

Las teclas son las de Mac. En Windows, Cmd es Ctrl y Option es Alt.

**Teclado**

| Tecla | Acción |
|---|---|
| ↑ / ↓ | Mueve el tono de las notas seleccionadas (según el modo de imán) |
| Shift + ↑ / ↓ | Mueve de cent en cent (ignora el imán) |
| ← / → | Pasa la selección a la nota anterior / siguiente |
| 1 / 2 / 3 / 4 | Herramienta: seleccionar / dividir / dibujar tono / tiempo |
| Shift + F | Vista completa (todo el tiempo y todo el registro cantado) |
| Cmd + Z | Deshacer |
| Shift + Cmd + Z, Cmd + Y | Rehacer |
| Cmd + A | Seleccionar todo |
| Shift + Cmd + D | Mostrar u ocultar el tono delta (lo editado) |
| Shift + Cmd + B | Mostrar u ocultar el tono base |
| Cmd + , | Ajustes |
| Cmd + O / Cmd + S / Shift + Cmd + S / Cmd + E | Abrir / guardar / guardar como / exportar audio (solo app) |
| Space | Reproducir / pausar (solo app; en el plugin manda el host) |
| Esc | Parar |
| Home / End | Principio / final |
| Cmd + Q | Salir (solo app) |

**Ratón (herramienta de selección)**

| Acción | Efecto |
|---|---|
| Clic | Selecciona solo esa nota |
| Shift + clic | Añade a la selección (se arrastran juntas) |
| Cmd + clic | Añade o quita de la selección |
| Shift + doble clic | Selecciona las siguientes notas con la misma altura |
| Doble clic en la nota | Restablece esa nota a su estado de análisis (tono, curva, formantes, volumen, tiempo); un solo deshacer lo revierte |
| Option + clic en la nota | Divide en ese punto |
| Arrastrar la nota arriba/abajo | Mueve el tono; durante el arrastre se escucha en bucle y al soltar se detiene |
| Clic o arrastre en vacío | Mueve la vista / deselecciona |
| Shift + arrastre desde vacío | Selección por rectángulo (también sobre la onda) |

**Modificadores durante el arrastre**

| Tecla mientras arrastras | Imán |
|---|---|
| Shift | Apagado (libre) |
| Cmd | Absoluto (semitono o nota de la escala exactos) |
| Option | Relativo (conserva la desviación original, en semitonos) |

Sin modificador manda el modo del inspector (por defecto, absoluto).

**Zoom y desplazamiento**

| Acción | Efecto |
|---|---|
| Rueda / Shift + rueda | Desplazamiento vertical / horizontal |
| Cmd + rueda | Zoom horizontal (centrado en el puntero) |
| Shift + Cmd + rueda, Option + rueda | Zoom vertical |
| Cmd + arrastre en vacío | Zoom horizontal y vertical |
| Option + arrastre en vacío | Zoom vertical |
| Option + doble clic en vacío | Vista completa (igual que Shift + F) |

Lo que VariAudio tiene y TuneX no: zoom con Option + rectángulo y reducción con
Option + clic (Option se asignó al zoom vertical) y una tecla para borrar notas.

## Modelos de IA

| Modelo | Uso | Licencia |
|---|---|---|
| FCPE | Detección de tono (por defecto) | MIT |
| GAME | Segmentación en notas | Pesos CC BY-NC-SA 4.0 (código MIT) |
| PC-NSF-HiFiGAN | Resíntesis (vocoder) | Pesos CC BY-NC-SA 4.0 (ver `licenses/PC-NSF-HiFiGAN-NOTICE.txt`) |

- Los pesos de GAME y del vocoder son **CC BY-NC-SA 4.0** (no comercial, compartir
  igual). TuneX se distribuye gratis; si vas a usarlo en un producto o servicio de
  pago, usa los motores propios o cambia el vocoder por uno permisivo.
- Los modelos van en la carpeta `models` del ZIP y en `Resources/models` del
  repositorio. Los textos completos de cada licencia están en el ZIP, en
  `licenses/` y en [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).
- **RMVPE** (detector alternativo, opcional) no se distribuye porque su origen no
  declara licencia. Si tienes derecho a obtenerlo, deja `rmvpe.onnx` en la carpeta
  `models` y aparecerá en los ajustes.

## Privacidad

- TuneX no se conecta a internet. La comprobación de actualizaciones de PitchNet
  está desactivada.
- ONNX Runtime (Microsoft) en Windows entrega telemetría a TraceLogging por
  defecto; TuneX la desactiva en cada inicialización. Por cómo está hecho ONNX
  Runtime quedan dos resquicios: un registro de arranque escrito antes de
  desactivarla y la reactivación posterior por parte del sistema. La build de
  macOS de ONNX Runtime no tiene telemetría.

## Compilar

Requisitos: CMake ≥ 3.22, un compilador C++17 y los submódulos del repositorio
(`git submodule update --init --recursive`).

```bash
# macOS arm64
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DPITCHNET_ARA_DIAGNOSTICS=OFF
cmake --build build --config Release --target PitchNetPlugin_VST3
```

```powershell
# Windows x64 con DirectML (por defecto en Windows)
cmake -S . -B build-dml -G "Visual Studio 18 2026" -A x64 `
      -DCMAKE_BUILD_TYPE=Release -DUSE_DIRECTML=ON `
      -DUSE_BUNDLED_DIRECTML_RUNTIME=ON -DUSE_ASIO=OFF
cmake --build build-dml --config Release --target PitchNetPlugin_VST3
ctest --test-dir build-dml -C Release --output-on-failure
```

Detalles útiles:

- `fcpe.onnx`, `cent_table.bin`, `mel_filterbank.bin` y `pc_nsf_hifigan.onnx` van
  en `Resources/models` y son obligatorios: el `configure` se detiene si faltan.
  Los modelos de GAME se descargan solos en el `configure`.
- ONNX Runtime se obtiene por `FetchContent` con SHA-256 fijado: paquete NuGet en
  macOS arm64 y en Windows con DirectML, tarball oficial en el resto.
- `-DPITCHNET_DEV_BUILD=ON` genera `TuneX Dev` con código de plugin `TunD` y bundle
  `com.gerx.tunex.dev`, para convivir con la versión normal.
- La build de macOS la hace GitHub Actions
  (`.github/workflows/macos-plugin.yml`): descarga y verifica el vocoder, firma
  ad-hoc, valida con pluginval *strictness 5* y publica el artefacto.
- La entrega oficial de Windows exige `pluginval` y `ctest` en verde antes de
  empaquetar.

## Límites conocidos

- **Motor Pitchcore sin retimeo**: su núcleo ignora `sourceFrame`, así que con ese
  motor una nota desplazada en el tiempo suena en su posición original.
- **Vocoder sin cifra propia** de exactitud de tiempo (S1/S2); sí la tienen los
  motores PSOLA y clásico.
- **Funciones visibles y deshabilitadas**: eliminar nota, nivelar, gesto propio de
  formantes/amplitud, guías MIDI consumidas por el motor, Bézier de frontera,
  separación por espacio, stretch y tempo map por sección.
- **Linux no soportado** por ahora.
- La entrega no está firmada ni notarizada: en macOS hace falta «Abrir igualmente»
  o `xattr`; en Windows, «Más información > Ejecutar de todas formas».

## Créditos

Gracias a Session Loops (PitchNet), a los colaboradores de HachiTune, a Keinoaza
por el fork del que partimos, a OpenVPI por GAME y por PC-NSF-HiFiGAN, y al
proyecto FCPE por el detector de tono. Los atajos y el flujo de trabajo siguen la
estela de VariAudio, sin relación con Steinberg.

---

## English summary

TuneX is an open-source (AGPLv3) monophonic vocal pitch editor with
VariAudio-style controls, shipped as a VST3 / ARA 2 plug-in. It is a fork of
PitchNet 0.7.2 by Session Loops, itself derived from HachiTune, previously forked
by Keinoaza (TuneX 0.1.0–0.1.4) and now maintained under the **GerX** vendor
identity. Existing sessions keep working: the plug-in codes are unchanged.

Requirements: macOS 13+ on Apple silicon, or Windows 10/11 x64, and an ARA 2 host
for the full editor. Models live in `…/GerX/TuneX/models` (the previous
`…/Keinoaza/TuneX/models` folder is still read as a fallback). The release is
unsigned: use "Open Anyway" on macOS or "More info > Run anyway" on Windows. The
vocoder and GAME model weights are CC BY-NC-SA 4.0, so TuneX may be shared for
free but not sold.
