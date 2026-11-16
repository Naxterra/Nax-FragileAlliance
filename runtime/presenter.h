// Draws the game picture into a window with Direct3D 12, at any source size and with a choice of filters.
#pragma once

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "runtime/display.h"
#include "runtime/neural.h"

namespace nax {

class Presenter {
public:
    static constexpr int kFilterCount = 4;  // sharp pixels, smooth edges, CRT, AI upscaling
    static constexpr int kAiFilter = 3;

    ~Presenter();
    // ai_model names the AI upscaling model without its extension (.param and .bin). Without it, or when it
    // cannot be used, the AI filter shows sharp pixels and ai_problem() says why.
    bool init(HWND hwnd, const std::filesystem::path& ai_model, std::string* error);
    bool ai_available() const { return ai_created_; }
    const std::string& ai_problem() const { return ai_problem_; }
    void resize();
    // Draws one frame of width x height RGBA pixels, shown at 4:3, and presents it with vertical sync. The
    // overlays are drawn over it in their order, each only where the frame still shows its original picture
    // in the given palette (RGBA, the colours the frame was made with). The CRT filter shows no overlays.
    void draw(const uint32_t* rgba, uint32_t width, uint32_t height, int filter, const uint32_t palette[256],
              const std::vector<Overlay>& overlays);
    // The rectangle inside the client area that shows the picture, for mapping mouse positions.
    RECT picture_rect() const;

private:
    static constexpr UINT kFrames = 2;
    void wait_idle();
    void create_targets();
    bool ensure_texture(uint32_t width, uint32_t height);
    bool init_overlays(std::string* error);
    // The textures of one overlay picture on the GPU: the original's indices and opacity, and the enhanced
    // picture, with their shader resource views in two neighbouring slots of the descriptor heap.
    struct GpuTexture {
        Microsoft::WRL::ComPtr<ID3D12Resource> original, enhanced;
        UINT slot = 0;
        UINT64 last_used = 0;
    };
    GpuTexture* upload_overlay(const OverlayTexture& t, UINT frame_index, UINT64* budget);
    void evict_unused();
    // The rectangles of the picture that changed since the last AI run, or the whole picture when the AI
    // result is not up to date.
    std::vector<PictureRect> changed_rects(const uint32_t* rgba, uint32_t width, uint32_t height);

    HWND hwnd_ = nullptr;
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap_, srv_heap_;
    UINT rtv_size_ = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> targets_[kFrames];
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocators_[kFrames];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    UINT64 fence_values_[kFrames]{};
    UINT64 next_fence_ = 1;
    HANDLE fence_event_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_;
    Microsoft::WRL::ComPtr<ID3D12Resource> texture_;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload_[kFrames];
    uint8_t* upload_ptr_[kFrames]{};
    bool texture_readable_ = false;
    uint32_t tex_width_ = 0, tex_height_ = 0;
    UINT width_ = 0, height_ = 0;

    // Descriptor slots: 0 the frame, 1 the palette, 2 the reference palettes, then a pair per texture. The
    // reference palette of the texture in pair n is row n of the references texture.
    static constexpr UINT kSlots = 4096;
    static constexpr UINT kAiSlot = 3;  // the AI result
    static constexpr UINT kFirstTextureSlot = 4;
    static constexpr UINT kTextureRows = (kSlots - kFirstTextureSlot) / 2;
    UINT srv_size_ = 0;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> overlay_root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> overlay_pso_;
    Microsoft::WRL::ComPtr<ID3D12Resource> palette_texture_, references_;
    bool references_readable_ = false;
    bool palette_readable_ = false;
    size_t palette_offset_ = 0;               // where the palette lies in each upload buffer
    std::unordered_map<uint64_t, GpuTexture> gpu_textures_;
    std::vector<UINT> free_slots_;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> pending_uploads_[kFrames];  // freed once the frame is done
    UINT64 frame_number_ = 0;

    NeuralUpscaler ai_;
    bool ai_created_ = false;
    std::string ai_problem_;
    std::vector<uint32_t> ai_previous_;    // the picture of the last AI run
    bool ai_valid_ = false;                // the AI result matches ai_previous_
    bool ai_readable_ = false;             // the AI result is in the pixel shader resource state
    ID3D12Resource* ai_described_ = nullptr;
};

}  // namespace nax
