// Draws the game picture into a window with Direct3D 12, at any source size and with a choice of filters.
#pragma once

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace nax {

class Presenter {
public:
    static constexpr int kFilterCount = 3;  // sharp pixels, smooth edges, CRT

    ~Presenter();
    bool init(HWND hwnd, std::string* error);
    void resize();
    // Draws one frame of width x height RGBA pixels, shown at 4:3, and presents it with vertical sync.
    void draw(const uint32_t* rgba, uint32_t width, uint32_t height, int filter);
    // The rectangle inside the client area that shows the picture, for mapping mouse positions.
    RECT picture_rect() const;

private:
    static constexpr UINT kFrames = 2;
    void wait_idle();
    void create_targets();
    bool ensure_texture(uint32_t width, uint32_t height);

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
};

}  // namespace nax
