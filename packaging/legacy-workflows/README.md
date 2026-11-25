# Workflows retirados (2026-11-25)

`windows-release.yml` (VST3 CPU desde un tag) y `windows-installer.yml` (instalador
construido desde el zip de ese release) fueron el camino de 0.1.3. Se sacaron de
`.github/workflows` porque podían producir un **segundo binario con la misma versión**:
`windows-release.yml` compila CPU, mientras la entrega de producción es DirectML.

El camino vigente es `.github/workflows/windows-pkg.yml`: compila el VST3 DirectML **una
vez**, lo comprueba (`check-vst3.ps1`), lo comprime y construye el instalador **desde ese
zip**, con `check-installer.ps1 -ExpectedSha` para que el SHA del VST3 instalado coincida
con el del zip. Produce los dos artefactos de la release.

Para un build CPU puntual, usar CMake con `-DUSE_DIRECTML=OFF` (ver `README.md`); no se
publica como entrega.

## `pkg-check.yml` (2026-11-26, D-300)

Era el workflow aparte que instalaba un `.pkg` ya construido y lo validaba con `pluginval`.
En 0.1.6 su contenido pasa al job `check` de `.github/workflows/macos-pkg.yml`: el `.pkg`
se construye y se verifica en la misma ejecución, y **solo se sube como artefacto final**
si instala y pasa `pluginval` strictness 5. Este fichero se conserva solo para revisar
artefactos antiguos; no es el camino vigente.
