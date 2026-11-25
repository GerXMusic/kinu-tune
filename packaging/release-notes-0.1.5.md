# TuneX 0.1.5

TuneX es un editor monofónico de afinación tipo Melodyne para VST3/ARA2, gratis y de
código abierto (AGPL-3.0). Esta versión cierra el último límite conocido del plan de render
y hace que el instalador de Windows lleve **exactamente** el mismo VST3 que el zip suelto.

## Qué hay de nuevo

- **El motor global forma parte de la instantánea del render (D-296).** `renderPlan::Plan`
  guarda el motor global con el que arrancó la cadena y cada pase usa ese valor. Un cambio
  de la preferencia compartida con una cadena en vuelo ya no cambia sus bloques restantes.
  El transitorio que quedaba abierto en D-292 desaparece; la cadena siguiente sigue usando
  el motor nuevo y converge.
- **El instalador sale del zip que se prueba (D-297).** El workflow de Windows compila el
  VST3 DirectML una vez, lo valida, lo comprime y arma el `.exe` **desde ese zip**. El VST3
  instalado tiene el mismo SHA que el zip suelto, y `check-installer.ps1` lo exige.
- **Versión 0.1.5 (D-294).** Sube el patch porque cambian el render y el empaquetado.

## Archivos

| plataforma | archivo | contenido |
|---|---|---|
| Windows x64 | `TuneX-0.1.5-Windows-x64-Setup.exe` | VST3 DirectML, modelos, licencias, VC++ Redistributable |
| macOS arm64 | `TuneX-0.1.5-macOS-arm64.pkg` | VST3, AU, modelos y licencias; instala en el home del usuario |
| Windows x64 | `TuneX-0.1.5-Windows-x64-VST3.zip` | solo el VST3; **no** incluye modelos |

El VST3 del `.exe` y el del zip son el mismo archivo:
`60ADDBA1CF1D809111AB0B48B4FA09519961367603F73318C1076F12C8C7407A`.

## Instalación

### Windows x64

1. Ejecuta `TuneX-0.1.5-Windows-x64-Setup.exe`.
2. SmartScreen avisará porque el instalador **no está firmado**: `Más información` →
   `Ejecutar de todas formas`.
3. El instalador deja el VST3 en `C:\Program Files\Common Files\VST3\TuneX.vst3`, los modelos
   en `C:\ProgramData\GerX\TuneX\models` y las licencias en `C:\Program Files\GerX\TuneX`.
4. Reinicia el DAW y vuelve a escanear plugins.

Si ya tenías una instalación manual con modelos en `%APPDATA%\GerX\TuneX\models`, el plugin
seguirá usando esa carpeta antes que la del instalador. Bórrala si quieres usar los modelos
del instalador.

### macOS arm64 (Apple Silicon)

1. Abre `TuneX-0.1.5-macOS-arm64.pkg`.
2. Gatekeeper avisará porque el paquete **no está firmado ni notarizado**. Ábrelo con clic
   derecho → `Abrir`, o desde `Ajustes del Sistema` → `Privacidad y seguridad` →
   `Abrir igualmente`. Los plugins instalados no quedan en cuarentena.
3. El paquete instala en el home del usuario, sin contraseña de administrador:
   - `~/Library/Audio/Plug-Ins/VST3/TuneX.vst3`
   - `~/Library/Audio/Plug-Ins/Components/TuneX.component`
   - `~/Library/Application Support/GerX/TuneX/` (modelos y licencias)
4. Reinicia el DAW y vuelve a escanear plugins.

**ARA en Mac:** el camino ARA es el **VST3**. REAPER no soporta ARA en AU y Logic en Apple
Silicon nativo ejecuta los AU fuera de proceso, sin ARA. El AU se incluye para quien lo use
sin ARA.

### VST3 suelto (Windows)

`TuneX-0.1.5-Windows-x64-VST3.zip` contiene solo el bundle. Necesita los modelos en
`%APPDATA%\GerX\TuneX\models` (instalación manual) o en `%PROGRAMDATA%\GerX\TuneX\models`.
Si no los tienes, usa el instalador `.exe`, que ya los lleva. El VST3 del zip es el mismo
que instala el `.exe`.

## Aviso de firma (D-298)

Los artefactos de 0.1.5 se publican **sin firma de código ni notarización**. Windows
SmartScreen y macOS Gatekeeper avisarán; es esperado. Los bundles de macOS van firmados
**ad hoc** (para que carguen), pero el `.pkg` no tiene firma de desarrollador. La firma y la
notarización quedan para una versión posterior, cuando existan certificados.

## Límites conocidos

- **Play a mitad del render (D-293).** Se puede oír menos de un segundo del audio anterior
  mientras el host agota su *render-ahead*. Espera a la barra o a `Published`.
- **Pitchcore no retimea.** El core del motor Pitchcore ignora `sourceFrame`; la cuantización
  de tiempo se escucha con PSOLA y con el vocoder.
- **Dos GPUs idénticas** se cuentan como una en la política de motor por defecto.
- **Vocoder y bloques (D-239).** Editar una nota con el vocoder reconstruye su bloque de
  motor; la nota vecina puede sonar distinta porque su salida depende de la ventana de
  contexto.
- **macOS AU y ARA.** La etiqueta ARA está en el AU, pero REAPER no ofrece ARA para AU y
  Logic en Apple Silicon nativo tampoco. Usa el VST3 para ARA.

El límite de D-292 («el motor global se lee por pase») queda **cerrado** en 0.1.5 por D-296.

## Verificación de esta publicación

- **Windows x64.** Run `45560066451` **verde** (24m25s): el VST3 DirectML se compiló una vez,
  `check-vst3.ps1` pasó sobre el VST3 del zip (pluginval y máquina limpia) y
  `check-installer.ps1 -ExpectedSha` pasó sobre el `.exe`: instalación silenciosa, VC++
  redistribuible, modelos, licencias, un solo `.vst3`, ningún backup (se sembró un
  `TuneX.old.vst3`), carga en el host mínimo y desinstalación. El VST3 instalado es
  `60ADDBA1…`, idéntico al del zip.
- **macOS arm64.** El `.pkg` se construyó en el run `45560070108` y se verificó en el run
  `45561734615`: instalación en el home de un runner `macos-15` limpio, VST3/AU/modelos/
  licencias, etiqueta ARA, `moduleinfo` 0.1.5 y `pluginval` *strictness 5* → **SUCCESS**.
  `pkgutil --check-signature` → `no signature`; los bundles son `adhoc`.
- **SHA-256.** En `SHA256SUMS.txt`; los tres archivos se verificaron localmente antes de
  subirlos.

## Licencias

- Código: **AGPL-3.0**. El proyecto es libre y gratuito; **no se vende**.
- Vocoder PC-NSF-HiFiGAN: pesos **CC BY-NC-SA 4.0**, incluidos en los instaladores. No se
  pueden vender ni usar como parte de un producto o servicio de pago.
- FCPE: MIT. ARA SDK: Apache-2.0. JUCE: AGPL. El resto de textos está en `licenses/` y
  `THIRD_PARTY_NOTICES.txt`.

Repositorio: https://github.com/GerXMusic/tunex · Problemas:
https://github.com/GerXMusic/tunex/issues
