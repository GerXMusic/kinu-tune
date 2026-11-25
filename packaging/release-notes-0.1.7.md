# TuneX 0.1.7

TuneX es un editor monofónico de afinación tipo Melodyne para VST3/ARA2, gratis y de
código abierto (AGPL-3.0). Esta versión cierra **eliminar nota**: el botón del inspector y
la fila del menú contextual vuelven a estar activos y el **ataque previo** de la nota
eliminada (la consonante que quedaba fuera de su tramo) también se apaga.

> Build **solo local**: 0.1.7 no se ha publicado como release ni tiene tag. Solo se ha
> construido el instalador de Windows. No hay `.pkg` de macOS en esta versión.

## Qué hay de nuevo

- **Eliminar nota, accesible (D-299, regresión de UI).** En 0.1.6 el motor y el comando
  existían, pero el botón `Eliminar nota` del inspector seguía deshabilitado y sin acción, y
  la fila `Eliminar` del clic derecho tampoco hacía nada. Ahora el botón se habilita cuando
  hay segmentos objetivo y la fila elimina la selección (si la nota pulsada no estaba
  seleccionada, pasa a ser la selección, igual que silenciar). Lo cubre
  `VariAudioInspectorTests` pulsando el botón real.
- **El ataque de la nota eliminada se apaga (D-302).** Eliminar una nota dejaba en silencio
  su `[start, end)`, pero la consonante anterior —que la expansión por VAD no alcanzó y que
  pertenece al tramo de la nota anterior— seguía sonando. Ahora el borde izquierdo del
  silencio se decide **por energía**, en muestras: se busca el último tramo de silencio con
  el criterio de D-129 y se apaga desde ahí, con el fade de 256 muestras cayendo sobre
  silencio. Sin estado nuevo en la nota: undo, JSON y guardar/reabrir no cambian.
  Medido en `NoteDeleteAttackTests`: consonante de **−19,7 dBFS** a **−180 dBFS**, cola de
  la nota anterior diff **0**, ataque de la nota siguiente diff **0**.
- **Versión 0.1.7.** Sube el patch porque cambia el audio (eliminar nota) y la UI.

## Archivos

| plataforma | archivo | contenido |
|---|---|---|
| Windows x64 | `TuneX-0.1.7-Windows-x64-Setup.exe` | VST3 DirectML, modelos, licencias, VC++ Redistributable |
| Windows x64 | `TuneX-0.1.7-Windows-x64-VST3.zip` | solo el VST3; **no** incluye modelos |

## Instalación (Windows x64)

1. Ejecuta `TuneX-0.1.7-Windows-x64-Setup.exe`.
2. SmartScreen avisará porque el instalador **no está firmado**: `Más información` →
   `Ejecutar de todas formas`.
3. El instalador aparta la versión anterior, deja el VST3 en
   `C:\Program Files\Common Files\VST3\TuneX.vst3`, los modelos en
   `C:\ProgramData\GerX\TuneX\models` y las licencias en `C:\Program Files\GerX\TuneX`.
4. Reinicia el DAW y vuelve a escanear plugins.

## Aviso de firma (D-298)

Los artefactos se construyen **sin firma de código ni notarización**. Windows SmartScreen
avisará; es esperado. No hay certificados en el proyecto.

## Límites conocidos

- **Eliminar nota y la cola derecha.** Esta versión solo extiende el borde **izquierdo**. Si
  el ataque de la nota siguiente empieza antes de su `start`, el fade de 256 muestras puede
  comerse parte de esa consonante; queda para el siguiente corte.
- **Eliminar nota y retime.** El borde se mide sobre el waveform original en coordenadas de
  destino; con cuantización de tiempo aplicada, la medida puede quedar desplazada.
- **Re-detectar notas descarta las lápidas** (mismo comportamiento que 0.1.6).
- **Play a mitad del render (D-293).** Se puede oír menos de un segundo del audio anterior
  mientras el host agota su *render-ahead*. Espera a la barra o a `Published`.
- **Pitchcore no retimea.** El core del motor Pitchcore ignora `sourceFrame`.
- **macOS.** 0.1.7 no tiene `.pkg`: solo se construyó Windows.

## Licencias

- Código: **AGPL-3.0**. El proyecto es libre y gratuito; **no se vende**.
- Vocoder PC-NSF-HiFiGAN: pesos **CC BY-NC-SA 4.0**, incluidos en los instaladores y en el
  zip de modelos. No se pueden vender ni usar como parte de un producto o servicio de pago.
- FCPE: MIT. ARA SDK: Apache-2.0. JUCE: AGPL. El resto de textos está en `licenses/` y
  `THIRD_PARTY_NOTICES.txt`.

Repositorio: https://github.com/GerXMusic/tunex · Problemas:
https://github.com/GerXMusic/tunex/issues
