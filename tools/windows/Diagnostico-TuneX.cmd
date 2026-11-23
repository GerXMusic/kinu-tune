@echo off
rem TuneX - diagnostico con doble clic.
rem Deja un ZIP en el Escritorio y dice su ruta. No cambia nada de tu equipo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0recoger-diagnostico.ps1"
echo.
pause
