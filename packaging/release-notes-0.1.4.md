# TuneX 0.1.4

TuneX es un editor monofónico de afinación tipo Melodyne para VST3/ARA2, gratis y de
código abierto (AGPL-3.0). Esta es la primera versión distribuible del prototipo 0.1.x:
Windows x64 y macOS arm64 (Apple Silicon).

## Qué hay de nuevo

- **Plan de render inmutable por cadena (D-292).** El render consume una instantánea de los
  bloques y las notas al arrancar cada cadena; una edición nueva no mezcla su trabajo con el
  plan anterior. Play ya no cancela una cadena en vuelo: suena el último audio publicado y el
  render termina en segundo plano. El caso «cambio de motor + Play + nudge» queda medido sin
  mezcla permanente.
- **La barra mide render/commit (D-293).** Cuando la barra se apaga, el audio nuevo ya está
  publicado en ARA. El hueco de menos de un segundo que se oye si se da Play a mitad del render
  es el *render-ahead* del host (REAPER ya tenía audio viejo por delante), no una publicación
  tardía. Flujo recomendado: cambiar el motor **parado**, esperar a que la barra termine (o a
  `Published`) y entonces dar Play.
- **Versión 0.1.4 (D-294).** La versión es manual en `CMakeLists.txt`; se sube con el artefacto
  que se distribuye.

## Archivos

| plataforma | archivo | contenido |
|---|---|---|
| Windows x64 | `TuneX-0.1.4-Windows-x64-Setup.exe` | VST3 DirectML, modelos, licencias, VC++ Redistributable |
| macOS arm64 | `TuneX-0.1.4-macOS-arm64.pkg` | VST3, AU, modelos y licencias; instala en el home del usuario |
| Windows x64 | `TuneX-0.1.4-Windows-x64-VST3.zip` | solo el VST3; **no** incluye modelos |

## Instalación

### Windows x64

1. Ejecuta `TuneX-0.1.4-Windows-x64-Setup.exe`.
2. SmartScreen avisará porque el instalador **no está firmado**: `Más información` →
   `Ejecutar de todas formas`.
3. El instalador deja el VST3 en `C:\Program Files\Common Files\VST3\TuneX.vst3`, los modelos
   en `C:\ProgramData\GerX\TuneX\models` y las licencias en `C:\Program Files\GerX\TuneX`.
4. Reinicia el DAW y vuelve a escanear plugins.

Si ya tenías una instalación manual con modelos en `%APPDATA%\GerX\TuneX\models`, el plugin
seguirá usando esa carpeta antes que la del instalador. Bórrala si quieres usar los modelos
del instalador.

### macOS arm64 (Apple Silicon)

1. Abre `TuneX-0.1.4-macOS-arm64.pkg`.
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

`TuneX-0.1.4-Windows-x64-VST3.zip` contiene solo el bundle. Necesita los modelos en
`%APPDATA%\GerX\TuneX\models` (instalación manual) o en `%PROGRAMDATA%\GerX\TuneX\models`.
Si no los tienes, usa el instalador `.exe`, que ya los lleva.

## Aviso de firma

Los artefactos de 0.1.4 se publican **sin firma de código ni notarización**. Windows
SmartScreen y macOS Gatekeeper avisarán; es esperado. Los bundles de macOS van firmados
**ad hoc** (para que carguen), pero el `.pkg` no tiene firma de desarrollador. La firma y la
notarización quedan para una versión posterior.

## Límites conocidos

- **Motor global en el plan (D-292).** El motor global se lee por pase, no está congelado en
  la instantánea. Cambiar el motor global dos veces con la primera cadena en vuelo (siempre
  con el transporte parado; con Play el cambio se rechaza, D-288) puede dejar un transitorio
  con bloques del motor anterior hasta que la cadena siguiente converge. No hay mezcla
  permanente. En el proyecto medido las notas tenían motor explícito Pitchcore, así que el
  cambio global no alteraba su audio.
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

## Verificación de esta publicación

- **Windows x64.** El `.exe` se construyó en la nube (commit `9c6c02f`, DirectML) y se
  verificó sobre esas mismas bytes en un contenedor limpio de Windows Server 2025 (run
  `45549183414`, verde):
  instalación silenciosa, creación del VST3 instalado, modelos, licencias, desinstalación,
  exactamente un `.vst3` y ningún backup (se sembró un `TuneX.old.vst3` y el instalador lo
  borró). El VST3 instalado carga en el host mínimo. **SHA del VST3 instalado:
  `E3F231B139FA3E6B696D512BC9DBA71477DC5B419F53C6C0B743EA9186B04CE8`**, distinto del
  `CDD62D0D…` del zip suelto porque son dos compilaciones separadas del mismo commit y la
  misma configuración (MSVC incrusta metadatos de build). El binario del zip suelto es el que
  se probó en audio; el del instalador pasó la prueba de instalación y carga en limpio. Para
  0.1.5, el instalador se construirá a partir del zip probado para que ambos SHA coincidan.
- **macOS arm64.** El `.pkg` se instaló en un runner macOS 15 arm64 limpio (run
  `45543651453`, verde), en el home del usuario: VST3, AU con etiqueta ARA, modelos y
  licencias; `moduleinfo` 0.1.4; `pluginval` *strictness 5* → **SUCCESS**.
  `pkgutil --check-signature` informa `no signature` y los bundles son `adhoc`, como se
  describe arriba.
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
