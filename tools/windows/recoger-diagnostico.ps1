# TuneX - recogida de diagnostico
# Uso: doble clic en Diagnostico-TuneX.cmd, o bien:
#   powershell -NoProfile -ExecutionPolicy Bypass -File recoger-diagnostico.ps1
# No cambia nada: solo lee y copia. Deja un ZIP en el Escritorio.
# Nota: los textos van sin tildes a proposito, para que PowerShell 5 no los estropee.

param(
    [string]$Rpp = "",
    [string]$OutDir = "$env:USERPROFILE\Desktop"
)

$ErrorActionPreference = 'Continue'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$work = Join-Path $env:TEMP "tunex-diagnostico-$stamp"
New-Item -ItemType Directory -Path $work -Force | Out-Null
$summary = New-Object System.Collections.Generic.List[string]

function Add-Line([string]$text) {
    $summary.Add($text)
    Write-Host $text
}

Add-Line "TuneX - diagnostico $stamp"
Add-Line "Equipo: $env:COMPUTERNAME  Usuario: $env:USERNAME"
Add-Line ""

# ---------------------------------------------------------------- REAPER
$reaper = Get-Process reaper -ErrorAction SilentlyContinue
if ($reaper) {
    foreach ($p in $reaper) {
        Add-Line "REAPER en marcha: PID $($p.Id), arrancado $($p.StartTime), hilos $($p.Threads.Count), CPU total $([math]::Round($p.CPU,1)) s"
    }
} else {
    Add-Line "REAPER no esta en marcha (abrelo con el proyecto cargado antes de lanzar esto)."
}
Add-Line ""

# ---------------------------------------------------- modulos cargados
$mods = @()
if ($reaper) {
    foreach ($p in $reaper) {
        try {
            $mods += $p.Modules | Where-Object { $_.ModuleName -match 'tune|kinu' }
        } catch {
            Add-Line "No se pudieron leer los modulos del proceso (prueba a ejecutar como administrador)."
        }
    }
}
$modsFile = Join-Path $work "modulos-cargados.txt"
if ($mods.Count -gt 0) {
    $mods | ForEach-Object {
        $hash = ""
        try { $hash = (Get-FileHash $_.FileName -Algorithm SHA256).Hash } catch {}
        $ver = ""
        try { $ver = (Get-Item $_.FileName).VersionInfo.FileVersion } catch {}
        "$($_.ModuleName)`t$($_.FileName)`t$ver`t$hash" | Out-File -FilePath $modsFile -Encoding UTF8 -Append
    }
    Add-Line "Plugin cargado en REAPER (esto es lo que suena de verdad):"
    $mods | ForEach-Object { Add-Line "   $($_.FileName)" }
} else {
    "sin modulos con 'tune' o 'kinu' cargados" | Out-File -FilePath $modsFile -Encoding UTF8
    Add-Line "REAPER no tiene cargado ningun plugin TuneX/Kinu en este momento."
}
Add-Line ""

# ------------------------------------------------- plugins instalados
$instFile = Join-Path $work "plugins-instalados.txt"
$vstDirs = @("C:\Program Files\Common Files\VST3")
foreach ($dir in $vstDirs) {
    if (-not (Test-Path $dir)) { continue }
    Get-ChildItem $dir -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match 'tune|kinu' } |
        ForEach-Object {
            $bundle = $_
            "$($bundle.FullName)`t$($bundle.LastWriteTime)" | Out-File $instFile -Encoding UTF8 -Append
            Get-ChildItem $bundle.FullName -Recurse -File -Filter *.vst3 -ErrorAction SilentlyContinue |
                ForEach-Object {
                    $h = ""
                    try { $h = (Get-FileHash $_.FullName -Algorithm SHA256).Hash } catch {}
                    "   $($_.FullName)`t$([math]::Round($_.Length/1MB,2)) MB`t$($_.LastWriteTime)`t$h" |
                        Out-File $instFile -Encoding UTF8 -Append
                }
            Get-ChildItem $bundle.FullName -Recurse -File -Filter *.dll -ErrorAction SilentlyContinue |
                ForEach-Object {
                    "   dll: $($_.Name)`t$([math]::Round($_.Length/1MB,2)) MB" | Out-File $instFile -Encoding UTF8 -Append
                }
        }
}
Add-Line "Plugins instalados (carpetas con 'tune' o 'kinu'):"
if (Test-Path $instFile) { Get-Content $instFile | ForEach-Object { Add-Line "   $_" } } else { Add-Line "   ninguna" }
Add-Line ""

# ------------------------------------------------------------ registros
$logRoots = @(
    "$env:APPDATA\TuneX\Logs",
    "$env:APPDATA\GerX\TuneX\Logs",
    "$env:APPDATA\Keinoaza\TuneX\Logs"
)
$logsOut = Join-Path $work "logs"
New-Item -ItemType Directory -Path $logsOut -Force | Out-Null
$anyLog = $false
foreach ($root in $logRoots) {
    if (-not (Test-Path $root)) { continue }
    $files = Get-ChildItem $root -File -Filter *.log -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 6
    foreach ($f in $files) {
        $anyLog = $true
        Copy-Item $f.FullName (Join-Path $logsOut $f.Name) -Force
        Add-Line "Registro: $($f.FullName) ($([math]::Round($f.Length/1KB,1)) KB, $($f.LastWriteTime))"
    }
}
if (-not $anyLog) { Add-Line "No hay registros del plugin en las carpetas habituales." }
Add-Line ""

# ----------------------------------------------------------- configuracion
$cfgOut = Join-Path $work "config"
New-Item -ItemType Directory -Path $cfgOut -Force | Out-Null
$cfgPaths = @(
    "$env:APPDATA\TuneX\config.json",
    "$env:APPDATA\GerX\TuneX\config.json",
    "$env:APPDATA\Keinoaza\TuneX\config.json",
    "$env:APPDATA\TuneX\Models\config.json"
)
foreach ($c in $cfgPaths) {
    if (Test-Path $c) {
        Copy-Item $c (Join-Path $cfgOut ((Split-Path $c -Parent | Split-Path -Leaf) + "-config.json")) -Force
        Add-Line "Config: $c"
    }
}
Add-Line ""

# --------------------------------------------------------------- modelos
$modFile = Join-Path $work "modelos.txt"
$modelDirs = @(
    "$env:APPDATA\TuneX\models",
    "$env:APPDATA\GerX\TuneX\models",
    "$env:APPDATA\Keinoaza\TuneX\models",
    "$env:PROGRAMDATA\TuneX\models",
    "$env:PROGRAMDATA\GerX\TuneX\models",
    "$env:PROGRAMDATA\Keinoaza\TuneX\models"
)
foreach ($d in $modelDirs) {
    if (Test-Path $d) {
        "$d" | Out-File $modFile -Encoding UTF8 -Append
        Get-ChildItem $d -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object {
            "   $($_.Name)`t$([math]::Round($_.Length/1MB,2)) MB" | Out-File $modFile -Encoding UTF8 -Append
        }
    }
}
Add-Line "Carpetas de modelos encontradas:"
if (Test-Path $modFile) { Get-Content $modFile | ForEach-Object { Add-Line "   $_" } } else { Add-Line "   ninguna" }
Add-Line ""

# --------------------------------------------------------------- proyecto
if (-not $Rpp) {
    $candidates = @()
    $searchRoots = @("$env:USERPROFILE\Desktop", "$env:USERPROFILE\Documents")
    foreach ($root in $searchRoots) {
        if (Test-Path $root) {
            $candidates += Get-ChildItem $root -Recurse -File -Filter GUI.rpp -ErrorAction SilentlyContinue |
                Sort-Object LastWriteTime -Descending | Select-Object -First 3
        }
    }
    if ($candidates.Count -gt 0) { $Rpp = $candidates[0].FullName }
}
if ($Rpp -and (Test-Path $Rpp)) {
    Add-Line "Proyecto: $Rpp ($((Get-Item $Rpp).LastWriteTime))"
    $refs = Join-Path $work "proyecto-referencias.txt"
    Select-String -Path $Rpp -Pattern 'tune|kinu|\.vst3|ARA' -CaseSensitive:$false |
        ForEach-Object { "$($_.LineNumber): $($_.Line.Trim())" } | Out-File $refs -Encoding UTF8
    Add-Line "Referencias del proyecto al plugin:"
    Get-Content $refs -ErrorAction SilentlyContinue | Select-Object -First 12 | ForEach-Object { Add-Line "   $_" }
} else {
    Add-Line "No se encontro el .rpp. Si lo sabes, pasa la ruta: -Rpp `"C:\ruta\GUI.rpp`""
}
Add-Line ""

# ------------------------------------------------------------------ salida
$summary | Out-File (Join-Path $work "resumen.txt") -Encoding UTF8
$zip = Join-Path $OutDir "TuneX-diagnostico-$stamp.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $work '*') -DestinationPath $zip -Force
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "Listo. Envia este archivo:"
Write-Host "   $zip"
