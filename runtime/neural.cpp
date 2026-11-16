// Live AI upscaling with Direct3D 12 compute shaders.
#include "runtime/neural.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace nax {

namespace {

// Activations are kept as 16-bit floats, two channels to a 32-bit word, 64 channels (32 words) per pixel,
// over a "domain": the picture with kPadding pixels on every side. Layer 0 reads the picture, mirrored at the
// edges into the padding; every layer treats pixels outside the domain as zero, as the network was trained.
//
// "conv" computes a 16x16 tile of output pixels per thread group: each of the 256 threads computes 4 pixels
// of one row for 16 output channels. The input tile with its one-pixel border goes through group shared
// memory 16 input channels at a time. The weights of a layer are laid out as [input][tap][64 outputs],
// followed by 64 biases and 64 PReLU slopes, so that one float4 load serves four outputs. For the last layer
// the 48 outputs are arranged so that each thread gets all three colours of four sub-pixels; it adds the
// enlarged input picture (nearest neighbour) and writes the finished pixels.
const char kShader[] = R"(
cbuffer Layer : register(b0) {
    int2 origin;      // first domain pixel this dispatch computes
    int2 extent;      // how many pixels it computes
    int2 domain;      // size of the domain
    int pad;          // picture pixel (0, 0) lies at domain pixel (pad, pad)
    uint weights;     // start of the layer in the weights, in float4 units
    uint flip;        // 0: read activations 1, write 0; 1: read 0, write 1
    uint last;        // the last layer writes the result picture
    uint2 picture;    // size of the picture
};
Texture2D<float4> frame : register(t0);
StructuredBuffer<float4> W : register(t1);
RWByteAddressBuffer act0 : register(u0);
RWByteAddressBuffer act1 : register(u1);
RWTexture2D<float4> result : register(u2);

uint address(int2 p, uint pair) { return ((uint(p.y) * uint(domain.x) + uint(p.x)) * 32 + pair) * 4; }
uint load_pair(uint a) { return flip ? act0.Load(a) : act1.Load(a); }
void store_pair(uint a, uint v) { if (flip) act1.Store(a, v); else act0.Store(a, v); }
float4 prelu(float4 v, float4 slope) { return v >= 0 ? v : v * slope; }

int mirror(int v, int size) {
    if (v < 0) v = -v;
    if (v >= size) v = 2 * size - 2 - v;
    return clamp(v, 0, size - 1);
}

[numthreads(8, 8, 1)]
void first(uint3 id : SV_DispatchThreadID) {
    if (any(int2(id.xy) >= extent)) return;
    int2 p = origin + int2(id.xy);
    float4 acc[16];
    [unroll] for (uint o = 0; o < 16; o++) acc[o] = W[weights + 3 * 9 * 16 + o];
    for (uint tap = 0; tap < 9; tap++) {
        int2 q = p + int2(int(tap % 3) - 1, int(tap / 3) - 1);
        float3 v = 0;
        if (all(q >= 0) && all(q < domain))
            v = frame.Load(int3(mirror(q.x - pad, int(picture.x)), mirror(q.y - pad, int(picture.y)), 0)).rgb;
        [unroll] for (uint c = 0; c < 3; c++) {
            uint base = weights + (c * 9 + tap) * 16;
            [unroll] for (uint o = 0; o < 16; o++) acc[o] += v[c] * W[base + o];
        }
    }
    [unroll] for (uint k = 0; k < 16; k++) {
        float4 a = prelu(acc[k], W[weights + 3 * 9 * 16 + 16 + k]);
        uint2 h = f32tof16(a.xz) | (f32tof16(a.yw) << 16);
        store_pair(address(p, k * 2), h.x);
        store_pair(address(p, k * 2 + 1), h.y);
    }
}

groupshared float tile[16 * 18 * 18];

[numthreads(256, 1, 1)]
void conv(uint3 gid : SV_GroupID, uint ti : SV_GroupIndex) {
    int2 base = origin + int2(gid.xy) * 16;
    uint group = ti / 64, slot = ti % 64;
    int row = int(slot / 4), col = int(slot % 4) * 4;
    float4 acc[4][4];
    [unroll] for (uint q = 0; q < 4; q++) {
        float4 b = W[weights + 64 * 9 * 16 + group * 4 + q];
        [unroll] for (uint p = 0; p < 4; p++) acc[p][q] = b;
    }
    for (uint c0 = 0; c0 < 64; c0 += 16) {
        for (uint n = ti; n < 18 * 18 * 8; n += 256) {
            uint pos = n / 8, pair = n % 8;
            int2 q = base - 1 + int2(int(pos % 18), int(pos / 18));
            float2 v = 0;
            if (all(q >= 0) && all(q < domain)) {
                uint u = load_pair(address(q, c0 / 2 + pair));
                v = float2(f16tof32(u & 0xFFFF), f16tof32(u >> 16));
            }
            tile[(pair * 2) * 324 + pos] = v.x;
            tile[(pair * 2 + 1) * 324 + pos] = v.y;
        }
        GroupMemoryBarrierWithGroupSync();
        for (uint ic = 0; ic < 16; ic++) {
            [unroll] for (uint tap = 0; tap < 9; tap++) {
                uint t = ic * 324 + (row + tap / 3) * 18 + col + tap % 3;
                float v0 = tile[t], v1 = tile[t + 1], v2 = tile[t + 2], v3 = tile[t + 3];
                uint wb = weights + ((c0 + ic) * 9 + tap) * 16 + group * 4;
                [unroll] for (uint q = 0; q < 4; q++) {
                    float4 w = W[wb + q];
                    acc[0][q] += v0 * w;
                    acc[1][q] += v1 * w;
                    acc[2][q] += v2 * w;
                    acc[3][q] += v3 * w;
                }
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }
    [unroll] for (uint p = 0; p < 4; p++) {
        int2 pixel = base + int2(col + int(p), row);
        if (any(pixel - origin >= extent)) continue;
        if (last == 0) {
            [unroll] for (uint q = 0; q < 4; q++) {
                float4 a = prelu(acc[p][q], W[weights + 64 * 9 * 16 + 16 + group * 4 + q]);
                uint2 h = f32tof16(a.xz) | (f32tof16(a.yw) << 16);
                store_pair(address(pixel, group * 8 + q * 2), h.x);
                store_pair(address(pixel, group * 8 + q * 2 + 1), h.y);
            }
        } else {
            int2 f = pixel - pad;
            float3 enlarged = frame.Load(int3(f, 0)).rgb;
            float v[16] = {acc[p][0].x, acc[p][0].y, acc[p][0].z, acc[p][0].w, acc[p][1].x, acc[p][1].y, acc[p][1].z, acc[p][1].w,
                           acc[p][2].x, acc[p][2].y, acc[p][2].z, acc[p][2].w, acc[p][3].x, acc[p][3].y, acc[p][3].z, acc[p][3].w};
            [unroll] for (uint s = 0; s < 4; s++) {
                uint sub = group * 4 + s;
                float3 c = float3(v[s * 3], v[s * 3 + 1], v[s * 3 + 2]) + enlarged;
                result[f * 4 + int2(sub % 4, sub / 4)] = float4(saturate(c), 1);
            }
        }
    }
}
)";

bool check(HRESULT hr, const char* what, std::string* error) {
    if (SUCCEEDED(hr)) return true;
    char text[160];
    std::snprintf(text, sizeof(text), "%s failed (0x%08lX).", what, static_cast<unsigned long>(hr));
    *error = text;
    return false;
}

D3D12_RESOURCE_DESC buffer_desc(UINT64 bytes, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    return d;
}

// The constants of one dispatch, in the order of the shader's constant buffer.
struct LayerConstants {
    int32_t origin[2], extent[2], domain[2], pad;
    uint32_t weights, flip, last, picture[2];
};

}  // namespace

bool NeuralModel::load(const std::filesystem::path& param, const std::filesystem::path& bin, std::string* error) {
    layers.clear();
    std::ifstream text(param);
    std::ifstream data(bin, std::ios::binary);
    if (!text || !data) {
        *error = "The model files were not found.";
        return false;
    }
    std::vector<uint8_t> blob((std::istreambuf_iterator<char>(data)), std::istreambuf_iterator<char>());
    size_t pos = 0;
    auto read_floats = [&](size_t count, std::vector<float>* out) {
        if (pos + count * 4 > blob.size()) return false;
        out->resize(count);
        std::memcpy(out->data(), blob.data() + pos, count * 4);
        pos += count * 4;
        return true;
    };
    std::string line;
    std::getline(text, line);  // magic number
    std::getline(text, line);  // layer and blob counts
    while (std::getline(text, line)) {
        std::istringstream in(line);
        std::string kind, name;
        int bottoms = 0, tops = 0;
        if (!(in >> kind >> name >> bottoms >> tops)) continue;
        std::string skip;
        for (int i = 0; i < bottoms + tops; i++) in >> skip;
        int option[32] = {};
        std::string item;
        while (in >> item) {
            const size_t eq = item.find('=');
            if (eq == std::string::npos) continue;
            const int key = std::atoi(item.substr(0, eq).c_str());
            if (key >= 0 && key < 32) option[key] = std::atoi(item.substr(eq + 1).c_str());
        }
        if (kind == "Convolution") {
            Layer layer;
            layer.outputs = option[0];
            const int kernel = option[1], count = option[6];
            if (kernel != 3 || layer.outputs <= 0 || count % (layer.outputs * 9) != 0 || pos + 4 > blob.size()) {
                *error = "The model has a convolution of an unexpected shape.";
                return false;
            }
            layer.inputs = count / (layer.outputs * 9);
            uint32_t tag = 0;
            std::memcpy(&tag, blob.data() + pos, 4);
            pos += 4;
            if (tag == 0) {
                if (!read_floats(size_t(count), &layer.weights)) break;
            } else if (tag == 0x01306B47) {  // 16-bit floats, padded to four bytes
                if (pos + size_t(count) * 2 > blob.size()) break;
                layer.weights.resize(size_t(count));
                for (int i = 0; i < count; i++) {
                    uint16_t h;
                    std::memcpy(&h, blob.data() + pos + size_t(i) * 2, 2);
                    const uint32_t sign = uint32_t(h & 0x8000) << 16, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
                    uint32_t bits;
                    if (exponent == 0) bits = sign;  // zero (subnormals are too small to matter here)
                    else if (exponent == 31) bits = sign | 0x7F800000 | (mantissa << 13);
                    else bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
                    std::memcpy(&layer.weights[size_t(i)], &bits, 4);
                }
                pos += (size_t(count) * 2 + 3) & ~size_t(3);
            } else {
                *error = "The model stores its weights in a form that is not supported.";
                return false;
            }
            if (option[5] && !read_floats(size_t(layer.outputs), &layer.bias)) break;
            if (!option[5]) layer.bias.assign(size_t(layer.outputs), 0.0f);
            layers.push_back(std::move(layer));
        } else if (kind == "PReLU") {
            if (layers.empty() || !read_floats(size_t(option[0]), &layers.back().slopes)) break;
        }
    }
    // The network must be the compact Real-ESRGAN layout this upscaler implements.
    bool ok = layers.size() >= 2 && pos == blob.size() && layers.front().inputs == 3 && layers.back().outputs == 48 &&
              layers.back().inputs == 64 && layers.back().slopes.empty();
    for (size_t i = 0; ok && i + 1 < layers.size(); i++)
        ok = layers[i].outputs == 64 && layers[i].slopes.size() == 64 && (i == 0 || layers[i].inputs == 64);
    if (!ok) {
        *error = "The model is not a compact Real-ESRGAN network with 64 channels and four times enlargement.";
        layers.clear();
    }
    return ok;
}

bool NeuralUpscaler::create(ID3D12Device* device, const NeuralModel& model, std::string* error) {
    device_ = device;
    // Weights in the layout the shader reads, see kShader.
    std::vector<float> packed;
    layer_offsets_.clear();
    for (size_t l = 0; l < model.layers.size(); l++) {
        const NeuralModel::Layer& layer = model.layers[l];
        const bool last = l + 1 == model.layers.size();
        layer_offsets_.push_back(uint32_t(packed.size() / 4));
        // Position k of 64 holds output channel order[k], or nothing (-1).
        int order[64];
        for (int k = 0; k < 64; k++) {
            if (!last) order[k] = k;
            else {
                const int group = k / 16, i = k % 16;
                order[k] = i < 12 ? (i % 3) * 16 + group * 4 + i / 3 : -1;
            }
        }
        for (int ic = 0; ic < layer.inputs; ic++)
            for (int tap = 0; tap < 9; tap++)
                for (int k = 0; k < 64; k++)
                    packed.push_back(order[k] < 0 ? 0.0f : layer.weights[(size_t(order[k]) * layer.inputs + ic) * 9 + tap]);
        for (int k = 0; k < 64; k++) packed.push_back(order[k] < 0 ? 0.0f : layer.bias[size_t(order[k])]);
        for (int k = 0; k < 64; k++) packed.push_back(layer.slopes.empty() || order[k] < 0 ? 0.0f : layer.slopes[size_t(order[k])]);
    }

    D3D12_HEAP_PROPERTIES def{}, up{};
    def.Type = D3D12_HEAP_TYPE_DEFAULT;
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    const UINT64 bytes = packed.size() * 4;
    auto bd = buffer_desc(bytes, D3D12_RESOURCE_FLAG_NONE);
    if (!check(device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&weights_)),
               "Creating the network weights", error) ||
        !check(device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&weights_upload_)),
               "Creating the network weight upload", error))
        return false;
    void* mapped = nullptr;
    D3D12_RANGE none{0, 0};
    if (!check(weights_upload_->Map(0, &none, &mapped), "Mapping the network weights", error)) return false;
    std::memcpy(mapped, packed.data(), size_t(bytes));
    weights_upload_->Unmap(0, nullptr);
    weights_copied_ = false;

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 3;
    ranges[1].OffsetInDescriptorsFromTableStart = 2;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = sizeof(LayerConstants) / 4;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2;
    params[1].DescriptorTable.pDescriptorRanges = ranges;
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 2;
    rd.pParameters = params;
    ComPtr<ID3DBlob> blob, errors;
    if (!check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors), "Building the network root signature", error) ||
        !check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_)),
               "Creating the network root signature", error))
        return false;
    for (const char* entry : {"first", "conv"}) {
        ComPtr<ID3DBlob> code;
        errors.Reset();
        if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "neural", nullptr, nullptr, entry, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                              &code, &errors))) {
            *error = "Compiling the network shader failed.";
            if (errors) *error += std::string(" ") + static_cast<const char*>(errors->GetBufferPointer());
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root_.Get();
        pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
        auto& pso = std::strcmp(entry, "first") == 0 ? pso_first_ : pso_conv_;
        if (!check(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)), "Creating the network pipeline", error)) return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 5;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (!check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)), "Creating the network descriptor heap", error)) return false;
    descriptor_size_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto at = heap_->GetCPUDescriptorHandleForHeapStart();
    at.ptr += descriptor_size_;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_UNKNOWN;
    sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Buffer.NumElements = UINT(packed.size() / 4);
    sd.Buffer.StructureByteStride = 16;
    device->CreateShaderResourceView(weights_.Get(), &sd, at);
    return true;
}

bool NeuralUpscaler::resize(uint32_t width, uint32_t height) {
    if (width == width_ && height == height_ && output_) return true;
    output_.Reset();
    activations_[0].Reset();
    activations_[1].Reset();
    frame_described_ = nullptr;
    width_ = width;
    height_ = height;
    domain_w_ = width + 2 * kPadding;
    domain_h_ = height + 2 * kPadding;
    D3D12_HEAP_PROPERTIES def{};
    def.Type = D3D12_HEAP_TYPE_DEFAULT;
    const UINT64 bytes = UINT64(domain_w_) * domain_h_ * 64 * 2;
    auto bd = buffer_desc(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    for (auto& a : activations_)
        if (FAILED(device_->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                    IID_PPV_ARGS(&a))))
            return false;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = UINT64(width) * kScale;
    td.Height = height * kScale;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(device_->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                IID_PPV_ARGS(&output_))))
        return false;
    auto at = heap_->GetCPUDescriptorHandleForHeapStart();
    at.ptr += SIZE_T(descriptor_size_) * 2;
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_R32_TYPELESS;
    ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = UINT(bytes / 4);
    ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    for (auto& a : activations_) {
        device_->CreateUnorderedAccessView(a.Get(), nullptr, &ud, at);
        at.ptr += descriptor_size_;
    }
    device_->CreateUnorderedAccessView(output_.Get(), nullptr, nullptr, at);
    return true;
}

void NeuralUpscaler::record(ID3D12GraphicsCommandList* list, ID3D12Resource* frame, const std::vector<PictureRect>& rects) {
    if (!ready() || rects.empty()) return;
    if (!weights_copied_) {
        list->CopyBufferRegion(weights_.Get(), 0, weights_upload_.Get(), 0, weights_->GetDesc().Width);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = weights_.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &b);
        weights_copied_ = true;
    }
    if (frame != frame_described_) {
        device_->CreateShaderResourceView(frame, nullptr, heap_->GetCPUDescriptorHandleForHeapStart());
        frame_described_ = frame;
    }
    ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root_.Get());
    list->SetComputeRootDescriptorTable(1, heap_->GetGPUDescriptorHandleForHeapStart());
    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    const int layers = int(layer_offsets_.size());
    for (const PictureRect& changed : rects) {
        // A changed pixel reaches one pixel further with every 3x3 layer, so the result changes within that
        // many pixels around the rectangle.
        PictureRect r;
        r.left = std::max(0, changed.left - layers);
        r.top = std::max(0, changed.top - layers);
        r.right = std::min(int(width_), changed.right + layers);
        r.bottom = std::min(int(height_), changed.bottom + layers);
        for (int l = 0; l < layers; l++) {
            // Each layer computes the pixels that the remaining layers still need: the rectangle grown by one
            // pixel for every layer after it, within the domain.
            const int grow = layers - 1 - l;
            const int x0 = std::max(0, r.left + kPadding - grow), y0 = std::max(0, r.top + kPadding - grow);
            const int x1 = std::min(int(domain_w_), r.right + kPadding + grow), y1 = std::min(int(domain_h_), r.bottom + kPadding + grow);
            if (x1 <= x0 || y1 <= y0) break;
            LayerConstants k{{x0, y0}, {x1 - x0, y1 - y0}, {int(domain_w_), int(domain_h_)}, kPadding, layer_offsets_[size_t(l)],
                             uint32_t(l & 1), l == layers - 1 ? 1u : 0u, {width_, height_}};
            list->SetPipelineState(l == 0 ? pso_first_.Get() : pso_conv_.Get());
            list->SetComputeRoot32BitConstants(0, sizeof(k) / 4, &k, 0);
            const UINT block = l == 0 ? 8 : 16;
            list->Dispatch(UINT((x1 - x0 + block - 1) / block), UINT((y1 - y0 + block - 1) / block), 1);
            list->ResourceBarrier(1, &uav);
        }
    }
}

}  // namespace nax
