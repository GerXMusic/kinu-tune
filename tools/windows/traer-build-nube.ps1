# TuneX - traer de GitHub el ultimo VST3 compilado en la nube e instalarlo
#
# Para que: compilar en la maquina local la dejaba inutilizable (D-276). El runner
# de GitHub compila el VST3 con la misma configuracion que produccion (DirectML ON,
# runtime DirectML incluido, diagnosticos OFF) y lo deja como artefacto 7 dias.
# Este script lo pide, lo descarga y lo instala.
#
# Requisitos (una sola vez):  winget install --id GitHub.cli
# Autenticacion: se usa el token que git ya tiene en el Credential Manager; si no
# hay, hay que hacer `gh auth login`. El script nunca imprime el token.
#
# Uso normal:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\windows\traer-build-nube.ps1
# Pedir un build nuevo y esperarlo:
#   ... -Disparar
# Solo descargar y mirar que trae:
#   ... -NoInstalar
#
# Guardas: D-240 (exactamente un .vst3 dentro del bundle, ningun respaldo dentro)
# y el plugin anterior se aparta con fecha en respaldo-plugins, no se borra.

[CmdletBinding()]
param(
    [string]$Workflow = 'windows-vst3.yml',
    [string]$Artefacto = 'TuneX-vst3-Windows-x64',
    [string]$Repo = '',        # owner/repo; por defecto, el remoto origin
    [string]$Ref = '',         # rama o commit a compilar (vacio = rama actual)
    [switch]$Disparar,
    [switch]$NoInstalar,
    [string]$Destino = 'C:\Program Files\Common Files\VST3',
    [string]$WorkDir = "$env:TEMP\opencode\tunex-nube",
    [string]$BackupRoot = "$env:USERPROFILE\Desktop\Opencode\respaldo-plugins",
    [string]$Source = ''       # interno: lo usa el relanzamiento con administrador
)

$ErrorActionPreference = 'Stop'

function EsAdmin {
    ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

function HashVst3([string]$bundle) {
    $inner = Get-ChildItem $bundle -Recurse -File -Filter *.vst3 | Select-Object -First 1
    if (-not $inner) { return $null }
    [pscustomobject]@{
        Path = $inner.FullName
        SHA  = (Get-FileHash $inner.FullName -Algorithm SHA256).Hash
        MB   = [math]::Round($inner.Length / 1MB, 2)
    }
}

function RepoDeOrigin {
    $url = (git remote get-url origin 2>$null)
    if (-not $url) { throw 'no hay remoto origin (¿estas dentro del repo tunex?)' }
    $m = [regex]::Match($url, 'github\.com[:/]([^/]+)/([^/.]+)')
    if (-not $m.Success) { throw "no pude leer owner/repo de origin: $url" }
    "$($m.Groups[1].Value)/$($m.Groups[2].Value)"
}

# ---------------------------------------------------------------- instalar ---
function Instalar([string]$bundle, [string]$destino, [string]$backupRoot) {
    $info = HashVst3 $bundle
    if (-not $info) { throw "el bundle no tiene ningun .vst3 dentro: $bundle" }

    $dest = Join-Path $destino 'TuneX.vst3'
    if (Test-Path $dest) {
        $actual = HashVst3 $dest
        if ($actual -and $actual.SHA -eq $info.SHA) {
            Write-Host "Ya esta instalado ese mismo fichero (SHA $($info.SHA.Substring(0, 8))...). No toco nada." -ForegroundColor Green
            return
        }
        $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
        $backup = Join-Path $backupRoot $stamp
        New-Item -ItemType Directory -Path $backup -Force | Out-Null
        Move-Item $dest (Join-Path $backup 'TuneX.vst3') -Force
        Write-Host "Instalado anterior apartado en: $backup" -ForegroundColor Yellow
    }

    # restos viejos sueltos en la carpeta de plugins
    $sueltos = @(Get-ChildItem $destino -Force -ErrorAction SilentlyContinue |
                 Where-Object { $_.Name -match '(?i)kinu|\.old\.' })
    if ($sueltos.Count) {
        $b2 = Join-Path $backupRoot ((Get-Date -Format 'yyyyMMdd-HHmmss') + '-sueltos')
        New-Item -ItemType Directory -Path $b2 -Force | Out-Null
        foreach ($s in $sueltos) {
            Move-Item $s.FullName (Join-Path $b2 $s.Name) -Force
            Write-Host "Apartado de la carpeta de plugins: $($s.Name)" -ForegroundColor Yellow
        }
    }

    Copy-Item $bundle $dest -Recurse -Force
    $nuevo = HashVst3 $dest
    Write-Host ''
    Write-Host "Instalado: $dest" -ForegroundColor Green
    Write-Host "  fichero: $($nuevo.Path)"
    Write-Host "  tamano : $($nuevo.MB) MB"
    Write-Host "  SHA256 : $($nuevo.SHA)"
    Write-Host ''
    Write-Host 'En REAPER: Opciones > Preferencias > Plug-ins > VST > "Re-escanear".'
    Write-Host 'Recuerda (D-240): instalado no es cargado. Verifica el modulo del proceso:'
    Write-Host '  Get-Process reaper | ForEach-Object { $_.Modules } | Where-Object ModuleName -like "*TuneX*" | Select-Object ModuleName,FileName'
}

if ($Source) {
    Instalar $Source $Destino $BackupRoot
    exit 0
}

# ------------------------------------------------------------------ gh listo ---
# winget instala gh en Program Files pero las sesiones ya abiertas no ven el PATH nuevo
$ghExe = $null
$cmd = Get-Command gh -ErrorAction SilentlyContinue
if ($cmd) {
    $ghExe = $cmd.Source
} else {
    foreach ($c in @("$env:ProgramFiles\GitHub CLI\gh.exe", "${env:ProgramFiles(x86)}\GitHub CLI\gh.exe",
                     "$env:LOCALAPPDATA\GitHubCLI\gh.exe", "$env:LOCALAPPDATA\Programs\GitHub CLI\gh.exe")) {
        if (Test-Path $c) { $ghExe = $c; break }
    }
    if ($ghExe) { $env:Path = "$(Split-Path -Parent $ghExe);$env:Path" }
}
if (-not $ghExe) {
    Write-Host 'No encuentro GitHub CLI (gh).' -ForegroundColor Red
    Write-Host '  winget install --id GitHub.cli' -ForegroundColor Yellow
    exit 1
}
$gh = $ghExe

if (-not $Repo) { $Repo = RepoDeOrigin }
Write-Host "[nube] repo $Repo  workflow $Workflow"

# autenticacion: gh auth, o el token de git en el Credential Manager
# (`gh auth status` escribe en stderr cuando no hay sesion, y con EAP=Stop eso aborta;
#  por eso se pregunta a cmd, que solo devuelve un codigo)
$null = & cmd.exe /d /c "`"$gh`" auth status >nul 2>nul"
if ($LASTEXITCODE -ne 0) {
    $cred = "protocol=https`nhost=github.com`n`n" | git credential fill 2>$null
    $tok = ($cred | Select-String '^password=').Line -replace '^password=', ''
    if (-not $tok) {
        Write-Host 'Sin autenticacion de GitHub.' -ForegroundColor Red
        Write-Host 'Ejecuta: gh auth login   (el navegador pide tu cuenta)' -ForegroundColor Yellow
        exit 1
    }
    $env:GH_TOKEN = $tok
    Write-Host '[nube] usando el token que git ya tenia guardado (no se muestra)' -ForegroundColor DarkGray
}

# --------------------------------------------------------------- disparar ---
if ($Disparar) {
    $antes = @(& gh run list -R $Repo --workflow=$Workflow --limit 10 --json databaseId | ConvertFrom-Json | ForEach-Object { $_.databaseId })
    $arg = @('workflow', 'run', $Workflow, '-R', $Repo)
    if ($Ref) { $arg += @('--ref', $Ref) }
    & gh @arg
    if ($LASTEXITCODE -ne 0) { throw 'no pude disparar el workflow (¿existe en la rama remota? ¿token sin permiso actions?)' }
    Write-Host '[nube] build pedido; esperando a que aparezca el run...'
    $runId = $null
    for ($i = 0; $i -lt 60 -and -not $runId; $i++) {
        Start-Sleep -Seconds 5
        $nuevos = @(& gh run list -R $Repo --workflow=$Workflow --limit 10 --json databaseId | ConvertFrom-Json | ForEach-Object { $_.databaseId })
        $runId = $nuevos | Where-Object { $_ -notin $antes } | Select-Object -First 1
    }
    if (-not $runId) { throw 'no aparecio ningun run nuevo en 5 minutos' }
    Write-Host "[nube] run $runId; compila en GitHub, tu PC no trabaja. Ctrl+C corta solo la espera." -ForegroundColor Cyan
    & gh run watch $runId -R $Repo --exit-status --interval 20
    if ($LASTEXITCODE -ne 0) {
        throw "el run $runId no termino en verde. Detalle: gh run view $runId -R $Repo --log-failed"
    }
    $id = $runId
} else {
    $runs = & gh run list -R $Repo --workflow=$Workflow --status success --limit 1 --json databaseId,headSha,createdAt | ConvertFrom-Json
    if (-not $runs) {
        Write-Host "No hay ningun run exitoso de $Workflow todavia." -ForegroundColor Red
        Write-Host 'Lanza uno con: ... -Disparar' -ForegroundColor Yellow
        exit 1
    }
    $id = $runs.databaseId
    Write-Host ("[nube] ultimo run verde: {0}  {1}  {2}" -f $id, $runs.createdAt, $runs.headSha.Substring(0, 8))
}

# -------------------------------------------------------------- descargar ---
New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null
Get-ChildItem $WorkDir -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
& gh run download $id -R $Repo -n $Artefacto -D $WorkDir
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
Write-Host ("[nube] descargado: {0}  {1} MB  SHA256 {2}..." -f $desc.Path, $desc.MB, $desc.SHA.Substring(0, 12))

if ($NoInstalar) {
    Write-Host "Sin instalar (-NoInstalar). Bundle en: $($bundle.FullName)" -ForegroundColor Yellow
    exit 0
}

# --------------------------------------------------------------- instalar ---
if (EsAdmin) {
    Instalar $bundle.FullName $Destino $BackupRoot
} else {
    Write-Host '[nube] instalando: Windows pedira permiso de administrador (UAC).' -ForegroundColor Cyan
    $p = Start-Process powershell.exe -Verb RunAs -Wait -PassThru -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"",
        '-Source', "`"$($bundle.FullName)`"", '-Destino', "`"$Destino`"", '-BackupRoot', "`"$BackupRoot`""
    )
    if ($p.ExitCode -ne 0) { throw "la instalacion con permisos de administrador salio con $($p.ExitCode)" }
}
