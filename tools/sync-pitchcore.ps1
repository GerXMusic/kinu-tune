<#
.SYNOPSIS
    Copia el nucleo de Pitchcore a third_party/pitchcore (D-095).

.DESCRIPTION
    El adaptador PitchcoreSynthesizer compila dentro del VST3, y el nucleo vive
    fuera del arbol del fork. Enlazarlo por ruta absoluta romperia la
    reproducibilidad (el mismo commit del fork no compilaria en otra maquina) y
    el hash del binario dejaria de estar atado a una fuente concreta.

    Este script deja una copia EXACTA de include/ y src/ y escribe VENDORED.md
    con el commit del nucleo. La copia NO se edita a mano: si el nucleo necesita
    un cambio, se hace en Pitchcore y se vuelve a sincronizar.

    Solo entran las 12 fuentes del motor clasico. Las del camino neuronal
    (neural_f0.cpp, nsf_vocoder.cpp, neural_engine.cpp) dependen de ONNX Runtime
    y quedan fuera a proposito, igual que CLIs, tests y tools.

.PARAMETER PitchcoreDir
    Ruta al directorio del nucleo (el que tiene include/ y src/).

.EXAMPLE
    pwsh -File tools/sync-pitchcore.ps1
    pwsh -File tools/sync-pitchcore.ps1 -PitchcoreDir ..\..\Pitchcore\codigo\pitchcore
#>
[CmdletBinding()]
param(
    [string] $PitchcoreDir = ''
)

$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $PitchcoreDir) {
    $PitchcoreDir = Join-Path $scriptDir '..\..\Pitchcore\codigo\pitchcore'
}

$repoRoot = Resolve-Path (Join-Path $scriptDir '..')
$dest     = Join-Path $repoRoot 'third_party\pitchcore'
$source   = Resolve-Path $PitchcoreDir

# Las 12 fuentes del motor clasico, en el mismo orden que el CMakeLists del
# nucleo (Pitchcore/codigo/pitchcore/CMakeLists.txt:10-13).
$coreSources = @(
    'f0_yin.cpp', 'segment.cpp', 'wav.cpp', 'psola.cpp', 'synth.cpp',
    'nsf.cpp', 'resample.cpp', 'pitch_curve.cpp', 'engine.cpp', 'dsp.cpp',
    'spectral_engine.cpp', 'render_dispatcher.cpp'
)
$neuralSources = @('neural_f0.cpp', 'nsf_vocoder.cpp', 'neural_engine.cpp')

# Cabeceras que usan esas fuentes (las tres del camino neuronal quedan fuera).
$coreHeaders = @(
    'dsp.h', 'engine.h', 'f0.h', 'nsf.h', 'pitch_curve.h', 'psola.h',
    'render_dispatcher.h', 'render_request.h', 'resample.h', 'segment.h',
    'spectral_engine.h', 'synth.h', 'wav.h'
)

if (-not (Test-Path (Join-Path $source 'include\pitchcore'))) {
    throw "No encuentro el nucleo en $source (falta include/pitchcore)."
}

foreach ($f in $coreSources) {
    if (-not (Test-Path (Join-Path $source "src\$f"))) {
        throw "Falta la fuente $f en $source\src."
    }
}

# Si el nucleo gana una fuente nueva, esto falla en vez de compilar una copia
# incompleta en silencio.
$upstream = Get-ChildItem (Join-Path $source 'src') -Filter *.cpp |
    Select-Object -ExpandProperty Name
$known = $coreSources + $neuralSources
$nuevas = $upstream | Where-Object { $known -notcontains $_ }
if ($nuevas) {
    throw ("Hay fuentes nuevas en el nucleo que este script no conoce: " +
           ($nuevas -join ', ') + ". Anadelas a `$coreSources (y al CMakeLists) o a `$neuralSources.")
}

$headers = Get-ChildItem (Join-Path $source 'include\pitchcore') -Filter *.h |
    Select-Object -ExpandProperty Name
$faltan = $headers | Where-Object { $coreHeaders -notcontains $_ -and $neuralSources -notcontains ($_ -replace '\.h$', '.cpp') }
if ($faltan) {
    throw ("Hay cabeceras nuevas que este script no conoce: " + ($faltan -join ', '))
}

# Copia limpia (nada de restos de una version anterior).
foreach ($sub in 'include\pitchcore', 'src') {
    $p = Join-Path $dest $sub
    if (Test-Path $p) { Remove-Item $p -Recurse -Force }
    New-Item -ItemType Directory -Path $p -Force | Out-Null
}

# El nucleo se guarda en git con LF y el checkout de Windows lo deja en CRLF.
# La copia se normaliza a LF para que el diff contra el commit anterior sea el
# cambio real y no todo el fichero por los finales de linea (y para que
# .gitattributes -text guarde los mismos bytes en cualquier maquina).
$utf8 = New-Object System.Text.UTF8Encoding($false)
function Copy-Normalized([string] $from, [string] $to) {
    $text = [System.IO.File]::ReadAllText($from, [System.Text.Encoding]::UTF8)
    $text = $text.Replace("`r`n", "`n")
    [System.IO.File]::WriteAllText($to, $text, $utf8)
}

foreach ($f in $coreHeaders) {
    Copy-Normalized (Join-Path $source "include\pitchcore\$f") (Join-Path $dest "include\pitchcore\$f")
}
foreach ($f in $coreSources) {
    Copy-Normalized (Join-Path $source "src\$f") (Join-Path $dest "src\$f")
}

# El CMakeLists es del fork (no se copia): se comprueba que lista justo esas
# fuentes, para que la copia y el build no se separen.
$cmake = Join-Path $dest 'CMakeLists.txt'
if (-not (Test-Path $cmake)) {
    throw "Falta ${cmake}: el CMakeLists del nucleo vendorizado es del fork y no se genera."
}
$listadas = Select-String -Path $cmake -Pattern 'src/([A-Za-z0-9_]+\.cpp)' -AllMatches |
    ForEach-Object { $_.Matches } | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
$esperadas = $coreSources | Sort-Object -Unique
$diff = Compare-Object $listadas $esperadas
if ($diff) {
    throw ("El CMakeLists vendorizado no coincide con la lista de fuentes: " +
           (($diff | ForEach-Object { "$($_.SideIndicator) $($_.InputObject)" }) -join '; '))
}

# Commit exacto del nucleo (si Pitchcore es un repo git).
$commit = 'desconocido (no es un repo git)'
$dirty  = ''
try {
    $commit = (& git -C $source rev-parse HEAD).Trim()
    $st = & git -C $source status --porcelain -- .
    if ($st) { $dirty = "`n> **AVISO**: el nucleo tenia cambios sin commitear al sincronizar; esta copia NO corresponde a un commit limpio.`n" }
} catch { }

$fecha = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
$md = @"
# Núcleo de Pitchcore vendorizado (D-095)

Copia **exacta** de las 12 fuentes del motor clásico y sus 13 cabeceras.
No se edita a mano: se regenera con ``tools/sync-pitchcore.ps1``.

- **Origen**: ``$source``
- **Commit**: ``$commit``
- **Sincronizado**: $fecha
$dirty
## Qué entra y qué no

Entra el motor clásico completo (YIN, segmentación, PSOLA, espectral,
despachador, curva y utilidades de audio). **No** entran
``neural_f0.cpp``, ``nsf_vocoder.cpp`` ni ``neural_engine.cpp``: dependen de
ONNX Runtime y el adaptador de M2 solo usa el clásico. Tampoco entran CLIs,
tests ni ``tools/``.

## Compilación

``CMakeLists.txt`` es **del fork** (no se copia del núcleo): declara ``pitchcore``
STATIC, fija C++17 y compila con ``/fp:precise`` (MSVC) o ``-fno-fast-math``
para que el árbol del fork no le meta fast-math (D-093: el bit a bit solo vale
sin fast-math).

## Regla

Si el núcleo necesita un cambio para compilar dentro del fork, se hace **en
Pitchcore** y se vuelve a sincronizar. Este directorio nunca se edita a mano.
"@
[System.IO.File]::WriteAllText((Join-Path $dest 'VENDORED.md'), $md + "`n", (New-Object System.Text.UTF8Encoding($false)))

Write-Host ("Sincronizado: {0} fuentes + {1} cabeceras -> {2}" -f $coreSources.Count, $coreHeaders.Count, $dest)
Write-Host ("Commit del nucleo: {0}" -f $commit)
