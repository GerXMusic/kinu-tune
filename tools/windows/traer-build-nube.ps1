# TuneX - traer de GitHub el ultimo VST3 compilado en la nube e instalarlo
#
# Para que: compilar en la maquina local la dejo inutilizable varias veces (D-276).
# El runner de GitHub compila el VST3 con la misma configuracion que produccion
# (DirectML ON, runtime DirectML incluido, diagnosticos OFF) y lo deja como
# artefacto durante 7 dias. Este script lo descarga y lo instala.
#
# Requisitos (una sola vez):
#   winget install --id GitHub.cli
#   gh auth login
#
# Uso normal:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\windows\traer-build-nube.ps1
#
# Pedir un build nuevo de la rama actual y esperarlo:
#   ... -Disparar
#
# Solo descargar, sin instalar (y mirar que trae):
#   ... -NoInstalar
#
# Guardas: D-240 (exactamente un .vst3 dentro del bundle, sin respaldos dentro),
# y el plugin instalado anterior se aparta con fecha, no se borra.

[CmdletBinding()]
param(
    [string]$Workflow = 'windows-vst3.yml',
    [string]$Artefacto = 'TuneX-vst3-Windows-x64',
    [string]$Ref = '',
    [switch]$Disparar,
    [switch]$NoInstalar,
    [string]$Destino = 'C:\Program Files\Common Files\VST3',
    [string]$WorkDir = "$env:TEMP\opencode\tunex-nube",
    [string]$BackupRoot = "$env:USERPROFILE\Desktop\Opencode\respaldo-plugins",
    [string]$Source = ''   # interno: lo usa el relanzamiento con permisos de administrador
)

$ErrorActionPreference = 'Stop'

function EsAdmin {
    ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

function HashVst3([string]$bundle) {
    $inner = Get-ChildItem $bundle -Recurse -File -Filter *.vst3 | Select-Object -First 1
    if (-not $inner) { return $null }
    [pscustomobject]@{ Path = $inner.FullName; SHA = (Get-FileHash $inner.FullName -Algorithm SHA256).Hash; MB = [math]::Round($inner.Length / 1MB, 2) }
}

# ---------------------------------------------------------------- instalar ---
function Instalar([string]$bundle, [string]$destino, [string]$backupRoot) {
    $info = HashVst3 $bundle
    if (-not $info) { throw "el bundle no tiene ningun .vst3 dentro: $bundle" }

    $dest = Join-Path $destino 'TuneX.vst3'
    if (Test-Path $dest) {
        $actual = HashVst3 $dest
        if ($actual -and $actual.SHA -eq $info.SHA) {
            Write-Host "Ya esta instalado ese mismo fichero (SHA $($info.SHA.Substring(0,8))...). No toco nada." -ForegroundColor Green
            return
        }
        $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
        $backup = Join-Path $backupRoot $stamp
        New-Item -ItemType Directory -Path $backup -Force | Out-Null
        Move-Item $dest (Join-Path $backup 'TuneX.vst3') -Force
        Write-Host "Instalado anterior apartado en: $backup" -ForegroundColor Yellow
    }

    # cualquier resto viejo que hubiera suelto en la carpeta de plugins
    Get-ChildItem $destino -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '(?i)kinu|\.old\.' } |
        ForEach-Object {
            $b2 = Join-Path $backupRoot ((Get-Date -Format 'yyyyMMdd-HHmmss') + '-sueltos')
            New-Item -ItemType Directory -Path $b2 -Force | Out-Null
            Move-Item $_.FullName (Join-Path $b2 $_.Name) -Force
            Write-Host "Apartado de la carpeta de plugins: $($_.Name)" -ForegroundColor Yellow
        }

    Copy-Item $bundle $dest -Recurse -Force
    $nuevo = HashVst3 $dest
    Write-Host ''
    Write-Host "Instalado: $dest" -ForegroundColor Green
    Write-Host "  fichero: $($nuevo.Path)"
    Write-Host "  tamano : $($nuevo.MB) MB"
    Write-Host "  SHA256 : $($nuevo.SHA)"
    Write-Host ''
    Write-Host 'Ahora, en REAPER: Opciones > Preferencias > Plug-ins > VST > "Re-escanear".'
    Write-Host 'Recuerda (D-240): instalado no es cargado. Verifica el modulo del proceso:'
    Write-Host '  Get-Process reaper | ForEach-Object { $_.Modules } | Where-Object ModuleName -like "*TuneX*" | Select-Object ModuleName,FileName'
}

# ------------------------------------------------------- relanzar como admin ---
if ($Source) {
    Instalar $Source $Destino $BackupRoot
    exit 0
}

# ------------------------------------------------------------------ gh listo ---
$gh = Get-Command gh -ErrorAction SilentlyContinue
if (-not $gh) {
    Write-Host 'No encuentro GitHub CLI (gh).' -ForegroundColor Red
    Write-Host 'Instalalo una vez y autoriza:' -ForegroundColor Yellow
    Write-Host '  winget install --id GitHub.cli'
    Write-Host '  gh auth login            (abre el navegador; lo autorizas tu)'
    exit 1
}
& gh auth status *> $null
if ($LASTEXITCODE -ne 0) {
    Write-Host 'gh no esta autorizado en esta maquina.' -ForegroundColor Red
    Write-Host 'Ejecuta: gh auth login   (el navegador pide tu cuenta; no lo puedo hacer yo)' -ForegroundColor Yellow
    exit 1
}

$repo = (& gh repo view --json nameWithOwner -q .nameWithOwner)
if ($LASTEXITCODE -ne 0 -or -not $repo) { throw 'no pude leer el repositorio con gh (¿estas dentro de tunex?)' }
Write-Host "[nube] repo $repo  workflow $Workflow"

# --------------------------------------------------------------- disparar ---
if ($Disparar) {
    $antes = @(& gh run list --workflow=$Workflow --limit 10 --json databaseId -q '.[].databaseId')
    $arg = @('workflow', 'run', $Workflow)
    if ($Ref) { $arg += @('--ref', $Ref) }
    & gh @arg
    if ($LASTEXITCODE -ne 0) { throw 'no pude disparar el workflow (¿existe en la rama remota? ¿hiciste push?)' }
    Write-Host '[nube] build pedido; esperando a que aparezca el run...'
    $runId = $null
    for ($i = 0; $i -lt 60 -and -not $runId; $i++) {
        Start-Sleep -Seconds 5
        $nuevos = @(& gh run list --workflow=$Workflow --limit 10 --json databaseId -q '.[].databaseId')
        $runId = $nuevos | Where-Object { $_ -notin $antes } | Select-Object -First 1
    }
    if (-not $runId) { throw 'no aparecio ningun run nuevo en 5 minutos' }
    Write-Host "[nube] run $runId; esto compila en GitHub, tu PC no trabaja. Ctrl+C corta solo la espera." -ForegroundColor Cyan
    & gh run watch $runId --exit-status --interval 20
    if ($LASTEXITCODE -ne 0) { throw "el run $runId no termino en verde (gh run view $runId --log-failed)" }
    $id = $runId
} else {
    $id = (& gh run list --workflow=$Workflow --status success --limit 1 --json databaseId -q '.[0].databaseId')
    if (-not $id) {
        Write-Host "No hay ningun run exitoso de $Workflow todavia." -ForegroundColor Red
        Write-Host 'Lanza uno con: ... -Disparar' -ForegroundColor Yellow
        exit 1
    }
    $meta = & gh run view $id --json headSha,createdAt,displayTitle -q '"\(.createdAt)  \(.headSha[0:8])  \(.displayTitle)"'
    Write-Host "[nube] ultimo run verde: $id  $meta"
}

# -------------------------------------------------------------- descargar ---
New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null
Get-ChildItem $WorkDir -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
& gh run download $id -n $Artefacto -D $WorkDir
if ($LASTEXITCODE -ne 0) { throw "no pude descargar el artefacto '$Artefacto' del run $id (¿caduco a los 7 dias?)" }

$zip = Get-ChildItem $WorkDir -Recurse -Filter *.zip | Select-Object -First 1
if (-not $zip) { throw "el artefacto no trae ningun .zip en $WorkDir" }
Expand-Archive $zip.FullName -DestinationPath (Join-Path $WorkDir 'extraido') -Force
$bundle = Get-ChildItem (Join-Path $WorkDir 'extraido') -Recurse -Directory -Filter 'TuneX.vst3' | Select-Object -First 1
if (-not $bundle) { throw 'el zip no trae TuneX.vst3' }

# guarda D-240 sobre lo descargado
$dllDir = Join-Path $bundle.FullName 'Contents/x86_64-win'
$vst3s = @(Get-ChildItem $dllDir -Filter *.vst3 -File)
if ($vst3s.Count -ne 1) { throw "el bundle descargado tiene $($vst3s.Count) .vst3 en x86_64-win" }
$respaldos = @(Get-ChildItem $bundle.FullName -Recurse -File | Where-Object { $_.Name -match '\.old\.' })
if ($respaldos.Count) { throw "el bundle descargado lleva $($respaldos.Count) respaldo(s)" }

$desc = HashVst3 $bundle.FullName
Write-Host ("[nube] descargado: {0}  {1} MB  SHA256 {2}" -f $desc.Path, $desc.MB, $desc.SHA.Substring(0, 12))

if ($NoInstalar) {
    Write-Host "Sin instalar (-NoInstalar). Bundle en: $($bundle.FullName)" -ForegroundColor Yellow
    exit 0
}

# --------------------------------------------------------------- instalar ---
if (EsAdmin) {
    Instalar $bundle.FullName $Destino $BackupRoot
} else {
    Write-Host '[nube] instalando: Windows pedira permiso de administrador (UAC).' -ForegroundColor Cyan
    $psi = @{
        FilePath     = 'powershell.exe'
        Verb         = 'RunAs'
        ArgumentList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath,
                         '-Source', $bundle.FullName, '-Destino', $Destino, '-BackupRoot', $BackupRoot)
        Wait         = $true
    }
    $p = Start-Process @psi -PassThru
    if ($p.ExitCode -ne 0) { throw "la instalacion con permisos de administrador salio con $($p.ExitCode)" }
}
