# Dispara windows-pkg.yml sobre una rama, espera y descarga los artefactos.
# Solo para builds locales: no crea release ni tag.
[CmdletBinding()]
param(
    [string]$Repo = 'GerXMusic/tunex',
    [string]$Workflow = 'windows-pkg.yml',
    [string]$Ref = 'fix/delete-note-attack',
    [string]$WorkDir = "$env:TEMP\opencode\tunex-017"
)

$ErrorActionPreference = 'Stop'
$gh = (Get-Command gh -ErrorAction Stop).Source

$null = & cmd.exe /d /c "`"$gh`" auth status >nul 2>nul"
if ($LASTEXITCODE -ne 0) {
    $cred = "protocol=https`nhost=github.com`n`n" | git credential fill 2>$null
    $tok = ($cred | Select-String '^password=').Line -replace '^password=', ''
    if (-not $tok) { throw 'sin autenticacion de GitHub (gh auth login)' }
    $env:GH_TOKEN = $tok
    Write-Host '[nube] token de git (no se muestra)'
}

$antes = @(& gh run list -R $Repo --workflow=$Workflow --limit 10 --json databaseId |
    ConvertFrom-Json | ForEach-Object { $_.databaseId })
& gh workflow run $Workflow -R $Repo --ref $Ref
if ($LASTEXITCODE -ne 0) { throw 'no pude disparar el workflow' }

$runId = $null
for ($i = 0; $i -lt 60 -and -not $runId; $i++) {
    Start-Sleep -Seconds 5
    $nuevos = @(& gh run list -R $Repo --workflow=$Workflow --limit 10 --json databaseId |
        ConvertFrom-Json | ForEach-Object { $_.databaseId })
    $runId = $nuevos | Where-Object { $_ -notin $antes } | Select-Object -First 1
}
if (-not $runId) { throw 'no aparecio ningun run nuevo en 5 minutos' }
Write-Host "[nube] run $runId ($Ref)"
$runId | Out-File -FilePath (Join-Path $env:TEMP 'opencode\logs\run-id.txt') -Encoding ascii

& gh run watch $runId -R $Repo --exit-status --interval 20
$watchExit = $LASTEXITCODE
& gh run view $runId -R $Repo --json conclusion,headSha,createdAt,url |
    Out-File -FilePath (Join-Path $env:TEMP 'opencode\logs\run-view.json') -Encoding utf8
if ($watchExit -ne 0) { throw "el run $runId no termino en verde" }

New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null
Get-ChildItem $WorkDir -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force
foreach ($artefacto in @('TuneX-Windows-x64-Setup', 'TuneX-vst3-Windows-x64', 'TuneX-models-Windows-x64')) {
    & gh run download $runId -R $Repo -n $artefacto -D (Join-Path $WorkDir $artefacto)
    if ($LASTEXITCODE -ne 0) { throw "no pude descargar $artefacto" }
}
Get-ChildItem $WorkDir -Recurse -File | Select-Object FullName, Length |
    Out-File -FilePath (Join-Path $env:TEMP 'opencode\logs\artefactos.txt') -Encoding utf8
Write-Host "[nube] artefactos en $WorkDir"
