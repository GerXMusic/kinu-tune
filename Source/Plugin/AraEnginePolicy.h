#pragma once

#include <optional>

#include "../Audio/SynthesisEngineType.h"

namespace pitchnet
{
// D-179: the engine written to the shared ARA modification by any instance wins
// over the stale engine cached in a per-instance Project copy. If the
// modification has no value, the Project's own value (or the global preference
// handled by the caller) is used.
inline std::optional<SynthesisEngineType> chooseSharedEngine(
    const std::optional<SynthesisEngineType> &modificationEngine,
    const std::optional<SynthesisEngineType> &projectEngine)
{
    // D-179: the shared modification value is the latest one written by any
    // instance; the per-instance Project copy may be stale.
    if (modificationEngine)
        return modificationEngine;
    return projectEngine;
}
} // namespace pitchnet
