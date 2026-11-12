// Entry point of the native Fragile Allegiance program. The translated game runs on its own thread with a
// large stack, and the main thread runs the window that shows its picture and collects input.
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
#include <cstdio>
#include <string>

#include "games/fragile/game.h"
#include "runtime/window.h"

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

void report(const std::string& text) {
    std::fprintf(stderr, "%s\n", text.c_str());
    const std::wstring wide(text.begin(), text.end());
    MessageBoxW(nullptr, wide.c_str(), L"Fragile Allegiance", MB_OK | MB_ICONERROR);
}

struct Run {
    nax::Game* game = nullptr;
    std::atomic<bool> finished{false};
    int code = 0;
};

DWORD WINAPI game_thread(void* parameter) {
    Run* run = static_cast<Run*>(parameter);
    run->code = run->game->run();
    run->finished = true;
    return 0;
}

int read_filter(const std::filesystem::path& settings, bool* fullscreen) {
    int filter = 0;
    if (FILE* f = _wfopen(settings.c_str(), L"r")) {
        char line[64];
        while (std::fgets(line, sizeof line, f)) {
            int value = 0;
            if (std::sscanf(line, "filter=%d", &value) == 1) filter = value;
            if (std::sscanf(line, "fullscreen=%d", &value) == 1) *fullscreen = value != 0;
        }
        std::fclose(f);
    }
    return filter < 0 ? 0 : filter;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // Messages also reach a console when the program is started from one.
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
    }
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::filesystem::path data;
    std::string arguments;
    bool fullscreen = false, windowed = false;
    double shot_after = 0;
    for (int i = 1; i < argc; i++) {
        const std::wstring arg = argv[i];
        if (arg == L"--data" && i + 1 < argc) data = argv[++i];
        else if (arg == L"--fullscreen") fullscreen = true;
        else if (arg == L"--window") windowed = true;
        else if (arg == L"--screenshot-after" && i + 1 < argc) shot_after = _wtof(argv[++i]);
        else {
            if (!arguments.empty()) arguments += ' ';
            arguments += std::filesystem::path(arg).string();
        }
    }
    LocalFree(argv);
    if (data.empty()) data = gog_folder();
    if (!is_game_folder(data)) {
        report("The Fragile Allegiance folder was not found. Pass --data <folder with FRAGILE.EXE>.");
        return 2;
    }

    static nax::Game game;
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
    game.load_shift = nax::fa::kShift;
    try {
        game.load(executable_folder() / L"image.bin");
    } catch (const std::exception& error) {
        report(error.what());
        return 3;
    }

    nax::WindowOptions options;
    options.title = L"Fragile Allegiance";
    options.settings = game.user_dir / L"settings.txt";
    options.shots = game.user_dir / L"shots";
    options.shot_after = shot_after;
    bool remembered_fullscreen = false;
    options.filter = read_filter(options.settings, &remembered_fullscreen);
    options.fullscreen = windowed ? false : (fullscreen || remembered_fullscreen);

    Run run;
    run.game = &game;
    // Translated calls nest on the host stack like the original calls, so the game thread gets a large one.
    HANDLE thread = CreateThread(nullptr, 256u << 20, game_thread, &run, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!thread) {
        report("Starting the game thread failed.");
        return 4;
    }
    std::string error;
    if (!nax::run_window(game.display, game.mem, options, run.finished, &error)) {
        game.display.quit = true;
        report(error);
    }
    // The window closed or the game ended. The game thread stops at its next poll.
    game.display.quit = true;
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    // Files the game was still writing are kept only when it ended by itself.
    game.files.close_all(game.failure().empty() && !game.quit_requested);
    if (!game.failure().empty()) {
        game.log("%s", game.failure().c_str());
        report(game.failure());
        return 1;
    }
    return run.code;
}
