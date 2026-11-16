// Live AI upscaling of the game picture: a compact Real-ESRGAN network (SRVGGNetCompact, as in Upscayl's
// "upscayl-lite-4x" model) run on the GPU with Direct3D 12 compute shaders. The network enlarges the picture
// four times. Only the parts of the picture that changed are computed again, since the game usually redraws
// small areas (the pointer, a counter) while most of the screen stays the same.
#pragma once

#include <windows.h>

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nax {

// The weights of the network, read from an ncnn model (.param and .bin), as Upscayl installs them.
struct NeuralModel {
    // Each convolution has 3x3 kernels. The first reads 3 channels (RGB), the last writes 48 (three colours of
    // 4x4 sub-pixels), and all others read and write 64; every convolution but the last is followed by PReLU.
    struct Layer {
        int inputs = 0, outputs = 0;
        std::vector<float> weights;  // [output][input][3][3]
        std::vector<float> bias;     // [output]
        std::vector<float> slopes;   // PReLU slopes [output], empty for the last layer
    };
    std::vector<Layer> layers;
    bool load(const std::filesystem::path& param, const std::filesystem::path& bin, std::string* error);
};

// A rectangle of the game picture in its own pixels, right and bottom exclusive.
struct PictureRect {
    int left = 0, top = 0, right = 0, bottom = 0;
};

class NeuralUpscaler {
public:
    static constexpr int kScale = 4;
    // Pixels of mirrored picture around the edges, as Upscayl adds them, so the edges look like its output.
    static constexpr int kPadding = 10;

    bool create(ID3D12Device* device, const NeuralModel& model, std::string* error);
    // Prepares the buffers for a picture of this size. The output texture is width x height times kScale.
    bool resize(uint32_t width, uint32_t height);
    // Records the network for each rectangle. frame is the picture (RGBA8, red in the lowest byte) and must be
    // readable by compute shaders; the output texture must be in the unordered access state. The command list
    // gets this upscaler's descriptor heap and must set its own again afterwards.
    void record(ID3D12GraphicsCommandList* list, ID3D12Resource* frame, const std::vector<PictureRect>& rects);
    ID3D12Resource* output() const { return output_.Get(); }
    bool ready() const { return pso_conv_ != nullptr && output_ != nullptr; }

private:
    ID3D12Device* device_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_first_, pso_conv_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
    UINT descriptor_size_ = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> weights_, weights_upload_, activations_[2], output_;
    bool weights_copied_ = false;
    std::vector<uint32_t> layer_offsets_;  // where each layer starts in the weights, in float4 units
    uint32_t width_ = 0, height_ = 0, domain_w_ = 0, domain_h_ = 0;
    ID3D12Resource* frame_described_ = nullptr;  // the frame texture the descriptor heap points at
};

}  // namespace nax
