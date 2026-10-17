// Exercise the production Windows shell-link writer against a scratch desktop.
#include <cassert>
#include <fstream>
#include "../runtime/ui/game_shortcut.mm"
#include <shellapi.h>

static std::string test_root;
static int test_slot = GAME_PROFILE_ACNH;
static bool library_ready = true;
static TitleProfile profile = {};
extern "C" const TitleProfile *title_profile_validate(const char *, char *, size_t) { return &profile; }
extern "C" bool gamelib_present(const char *) { return library_ready; }
extern "C" const char *rt_data_root(void) { return test_root.c_str(); }
extern "C" int game_profile_index(const char *, const char *) { return test_slot; }
extern "C" const char *game_profile_id(int) { return "acnh/1.0.0"; }

int main(int argc, char **argv) {
    assert(argc == 2);
    namespace fs = std::filesystem;
    test_root = utf8((fs::u8path(argv[1]) / L"\u6e38\u620f \u6570\u636e").wstring());
    fs::create_directories(fs::u8path(test_root));
    profile.key = "acnh"; profile.name = "Animal Crossing: New Horizons"; profile.version = "1.0.0";
    fs::path desktop = fs::u8path(test_root) / L"desktop \u684c\u9762";
    fs::create_directories(desktop);
    _wputenv_s(L"LARECOMP_SHORTCUT_DIR", desktop.c_str());
    fs::path game = fs::u8path(test_root) / L"games with spaces";
    fs::create_directories(game);
    fs::path unrelated = desktop / L"Animal Crossing- New Horizons 1.0.0.lnk";
    { std::ofstream file(unrelated); file << "unrelated"; }
    char output[4096], reason[512];
    library_ready = false;
    assert(!game_shortcut_create(utf8(game.wstring()).c_str(), output, sizeof output, reason, sizeof reason));
    assert(std::distance(fs::directory_iterator(desktop), fs::directory_iterator{}) == 1);
    library_ready = true;
    if (!game_shortcut_create(utf8(game.wstring()).c_str(), output, sizeof output, reason, sizeof reason)) {
        fprintf(stderr, "%s\n", reason); return 1;
    }
    std::string first = output;
    assert(fs::u8path(first).filename() == L"Animal Crossing- New Horizons 1.0.0 (2).lnk");
    assert(game_shortcut_create(utf8(game.wstring()).c_str(), output, sizeof output, reason, sizeof reason));
    assert(first == output && std::distance(fs::directory_iterator(desktop), fs::directory_iterator{}) == 2);
    { std::ifstream file(unrelated); std::string value; file >> value; assert(value == "unrelated"); }
    assert(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)));
    {
        ComOwner<IShellLinkW> link; ComOwner<IPersistFile> file;
        assert(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void **)&link.p)));
        assert(SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void **)&file.p)));
        assert(SUCCEEDED(file->Load(fs::u8path(output).c_str(), STGM_READ)));
        wchar_t target[32768], current[32768], arguments[32768], working[32768];
        GetModuleFileNameW(nullptr, current, 32768);
        assert(SUCCEEDED(link->GetPath(target, 32768, nullptr, SLGP_RAWPATH)) && !wcscmp(target, current));
        assert(SUCCEEDED(link->GetWorkingDirectory(working, 32768)) && wide(test_root.c_str()) == working);
        assert(SUCCEEDED(link->GetArguments(arguments, 32768)));
        int count;
        LPWSTR *parts = CommandLineToArgvW((L"runtime " + std::wstring(arguments)).c_str(), &count);
        assert(parts && count == 6 && std::wstring(parts[1]) == L"--play" && std::wstring(parts[2]) == L"--data-root");
        assert(std::wstring(parts[3]) == wide(test_root.c_str()) && std::wstring(parts[4]) == L"--game" && std::wstring(parts[5]) == L"acnh/1.0.0");
        LocalFree(parts);
        // Quoting also round-trips trailing slashes and embedded quotes.
        std::wstring unusual = L"C:\\space folder\\\"quoted\\";
        parts = CommandLineToArgvW((L"runtime " + quote_arg(unusual)).c_str(), &count);
        assert(parts && count == 2 && std::wstring(parts[1]) == unusual); LocalFree(parts);
    }
    CoUninitialize();
    puts("PASS: native shell links, Unicode paths, fixed title arguments, replacement and unrelated-file preservation");
}
