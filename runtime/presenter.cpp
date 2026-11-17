// Draws the game picture into a window with Direct3D 12.
#include "runtime/presenter.h"

#include <d3dcompiler.h>

#include <algorithm>
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
//   3  AI upscaling: the picture enlarged four times by the neural network (see neural.cpp), scaled to the
//      window with bilinear filtering.
const char kShader[] = R"(
cbuffer Params : register(b0) { float2 scale; float2 size; uint mode; uint3 unused; };
Texture2D picture : register(t0);
Texture2D enlarged : register(t3);
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
    if (mode == 3) return float4(enlarged.SampleLevel(smooth, i.uv, 0).rgb, 1);
    return float4(sharp_at(i.uv, size, scale), 1);
}
)";

// Draws one enhanced picture over the frame. rect is its place in game pixels and size the frame size. Where
// the original pixel is drawn, the enhanced picture shows only while the frame still has the original's
// colour there, so anything the game drew over the original stays visible. Where the original is
// transparent, the enhanced picture's own alpha decides. The enhanced picture was made under a reference
// palette (row "row" of the references texture). Each pixel follows the change of its original index from
// that palette to the one shown now, which covers fades and colour cycling; transparent pixels, which have
// no index, follow tint, the average change over the whole picture.
const char kOverlayShader[] = R"(
cbuffer Overlay : register(b0) { float4 rect; float2 size; float row; float scaled; float4 tint; float2 source; float2 unused; };
Texture2D picture : register(t0);
Texture2D palette : register(t1);
Texture2D references : register(t2);
Texture2D<uint2> original : register(t3);
Texture2D enhanced : register(t4);
SamplerState smooth : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    V o;
    float2 uv = float2(id & 1, id >> 1);
    o.uv = uv;
    float2 p = (rect.xy + uv * rect.zw) / size;
    o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps(V i) : SV_Target {
    // local is the original's pixel under this point and at the game pixel; for an unscaled picture they
    // correspond one to one. A scaled picture is shrunk by leaving pixels out, so the game pixel may show a
    // neighbour of local instead, and any of the nine counts.
    int2 local = clamp(int2(floor(i.uv * source)), int2(0, 0), int2(source) - 1);
    int2 at = int2(floor(rect.xy + i.uv * rect.zw));
    uint2 o = original.Load(int3(local, 0));
    if (o.y != 0) {
        if (any(at < 0) || any(at >= int2(size))) discard;
        float3 shown = picture.Load(int3(at, 0)).rgb;
        float3 wanted = palette.Load(int3(o.x, 0, 0)).rgb;
        if (any(abs(shown - wanted) > 0.001)) {
            bool found = false;
            if (scaled != 0) {
                for (int dy = -1; dy <= 1 && !found; dy++)
                    for (int dx = -1; dx <= 1 && !found; dx++) {
                        uint2 n = original.Load(int3(clamp(local + int2(dx, dy), int2(0, 0), int2(source) - 1), 0));
                        if (n.y != 0 && all(abs(shown - palette.Load(int3(n.x, 0, 0)).rgb) <= 0.001)) found = true;
                    }
            }
            if (!found) discard;
        }
        float4 c = enhanced.SampleLevel(smooth, i.uv, 0);
        float3 then = references.Load(int3(o.x, int(row), 0)).rgb;
        // Bright reference colours are scaled; near-black ones, which a ratio would blow up, are shifted.
        float3 ratio = c.rgb * wanted / max(then, 0.001), shifted = c.rgb + wanted - then;
        return float4(saturate(then > 0.06 ? ratio : shifted), c.a);
    }
    float4 c = enhanced.SampleLevel(smooth, i.uv, 0);
    return float4(saturate(c.rgb * tint.rgb), c.a);
}
)";

UINT64 align(UINT64 value, UINT64 to) { return (value + to - 1) / to * to; }

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

bool Presenter::init(HWND hwnd, const std::filesystem::path& ai_model, std::string* error) {
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
    sh.NumDescriptors = kSlots;
    sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (!check(device_->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&srv_heap_)), "Creating the texture heap", error)) return false;
    srv_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

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
    range.NumDescriptors = 4;  // the frame, the palette, the reference palettes and the AI result
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
    if (!init_overlays(error)) return false;
    // The AI filter is optional: without the model the other filters still work.
    if (ai_model.empty()) {
        ai_problem_ = "No AI model was given.";
    } else {
        NeuralModel model;
        std::filesystem::path param = ai_model, bin = ai_model;
        param += L".param";
        bin += L".bin";
        if (!std::filesystem::exists(param)) ai_problem_ = "Upscayl is not installed (winget install Upscayl.Upscayl).";
        else ai_created_ = model.load(param, bin, &ai_problem_) && ai_.create(device_.Get(), model, &ai_problem_);
    }
    create_targets();
    return true;
}

bool Presenter::init_overlays(std::string* error) {
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 3;
    ranges[1].NumDescriptors = 2;
    ranges[1].BaseShaderRegister = 3;
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 16;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    for (int i = 0; i < 2; i++) {
        params[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1 + i].DescriptorTable.NumDescriptorRanges = 1;
        params[1 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 3;
    rd.pParameters = params;
    rd.NumStaticSamplers = 1;
    rd.pStaticSamplers = &sampler;
    ComPtr<ID3DBlob> blob, errors;
    if (!check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors), "Building the overlay root signature", error))
        return false;
    if (!check(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&overlay_root_)),
               "Creating the overlay root signature", error))
        return false;
    ComPtr<ID3DBlob> vs, ps;
    auto compile = [&](const char* entry, const char* target, ComPtr<ID3DBlob>* out, const char* what) {
        errors.Reset();
        if (check(D3DCompile(kOverlayShader, sizeof(kOverlayShader) - 1, "overlay", nullptr, nullptr, entry, target, 0, 0,
                             out->GetAddressOf(), &errors),
                  what, error))
            return true;
        if (errors) *error += std::string(" ") + static_cast<const char*>(errors->GetBufferPointer());
        return false;
    };
    if (!compile("vs", "vs_5_0", &vs, "Compiling the overlay vertex shader")) return false;
    if (!compile("ps", "ps_5_0", &ps, "Compiling the overlay pixel shader")) return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = overlay_root_.Get();
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    auto& blend = pd.BlendState.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    pd.SampleDesc.Count = 1;
    if (!check(device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&overlay_pso_)), "Creating the overlay pipeline", error)) return false;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 256;
    td.Height = 1;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    if (!check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&palette_texture_)),
               "Creating the palette texture", error))
        return false;
    td.Height = kTextureRows;
    if (!check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&references_)),
               "Creating the reference palette texture", error))
        return false;
    auto slot = srv_heap_->GetCPUDescriptorHandleForHeapStart();
    slot.ptr += srv_size_;
    device_->CreateShaderResourceView(palette_texture_.Get(), nullptr, slot);
    slot.ptr += srv_size_;
    device_->CreateShaderResourceView(references_.Get(), nullptr, slot);
    for (UINT row = kTextureRows; row-- > 0;) free_slots_.push_back(kFirstTextureSlot + 2 * row);
    return true;
}

Presenter::GpuTexture* Presenter::upload_overlay(const OverlayTexture& t, UINT frame_index, UINT64* budget) {
    if (t.width <= 0 || t.height <= 0 || t.hd_width <= 0 || t.hd_height <= 0 ||
        t.index.size() != size_t(t.width) * t.height || t.opaque.size() != t.index.size() ||
        t.hd.size() != size_t(t.hd_width) * t.hd_height)
        return nullptr;
    const UINT64 original_pitch = align(UINT64(t.width) * 2, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const UINT64 enhanced_pitch = align(UINT64(t.hd_width) * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const UINT64 enhanced_offset = align(original_pitch * t.height, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    const UINT64 reference_offset = align(enhanced_offset + enhanced_pitch * t.hd_height, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    const UINT64 bytes = reference_offset + 256 * 4;
    if (bytes > *budget) return nullptr;  // uploaded during a later frame
    if (free_slots_.empty()) evict_unused(frame_index);
    if (free_slots_.empty()) return nullptr;

    GpuTexture g;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.SampleDesc.Count = 1;
    td.Width = UINT64(t.width);
    td.Height = UINT(t.height);
    td.Format = DXGI_FORMAT_R8G8_UINT;
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&g.original))))
        return nullptr;
    td.Width = UINT64(t.hd_width);
    td.Height = UINT(t.hd_height);
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&g.enhanced))))
        return nullptr;
    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = bytes;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> staging;
    if (FAILED(device_->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                IID_PPV_ARGS(&staging))))
        return nullptr;
    uint8_t* mapped = nullptr;
    D3D12_RANGE none{0, 0};
    if (FAILED(staging->Map(0, &none, reinterpret_cast<void**>(&mapped)))) return nullptr;
    for (int y = 0; y < t.height; y++) {
        uint8_t* row = mapped + original_pitch * UINT64(y);
        for (int x = 0; x < t.width; x++) {
            row[x * 2] = t.index[size_t(y) * t.width + x];
            row[x * 2 + 1] = t.opaque[size_t(y) * t.width + x];
        }
    }
    for (int y = 0; y < t.hd_height; y++)
        std::memcpy(mapped + enhanced_offset + enhanced_pitch * UINT64(y), &t.hd[size_t(y) * t.hd_width], size_t(t.hd_width) * 4);
    for (int i = 0; i < 256; i++) {
        uint8_t* entry = mapped + reference_offset + i * 4;
        entry[0] = t.reference[i][0];
        entry[1] = t.reference[i][1];
        entry[2] = t.reference[i][2];
        entry[3] = 255;
    }
    staging->Unmap(0, nullptr);
    g.slot = free_slots_.back();
    free_slots_.pop_back();
    g.version = t.version;

    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.pResource = staging.Get();
    dst.pResource = g.original.Get();
    src.PlacedFootprint.Offset = 0;
    src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8_UINT, UINT(t.width), UINT(t.height), 1, UINT(original_pitch)};
    list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    dst.pResource = g.enhanced.Get();
    src.PlacedFootprint.Offset = enhanced_offset;
    src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, UINT(t.hd_width), UINT(t.hd_height), 1, UINT(enhanced_pitch)};
    list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER barriers[2] = {
        transition(g.original.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        transition(g.enhanced.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
    };
    list_->ResourceBarrier(2, barriers);
    // The reference palette goes into the row of the references texture that belongs to the slot.
    if (references_readable_) {
        const auto b = transition(references_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        list_->ResourceBarrier(1, &b);
    }
    dst.pResource = references_.Get();
    src.PlacedFootprint.Offset = reference_offset;
    src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 256, 1, 1, 256 * 4};
    list_->CopyTextureRegion(&dst, 0, (g.slot - kFirstTextureSlot) / 2, 0, &src, nullptr);
    const auto back = transition(references_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list_->ResourceBarrier(1, &back);
    references_readable_ = true;
    pending_uploads_[frame_index].push_back(staging);
    *budget -= bytes;
    auto handle = srv_heap_->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += SIZE_T(g.slot) * srv_size_;
    device_->CreateShaderResourceView(g.original.Get(), nullptr, handle);
    handle.ptr += srv_size_;
    device_->CreateShaderResourceView(g.enhanced.Get(), nullptr, handle);
    return &(gpu_textures_[t.key] = std::move(g));
}

// Frees the textures that were not drawn in the last few frames. The GPU is idle first, as one of them
// could still be in use by a frame in flight.
std::vector<PictureRect> Presenter::changed_rects(const uint32_t* rgba, uint32_t width, uint32_t height) {
    std::vector<PictureRect> rects;
    const size_t pixels = size_t(width) * height;
    const PictureRect whole{0, 0, int(width), int(height)};
    if (!ai_valid_ || ai_previous_.size() != pixels) {
        ai_previous_.assign(rgba, rgba + pixels);
        rects.push_back(whole);
        return rects;
    }
    // The picture is compared in tiles of 16x16 pixels. Changed tiles next to each other in a row form a run,
    // and runs over the same columns in following rows are joined into one rectangle.
    constexpr int kTile = 16;
    const int tiles_x = int(width + kTile - 1) / kTile, tiles_y = int(height + kTile - 1) / kTile;
    std::vector<uint8_t> changed(size_t(tiles_x) * tiles_y, 0);
    int count = 0;
    for (uint32_t y = 0; y < height; y++) {
        const uint32_t* now = rgba + size_t(y) * width;
        const uint32_t* before = &ai_previous_[size_t(y) * width];
        if (std::memcmp(now, before, size_t(width) * 4) == 0) continue;
        for (int tx = 0; tx < tiles_x; tx++) {
            uint8_t& c = changed[size_t(y / kTile) * tiles_x + tx];
            const int x0 = tx * kTile, n = std::min(kTile, int(width) - x0);
            if (!c && std::memcmp(now + x0, before + x0, size_t(n) * 4) != 0) {
                c = 1;
                count++;
            }
        }
    }
    std::memcpy(ai_previous_.data(), rgba, pixels * 4);
    if (count == 0) return rects;
    if (count * 2 > tiles_x * tiles_y) {
        rects.push_back(whole);
        return rects;
    }
    struct Run { int x0, x1, y0, y1; };
    std::vector<Run> open, done;
    for (int ty = 0; ty < tiles_y; ty++) {
        std::vector<Run> next;
        for (int tx = 0; tx < tiles_x;) {
            if (!changed[size_t(ty) * tiles_x + tx]) { tx++; continue; }
            int end = tx;
            while (end < tiles_x && changed[size_t(ty) * tiles_x + end]) end++;
            Run run{tx, end, ty, ty + 1};
            for (Run& o : open)
                if (o.x0 == tx && o.x1 == end && o.y1 == ty) { run.y0 = o.y0; o.y1 = -1; }
            next.push_back(run);
            tx = end;
        }
        for (const Run& o : open)
            if (o.y1 != -1) done.push_back(o);
        open.swap(next);
    }
    done.insert(done.end(), open.begin(), open.end());
    PictureRect box{int(width), int(height), 0, 0};
    for (const Run& r : done) {
        const PictureRect p{r.x0 * kTile, r.y0 * kTile, std::min(int(width), r.x1 * kTile), std::min(int(height), r.y1 * kTile)};
        rects.push_back(p);
        box = PictureRect{std::min(box.left, p.left), std::min(box.top, p.top), std::max(box.right, p.right), std::max(box.bottom, p.bottom)};
    }
    // Many scattered changes cost more one by one than together.
    if (rects.size() > 24) rects.assign(1, box);
    return rects;
}

void Presenter::evict_unused(UINT frame_index) {
    wait_idle();
    // Once the GPU is idle, textures replaced in earlier frames are free as well. Those replaced while the frame
    // frame_index is being recorded stay until that frame is done.
    for (UINT f = 0; f < kFrames; ++f) {
        if (f == frame_index) continue;
        for (GpuTexture& old : retired_[f]) free_slots_.push_back(old.slot);
        retired_[f].clear();
    }
    for (auto it = gpu_textures_.begin(); it != gpu_textures_.end();) {
        if (it->second.last_used + 2 < frame_number_) {
            free_slots_.push_back(it->second.slot);
            it = gpu_textures_.erase(it);
        } else {
            ++it;
        }
    }
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
    palette_offset_ = size_t(align(UINT64(width) * 4 * height, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT));
    bd.Width = palette_offset_ + 256 * 4;
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

void Presenter::draw(const uint32_t* rgba, uint32_t width, uint32_t height, int filter, const uint32_t palette[256],
                     const std::vector<Overlay>& overlays) {
    if (!swap_ || width_ == 0 || width == 0 || height == 0) return;
    if (!ensure_texture(width, height)) return;
    const UINT i = swap_->GetCurrentBackBufferIndex();
    if (fence_->GetCompletedValue() < fence_values_[i]) {
        fence_->SetEventOnCompletion(fence_values_[i], fence_event_);
        WaitForSingleObject(fence_event_, INFINITE);
    }
    pending_uploads_[i].clear();  // the frame that used them is done
    for (GpuTexture& old : retired_[i]) free_slots_.push_back(old.slot);
    retired_[i].clear();
    frame_number_++;
    std::memcpy(upload_ptr_[i], rgba, size_t(width) * 4 * height);
    std::memcpy(upload_ptr_[i] + palette_offset_, palette, 256 * 4);
    allocators_[i]->Reset();
    list_->Reset(allocators_[i].Get(), pso_.Get());
    if (texture_readable_) {
        const D3D12_RESOURCE_BARRIER b[2] = {
            transition(texture_.Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
            transition(palette_texture_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
        };
        list_->ResourceBarrier(2, b);
    }
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = texture_.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = upload_[i].Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, width * 4};
    list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    dst.pResource = palette_texture_.Get();
    src.PlacedFootprint.Offset = palette_offset_;
    src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 256, 1, 1, 256 * 4};
    list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER barriers[3] = {
        transition(texture_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
        transition(palette_texture_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        transition(targets_[i].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET),
    };
    list_->ResourceBarrier(3, barriers);
    texture_readable_ = true;

    // Textures seen for the first time are uploaded before anything is drawn, up to 64 MB per frame.
    struct Drawn { const Overlay* o; const OverlayTexture* t; GpuTexture* g; };  // t: the version on the GPU
    std::vector<Drawn> drawn;
    if (filter != 2) {
        UINT64 budget = 64ull << 20;
        // A pack picture can be reloaded between two sprites of the same frame, so the overlays may carry two
        // versions of one key. The newest one is drawn for all of them, and every replacement is settled before
        // any texture is looked up, so no pointer taken below can point at a retired texture.
        std::unordered_map<uint64_t, const OverlayTexture*> newest;
        for (const Overlay& o : overlays) {
            if (!o.texture) continue;
            const OverlayTexture*& n = newest[o.texture->key];
            if (!n || o.texture->version > n->version) n = o.texture.get();
        }
        for (const auto& [key, t] : newest) {
            auto found = gpu_textures_.find(key);
            if (found != gpu_textures_.end() && found->second.version != t->version) {
                // The newer version replaces the old one, which an earlier frame may still be drawing.
                retired_[i].push_back(std::move(found->second));
                gpu_textures_.erase(found);
            }
        }
        for (const Overlay& o : overlays) {
            if (!o.texture) continue;
            auto found = gpu_textures_.find(o.texture->key);
            GpuTexture* g = found != gpu_textures_.end() ? &found->second : upload_overlay(*newest[o.texture->key], i, &budget);
            if (!g) continue;
            g->last_used = frame_number_;
            drawn.push_back({&o, newest[o.texture->key], g});
        }
    }

    // The AI filter runs the network over the parts of the picture that changed since its last run.
    bool use_ai = filter == kAiFilter && ai_created_ && ai_.resize(width, height);
    if (use_ai) {
        if (ai_.output() != ai_described_) {
            auto slot = srv_heap_->GetCPUDescriptorHandleForHeapStart();
            slot.ptr += SIZE_T(kAiSlot) * srv_size_;
            device_->CreateShaderResourceView(ai_.output(), nullptr, slot);
            ai_described_ = ai_.output();
            ai_valid_ = false;
            ai_readable_ = false;  // a new output texture starts in the unordered access state
        }
        const std::vector<PictureRect> rects = changed_rects(rgba, width, height);
        if (!rects.empty()) {
            if (ai_readable_) {
                const auto b = transition(ai_.output(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                list_->ResourceBarrier(1, &b);
                ai_readable_ = false;
            }
            ai_.record(list_.Get(), texture_.Get(), rects);
        }
        if (!ai_readable_) {
            const auto b = transition(ai_.output(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            list_->ResourceBarrier(1, &b);
            ai_readable_ = true;
        }
        ai_valid_ = true;
    } else {
        ai_valid_ = false;  // the picture goes on changing without the network
    }
    const uint32_t mode = use_ai ? 3u : uint32_t(filter == kAiFilter ? 0 : filter);

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
    } params{{vp.Width / float(width), vp.Height / float(height)}, {float(width), float(height)}, mode, {0, 0, 0}};
    list_->SetGraphicsRoot32BitConstants(0, 8, &params, 0);
    list_->SetGraphicsRootDescriptorTable(1, srv_heap_->GetGPUDescriptorHandleForHeapStart());
    list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list_->DrawInstanced(3, 1, 0, 0);

    if (!drawn.empty()) {
        list_->SetPipelineState(overlay_pso_.Get());
        list_->SetGraphicsRootSignature(overlay_root_.Get());
        list_->SetGraphicsRootDescriptorTable(1, srv_heap_->GetGPUDescriptorHandleForHeapStart());
        list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        for (const auto& [o, drawn_texture, g] : drawn) {
            const OverlayTexture& t = *drawn_texture;
            // The tint compares the colours of the original's indices now with those the enhanced picture
            // was made under, weighted by how often each index occurs.
            double now[3] = {0, 0, 0}, then[3] = {0, 0, 0};
            for (const auto& [index, count] : t.used)
                for (int k = 0; k < 3; k++) {
                    now[k] += double((palette[index] >> (8 * k)) & 0xFF) * count;
                    then[k] += double(t.reference[index][k]) * count;
                }
            float tint[3];
            for (int k = 0; k < 3; k++) tint[k] = then[k] > 0 ? float(now[k] / then[k]) : 1.0f;
            const float row = float((g->slot - kFirstTextureSlot) / 2);
            const bool scaled = o->width != float(t.width) || o->height != float(t.height);
            const float constants[16] = {o->x, o->y, o->width, o->height, float(width), float(height), row, scaled ? 1.0f : 0.0f,
                                         tint[0], tint[1], tint[2], 1, float(t.width), float(t.height), 0, 0};
            list_->SetGraphicsRoot32BitConstants(0, 16, constants, 0);
            auto table = srv_heap_->GetGPUDescriptorHandleForHeapStart();
            table.ptr += UINT64(g->slot) * srv_size_;
            list_->SetGraphicsRootDescriptorTable(2, table);
            list_->DrawInstanced(4, 1, 0, 0);
        }
    }
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
