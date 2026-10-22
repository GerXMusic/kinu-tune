// Kinu Tune - Copyright (C) 2026 Keinoaza
// Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.
#pragma once

#include "../Audio/EditorController.h"
#include "../Audio/SynthesisEngineType.h"

#include <memory>

/**
 * The one place the plug-in creates its rendering controllers.
 *
 * The processor keeps several EditorControllers (composite analysis, the
 * region on the canvas, one per modification rendered in the background) and
 * binds the editor to only one of them. Every one of them must render with the
 * engine the user chose, so they are all created here and handed the same
 * SynthesisEnginePreference; a controller created anywhere else would start
 * on the default engine and never learn of a change. tests/SourceGuard.cmake
 * (rule render-controllers) fails the build when Source/Plugin constructs an
 * EditorController(false) outside this file.
 */
inline std::unique_ptr<EditorController>
makeSharedEngineController(std::shared_ptr<SynthesisEnginePreference> engine)
{
  auto controller = std::make_unique<EditorController>(false);
  controller->shareSynthesisEngine(std::move(engine));
  return controller;
}
