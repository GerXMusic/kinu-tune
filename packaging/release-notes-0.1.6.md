# TuneX 0.1.6

TuneX es un editor monofónico de afinación tipo Melodyne para VST3/ARA2, gratis y de
código abierto (AGPL-3.0). Esta versión añade **eliminar nota** (M5 v2), limpia la
instalación de macOS y añade un zip de modelos para el VST3 suelto.

## Qué hay de nuevo

- **Eliminar nota (D-299).** La nota desaparece y su tramo queda en **silencio**, con un
  fade corto; no se revela el audio original y las vecinas no se mueven ni se estiran.
  Undo/redo restaura la nota, su selección y su audio. La eliminación se guarda con el
  proyecto y sobrevive a reabrir. Se mide en `NoteDeleteTests`: tramo eliminado
  **−180 dBFS**, vecina diff **0**, undo dentro de 1 dB.
- **Actualización de macOS sin backups dentro del bundle (D-300).** El `.pkg` borra el
  `TuneX.vst3`/`TuneX.component` anterior antes de instalar, así que un
  `TuneX.old.vst3` de una instalación manual no sobrevive. El check del `.pkg` (instalar
  en un runner limpio + `pluginval` strictness 5) pasa a formar parte de
  `macos-pkg.yml`; el `.pkg` no se sube como artefacto final si falla.
- **Zip de modelos para el VST3 suelto (D-301).** `TuneX-0.1.6-Windows-x64-models.zip`
  lleva `models/`, las licencias y `INSTALL-Windows-models.txt`. No cambia el `.exe`, que
  sigue incluyendo los modelos.
- **Versión 0.1.6 (D-294).** Sube el patch porque cambian el audio (eliminar nota) y el
  empaquetado.

## Archivos

| plataforma | archivo | contenido |
|---|---|---|
| Windows x64 | `TuneX-0.1.6-Windows-x64-Setup.exe` | VST3 DirectML, modelos, licencias, VC++ Redistributable |
| macOS arm64 | `TuneX-0.1.6-macOS-arm64.pkg` | VST3, AU, modelos y licencias; instala en el home del usuario |
| Windows x64 | `TuneX-0.1.6-Windows-x64-VST3.zip` | solo el VST3; **no** incluye modelos |
| Windows x64 | `TuneX-0.1.6-Windows-x64-models.zip` | modelos, licencias y `INSTALL-Windows-models.txt`; para el VST3 suelto |

El VST3 del `.exe` y el del zip son el mismo archivo (SHA pendiente de la construcción
final).

## Instalación

### Windows x64

1. Ejecuta `TuneX-0.1.6-Windows-x64-Setup.exe`.
2. SmartScreen avisará porque el instalador **no está firmado**: `Más información` →
   `Ejecutar de todas formas`.
3. El instalador deja el VST3 en `C:\Program Files\Common Files\VST3\TuneX.vst3`, los modelos
   en `C:\ProgramData\GerX\TuneX\models` y las licencias en `C:\Program Files\GerX\TuneX`.
4. Reinicia el DAW y vuelve a escanear plugins.

Si ya tenías una instalación manual con modelos en `%APPDATA%\GerX\TuneX\models`, el plugin
seguirá usando esa carpeta antes que la del instalador. Bórrala si quieres usar los modelos
del instalador.

### macOS arm64 (Apple Silicon)

1. Abre `TuneX-0.1.6-macOS-arm64.pkg`.
2. Gatekeeper avisará porque el paquete **no está firmado ni notarizado**. Ábrelo con clic
   derecho → `Abrir`, o desde `Ajustes del Sistema` → `Privacidad y seguridad` →
   `Abrir igualmente`.
3. El paquete instala en el home del usuario, sin contraseña de administrador:
   - `~/Library/Audio/Plug-Ins/VST3/TuneX.vst3`
   - `~/Library/Audio/Plug-Ins/Components/TuneX.component`
   - `~/Library/Application Support/GerX/TuneX/` (modelos y licencias)
4. Reinicia el DAW y vuelve a escanear plugins.

**ARA en Mac:** el camino ARA es el **VST3**. REAPER no soporta ARA en AU y Logic en Apple
Silicon nativo ejecuta los AU fuera de proceso, sin ARA. El AU se incluye para quien lo use
sin ARA.

### VST3 suelto (Windows)

`TuneX-0.1.6-Windows-x64-VST3.zip` contiene solo el bundle. Necesita los modelos de
`TuneX-0.1.6-Windows-x64-models.zip` en `%APPDATA%\GerX\TuneX\models` (instalación manual)
o en `%PROGRAMDATA%\GerX\TuneX\models`. Si no los tienes, usa el instalador `.exe`, que ya
los lleva. El VST3 del zip es el mismo que instala el `.exe`.

## Aviso de firma (D-298)

Los artefactos de 0.1.6 se publican **sin firma de código ni notarización**. Windows
SmartScreen y macOS Gatekeeper avisarán; es esperado. Los bundles de macOS van firmados
**ad hoc** (para que carguen), pero el `.pkg` no tiene firma de desarrollador. No hay
certificados en el proyecto; la firma queda fuera del alcance por ahora.

## Límites conocidos

- **Play a mitad del render (D-293).** Se puede oír menos de un segundo del audio anterior
  mientras el host agota su *render-ahead*. Espera a la barra o a `Published`.
- **Pitchcore no retimea.** El core del motor Pitchcore ignora `sourceFrame`; la cuantización
  de tiempo se escucha con PSOLA y con el vocoder.
- **Dos GPUs idénticas** se cuentan como una en la política de motor por defecto.
- **Vocoder y bloques (D-239).** Editar una nota con el vocoder reconstruye su bloque de
  motor; la nota vecina puede sonar distinta porque su salida depende de la ventana de
  contexto.
- **Eliminar nota** calla el `[start, end)` de la nota; la cola fuera de ese tramo pertenece
  a la nota siguiente o a la región. Re-detectar notas descarta las lápidas.
- **macOS AU y ARA.** La etiqueta ARA está en el AU, pero REAPER no ofrece ARA para AU y
  Logic en Apple Silicon nativo tampoco. Usa el VST3 para ARA.

## Verificación de esta publicación

Pendiente de rellenar con los runs definitivos de 0.1.6:

- **Windows x64.** Run `—`: VST3 DirectML, `check-vst3.ps1`, `check-installer.ps1
  -ExpectedSha`, instalación limpia, modelos, licencias, un solo `.vst3`, sin backups (se
  siembra `TuneX.old.vst3`), carga y desinstalación. SHA del VST3 instalado: `—`.
- **macOS arm64.** Run `—`: build + check en `macos-pkg.yml`, instalación en el home de un
  runner limpio, VST3/AU/modelos/licencias, etiqueta ARA, `moduleinfo` 0.1.6,
  `pluginval` strictness 5 **SUCCESS**, y el bundle/backup sembrado no sobrevive. `pkgutil
  --check-signature` → `no signature`.
- **SHA-256.** En `SHA256SUMS.txt`.

## Licencias

- Código: **AGPL-3.0**. El proyecto es libre y gratuito; **no se vende**.
- Vocoder PC-NSF-HiFiGAN: pesos **CC BY-NC-SA 4.0**, incluidos en los instaladores y en el
  zip de modelos. No se pueden vender ni usar como parte de un producto o servicio de pago.
- FCPE: MIT. ARA SDK: Apache-2.0. JUCE: AGPL. El resto de textos está en `licenses/` y
  `THIRD_PARTY_NOTICES.txt`.

Repositorio: https://github.com/GerXMusic/tunex · Problemas:
https://github.com/GerXMusic/tunex/issues
