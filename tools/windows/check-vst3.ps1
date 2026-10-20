# Kinu Tune - Copyright (C) 2026 Keinoaza
# Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
# Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#
# Checks a built Windows VST3 the way a host uses it. Run from the repository
# root on a Windows runner after building PitchNetPlugin_VST3:
#
#   pwsh tools/windows/check-vst3.ps1 -Vst3 "build/PitchNetPlugin_artefacts/Release/VST3/Kinu Tune.vst3"
#
# 1. Creates and activates the plug-in in a minimal host on the runner's own
#    Visual C++ runtime (the control).
# 2. Does the same with the Visual C++ 14.29 runtime (Visual Studio 2019) next
#    to the host. A DAW that ships an older app-local runtime loads the plug-in
#    like this. The runner always has the newest runtime installed, so without
#    this step CI cannot see the problem.
# 3. Runs pluginval at strictness 5, editor tests included.
#
# Every check runs even if an earlier one fails; the script fails at the end
# if any of them did.
param(
    [Parameter(Mandatory = $true)][string] $Vst3
)

$ErrorActionPreference = "Stop"
$failures = @()

$dll = Join-Path $Vst3 "Contents/x86_64-win/Kinu Tune.vst3"
if (-not (Test-Path $dll)) { throw "plug-in DLL not found at $dll" }
$dll = (Resolve-Path $dll).Path
$work = Join-Path ([System.IO.Path]::GetTempPath()) "kinu-vst3-check"
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Path $work | Out-Null

function Run-Host([string] $label, [string] $exe) {
    Write-Host "::group::$label"
    # Out-Host keeps the host's output out of this function's return value.
    & $exe $dll | Out-Host
    $code = $LASTEXITCODE
    Write-Host "::endgroup::"
    Write-Host "$label -> exit code $code"
    return $code
}

# --- The minimal host --------------------------------------------------------
cmake -S tools/windows/vst3-instantiate -B "$work/host-build"
if ($LASTEXITCODE -ne 0) { throw "configuring the test host failed" }
cmake --build "$work/host-build" --config Release
if ($LASTEXITCODE -ne 0) { throw "building the test host failed" }
$hostExe = (Resolve-Path "$work/host-build/Release/vst3_instantiate.exe").Path

# --- 1. Control: the runner's runtime ----------------------------------------
if ((Run-Host "Create the plug-in (runner's Visual C++ runtime)" $hostExe) -ne 0) {
    $failures += "create the plug-in on the runner's Visual C++ runtime"
}

# --- 2. An older runtime next to the host ------------------------------------
# The msvc-runtime package on PyPI repackages Microsoft's redistributable DLLs
# at fixed versions. Pinned by hash.
$wheelUrl = "https://files.pythonhosted.org/packages/6d/4a/602120a9e6625169fbddbdd036fe5559af638986dc0c3c3b602d3d60f95e/msvc_runtime-14.29.30133-cp310-cp310-win_amd64.whl"
$wheelHash = "1F1BE3E62B94F824A695B5091DE56A4E22CC682BA398E066AB4314A5BF43A696"
$wheel = Join-Path $work "msvc-runtime.zip"
Invoke-WebRequest -Uri $wheelUrl -OutFile $wheel
$actual = (Get-FileHash -Algorithm SHA256 $wheel).Hash
if ($actual -ne $wheelHash) { throw "msvc-runtime wheel hash mismatch: $actual" }
Expand-Archive $wheel -DestinationPath "$work/msvc-runtime"

$oldHostDir = Join-Path $work "old-runtime-host"
New-Item -ItemType Directory -Path $oldHostDir | Out-Null
Copy-Item $hostExe $oldHostDir
$runtimeDir = "$work/msvc-runtime/msvc_runtime-14.29.30133.data/data/Scripts"
foreach ($name in "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll", "vcruntime140.dll", "vcruntime140_1.dll") {
    Copy-Item (Join-Path $runtimeDir $name) $oldHostDir
    $version = (Get-Item (Join-Path $oldHostDir $name)).VersionInfo.FileVersion
    Write-Host "old runtime: $name $version"
}

if ((Run-Host "Create the plug-in (Visual C++ 14.29 runtime next to the host)" (Join-Path $oldHostDir "vst3_instantiate.exe")) -ne 0) {
    $failures += "create the plug-in on the Visual C++ 14.29 runtime"
}

# --- 3. pluginval ------------------------------------------------------------
Invoke-WebRequest -Uri "https://github.com/Tracktion/pluginval/releases/download/v1.0.4/pluginval_Windows.zip" -OutFile "$work/pluginval.zip"
Expand-Archive "$work/pluginval.zip" -DestinationPath "$work/pluginval"
Write-Host "::group::pluginval (strictness 5)"
& "$work/pluginval/pluginval.exe" --strictness-level 5 --timeout-ms 600000 --validate $Vst3
$pluginvalCode = $LASTEXITCODE
Write-Host "::endgroup::"
Write-Host "pluginval -> exit code $pluginvalCode"
if ($pluginvalCode -ne 0) { $failures += "pluginval" }

if ($failures.Count -gt 0) {
    Write-Host "FAILED:"
    $failures | ForEach-Object { Write-Host "  - $_" }
    exit 1
}
Write-Host "All Windows VST3 checks passed."
