// Cases for tests/test_neural.py. Each case writes a small random model of the compact Real-ESRGAN layout
// into its folder (the second argument), runs it on the GPU (or on WARP, the software rasteriser, when there
// is no hardware device) and compares the result with a plain CPU computation of the same network.
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "runtime/neural.h"

using namespace nax;
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("failed: %s\n", what);
        failures++;
    }
}

// Writes an ncnn model with convolutions 3->64, (hidden times) 64->64 and 64->48, PReLU after all but the
// last. With half set, the weights are stored as 16-bit floats.
void write_model(const fs::path& folder, int hidden, bool half, NeuralModel* expected) {
    std::mt19937 random(1234);
    std::uniform_real_distribution<float> weight_range(-0.08f, 0.08f), slope_range(0.0f, 0.3f);
    FILE* param = _wfopen((folder / "model.param").c_str(), L"w");
    FILE* bin = _wfopen((folder / "model.bin").c_str(), L"wb");
    std::fprintf(param, "7767517\n%d %d\nInput input 0 1 data\n", 2 + 2 * (hidden + 1) + 1, 0);
    expected->layers.clear();
    const int count = hidden + 2;
    for (int l = 0; l < count; l++) {
        NeuralModel::Layer layer;
        layer.inputs = l == 0 ? 3 : 64;
        layer.outputs = l == count - 1 ? 48 : 64;
        const int n = layer.inputs * layer.outputs * 9;
        std::fprintf(param, "Convolution conv%d 1 1 a b 0=%d 1=3 4=1 5=1 6=%d\n", l, layer.outputs, n);
        for (int i = 0; i < n; i++) layer.weights.push_back(weight_range(random));
        for (int i = 0; i < layer.outputs; i++) layer.bias.push_back(weight_range(random));
        if (half) {
            const uint32_t tag = 0x01306B47;
            std::fwrite(&tag, 4, 1, bin);
            for (float& w : layer.weights) {
                // Round to a 16-bit float, and keep the rounded value as the expected weight.
                uint32_t bits;
                std::memcpy(&bits, &w, 4);
                const uint32_t sign = (bits >> 16) & 0x8000;
                const int exponent = int((bits >> 23) & 0xFF) - 112;
                uint16_t h = uint16_t(sign);
                if (exponent > 0) h = uint16_t(sign | (uint32_t(exponent) << 10) | ((bits >> 13) & 0x3FF));
                std::fwrite(&h, 2, 1, bin);
                const uint32_t back = h & 0x7FFF ? ((h & 0x8000u) << 16) | ((((h >> 10) & 0x1Fu) + 112) << 23) | ((h & 0x3FFu) << 13)
                                                 : (h & 0x8000u) << 16;
                std::memcpy(&w, &back, 4);
            }
            if (n % 2) std::fwrite("\0\0", 2, 1, bin);
        } else {
            const uint32_t tag = 0;
            std::fwrite(&tag, 4, 1, bin);
            std::fwrite(layer.weights.data(), 4, size_t(n), bin);
        }
        std::fwrite(layer.bias.data(), 4, layer.bias.size(), bin);
        if (l != count - 1) {
            std::fprintf(param, "PReLU prelu%d 1 1 b c 0=64\n", l);
            for (int i = 0; i < 64; i++) layer.slopes.push_back(slope_range(random));
            std::fwrite(layer.slopes.data(), 4, 64, bin);
        }
        expected->layers.push_back(layer);
    }
    std::fprintf(param, "PixelShuffle shuffle 1 1 c d 0=4\nInterp resize 1 1 data e 0=1 1=4.0 2=4.0\nBinaryOp add 2 1 d e output\n");
    std::fclose(param);
    std::fclose(bin);
}

// The network on the CPU, over the picture mirrored into kPadding pixels on every side, with zero padding
// beyond that, as NeuralUpscaler computes it. Returns RGB 0..255 of the enlarged picture.
std::vector<float> reference(const NeuralModel& model, const std::vector<uint32_t>& rgba, int w, int h) {
    const int P = NeuralUpscaler::kPadding, dw = w + 2 * P, dh = h + 2 * P;
    auto mirror = [](int v, int size) { if (v < 0) v = -v; if (v >= size) v = 2 * size - 2 - v; return v; };
    std::vector<float> x(size_t(3) * dw * dh);
    for (int y = 0; y < dh; y++)
        for (int i = 0; i < dw; i++) {
            const uint32_t p = rgba[size_t(mirror(y - P, h)) * w + mirror(i - P, w)];
            for (int c = 0; c < 3; c++) x[(size_t(c) * dh + y) * dw + i] = float((p >> (8 * c)) & 0xFF) / 255.0f;
        }
    std::vector<float> a = x;
    for (const NeuralModel::Layer& layer : model.layers) {
        std::vector<float> b(size_t(layer.outputs) * dw * dh);
        for (int o = 0; o < layer.outputs; o++)
            for (int y = 0; y < dh; y++)
                for (int i = 0; i < dw; i++) {
                    double sum = layer.bias[size_t(o)];
                    for (int c = 0; c < layer.inputs; c++)
                        for (int ky = 0; ky < 3; ky++)
                            for (int kx = 0; kx < 3; kx++) {
                                const int yy = y + ky - 1, xx = i + kx - 1;
                                if (yy < 0 || xx < 0 || yy >= dh || xx >= dw) continue;
                                sum += double(layer.weights[((size_t(o) * layer.inputs + c) * 3 + ky) * 3 + kx]) *
                                       a[(size_t(c) * dh + yy) * dw + xx];
                            }
                    float v = float(sum);
                    if (!layer.slopes.empty() && v < 0) v *= layer.slopes[size_t(o)];
                    b[(size_t(o) * dh + y) * dw + i] = v;
                }
        a.swap(b);
    }
    std::vector<float> out(size_t(w) * 4 * h * 4 * 3);
    for (int y = 0; y < h * 4; y++)
        for (int i = 0; i < w * 4; i++)
            for (int c = 0; c < 3; c++) {
                const int ch = c * 16 + (y % 4) * 4 + (i % 4);
                float v = a[(size_t(ch) * dh + (y / 4 + P)) * dw + (i / 4 + P)] + x[(size_t(c) * dh + (y / 4 + P)) * dw + (i / 4 + P)];
                out[(size_t(y) * w * 4 + i) * 3 + c] = std::fmin(std::fmax(v, 0.0f), 1.0f) * 255.0f;
            }
    return out;
}

// A device, a command list and the transfer of the picture and the result.
struct Gpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 fence_value = 0;
    ComPtr<ID3D12Resource> frame, upload, readback;
    int w = 0, h = 0;

    bool init(int width, int height) {
        w = width;
        h = height;
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
            ComPtr<IDXGIFactory4> factory;
            ComPtr<IDXGIAdapter> warp;
            if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
                FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
                return false;
        }
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
        device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
        device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list));
        list->Close();
        device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        D3D12_HEAP_PROPERTIES def{}, up{}, rb{};
        def.Type = D3D12_HEAP_TYPE_DEFAULT;
        up.Type = D3D12_HEAP_TYPE_UPLOAD;
        rb.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = UINT64(w);
        td.Height = UINT(h);
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&frame));
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 1 << 20;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
        device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
        return true;
    }

    static D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        return b;
    }

    // Uploads the picture, runs the upscaler over rects and reads the enlarged picture back as RGBA.
    std::vector<uint32_t> run(NeuralUpscaler& up, const std::vector<uint32_t>& rgba, const std::vector<PictureRect>& rects, bool first) {
        const UINT pitch = UINT((w * 4 + 255) & ~255);
        uint8_t* mapped = nullptr;
        upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
        for (int y = 0; y < h; y++) std::memcpy(mapped + size_t(y) * pitch, &rgba[size_t(y) * w], size_t(w) * 4);
        upload->Unmap(0, nullptr);
        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
        if (!first) {
            auto b = transition(frame.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            list->ResourceBarrier(1, &b);
            auto o = transition(up.output(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->ResourceBarrier(1, &o);
        }
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = frame.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, UINT(w), UINT(h), 1, pitch};
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        auto b = transition(frame.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &b);
        up.record(list.Get(), frame.Get(), rects);
        auto o = transition(up.output(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &o);
        const UINT out_pitch = UINT((w * 16 + 255) & ~255);
        D3D12_TEXTURE_COPY_LOCATION rdst{}, rsrc{};
        rdst.pResource = readback.Get();
        rdst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        rdst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, UINT(w * 4), UINT(h * 4), 1, out_pitch};
        rsrc.pResource = up.output();
        rsrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&rdst, 0, 0, 0, &rsrc, nullptr);
        list->Close();
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence.Get(), ++fence_value);
        HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(fence_value, done);
        WaitForSingleObject(done, INFINITE);
        CloseHandle(done);
        std::vector<uint32_t> out(size_t(w) * 4 * h * 4);
        readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
        for (int y = 0; y < h * 4; y++) std::memcpy(&out[size_t(y) * w * 4], mapped + size_t(y) * out_pitch, size_t(w) * 16);
        D3D12_RANGE none{0, 0};
        readback->Unmap(0, &none);
        return out;
    }
};

// The largest difference between the GPU result and the CPU reference, in levels of 0..255.
float largest_difference(const std::vector<uint32_t>& gpu, const std::vector<float>& cpu) {
    float worst = 0;
    for (size_t i = 0; i < gpu.size(); i++)
        for (int c = 0; c < 3; c++) worst = std::fmax(worst, std::fabs(float((gpu[i] >> (8 * c)) & 0xFF) - cpu[i * 3 + size_t(c)]));
    return worst;
}

std::vector<uint32_t> random_picture(int w, int h, unsigned seed) {
    std::mt19937 random(seed);
    std::vector<uint32_t> p(size_t(w) * h);
    for (auto& v : p) v = random() | 0xFF000000u;
    return p;
}

int gpu_result_matches_the_cpu_network(const fs::path& folder) {
    NeuralModel model, expected;
    write_model(folder, 2, false, &expected);
    std::string error;
    check(model.load(folder / "model.param", folder / "model.bin", &error), "the model loads");
    const int w = 23, h = 17;
    Gpu gpu;
    NeuralUpscaler up;
    if (!gpu.init(w, h) || !up.create(gpu.device.Get(), model, &error) || !up.resize(w, h)) {
        std::printf("failed: setting up the GPU (%s)\n", error.c_str());
        return 1;
    }
    const auto picture = random_picture(w, h, 7);
    const auto result = gpu.run(up, picture, {PictureRect{0, 0, w, h}}, true);
    const float worst = largest_difference(result, reference(expected, picture, w, h));
    std::printf("largest difference %.2f\n", worst);
    check(worst <= 2.0f, "every enlarged pixel matches the CPU network within 2 levels");
    return failures;
}

int changed_area_alone_gives_the_same_picture_as_a_full_run(const fs::path& folder) {
    NeuralModel model, expected;
    write_model(folder, 3, false, &expected);
    std::string error;
    model.load(folder / "model.param", folder / "model.bin", &error);
    const int w = 40, h = 30;
    Gpu gpu;
    NeuralUpscaler up;
    if (!gpu.init(w, h) || !up.create(gpu.device.Get(), model, &error) || !up.resize(w, h)) {
        std::printf("failed: setting up the GPU (%s)\n", error.c_str());
        return 1;
    }
    auto picture = random_picture(w, h, 11);
    gpu.run(up, picture, {PictureRect{0, 0, w, h}}, true);
    // Two separate changes, one of them at the edge, each run only over its own rectangle.
    for (int y = 12; y < 15; y++)
        for (int x = 20; x < 24; x++) picture[size_t(y) * w + x] = 0xFF00FFFFu;
    for (int y = 26; y < 30; y++)
        for (int x = 0; x < 3; x++) picture[size_t(y) * w + x] = 0xFF2040FFu;
    const auto result = gpu.run(up, picture, {PictureRect{20, 12, 24, 15}, PictureRect{0, 26, 3, 30}}, false);
    const float worst = largest_difference(result, reference(expected, picture, w, h));
    std::printf("largest difference %.2f\n", worst);
    check(worst <= 2.0f, "after updating only the changed areas the whole picture matches a full computation");
    return failures;
}

int model_with_half_precision_weights_loads(const fs::path& folder) {
    NeuralModel model, expected;
    write_model(folder, 1, true, &expected);
    std::string error;
    check(model.load(folder / "model.param", folder / "model.bin", &error), "a model with 16-bit weights loads");
    check(model.layers.size() == 3, "it has all three convolutions");
    bool same = model.layers.size() == 3;
    for (size_t l = 0; same && l < 3; l++) same = model.layers[l].weights == expected.layers[l].weights;
    check(same, "the 16-bit weights are read as the values they stand for");
    return failures;
}

int model_of_another_layout_is_refused(const fs::path& folder) {
    FILE* param = _wfopen((folder / "model.param").c_str(), L"w");
    std::fprintf(param, "7767517\n2 2\nInput input 0 1 data\nConvolution conv 1 1 data out 0=8 1=3 4=1 5=1 6=216\n");
    std::fclose(param);
    FILE* bin = _wfopen((folder / "model.bin").c_str(), L"wb");
    std::vector<float> zeros(1 + 216 + 8, 0.0f);
    std::fwrite(zeros.data(), 4, zeros.size(), bin);
    std::fclose(bin);
    NeuralModel model;
    std::string error;
    check(!model.load(folder / "model.param", folder / "model.bin", &error), "a network of another shape is not accepted");
    check(!error.empty(), "and the reason is given");
    return failures;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string name = argc > 1 ? argv[1] : "";
    const fs::path folder = argc > 2 ? fs::path(argv[2]) : fs::path(".");
    std::error_code ec;
    fs::remove_all(folder, ec);
    fs::create_directories(folder, ec);
    const struct {
        const char* name;
        int (*run)(const fs::path&);
    } cases[] = {
        {"gpu_result_matches_the_cpu_network", gpu_result_matches_the_cpu_network},
        {"changed_area_alone_gives_the_same_picture_as_a_full_run", changed_area_alone_gives_the_same_picture_as_a_full_run},
        {"model_with_half_precision_weights_loads", model_with_half_precision_weights_loads},
        {"model_of_another_layout_is_refused", model_of_another_layout_is_refused},
    };
    for (const auto& c : cases)
        if (name == c.name) return c.run(folder);
    std::printf("unknown case %s\n", name.c_str());
    return 2;
}
