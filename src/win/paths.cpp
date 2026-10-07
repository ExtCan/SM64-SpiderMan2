#ifdef _WIN32

#include "paths.h"

#include <system_error>
#include <vector>

namespace sm2m {

static std::filesystem::path ModuleFile(HMODULE mod) {
    std::vector<wchar_t> buf(1024);
    for (;;) {
        DWORD n = GetModuleFileNameW(mod, buf.data(), DWORD(buf.size()));
        if (n == 0) return {};
        if (n < buf.size() - 1) return std::filesystem::path(std::wstring(buf.data(), n));
        buf.resize(buf.size() * 2);
    }
}

bool ResolveModPaths(HMODULE self, ModPaths& out) {
    out.gameExe = ModuleFile(nullptr);
    std::filesystem::path dll = ModuleFile(self);
    if (out.gameExe.empty() || dll.empty()) return false;
    out.gameDir = out.gameExe.parent_path();
    out.moduleDir = dll.parent_path();
    out.resourcesDir = out.moduleDir / L"resources" / L"sm2mario";
    out.dataDir = out.gameDir / L"sm2mario";
    std::error_code ec;
    std::filesystem::create_directories(out.dataDir, ec);
    return true;
}

bool CopyIfMissing(const std::filesystem::path& src, const std::filesystem::path& dst) {
    std::error_code ec;
    if (std::filesystem::exists(dst, ec)) return true;
    if (!std::filesystem::exists(src, ec)) return false;
    return std::filesystem::copy_file(src, dst, ec);
}

std::string PathToUtf8(const std::filesystem::path& p) {
    const std::wstring& w = p.native();
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

} // namespace sm2m

#endif
