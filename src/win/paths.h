#pragma once

#ifdef _WIN32

#include <filesystem>
#include <string>

#include "../common/platform.h"

namespace sm2m {

struct ModPaths {
    std::filesystem::path gameDir;      // folder of the game executable
    std::filesystem::path gameExe;      // full path of the game executable
    std::filesystem::path moduleDir;    // folder of this DLL (<game>/scripts)
    std::filesystem::path resourcesDir; // <moduleDir>/resources/sm2mario (shipped defaults, sm64.dll)
    std::filesystem::path dataDir;      // <game>/sm2mario (ROM, user config, logs; survives Overstrike reinstalls)
};

bool ResolveModPaths(HMODULE self, ModPaths& out);

// Copies `src` to `dst` if `dst` does not exist yet.
bool CopyIfMissing(const std::filesystem::path& src, const std::filesystem::path& dst);

std::string PathToUtf8(const std::filesystem::path& p);

} // namespace sm2m

#endif
