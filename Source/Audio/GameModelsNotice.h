#pragma once

#include <juce_core/juce_core.h>

/**
 * V6: la falta de modelos GAME es estado del editor, no un dialogo.
 *
 * `EditorController::segmentIntoNotes` publica este estado y el editor lo pinta
 * en el rail (EnginePanel::setBlockNotice); el detalle va al log y al tooltip.
 * Nada de este camino abre una ventana modal, y por eso se puede probar sin GUI.
 */
namespace gameModels
{
inline constexpr const char *requiredFiles[] = {"encoder.onnx", "segmenter.onnx",
                                                "estimator.onnx", "bd2dur.onnx",
                                                "config.json"};

struct ModelFiles
{
  bool allPresent = false;
  int presentCount = 0;
  int missingCount = 0;
};

inline ModelFiles checkRequiredFiles(const juce::File &modelDir)
{
  ModelFiles files;
  for (const auto *name : requiredFiles)
  {
    if (modelDir.getChildFile(name).existsAsFile())
      ++files.presentCount;
    else
      ++files.missingCount;
  }
  files.allPresent = files.missingCount == 0;
  return files;
}

// The same information the old modal carried, as plain text for the log and
// the rail tooltip: both paths, the detector state and per-file OK/MISSING.
inline juce::String describeUnavailable(const juce::File &modelDir,
                                        const juce::File &bundleDir,
                                        bool detectorCreated,
                                        bool detectorLoaded,
                                        const juce::String &headline)
{
  juce::String detail = headline + "\n\n"
                                   "Searched path: " +
                        modelDir.getFullPathName() + "\n"
                                                     "Bundle path: " +
                        bundleDir.getFullPathName() + "\n"
                                                      "gameDetector: " +
                        juce::String(detectorCreated ? "created" : "null") +
                        "\n"
                        "isLoaded: " +
                        juce::String(!detectorCreated ? "N/A"
                                                      : (detectorLoaded ? "true"
                                                                        : "false")) +
                        "\n"
                        "isDirectory: " +
                        juce::String(modelDir.isDirectory() ? "true" : "false") +
                        "\n\n"
                        "Required files: encoder.onnx, segmenter.onnx, "
                        "estimator.onnx, bd2dur.onnx, config.json\n\n";

  for (const auto *name : requiredFiles)
  {
    const auto file = modelDir.getChildFile(name);
    detail += juce::String(name) + ": " +
              (file.existsAsFile() ? "OK" : "MISSING") + "\n";
  }
  return detail;
}
} // namespace gameModels
