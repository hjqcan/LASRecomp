#include "../game_shortcut.h"
#include "../game_profiles.h"
#include "../gamelib.h"
#include "../paths.h"
#include "../title_profile.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>

template<class T> struct ComOwner {
    T *p = nullptr;
    ~ComOwner() { if (p) p->Release(); }
    T *operator->() const { return p; }
};
static std::wstring wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, nullptr, 0);
    if (!n) return {};
    std::wstring result((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, result.data(), n);
    result.pop_back(); return result;
}
static std::string utf8(const std::wstring &s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string result((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), result.data(), n, nullptr, nullptr);
    return result;
}
static std::wstring quote_arg(const std::wstring &s) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t ch : s) {
        if (ch == L'\\') { slashes++; continue; }
        result.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0; result += ch;
    }
    result.append(slashes * 2, L'\\'); result += L'"'; return result;
}
static bool managed_link(const std::filesystem::path &path, const std::wstring &identity) {
    ComOwner<IShellLinkW> link; ComOwner<IPersistFile> file;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void **)&link.p)) ||
        FAILED(link->QueryInterface(IID_IPersistFile, (void **)&file.p)) ||
        FAILED(file->Load(path.c_str(), STGM_READ))) return false;
    wchar_t description[2048] = {};
    return SUCCEEDED(link->GetDescription(description, 2048)) && identity == description;
}
static bool create_windows_shortcut(const char *game_dir, const TitleProfile *profile,
                                    char *output, size_t output_size, char *why, size_t why_size) {
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
        snprintf(why, why_size, "Cannot initialize desktop shortcut support (%08lx).", (unsigned long)initialized); return false;
    }
    struct Apartment { bool owned; ~Apartment() { if (owned) CoUninitialize(); } } apartment{SUCCEEDED(initialized)};
    try {
        namespace fs = std::filesystem;
        fs::path desktop;
        // Test runs use a scratch desktop. Windows resolves the real desktop,
        // including OneDrive and other redirected known-folder locations.
        if (const wchar_t *override_dir = _wgetenv(L"LARECOMP_SHORTCUT_DIR")) desktop = override_dir;
        else {
            PWSTR value = nullptr;
            HRESULT result = SHGetKnownFolderPath(FOLDERID_Desktop, KF_FLAG_DEFAULT, nullptr, &value);
            if (FAILED(result)) { snprintf(why, why_size, "Cannot locate the Windows desktop."); return false; }
            desktop = value; CoTaskMemFree(value);
        }
        if (!fs::is_directory(desktop)) { snprintf(why, why_size, "The desktop folder is unavailable."); return false; }
        wchar_t executable[32768];
        DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
        if (!length || length >= 32768) { snprintf(why, why_size, "Cannot locate the running application."); return false; }
        fs::path root = fs::absolute(fs::u8path(game_dir)).lexically_normal();
        int slot = game_profile_index(game_dir, rt_data_root());
        fs::path stable = slot >= 0 ? fs::absolute(fs::u8path(rt_data_root()) / game_profile_install_dir(slot)).lexically_normal() : root;
        std::wstring identity = L"LAS Recomp game: " + wide(profile->key) + L"/" + wide(profile->version) + L" @ " + stable.wstring();
        std::wstring args = L"--play --data-root " + quote_arg(wide(rt_data_root()));
        if (slot > GAME_PROFILE_EXISTING) args += L" --game " + quote_arg(wide(game_profile_id(slot)));
        else args += L" " + quote_arg(stable.wstring());
        std::wstring label = wide(profile->name) + L" " + wide(profile->version);
        for (wchar_t &ch : label) if (wcschr(L"<>:\"/\\|?*", ch)) ch = L'-';
        fs::path destination;
        for (int i = 0; i < 100; i++) {
            destination = desktop / (label + (i ? L" (" + std::to_wstring(i + 1) + L")" : L"") + L".lnk");
            if (!fs::exists(destination) || managed_link(destination, identity)) break;
            if (i == 99) { snprintf(why, why_size, "Too many existing desktop shortcuts with this name."); return false; }
        }
        ComOwner<IShellLinkW> link; ComOwner<IPersistFile> file;
        HRESULT result = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void **)&link.p);
        if (SUCCEEDED(result)) result = link->SetPath(executable);
        if (SUCCEEDED(result)) result = link->SetArguments(args.c_str());
        if (SUCCEEDED(result)) result = link->SetWorkingDirectory(wide(rt_data_root()).c_str());
        if (SUCCEEDED(result)) result = link->SetDescription(identity.c_str());
        fs::path icon = root / "icon.ico";
        if (SUCCEEDED(result)) result = link->SetIconLocation(fs::is_regular_file(icon) ? icon.c_str() : executable, 0);
        if (SUCCEEDED(result)) result = link->SetShowCmd(SW_SHOWNORMAL);
        if (SUCCEEDED(result)) result = link->QueryInterface(IID_IPersistFile, (void **)&file.p);
        fs::path temporary = destination;
        temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
        if (SUCCEEDED(result)) result = file->Save(temporary.c_str(), TRUE);
        if (FAILED(result) || !MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            std::error_code ignored; fs::remove(temporary, ignored);
            snprintf(why, why_size, "Cannot save the desktop shortcut (%08lx).", (unsigned long)result); return false;
        }
        std::string path = utf8(destination.wstring());
        snprintf(output, output_size, "%s", path.c_str());
        return true;
    } catch (const std::exception &error) {
        snprintf(why, why_size, "Cannot create the desktop shortcut: %s", error.what()); return false;
    }
}
#endif

extern "C" bool game_shortcut_create(const char *game_dir, char *output, size_t output_size, char *why, size_t why_size) {
    if (output_size) output[0] = 0;
    const TitleProfile *profile = title_profile_validate(game_dir, why, why_size);
    if (!profile) return false;
    if (!gamelib_present(game_dir)) { snprintf(why, why_size, "Build this game's code before creating its shortcut."); return false; }
#ifdef _WIN32
    return create_windows_shortcut(game_dir, profile, output, output_size, why, why_size);
#else
    snprintf(why, why_size, "Desktop shortcut creation is available on Windows."); return false;
#endif
}
