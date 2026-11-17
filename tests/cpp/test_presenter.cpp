// Pixel checks of the actual DX12 compositor with synthetic textures in a hidden window.
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "runtime/image.h"
#include "runtime/presenter.h"

namespace fs = std::filesystem;

struct Fixture {
    HWND window = nullptr;
    std::unique_ptr<nax::Presenter> presenter;
    fs::path folder;
    uint32_t palette[256]{};
    std::vector<uint32_t> frame{std::vector<uint32_t>(64 * 48, 0xFF303030u)};
    std::shared_ptr<nax::OverlayTexture> texture = std::make_shared<nax::OverlayTexture>();
    std::vector<nax::Overlay> overlays;
    int number = 0;

    explicit Fixture(const fs::path& path, int captures = 1, bool neural = false) : folder(fs::absolute(path)) {
        fs::create_directories(folder);
        SetEnvironmentVariableW(L"NAX_CAPTURE_DIR", folder.c_str());
        std::ofstream(folder / "capture.txt") << captures;
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"NaxPresenterPixelTest";
        RegisterClassW(&wc);
        RECT r{0, 0, 128, 96};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        window = CreateWindowW(wc.lpszClassName, L"Compositor test", WS_OVERLAPPEDWINDOW, 0, 0,
                               r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
        if (!window) throw std::runtime_error("Creating the hidden test window failed");
        presenter = std::make_unique<nax::Presenter>();
        std::string error;
        fs::path model;
        if (neural) {
            // Two zero convolutions leave only the network's nearest-neighbour
            // residual. This exercises compute-to-graphics presentation without
            // requiring installed models or hiding failures behind learned detail.
            model = folder / "identity";
            std::ofstream param(folder / "identity.param");
            param << "7767517\n4 4\nInput input 0 1 data\n"
                  << "Convolution first 1 1 data a 0=64 1=3 5=1 6=1728\n"
                  << "PReLU prelu 1 1 a b 0=64\n"
                  << "Convolution last 1 1 b out 0=48 1=3 5=1 6=27648\n";
            std::ofstream bin(folder / "identity.bin", std::ios::binary);
            const uint32_t tag = 0;
            for (size_t count : {size_t(1728 + 64 + 64), size_t(27648 + 48)}) {
                bin.write(reinterpret_cast<const char*>(&tag), sizeof tag);
                const std::vector<float> zeros(count, 0);
                bin.write(reinterpret_cast<const char*>(zeros.data()), std::streamsize(count * sizeof(float)));
            }
        }
        if (!presenter->init(window, model, &error)) throw std::runtime_error(error);
        if (neural && !presenter->ai_available()) throw std::runtime_error(presenter->ai_problem());
        for (auto& c : palette) c = 0xFF000000u;
        palette[1] = 0xFFFFFFFFu;
        palette[2] = 0xFFFF0000u;
        texture->key = 1;
        texture->version = 1;
        texture->width = texture->height = 4;
        texture->index.assign(16, 1);
        texture->opaque.assign(16, 1);
        texture->hd_width = texture->hd_height = 16;
        texture->hd.assign(256, 0xFF00FF00u);
        texture->used = {{1, 16}};
        for (int c = 0; c < 3; ++c) texture->reference[1][c] = 255;
        texture->reference[2][2] = 255;
        overlays = {{texture, 8, 8, 4, 4}};
        original_rectangle(8, 8, 4, 4, palette[1]);
    }
    ~Fixture() { presenter.reset(); if (window) DestroyWindow(window); }

    void original_rectangle(int x, int y, int w, int h, uint32_t colour) {
        for (int row = y; row < y + h; ++row)
            for (int col = x; col < x + w; ++col) frame[size_t(row) * 64 + col] = colour;
    }
    uint32_t capture(int x = 16, int y = 16, int filter = 0) {
        presenter->draw(frame.data(), 64, 48, filter, palette, overlays);
        wchar_t name[32];
        swprintf(name, 32, L"frame-%05d.png", number++);
        std::vector<uint32_t> output;
        int w = 0, h = 0;
        if (!nax::load_png(folder / name, &output, &w, &h) || w != 128 || h != 96)
            throw std::runtime_error("The actual DX12 frame was not captured");
        return output[size_t(y) * w + x];
    }
};

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const std::string name = argv[1];
    try {
        const bool neural = name == "neural_presentation";
        Fixture f(argv[2], neural || name == "reload_budget" || name == "palette_animation" ? 3 : 1, neural);
        if (neural) {
            f.overlays.clear();
            f.frame.assign(64 * 48, 0xFF506070u);
            const uint32_t computed = f.capture(16, 16, nax::Presenter::kAiFilter);
            const uint32_t cached = f.capture(16, 16, nax::Presenter::kAiFilter);
            f.frame[0] = 0xFFFFFFFFu;  // dispatch compute again while this pixel stays unchanged
            const uint32_t changed = f.capture(16, 16, nax::Presenter::kAiFilter);
            std::printf("neural presentation: computed %08x, cached %08x, changed %08x\n", computed, cached, changed);
            return computed == 0xFF506070u && cached == computed && changed == computed ? 0 : 1;
        }
        uint32_t expected = 0xFF00FF00u;
        if (name == "black_fade" || name == "half_fade" || name == "palette_animation") {
            for (int c = 0; c < 3; ++c) f.texture->reference[1][c] = 8;
            f.texture->hd.assign(256, 0xFF808080u);
            f.palette[1] = name == "half_fade" ? 0xFF040404u : 0xFF000000u;
            f.original_rectangle(8, 8, 4, 4, f.palette[1]);
            expected = name == "half_fade" ? 0xFF404040u : 0xFF000000u;
            if (name == "palette_animation") {
                if (f.capture() != 0xFF000000u) throw std::runtime_error("The first black fade glowed");
                f.palette[1] = 0xFF040404u;
                f.original_rectangle(8, 8, 4, 4, f.palette[1]);
                if (f.capture() != 0xFF404040u) throw std::runtime_error("The half fade did not halve the enhanced detail");
                f.palette[1] = 0xFF000000u;
                f.original_rectangle(8, 8, 4, 4, f.palette[1]);
            }
        } else if (name == "scaled_occlusion" || name == "scaled_visible" || name == "scaled_transparency") {
            f.overlays[0].width = f.overlays[0].height = 2;
            f.frame[8 * 64 + 8] = f.palette[2];
            if (name == "scaled_occlusion") {
                // The original blitter overwrites its first source row with
                // the second. This blue neighbour is not its final writer.
                f.texture->index[1] = 2;
                expected = f.palette[2];
            } else if (name == "scaled_visible") {
                f.texture->index[5] = 2;  // final opaque writer of pixel (8,8)
            } else {
                f.texture->index[4] = 2;
                f.texture->opaque[5] = 0;  // leaves the preceding blue pixel visible
            }
        } else if (name == "fractional_scale") {
            f.overlays[0].x = f.overlays[0].y = 8.5f;
            f.overlays[0].width = f.overlays[0].height = 2;
            f.texture->index[0] = 2;
            f.frame[8 * 64 + 8] = f.palette[2];
        } else if (name == "transparent_hole") {
            f.texture->opaque[0] = 0;
            f.frame[8 * 64 + 8] = 0xFF303030u;
            expected = 0xFF303030u;
        } else if (name == "reload_budget") {
            if (f.capture() != expected) throw std::runtime_error("The initial texture did not draw");
            auto replacement = std::make_shared<nax::OverlayTexture>(*f.texture);
            replacement->version = 2;
            replacement->hd_width = replacement->hd_height = 4096;
            replacement->hd.assign(size_t(4096) * 4096, 0xFFFF0000u);
            for (int c = 0; c < 3; ++c) replacement->reference[1][c] = 8;
            f.overlays[0].texture = replacement;
        } else {
            throw std::runtime_error("Unknown compositor case");
        }
        const uint32_t actual = f.capture();
        std::printf("%s: expected %08x, actual %08x\n", name.c_str(), expected, actual);
        return actual == expected ? 0 : 1;
    } catch (const std::exception& error) {
        std::printf("failed: %s\n", error.what());
        return 2;
    }
}
