# TuneX - instalar la build local en el sistema (requiere administrador)
# Uso: clic derecho > Ejecutar como administrador, sobre Instalar-TuneX-local.cmd
# Hace tres cosas:
#   1. Aparta el "Kinu Tune.vst3" viejo fuera de la carpeta de plugins.
#   2. Copia la build nueva "TuneX.vst3" a C:\Program Files\Common Files\VST3.
#   3. Dice que hay que reescanear en el DAW.
# No borra nada: lo apartado se queda en una carpeta de respaldo con fecha.

param(
    [string]$Source = "",
    [string]$Vst3Dir = "C:\Program Files\Common Files\VST3",
    [string]$BackupRoot = "$env:USERPROFILE\Desktop\Opencode\respaldo-plugins"
)

$ErrorActionPreference = 'Stop'

$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "Este script necesita permisos de administrador." -ForegroundColor Yellow
    Write-Host "Cierra esta ventana y usa clic derecho > Ejecutar como administrador."
    Read-Host "Pulsa Intro para salir"
    exit 1
}

if (-not $Source) {
    $candidates = Get-ChildItem "$PSScriptRoot\..\..\build-dml\PitchNetPlugin_artefacts\Release\VST3" -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -eq 'TuneX.vst3' }
    if ($candidates.Count -gt 0) { $Source = $candidates[0].FullName }
}
if (-not $Source -or -not (Test-Path $Source)) {
    Write-Host "No encuentro la build compilada (build-dml\...\VST3\TuneX.vst3)." -ForegroundColor Red
    Write-Host "Compila primero el objetivo PitchNetPlugin_VST3."
    Read-Host "Pulsa Intro para salir"
    exit 1
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$backup = Join-Path $BackupRoot $stamp
New-Item -ItemType Directory -Path $backup -Force | Out-Null

# 1. Apartar el plugin viejo (y cualquier backup que hubiera dentro de la carpeta VST3)
Get-ChildItem $Vst3Dir -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match 'kinu|\.old\.' } |
    ForEach-Object {
        Move-Item $_.FullName (Join-Path $backup $_.Name) -Force
        Write-Host "Apartado: $($_.Name)  ->  $backup" -ForegroundColor Yellow
    }
Get-ChildItem $Vst3Dir -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match 'kinu|\.old\.' } |
    ForEach-Object {
        Move-Item $_.FullName (Join-Path $backup $_.Name) -Force
        Write-Host "Apartado: $($_.Name)  ->  $backup" -ForegroundColor Yellow
    }

# 2. Instalar la build nueva
$dest = Join-Path $Vst3Dir 'TuneX.vst3'
if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
Copy-Item $Source $dest -Recurse -Force
$inner = Get-ChildItem $dest -Recurse -File -Filter *.vst3 | Select-Object -First 1
$hash = (Get-FileHash $inner.FullName -Algorithm SHA256).Hash

Write-Host ""
Write-Host "Instalado: $dest" -ForegroundColor Green
Write-Host "  fichero: $($inner.FullName)"
Write-Host "  tamano : $([math]::Round($inner.Length/1MB,2)) MB"
Write-Host "  SHA256 : $hash"
Write-Host ""
Write-Host "Ahora, en el DAW: Opciones > Preferencias > Plug-ins > VST, pulsa 'Re-escanear' (o 'Clear cache/re-scan')."
Write-Host "El plugin aparecera como 'TuneX (GerX)'."
Read-Host "Pulsa Intro para cerrar"
