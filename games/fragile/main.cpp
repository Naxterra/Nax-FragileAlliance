// Entry point of the native Fragile Allegiance program.
#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <string>

#include "games/fragile/game.h"

namespace nax {
const ProgramInfo* translated_program();
}

namespace {

std::filesystem::path executable_folder() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}

// The GOG installer records the game folder under its product id.
std::filesystem::path gog_folder() {
    wchar_t value[MAX_PATH];
    DWORD size = sizeof value;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\GOG.com\\Games\\1207659059", L"path", RRF_RT_REG_SZ,
                     nullptr, value, &size) == ERROR_SUCCESS)
        return value;
    return {};
}

std::filesystem::path user_folder() {
    PWSTR base = nullptr;
    std::filesystem::path folder;
    if (SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base) == S_OK) folder = std::filesystem::path(base) / L"Nax Fragile Allegiance";
    CoTaskMemFree(base);
    if (folder.empty()) folder = executable_folder() / L"user";
    std::filesystem::create_directories(folder);
    return folder;
}

bool is_game_folder(const std::filesystem::path& folder) {
    return !folder.empty() && std::filesystem::exists(folder / L"FRAGILE.EXE");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::filesystem::path data;
    std::string arguments;
    for (int i = 1; i < argc; i++) {
        const std::wstring arg = argv[i];
        if (arg == L"--data" && i + 1 < argc) {
            data = argv[++i];
        } else {
            if (!arguments.empty()) arguments += ' ';
            arguments += std::filesystem::path(arg).string();
        }
    }
    if (data.empty()) data = gog_folder();
    if (!is_game_folder(data)) {
        std::fprintf(stderr, "The Fragile Allegiance folder was not found. Pass --data <folder with FRAGILE.EXE>.\n");
        return 2;
    }

    nax::Game game;
    game.data_dir = data;
    game.user_dir = user_folder();
    game.arguments = arguments;
    game.set_log(game.user_dir / L"runtime.log");
    // Drive C: is the game folder as the GOG release sets it up for DOSBox, with GOG's cloud save folder and
    // then the user folder on top. Drive D: is the CD image.
    game.files.mount_folder('C', game.user_dir / L"C", {data / L"cloud_saves", data});
    if (!game.files.mount_image('D', data / L"FRAGILE.DAT"))
        game.log("The CD image FRAGILE.DAT could not be read, so drive D: is missing.");
    game.files.clean_temporary_files();
    game.install(nax::translated_program());
    try {
        game.load(executable_folder() / L"image.bin");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 3;
    }
    const int code = game.run();
    game.files.close_all(game.failure().empty());
    if (!game.failure().empty()) {
        game.log("%s", game.failure().c_str());
        std::fprintf(stderr, "%s\n", game.failure().c_str());
        return 1;
    }
    return code;
}
