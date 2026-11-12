// The game window: shows the picture from a Display through the Direct3D 12 presenter at the monitor's
// refresh rate and turns Windows keyboard and mouse messages into input events for the game thread.
#pragma once

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <string>

#include "runtime/display.h"

namespace nax {

struct WindowOptions {
    std::wstring title;
    int filter = 0;           // 0 sharp pixels, 1 smooth edges, 2 CRT
    bool fullscreen = false;  // borderless fullscreen on the current monitor
    std::filesystem::path settings;  // where the last filter is remembered
    std::filesystem::path shots;     // where F12 saves screenshots of the game picture
    double shot_after = 0;           // with a value above 0, one screenshot is saved after that many seconds
};

// Runs the window until the user closes it or finished becomes true. Must be called on the main thread.
// Returns false with *error set when the window or Direct3D 12 cannot be created.
bool run_window(Display& display, const uint8_t* guest_memory, const WindowOptions& options,
                const std::atomic<bool>& finished, std::string* error);

}  // namespace nax
