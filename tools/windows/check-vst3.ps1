# Kinu Tune - Copyright (C) 2026 Keinoaza
# Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
# Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#
# Checks a built Windows VST3 the way a host uses it. Run from the repository
# root on a Windows runner after building PitchNetPlugin_VST3 (configuring
# also downloads the GAME models into Resources/models/GAME):
#
#   pwsh tools/windows/check-vst3.ps1 -Vst3 "build/PitchNetPlugin_artefacts/Release/VST3/Kinu Tune.vst3"
#
# 0. The plug-in DLL must not import the shared Visual C++ runtime.
# 1. Creates and activates the plug-in in a minimal host (tools/windows/
#    vst3-instantiate) on the runner's own Visual C++ runtime (the control).
# 2. Does the same with the Visual C++ 14.29 runtime (Visual Studio 2019) next
#    to the host. A DAW that ships an older app-local runtime loads the plug-in
#    like this. The runner always has the newest runtime installed, so without
#    this step CI cannot see the problem.
# 3. pluginval at strictness 5 with the models installed. GUI tests are
#    skipped: the runner is not a real user desktop, so an editor result
#    there would prove little either way.
# 4. Unless -SkipCleanMachine: the minimal host again, inside a Windows Server
#    container that has no Visual C++ runtime and no developer tools. The
#    bundled onnxruntime.dll cannot load there; the plug-in must still be
#    created and activated without crashing, load no onnxruntime.dll (not the
#    older one in System32 either) and report the missing runtime. pluginval
#    cannot run there (it needs the runtime itself, and shipping one next to it
#    would hand that runtime to the plug-in too), so this is the host only.
#    Then once more in a fresh container after installing the Visual C++
#    Redistributable, as INSTALL.txt asks users to: there it must fully work.
#
# Every check runs even if an earlier one fails; the script fails at the end
# if any of them did.
param(
    [Parameter(Mandatory = $true)][string] $Vst3,
    [switch] $SkipCleanMachine
)

$ErrorActionPreference = "Stop"
$failures = @()
$modelFiles = "fcpe.onnx", "cent_table.bin", "mel_filterbank.bin", "pc_nsf_hifigan.onnx"

$Vst3 = (Resolve-Path $Vst3).Path
$dll = Join-Path $Vst3 "Contents\x86_64-win\Kinu Tune.vst3"
if (-not (Test-Path $dll)) { throw "plug-in DLL not found at $dll" }
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

function Install-Models([string] $destination) {
    New-Item -ItemType Directory -Force -Path $destination | Out-Null
    foreach ($f in $modelFiles) { Copy-Item "Resources/models/$f" $destination }
    Copy-Item -Recurse "Resources/models/GAME" $destination
}

# --- 0. The plug-in must not import the Visual C++ runtime -------------------
# CMakeLists.txt links it statically (CMAKE_MSVC_RUNTIME_LIBRARY). Check 2
# below shows why; this check names the cause directly if that setting is lost.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$dumpbin = & $vswhere -latest -products * -find "VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe" | Select-Object -First 1
if (-not $dumpbin) { throw "dumpbin.exe not found" }

function Get-Imports([string] $path) {
    $lines = & $dumpbin /nologo /dependents $path
    Write-Host "::group::DLLs $(Split-Path -Leaf $path) imports"
    $lines | Out-Host
    Write-Host "::endgroup::"
    return @($lines | Where-Object { $_ -match '^\s+\S+\.dll\s*$' } | ForEach-Object { $_.Trim() })
}

$pluginImports = Get-Imports $dll
# A positive control, so an empty or unreadable listing cannot pass.
if ($pluginImports -notcontains "KERNEL32.dll") { throw "dumpbin did not list the plug-in's imports" }
$runtimeImports = @($pluginImports | Where-Object { $_ -match '^(msvcp140|vcruntime140)' })
if ($runtimeImports.Count -gt 0) {
    Write-Host "The plug-in imports the shared Visual C++ runtime: $($runtimeImports -join ', ')"
    $failures += "the plug-in links the Visual C++ runtime statically"
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
Install-Models (Join-Path $env:APPDATA "Keinoaza\Kinu Tune\models")
Invoke-WebRequest -Uri "https://github.com/Tracktion/pluginval/releases/download/v1.0.4/pluginval_Windows.zip" -OutFile "$work/pluginval.zip"
Expand-Archive "$work/pluginval.zip" -DestinationPath "$work/pluginval"
Write-Host "::group::pluginval (strictness 5, no GUI tests)"
# pluginval.exe is a GUI-subsystem program: without the pipe PowerShell does
# not wait for it, and $LASTEXITCODE would be the previous command's.
& "$work/pluginval/pluginval.exe" --strictness-level 5 --skip-gui-tests --timeout-ms 600000 --validate $Vst3 2>&1 | Out-Host
$pluginvalCode = $LASTEXITCODE
Write-Host "::endgroup::"
Write-Host "pluginval -> exit code $pluginvalCode"
if ($pluginvalCode -ne 0) { $failures += "pluginval" }

# --- 4. A clean Windows without any Visual C++ runtime -----------------------
if (-not $SkipCleanMachine) {
    # Process isolation needs the container's Windows build to match the
    # runner's. The full "server" image is used rather than Server Core because
    # the plug-in imports Direct2D, DXGI and DirectComposition, which Server
    # Core lacks; neither image has a Visual C++ runtime.
    $build = [System.Environment]::OSVersion.Version.Build
    $tag = switch ($build) { 20348 { "ltsc2022" } 26100 { "ltsc2025" } default { throw "no container tag for Windows build $build" } }
    $image = "mcr.microsoft.com/windows/server:$tag"

    $stage = "C:\kinu-clean"
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    New-Item -ItemType Directory -Path $stage | Out-Null
    Copy-Item -Recurse $Vst3 $stage
    Copy-Item $hostExe $stage
    Install-Models (Join-Path $stage "models")
    $bundleDir = Join-Path $stage "Kinu Tune.vst3\Contents\x86_64-win"

    # Everything the plug-in and the DLLs shipped next to it import, so the
    # container can say which ones it does not have.
    $imports = @($pluginImports)
    foreach ($shipped in Get-ChildItem $bundleDir -Filter *.dll) { $imports += Get-Imports $shipped.FullName }
    $imports | Sort-Object -Unique | Set-Content (Join-Path $stage "imports.txt")

    @'
param([switch] $InstallRedist)
$ErrorActionPreference = "Stop"
$bundle = "C:\kinu\Kinu Tune.vst3\Contents\x86_64-win"
Write-Host "Windows $([System.Environment]::OSVersion.Version)"
if ($InstallRedist) {
    # What INSTALL.txt tells a user to do.
    Invoke-WebRequest -Uri "https://aka.ms/vc14/vc_redist.x64.exe" -OutFile C:\vc_redist.x64.exe
    $installer = Start-Process C:\vc_redist.x64.exe -ArgumentList "/install", "/quiet", "/norestart" -Wait -PassThru
    Write-Host "vc_redist.x64.exe -> exit code $($installer.ExitCode)"
    Write-Host "System32\msvcp140.dll: $((Get-Item (Join-Path $env:SystemRoot "System32\msvcp140.dll")).VersionInfo.FileVersion)"
}
$missing = @()
foreach ($name in Get-Content C:\kinu\imports.txt) {
    if ($name -like "api-ms-*" -or $name -like "ext-ms-*") { continue }
    $found = (Test-Path (Join-Path $env:SystemRoot "System32\$name")) -or (Test-Path (Join-Path $bundle $name))
    if (-not $found) { $missing += $name }
}
if ($missing.Count -gt 0) { Write-Host "Imported DLLs that are neither in System32 nor in the bundle: $($missing -join ', ')" }
else { Write-Host "Every imported DLL is in System32 or in the bundle." }
# Without the redistributable only the Visual C++ runtime may be missing
# (onnxruntime.dll needs it; the plug-in itself links it statically). With
# it, nothing may be.
$unexpected = @($missing | Where-Object { $InstallRedist -or $_ -notmatch '^(msvcp140|vcruntime140)' })
if ($unexpected.Count -gt 0) { Write-Host "Missing and not explained by the Visual C++ runtime: $($unexpected -join ', ')" }
foreach ($name in "msvcp140.dll", "vcruntime140.dll", "onnxruntime.dll") {
    Write-Host "System32\$name present: $(Test-Path (Join-Path $env:SystemRoot "System32\$name"))"
}
$models = Join-Path $env:APPDATA "Keinoaza\Kinu Tune"
New-Item -ItemType Directory -Force -Path $models | Out-Null
Copy-Item -Recurse C:\kinu\models $models
# Without the redistributable the plug-in must still be created, must not
# load any onnxruntime.dll, and must report the missing runtime.
$hostArgs = @()
if (-not $InstallRedist) { $hostArgs += "--expect-onnx-runtime-missing" }
& C:\kinu\vst3_instantiate.exe @hostArgs "$bundle\Kinu Tune.vst3"
$code = $LASTEXITCODE
if ($unexpected.Count -gt 0 -and $code -eq 0) { $code = 4 }
exit $code
'@ | Set-Content (Join-Path $stage "in-container.ps1")

    Write-Host "::group::Pull $image"
    docker pull $image | Out-Host
    Write-Host "::endgroup::"
    Write-Host "::group::Create the plug-in in a clean $image container"
    docker run --rm --isolation=process -v "${stage}:C:\kinu" $image powershell -NoProfile -ExecutionPolicy Bypass -File C:\kinu\in-container.ps1 | Out-Host
    $cleanCode = $LASTEXITCODE
    Write-Host "::endgroup::"
    Write-Host "Clean machine -> exit code $cleanCode"
    if ($cleanCode -ne 0) { $failures += "create the plug-in on a clean Windows (no Visual C++ runtime) and have it report the missing runtime" }

    # The same clean machine after installing the Visual C++ Redistributable,
    # as INSTALL.txt asks. Each run starts from a fresh container.
    Write-Host "::group::Create the plug-in in a clean $image container with the Visual C++ Redistributable installed"
    docker run --rm --isolation=process -v "${stage}:C:\kinu" $image powershell -NoProfile -ExecutionPolicy Bypass -File C:\kinu\in-container.ps1 -InstallRedist | Out-Host
    $redistCode = $LASTEXITCODE
    Write-Host "::endgroup::"
    Write-Host "Clean machine with the redistributable -> exit code $redistCode"
    if ($redistCode -ne 0) { $failures += "create the plug-in on a clean Windows with the Visual C++ Redistributable installed" }
}

if ($failures.Count -gt 0) {
    Write-Host "FAILED:"
    $failures | ForEach-Object { Write-Host "  - $_" }
    exit 1
}
Write-Host "All Windows VST3 checks passed."
