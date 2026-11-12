// The game window.
#include "runtime/window.h"

#include <windowsx.h>

#include <cstdio>
#include <vector>

#include "runtime/presenter.h"

namespace nax {

namespace {

struct State {
    Display* display = nullptr;
    Presenter* presenter = nullptr;
    int filter = 0;
    bool fullscreen = false;
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
    std::filesystem::path settings;
    std::wstring title;
};
State* g_state = nullptr;

// Saves the game picture, at its own size and before filtering, as a 24-bit BMP file.
bool save_bmp(const std::filesystem::path& file, const uint32_t* rgba, uint32_t w, uint32_t h) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    FILE* f = _wfopen(file.c_str(), L"wb");
    if (!f) return false;
    const uint32_t row = (w * 3 + 3) & ~3u, size = 54 + row * h;
    uint8_t header[54] = {'B', 'M'};
    auto put32 = [&](int at, uint32_t v) { for (int i = 0; i < 4; i++) header[at + i] = uint8_t(v >> (8 * i)); };
    put32(2, size);
    put32(10, 54);
    put32(14, 40);
    put32(18, w);
    put32(22, h);
    header[26] = 1;
    header[28] = 24;
    put32(34, row * h);
    std::fwrite(header, 1, 54, f);
    std::vector<uint8_t> line(row, 0);
    for (uint32_t y = h; y-- > 0;) {
        for (uint32_t x = 0; x < w; x++) {
            const uint32_t p = rgba[size_t(y) * w + x];
            line[x * 3 + 0] = uint8_t(p >> 16);
            line[x * 3 + 1] = uint8_t(p >> 8);
            line[x * 3 + 2] = uint8_t(p);
        }
        std::fwrite(line.data(), 1, row, f);
    }
    std::fclose(f);
    return true;
}

bool g_shot_requested = false;

const wchar_t* const kFilterNames[Presenter::kFilterCount] = {L"sharp pixels", L"smooth edges", L"CRT"};

void update_title(HWND hwnd) {
    const std::wstring title = g_state->title + L" - " + kFilterNames[g_state->filter] + L" (F11 changes the filter)";
    SetWindowTextW(hwnd, title.c_str());
}

void save_settings() {
    if (g_state->settings.empty()) return;
    if (FILE* f = _wfopen(g_state->settings.c_str(), L"w")) {
        std::fprintf(f, "filter=%d\nfullscreen=%d\n", g_state->filter, g_state->fullscreen ? 1 : 0);
        std::fclose(f);
    }
}

void set_fullscreen(HWND hwnd, bool on) {
    if (on == g_state->fullscreen) return;
    g_state->fullscreen = on;
    if (on) {
        GetWindowPlacement(hwnd, &g_state->placement);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(hwnd, &g_state->placement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    save_settings();
}

// Maps a client-area position to a pixel of the game picture, clamped to the picture.
void to_game(HWND hwnd, int cx, int cy, int* gx, int* gy) {
    (void)hwnd;
    const RECT r = g_state->presenter->picture_rect();
    const int w = int(g_state->display->width.load()), h = int(g_state->display->height.load());
    const int pw = r.right - r.left, ph = r.bottom - r.top;
    if (w <= 0 || h <= 0 || pw <= 0 || ph <= 0) { *gx = *gy = 0; return; }
    int x = int((long long)(cx - r.left) * w / pw), y = int((long long)(cy - r.top) * h / ph);
    *gx = x < 0 ? 0 : x >= w ? w - 1 : x;
    *gy = y < 0 ? 0 : y >= h ? h - 1 : y;
}

void mouse_button(int button, bool down) {
    InputEvent e{InputEvent::MouseButton};
    e.button = button;
    e.down = down;
    g_state->display->push(e);
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        if (g_state && g_state->presenter) g_state->presenter->resize();
        return 0;
    case WM_CLOSE:
        g_state->display->quit = true;  // the game thread ends and the main loop then closes the window
        return 0;
    case WM_SETCURSOR:
        // The game draws its own mouse pointer, so the Windows pointer is hidden over the client area.
        if (LOWORD(lp) == HTCLIENT) { SetCursor(nullptr); return TRUE; }
        break;
    case WM_SYSKEYDOWN:
    case WM_KEYDOWN:
    case WM_SYSKEYUP:
    case WM_KEYUP: {
        const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
        if (wp == VK_RETURN && (lp & (1 << 29))) {  // Alt+Enter
            if (down && !(lp & (1 << 30))) set_fullscreen(hwnd, !g_state->fullscreen);
            return 0;
        }
        if (wp == VK_F12) {
            if (down && !(lp & (1 << 30))) g_shot_requested = true;
            return 0;
        }
        if (wp == VK_F11) {
            if (down && !(lp & (1 << 30))) {
                g_state->filter = (g_state->filter + 1) % Presenter::kFilterCount;
                update_title(hwnd);
                save_settings();
            }
            return 0;
        }
        if (wp == VK_F4 && (lp & (1 << 29))) break;  // Alt+F4 closes the window
        InputEvent e{InputEvent::Key};
        e.scancode = uint8_t((lp >> 16) & 0xFF);
        e.extended = (lp & (1 << 24)) != 0;
        e.down = down;
        if (e.scancode) g_state->display->push(e);
        return 0;
    }
    case WM_CHAR: {
        InputEvent e{InputEvent::Character};
        e.character = uint16_t(wp);
        e.scancode = uint8_t((lp >> 16) & 0xFF);
        g_state->display->push(e);
        return 0;
    }
    case WM_MOUSEMOVE: {
        InputEvent e{InputEvent::MouseMove};
        to_game(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &e.x, &e.y);
        g_state->display->push(e);
        return 0;
    }
    case WM_LBUTTONDOWN: SetCapture(hwnd); mouse_button(0, true); return 0;
    case WM_LBUTTONUP: ReleaseCapture(); mouse_button(0, false); return 0;
    case WM_RBUTTONDOWN: SetCapture(hwnd); mouse_button(1, true); return 0;
    case WM_RBUTTONUP: ReleaseCapture(); mouse_button(1, false); return 0;
    case WM_MBUTTONDOWN: mouse_button(2, true); return 0;
    case WM_MBUTTONUP: mouse_button(2, false); return 0;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

uint32_t expand(uint8_t six) { return uint32_t((six & 63) << 2 | (six & 63) >> 4); }

}  // namespace

bool run_window(Display& display, const uint8_t* guest_memory, const WindowOptions& options,
                const std::atomic<bool>& finished, std::string* error) {
    State state;
    state.display = &display;
    state.filter = options.filter % Presenter::kFilterCount;
    state.settings = options.settings;
    state.title = options.title;
    g_state = &state;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"NaxFragileAllegiance";
    RegisterClassExW(&wc);
    // The default size shows 640x480 at twice its size where the screen allows it.
    const UINT dpi = GetDpiForSystem();
    RECT r{0, 0, 1280, 960};
    const int screen_h = GetSystemMetrics(SM_CYMAXIMIZED);
    if (r.bottom > screen_h - 40 * int(dpi) / 96) r = {0, 0, 960, 720};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, options.title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        *error = "Creating the game window failed.";
        g_state = nullptr;
        return false;
    }
    Presenter presenter;
    state.presenter = &presenter;
    if (!presenter.init(hwnd, error)) {
        DestroyWindow(hwnd);
        g_state = nullptr;
        return false;
    }
    ShowWindow(hwnd, SW_SHOW);
    update_title(hwnd);
    if (options.fullscreen) set_fullscreen(hwnd, true);

    std::vector<uint32_t> frame;
    bool running = true;
    const ULONGLONG started = GetTickCount64();
    bool timed_shot_done = options.shot_after <= 0;
    int shot_number = 0;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (finished) break;
        uint32_t w = display.width, h = display.height;
        const uint32_t pitch = display.pitch, fb = display.framebuffer;
        if (!display.graphics || w == 0 || h == 0 || w > 2048 || h > 2048) {
            w = 640;
            h = 480;
            frame.assign(size_t(w) * h, 0xFF000000u);
        } else {
            uint32_t palette[256];
            for (int i = 0; i < 256; i++)
                palette[i] = expand(display.palette[i][0]) | (expand(display.palette[i][1]) << 8) |
                             (expand(display.palette[i][2]) << 16) | 0xFF000000u;
            frame.resize(size_t(w) * h);
            if (!display.enabled) {
                std::fill(frame.begin(), frame.end(), 0xFF000000u);
            } else {
                const bool banked = display.banked;
                const uint32_t bank = display.window_bank, window = display.window;
                for (uint32_t y = 0; y < h; y++) {
                    uint32_t* out = &frame[size_t(y) * w];
                    for (uint32_t x = 0; x < w; x++) {
                        const uint32_t offset = y * pitch + x;
                        const uint8_t pixel = banked && (offset >> 16) == bank ? guest_memory[window + (offset & 0xFFFF)]
                                                                              : guest_memory[uint32_t(fb + offset)];
                        out[x] = palette[pixel];
                    }
                }
            }
        }
        if (!timed_shot_done && double(GetTickCount64() - started) / 1000.0 >= options.shot_after) {
            timed_shot_done = true;
            g_shot_requested = true;
        }
        if (g_shot_requested && !options.shots.empty()) {
            g_shot_requested = false;
            wchar_t name[64];
            SYSTEMTIME now;
            GetLocalTime(&now);
            swprintf(name, 64, L"shot-%04u%02u%02u-%02u%02u%02u-%d.bmp", now.wYear, now.wMonth, now.wDay, now.wHour,
                     now.wMinute, now.wSecond, shot_number++);
            save_bmp(options.shots / name, frame.data(), w, h);
        }
        presenter.draw(frame.data(), w, h, state.filter);  // waits for the next vertical blank
    }
    DestroyWindow(hwnd);
    g_state = nullptr;
    return true;
}

}  // namespace nax
