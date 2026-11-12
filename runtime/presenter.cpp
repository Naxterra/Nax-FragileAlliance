// Draws the game picture into a window with Direct3D 12.
#include "runtime/presenter.h"

#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace nax {

namespace {

// A full-screen triangle draws the picture with one of three filters, chosen by "mode":
//   0  Sharp pixels: each source pixel is enlarged by the largest whole factor and only the remaining fraction
//      is blended ("sharp bilinear"), which keeps pixels crisp at any window size.
//   1  Smooth edges: the EPX rule (Scale2x) doubles the picture, so stair steps along diagonal and round edges
//      become smoother while flat areas stay unchanged; the doubled picture is then scaled sharply.
//   2  CRT: scanlines with a Gaussian beam profile and an aperture grille, computed in linear light.
const char kShader[] = R"(
cbuffer Params : register(b0) { float2 scale; float2 size; uint mode; uint3 unused; };
Texture2D picture : register(t0);
SamplerState smooth : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    V o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float3 sharp_at(float2 uv, float2 sz, float2 sc) {
    float2 texel = uv * sz;
    float2 whole = max(floor(sc), 1);
    float2 base = floor(texel);
    float2 dist = frac(texel) - 0.5;
    float2 range = 0.5 - 0.5 / whole;
    float2 f = (dist - clamp(dist, -range, range)) * whole + 0.5;
    return picture.SampleLevel(smooth, (base + f) / sz, 0).rgb;
}

float3 texel(int2 p) { return picture.Load(int3(clamp(p, int2(0, 0), int2(size) - 1), 0)).rgb; }
bool same(float3 a, float3 b) { return all(abs(a - b) < 0.002); }

float3 epx(float3 P, float3 A, float3 B, float3 C, float3 D, int2 q) {
    if (q.x == 0 && q.y == 0) return (same(C, A) && !same(C, D) && !same(A, B)) ? A : P;
    if (q.x == 1 && q.y == 0) return (same(A, B) && !same(A, C) && !same(B, D)) ? B : P;
    if (q.x == 0 && q.y == 1) return (same(D, C) && !same(D, B) && !same(C, A)) ? C : P;
    return (same(B, D) && !same(B, A) && !same(D, C)) ? D : P;
}
float3 smooth_edges(float2 uv) {
    int2 p2 = clamp(int2(floor(uv * size * 2)), int2(0, 0), int2(size * 2) - 1);
    int2 p = p2 >> 1;
    return epx(texel(p), texel(p + int2(0, -1)), texel(p + int2(1, 0)), texel(p + int2(-1, 0)), texel(p + int2(0, 1)), p2 & 1);
}

float3 to_linear(float3 c) { return pow(max(c, 0), 2.4); }
float3 row_colour(float x, float row) {
    float whole = max(floor(scale.x), 1);
    float base = floor(x);
    float dist = frac(x) - 0.5;
    float range = 0.5 - 0.75 / whole;
    float f = (dist - clamp(dist, -range, range)) * whole + 0.5;
    return to_linear(picture.SampleLevel(smooth, float2((base + f) / size.x, (clamp(row, 0, size.y - 1) + 0.5) / size.y), 0).rgb);
}
float3 crt(float2 uv, float2 pos) {
    float2 src = uv * size;
    float y = src.y - 0.5;
    float row = floor(y);
    float f = y - row;
    float3 a = row_colour(src.x, row), b = row_colour(src.x, row + 1);
    float3 wa = exp(-pow(f / (0.30 + 0.12 * a), 2) * 0.5);
    float3 wb = exp(-pow((1 - f) / (0.30 + 0.12 * b), 2) * 0.5);
    float3 c = a * wa + b * wb;
    if (scale.x >= 3) {
        uint m = uint(pos.x) % 3;
        c *= float3(m == 0 ? 1.20 : 0.80, m == 1 ? 1.20 : 0.80, m == 2 ? 1.20 : 0.80);
    }
    return pow(saturate(c * 1.35), 1 / 2.2);
}

float4 ps(V i) : SV_Target {
    if (mode == 1) return float4(smooth_edges(i.uv), 1);
    if (mode == 2) return float4(crt(i.uv, i.pos.xy), 1);
    return float4(sharp_at(i.uv, size, scale), 1);
}
)";

bool check(HRESULT hr, const char* what, std::string* error) {
    if (SUCCEEDED(hr)) return true;
    char text[160];
    std::snprintf(text, sizeof(text), "%s failed (0x%08lX).", what, static_cast<unsigned long>(hr));
    *error = text;
    return false;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return b;
}

}  // namespace

Presenter::~Presenter() {
    if (device_) wait_idle();
    if (fence_event_) CloseHandle(fence_event_);
}

bool Presenter::init(HWND hwnd, std::string* error) {
    hwnd_ = hwnd;
    if (!check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_)), "Creating the DXGI factory", error)) return false;
    // The fastest adapter is preferred, so a laptop with two graphics chips uses the stronger one.
    ComPtr<IDXGIFactory6> factory6;
    ComPtr<IDXGIAdapter1> adapter;
    if (SUCCEEDED(factory_.As(&factory6)))
        factory6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)))) {
        ComPtr<IDXGIAdapter> warp;
        if (!check(factory_->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "Finding a Direct3D 12 adapter", error)) return false;
        if (!check(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)), "Creating the Direct3D 12 device", error))
            return false;
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (!check(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_)), "Creating the command queue", error)) return false;

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kFrames;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> sc;
    if (!check(factory_->CreateSwapChainForHwnd(queue_.Get(), hwnd, &sd, nullptr, nullptr, &sc), "Creating the swap chain", error)) return false;
    factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (!check(sc.As(&swap_), "Querying the swap chain", error)) return false;

    D3D12_DESCRIPTOR_HEAP_DESC rh{};
    rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rh.NumDescriptors = kFrames;
    if (!check(device_->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&rtv_heap_)), "Creating the target heap", error)) return false;
    rtv_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_DESCRIPTOR_HEAP_DESC sh{};
    sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    sh.NumDescriptors = 1;
    sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (!check(device_->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&srv_heap_)), "Creating the texture heap", error)) return false;

    for (UINT i = 0; i < kFrames; i++)
        if (!check(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators_[i])), "Creating an allocator", error))
            return false;
    if (!check(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[0].Get(), nullptr, IID_PPV_ARGS(&list_)),
               "Creating the command list", error))
        return false;
    list_->Close();
    if (!check(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "Creating the fence", error)) return false;
    fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 8;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 2;
    rd.pParameters = params;
    rd.NumStaticSamplers = 1;
    rd.pStaticSamplers = &sampler;
    ComPtr<ID3DBlob> blob, errors;
    if (!check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors), "Building the root signature", error)) return false;
    if (!check(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_)), "Creating the root signature", error))
        return false;

    ComPtr<ID3DBlob> vs, ps;
    auto compile = [&](const char* entry, const char* target, ComPtr<ID3DBlob>* out, const char* what) {
        errors.Reset();
        if (check(D3DCompile(kShader, sizeof(kShader) - 1, "picture", nullptr, nullptr, entry, target, 0, 0, out->GetAddressOf(), &errors), what, error))
            return true;
        if (errors) *error += std::string(" ") + static_cast<const char*>(errors->GetBufferPointer());
        return false;
    };
    if (!compile("vs", "vs_5_0", &vs, "Compiling the vertex shader")) return false;
    if (!compile("ps", "ps_5_0", &ps, "Compiling the pixel shader")) return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root_.Get();
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    pd.SampleDesc.Count = 1;
    if (!check(device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso_)), "Creating the pipeline", error)) return false;
    create_targets();
    return true;
}

bool Presenter::ensure_texture(uint32_t width, uint32_t height) {
    if (texture_ && width == tex_width_ && height == tex_height_) return true;
    wait_idle();
    texture_.Reset();
    for (auto& u : upload_) u.Reset();
    texture_readable_ = false;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = width;
    td.Height = height;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture_))))
        return false;
    device_->CreateShaderResourceView(texture_.Get(), nullptr, srv_heap_->GetCPUDescriptorHandleForHeapStart());
    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = UINT64(width) * 4 * height;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (UINT i = 0; i < kFrames; i++) {
        if (FAILED(device_->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&upload_[i]))))
            return false;
        D3D12_RANGE none{0, 0};
        upload_[i]->Map(0, &none, reinterpret_cast<void**>(&upload_ptr_[i]));
    }
    tex_width_ = width;
    tex_height_ = height;
    return true;
}

void Presenter::create_targets() {
    DXGI_SWAP_CHAIN_DESC1 sd{};
    swap_->GetDesc1(&sd);
    width_ = sd.Width;
    height_ = sd.Height;
    auto handle = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrames; i++) {
        swap_->GetBuffer(i, IID_PPV_ARGS(&targets_[i]));
        device_->CreateRenderTargetView(targets_[i].Get(), nullptr, handle);
        handle.ptr += rtv_size_;
    }
}

void Presenter::wait_idle() {
    const UINT64 v = next_fence_++;
    queue_->Signal(fence_.Get(), v);
    if (fence_->GetCompletedValue() < v) {
        fence_->SetEventOnCompletion(v, fence_event_);
        WaitForSingleObject(fence_event_, INFINITE);
    }
}

void Presenter::resize() {
    if (!swap_) return;
    RECT r;
    GetClientRect(hwnd_, &r);
    if (r.right - r.left <= 0 || r.bottom - r.top <= 0) return;
    wait_idle();
    for (auto& t : targets_) t.Reset();
    swap_->ResizeBuffers(kFrames, 0, 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    create_targets();
}

RECT Presenter::picture_rect() const {
    // Every mode of the game filled a 4:3 monitor.
    RECT client;
    GetClientRect(hwnd_, &client);
    const float w = float(client.right), h = float(client.bottom);
    float pw = w, ph = w * 3.0f / 4.0f;
    if (ph > h) {
        ph = h;
        pw = h * 4.0f / 3.0f;
    }
    RECT r;
    r.left = LONG((w - pw) / 2);
    r.top = LONG((h - ph) / 2);
    r.right = LONG(r.left + pw);
    r.bottom = LONG(r.top + ph);
    return r;
}

void Presenter::draw(const uint32_t* rgba, uint32_t width, uint32_t height, int filter) {
    if (!swap_ || width_ == 0 || width == 0 || height == 0) return;
    if (!ensure_texture(width, height)) return;
    const UINT i = swap_->GetCurrentBackBufferIndex();
    if (fence_->GetCompletedValue() < fence_values_[i]) {
        fence_->SetEventOnCompletion(fence_values_[i], fence_event_);
        WaitForSingleObject(fence_event_, INFINITE);
    }
    std::memcpy(upload_ptr_[i], rgba, size_t(width) * 4 * height);
    allocators_[i]->Reset();
    list_->Reset(allocators_[i].Get(), pso_.Get());
    if (texture_readable_) {
        const auto b = transition(texture_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        list_->ResourceBarrier(1, &b);
    }
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = texture_.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = upload_[i].Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, width * 4};
    list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER barriers[2] = {
        transition(texture_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        transition(targets_[i].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET),
    };
    list_->ResourceBarrier(2, barriers);
    texture_readable_ = true;

    auto rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += SIZE_T(i) * rtv_size_;
    const float black[4] = {0, 0, 0, 1};
    list_->ClearRenderTargetView(rtv, black, 0, nullptr);
    list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const RECT pr = picture_rect();
    D3D12_VIEWPORT vp{float(pr.left), float(pr.top), float(pr.right - pr.left), float(pr.bottom - pr.top), 0, 1};
    D3D12_RECT scissor{0, 0, LONG(width_), LONG(height_)};
    list_->RSSetViewports(1, &vp);
    list_->RSSetScissorRects(1, &scissor);
    list_->SetGraphicsRootSignature(root_.Get());
    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
    list_->SetDescriptorHeaps(1, heaps);
    struct {
        float scale[2];
        float size[2];
        uint32_t mode, unused[3];
    } params{{vp.Width / float(width), vp.Height / float(height)}, {float(width), float(height)}, uint32_t(filter), {0, 0, 0}};
    list_->SetGraphicsRoot32BitConstants(0, 8, &params, 0);
    list_->SetGraphicsRootDescriptorTable(1, srv_heap_->GetGPUDescriptorHandleForHeapStart());
    list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list_->DrawInstanced(3, 1, 0, 0);
    const auto back = transition(targets_[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    list_->ResourceBarrier(1, &back);
    list_->Close();
    ID3D12CommandList* lists[] = {list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    swap_->Present(1, 0);
    fence_values_[i] = next_fence_++;
    queue_->Signal(fence_.Get(), fence_values_[i]);
}

}  // namespace nax
