# Kinu Tune - Copyright (C) 2026 Keinoaza
# Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
# Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#
# Checks the Windows installer the way a user meets it: on a clean Windows
# (a Windows Server container without any Visual C++ runtime or developer
# tools), run the installer silently, then create the installed plug-in in
# the minimal host (tools/windows/vst3-instantiate), which must find the
# bundled onnxruntime.dll working (so the installer put the Visual C++
# Redistributable in place). Then uninstall and check that the plug-in and
# the models are gone. Run from the repository root on a Windows runner with
# the JUCE submodule checked out (the host needs the VST3 SDK headers):
#
#   pwsh tools/windows/check-installer.ps1 -Setup Output/Kinu-Tune-<version>-Windows-x64-Setup.exe
param(
    [Parameter(Mandatory = $true)][string] $Setup
)

$ErrorActionPreference = "Stop"
$Setup = (Resolve-Path $Setup).Path
$work = Join-Path ([System.IO.Path]::GetTempPath()) "kinu-installer-check"
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Path $work | Out-Null

cmake -S tools/windows/vst3-instantiate -B "$work/host-build"
if ($LASTEXITCODE -ne 0) { throw "configuring the test host failed" }
cmake --build "$work/host-build" --config Release
if ($LASTEXITCODE -ne 0) { throw "building the test host failed" }

$stage = "C:\kinu-installer"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Path $stage | Out-Null
Copy-Item $Setup (Join-Path $stage "setup.exe")
Copy-Item "$work/host-build/Release/vst3_instantiate.exe" $stage

@'
$ErrorActionPreference = "Stop"
$failures = @()
$vst3 = "C:\Program Files\Common Files\VST3\Kinu Tune.vst3"
$dll = "$vst3\Contents\x86_64-win\Kinu Tune.vst3"
$models = "C:\ProgramData\Keinoaza\Kinu Tune\models"
$uninstaller = "C:\Program Files\Keinoaza\Kinu Tune\unins000.exe"
$appDir = "C:\Program Files\Keinoaza\Kinu Tune"
$uninstallKey = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{38373E75-C136-4F15-8F5D-6F966750C257}_is1"
Write-Host "Windows $([System.Environment]::OSVersion.Version)"
# Without this the check below could not tell that the installer brought the
# Visual C++ runtime.
if (Test-Path (Join-Path $env:SystemRoot "System32\msvcp140.dll")) {
    Write-Host "This image already has the Visual C++ runtime; it is not a clean machine."
    exit 1
}
Write-Host "Before: no Visual C++ runtime in System32"

$run = Start-Process C:\kinu\setup.exe -ArgumentList "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/LOG=C:\kinu\setup.log" -Wait -PassThru
Write-Host "setup.exe -> exit code $($run.ExitCode)"
if ($run.ExitCode -ne 0) { $failures += "the installer exited with $($run.ExitCode)" }

foreach ($path in $dll, "$vst3\Contents\x86_64-win\onnxruntime.dll", "$models\fcpe.onnx", "$models\cent_table.bin",
                  "$models\mel_filterbank.bin", "$models\pc_nsf_hifigan.onnx", "$models\GAME", $uninstaller,
                  "$appDir\LICENSE", "$appDir\THIRD_PARTY_NOTICES.txt", "$appDir\licenses", $uninstallKey) {
    if (-not (Test-Path $path)) { $failures += "missing after install: $path" }
}
$msvcp = Join-Path $env:SystemRoot "System32\msvcp140.dll"
if (Test-Path $msvcp) { Write-Host "After: System32\msvcp140.dll $((Get-Item $msvcp).VersionInfo.FileVersion)" }
else { $failures += "the Visual C++ Redistributable was not installed" }

# The plug-in must load, and its onnxruntime.dll must work (no "unavailable").
& C:\kinu\vst3_instantiate.exe $dll
if ($LASTEXITCODE -ne 0) { $failures += "create the installed plug-in (exit code $LASTEXITCODE)" }

# The uninstaller hands over to a copy of itself; wait for the files to go.
if (Test-Path $uninstaller) {
    $un = Start-Process $uninstaller -ArgumentList "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART" -Wait -PassThru
    Write-Host "unins000.exe -> exit code $($un.ExitCode)"
    if ($un.ExitCode -ne 0) { $failures += "the uninstaller exited with $($un.ExitCode)" }
    $left = { @($vst3, $models, $appDir, $uninstallKey | Where-Object { Test-Path $_ }) }
    $deadline = (Get-Date).AddSeconds(120)
    while ((& $left).Count -gt 0 -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 2 }
    foreach ($path in & $left) { $failures += "still there after uninstalling: $path" }
    if ((& $left).Count -eq 0) { Write-Host "Uninstall removed the plug-in, the models, the app folder and the uninstall entry." }
}

Write-Host "::group::Installer log"
Get-Content C:\kinu\setup.log | Out-Host
Write-Host "::endgroup::"
if ($failures.Count -gt 0) {
    Write-Host "FAILED:"
    $failures | ForEach-Object { Write-Host "  - $_" }
    exit 1
}
Write-Host "Installer check passed."
exit 0
'@ | Set-Content (Join-Path $stage "in-container.ps1")

# Process isolation needs the container's Windows build to match the runner's.
# The full "server" image, as in check-vst3.ps1: the plug-in needs Direct2D,
# which Server Core lacks.
$build = [System.Environment]::OSVersion.Version.Build
$tag = switch ($build) { 20348 { "ltsc2022" } 26100 { "ltsc2025" } default { throw "no container tag for Windows build $build" } }
$image = "mcr.microsoft.com/windows/server:$tag"
Write-Host "::group::Pull $image"
docker pull $image | Out-Host
Write-Host "::endgroup::"
docker run --rm --isolation=process -v "${stage}:C:\kinu" $image powershell -NoProfile -ExecutionPolicy Bypass -File C:\kinu\in-container.ps1 | Out-Host
$code = $LASTEXITCODE
Write-Host "Clean machine install -> exit code $code"
if ($code -ne 0) { exit 1 }
