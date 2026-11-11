// D-204: politica pura del veto de re-analisis cuando existe un archivo ARA
// restaurado. Sin JUCE ni host, para poder probarla en Release.
#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace ara_restore
{
enum class Decision
{
    Analyze,
    Hydrate,
    Pending
};

// Un archivo restaurado valido manda: no se analiza salvo re-analisis explicito.
inline bool shouldVetoAnalysis(bool hasValidRestoredArchive,
                               bool explicitReanalysis)
{
    return hasValidRestoredArchive && !explicitReanalysis;
}

// Con archivo: hidratar; si falla, quedarse pendiente (nunca analizar).
inline Decision decideRestoreAction(bool hasValidRestoredArchive, bool hydrated,
                                    bool explicitReanalysis)
{
    if (!hasValidRestoredArchive || explicitReanalysis)
        return Decision::Analyze;
    return hydrated ? Decision::Hydrate : Decision::Pending;
}

// El analisis no puede pisar un archivo restaurado salvo re-analisis explicito.
inline bool shouldOverwriteArchive(bool hasValidRestoredArchive,
                                   bool explicitReanalysis)
{
    return !hasValidRestoredArchive || explicitReanalysis;
}

// Un fallo de hidratacion con archivo valido se registra y se avisa; no se
// convierte en un analisis silencioso.
inline bool shouldWarnHydrationFailure(bool hasValidRestoredArchive,
                                       bool hydrated, bool explicitReanalysis)
{
    return hasValidRestoredArchive && !hydrated && !explicitReanalysis;
}

inline std::string normalizeArchivePath(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    std::transform(path.begin(), path.end(), path.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    while (!path.empty() && path.back() == '/')
        path.pop_back();
    return path;
}

inline std::string archiveBasename(const std::string &path)
{
    const auto normalized = normalizeArchivePath(path);
    const auto slash = normalized.find_last_of('/');
    return slash == std::string::npos ? normalized : normalized.substr(slash + 1);
}

// El persistentID de ARA es mutable (REAPER lo pasa de absoluto a relativo al
// primer guardado). Se compara la ruta completa y, si no coincide, el nombre de
// archivo; liveSourceName cubre hosts que dan un ID opaco.
inline bool archivedIdMatches(const std::string &archived,
                              const std::string &liveId,
                              const std::string &liveSourceName = {})
{
    const auto archivedPath = normalizeArchivePath(archived);
    const auto livePath = normalizeArchivePath(liveId);
    const auto sourcePath = normalizeArchivePath(liveSourceName);
    if (archivedPath.empty() || (livePath.empty() && sourcePath.empty()))
        return false;
    if (!livePath.empty() && archivedPath == livePath)
        return true;

    const auto archivedName = archiveBasename(archivedPath);
    if (archivedName.empty())
        return false;
    if (!livePath.empty() && archivedName == archiveBasename(livePath))
        return true;
    return !sourcePath.empty() && archivedName == archiveBasename(sourcePath);
}
} // namespace ara_restore
