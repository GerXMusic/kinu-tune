# Nucleo de Pitchcore vendorizado (D-095).
#
# Este CMakeLists es DEL FORK: no se copia del nucleo (que arrastraria CLIs,
# tests y ONNX Runtime). Lista las 12 fuentes del motor clasico; si el nucleo
# gana una fuente, tools/sync-pitchcore.ps1 falla hasta que se anada aqui.
#
# La copia de include/ y src/ la genera tools/sync-pitchcore.ps1 y no se edita
# a mano.

add_library(pitchcore STATIC
    src/f0_yin.cpp
    src/segment.cpp
    src/wav.cpp
    src/psola.cpp
    src/synth.cpp
    src/nsf.cpp
    src/resample.cpp
    src/pitch_curve.cpp
    src/engine.cpp
    src/dsp.cpp
    src/spectral_engine.cpp
    src/render_dispatcher.cpp)

target_include_directories(pitchcore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_features(pitchcore PUBLIC cxx_std_17)

# D-093: el bit a bit solo vale sin fast-math. El arbol del fork compila
# pitchnet_core con -ffast-math en compiladores no MSVC; este target fija su
# propio modo para no heredarlo (los flags por target no se propagan, pero el
# CMAKE_CXX_FLAGS global si, y un futuro cambio ahi no debe tocar el nucleo).
if(MSVC)
    target_compile_options(pitchcore PRIVATE /fp:precise)
else()
    target_compile_options(pitchcore PRIVATE -fno-fast-math)
endif()

# Aviso barato si alguien compila la copia con fast-math global.
if(MSVC AND CMAKE_CXX_FLAGS MATCHES "/fp:fast")
    message(WARNING "pitchcore: CMAKE_CXX_FLAGS lleva /fp:fast; D-093 exige sin fast-math.")
elseif(NOT MSVC AND CMAKE_CXX_FLAGS MATCHES "-ffast-math")
    message(WARNING "pitchcore: CMAKE_CXX_FLAGS lleva -ffast-math; D-093 exige sin fast-math.")
endif()
