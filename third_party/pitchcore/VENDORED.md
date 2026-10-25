# NÃºcleo de Pitchcore vendorizado (D-095)

Copia **exacta** de las 12 fuentes del motor clÃ¡sico y sus 13 cabeceras.
No se edita a mano: se regenera con `tools/sync-pitchcore.ps1`.

- **Origen**: `C:\Users\GumiPC\Desktop\Opencode\Pitchcore\codigo\pitchcore`
- **Commit**: `040075e65f4b4ca1f755a23836c665f26c4addfd`
- **Sincronizado**: 2026-10-24 23:08:31

## QuÃ© entra y quÃ© no

Entra el motor clÃ¡sico completo (YIN, segmentaciÃ³n, PSOLA, espectral,
despachador, curva y utilidades de audio). **No** entran
`neural_f0.cpp`, `nsf_vocoder.cpp` ni `neural_engine.cpp`: dependen de
ONNX Runtime y el adaptador de M2 solo usa el clÃ¡sico. Tampoco entran CLIs,
tests ni `tools/`.

## CompilaciÃ³n

`CMakeLists.txt` es **del fork** (no se copia del nÃºcleo): declara `pitchcore`
STATIC, fija C++17 y compila con `/fp:precise` (MSVC) o `-fno-fast-math`
para que el Ã¡rbol del fork no le meta fast-math (D-093: el bit a bit solo vale
sin fast-math).

## Regla

Si el nÃºcleo necesita un cambio para compilar dentro del fork, se hace **en
Pitchcore** y se vuelve a sincronizar. Este directorio nunca se edita a mano.
