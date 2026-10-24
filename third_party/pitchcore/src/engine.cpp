#include "pitchcore/engine.h"

namespace pitchcore {

const char* engineName (EngineKind k)
{
    switch (k)
    {
        case EngineKind::Neural:   return "neural";
        case EngineKind::Spectral: return "espectral";
        case EngineKind::Classic:  break;
    }
    return "classic";
}

bool parseEngine (const std::string& s, EngineKind& out)
{
    // Se aceptan los nombres en español (los que usa el utillaje) y los
    // internos en inglés.
    if (s == "classic"   || s == "clasico"  || s == "0" || s == "psola")
    { out = EngineKind::Classic;  return true; }
    if (s == "neural"    || s == "neuronal" || s == "1" || s == "nsf")
    { out = EngineKind::Neural;   return true; }
    if (s == "spectral"  || s == "espectral"|| s == "2")
    { out = EngineKind::Spectral; return true; }
    return false;
}

} // namespace pitchcore
