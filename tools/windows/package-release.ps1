# Kinu Tune - packaging de publicación (Windows)
# Copyright (C) 2026 Keinoaza. AGPL-3.0; ver LICENSE y MODIFICATIONS.md.
#
# Arma el paquete de publicación a partir del bundle YA COMPILADO:
#
#   powershell -NoProfile -ExecutionPolicy Bypass `
#       -File tools\windows\package-release.ps1 `
#       -Vst3 "build-dml\PitchNetPlugin_artefacts\Release\VST3\Kinu Tune.vst3" `
#       -Version 0.1.0
#
# Qué garantiza (lecciones D-240):
#   - exactamente UN .vst3 dentro de Contents\x86_64-win;
#   - ningún backup (.old.vst3, .bak, *~) dentro del bundle;
#   - los pesos del vocoder (CC BY-NC-SA) van en un ZIP APARTE con su NOTICE;
#   - SHA-256 de cada archivo en SHA256SUMS.txt y del bundle en RELEASE.txt.
#
# No instala nada y no valida el artefacto: eso es `check-vst3.ps1`.
param(
    [Parameter(Mandatory = $true)][string] $Vst3,
    [string] $Version = "0.1.0",
    [string] $Out = (Join-Path $env:TEMP "opencode\kinu-release")
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)  # repo root
$Vst3 = (Resolve-Path $Vst3).Path
$bundleName = Split-Path -Leaf $Vst3                      # "Kinu Tune.vst3"
$dllDir = Join-Path $Vst3 "Contents\x86_64-win"

if (-not (Test-Path $dllDir)) { throw "no existe $dllDir" }

# --- 1. Un solo .vst3 y ningún backup --------------------------------------
$vst3Files = @(Get-ChildItem $dllDir -Filter *.vst3 -File)
if ($vst3Files.Count -ne 1) {
    throw "el bundle debe tener exactamente un .vst3 en x86_64-win; hay $($vst3Files.Count): $($vst3Files.Name -join ', ')"
}
$pluginDll = $vst3Files[0].FullName
if ($vst3Files[0].Name -ne "Kinu Tune.vst3") {
    throw "el .vst3 del bundle se llama '$($vst3Files[0].Name)'; debe ser 'Kinu Tune.vst3'"
}

$backups = @(Get-ChildItem $Vst3 -Recurse -File | Where-Object {
    $_.Name -match '\.old(\.|$)|\.bak|~$'
})
if ($backups.Count -gt 0) {
    throw "hay backups dentro del bundle (D-240): $($backups.FullName -join ', ')"
}

# --- 2. Salida limpia -------------------------------------------------------
if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
$pkg = Join-Path $Out "Kinu Tune $Version"
New-Item -ItemType Directory -Force -Path $pkg | Out-Null

Copy-Item -Recurse $Vst3 (Join-Path $pkg $bundleName)
Copy-Item -Recurse (Join-Path $root "licenses") (Join-Path $pkg "licenses")
Copy-Item (Join-Path $root "LICENSE") (Join-Path $pkg "LICENSE")
Copy-Item (Join-Path $root "MODIFICATIONS.md") (Join-Path $pkg "MODIFICATIONS.md")
Copy-Item (Join-Path $root "THIRD_PARTY_NOTICES.txt") (Join-Path $pkg "THIRD_PARTY_NOTICES.txt")
Copy-Item (Join-Path $root "packaging\INSTALL-Windows.txt") (Join-Path $pkg "INSTALL-Windows.txt")

# --- 3. Pesos del vocoder, APARTE (CC BY-NC-SA 4.0) -------------------------
$weightsName = "Kinu Tune $Version - pesos del vocoder"
$weights = Join-Path $Out $weightsName
New-Item -ItemType Directory -Force -Path $weights | Out-Null
$vocoder = Join-Path $root "Resources\models\pc_nsf_hifigan.onnx"
if (-not (Test-Path $vocoder)) { throw "falta $vocoder" }
Copy-Item $vocoder $weights
Copy-Item (Join-Path $root "licenses\PC-NSF-HiFiGAN-NOTICE.txt") (Join-Path $weights "NOTICE.txt")

# --- 4. Manifiesto y RELEASE.txt -------------------------------------------
$pluginHash = (Get-FileHash -Algorithm SHA256 $pluginDll).Hash
$pluginSize = (Get-Item $pluginDll).Length

$release = @"
Kinu Tune $Version (Windows x64, VST3/ARA2)
Empaquetado: $(Get-Date -Format 'yyyy-MM-dd HH:mm')

Plugin: $bundleName
SHA-256: $pluginHash
Tamano: $pluginSize bytes

Limites conocidos de esta version:
- El motor Pitchcore no retimea (el core ignora sourceFrame); la cuantizacion de
  tiempo se escucha con PSOLA y con el vocoder.
- Con el vocoder, editar una nota reconstruye su bloque de motor (D-239).
- Dos GPUs identicas se cuentan como una en la politica de motor por defecto.
- Los pesos del vocoder (CC BY-NC-SA 4.0) van en el paquete aparte
  "$weightsName". No se pueden vender.
"@
Set-Content -Path (Join-Path $pkg "RELEASE.txt") -Value $release -Encoding utf8

$sums = Get-ChildItem $pkg -Recurse -File |
    ForEach-Object {
        "{0}  {1}" -f (Get-FileHash -Algorithm SHA256 $_.FullName).Hash,
        $_.FullName.Substring($pkg.Length + 1).Replace('\', '/')
    }
Set-Content -Path (Join-Path $pkg "SHA256SUMS.txt") -Value $sums -Encoding utf8

$weightsSums = Get-ChildItem $weights -File |
    ForEach-Object {
        "{0}  {1}" -f (Get-FileHash -Algorithm SHA256 $_.FullName).Hash, $_.Name
    }
Set-Content -Path (Join-Path $weights "SHA256SUMS.txt") -Value $weightsSums -Encoding utf8

# --- 5. ZIPs ----------------------------------------------------------------
$pluginZip = Join-Path $Out ("Kinu Tune {0} - Windows x64.zip" -f $Version)
$weightsZip = Join-Path $Out ("Kinu Tune {0} - pesos del vocoder.zip" -f $Version)
Compress-Archive -Path $pkg -DestinationPath $pluginZip -CompressionLevel Optimal
Compress-Archive -Path $weights -DestinationPath $weightsZip -CompressionLevel Optimal

Write-Host "plugin:  $pluginDll"
Write-Host "SHA-256: $pluginHash ($pluginSize bytes)"
Write-Host "paquete: $pluginZip"
Write-Host "pesos:   $weightsZip"
Write-Host "OK: un solo .vst3, sin backups, pesos aparte con NOTICE."
