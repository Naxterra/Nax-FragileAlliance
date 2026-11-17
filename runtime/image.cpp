// PNG reading and writing through the Windows Imaging Component.
#include "runtime/image.h"

#include <windows.h>

#include <wincodec.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace nax {

namespace {

// WIC needs COM on the calling thread. Each thread that touches pictures initialises it once for its lifetime;
// a thread that already chose another apartment model keeps that one, which WIC accepts. The factory is made
// for each call and released with it: a factory kept until the thread ends would be released after other code
// on that thread (the sound output) has shut COM down, which crashed the program.
ComPtr<IWICImagingFactory> factory() {
    thread_local bool initialised = false;
    if (!initialised) {
        initialised = true;
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    }
    ComPtr<IWICImagingFactory> made;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&made));
    return made;
}

// WIC's PNG codec works in BGRA, so red and blue change places on the way in and out.
void swap_red_blue(uint32_t* pixels, size_t count) {
    for (size_t i = 0; i < count; i++) {
        const uint32_t p = pixels[i];
        pixels[i] = (p & 0xFF00FF00u) | (p & 0xFF) << 16 | (p >> 16 & 0xFF);
    }
}

}  // namespace

bool save_png(const std::filesystem::path& file, const uint32_t* rgba, int width, int height) {
    ComPtr<IWICImagingFactory> wic = factory();
    if (!wic || width <= 0 || height <= 0) return false;
    // The picture goes to a temporary file first, so a failed write never leaves a broken PNG behind.
    std::filesystem::path temporary = file;
    temporary += L".tmp";
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    bool ok = SUCCEEDED(wic->CreateStream(&stream)) &&
              SUCCEEDED(stream->InitializeFromFilename(temporary.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
              SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
              SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
              SUCCEEDED(frame->SetSize(UINT(width), UINT(height)));
    if (ok) {
        std::vector<uint32_t> bgra(rgba, rgba + size_t(width) * height);
        swap_red_blue(bgra.data(), bgra.size());
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        ok = SUCCEEDED(frame->SetPixelFormat(&format)) && format == GUID_WICPixelFormat32bppBGRA &&
             SUCCEEDED(frame->WritePixels(UINT(height), UINT(width) * 4, UINT(width) * 4 * UINT(height),
                                          reinterpret_cast<BYTE*>(bgra.data()))) &&
             SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    }
    frame.Reset();
    encoder.Reset();
    stream.Reset();
    if (!ok || !MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

bool load_png(const std::filesystem::path& file, std::vector<uint32_t>* rgba, int* width, int* height) {
    ComPtr<IWICImagingFactory> wic = factory();
    if (!wic) return false;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    UINT w = 0, h = 0;
    if (FAILED(wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0 || w > 16384 || h > 16384 ||
        FAILED(wic->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)))
        return false;
    rgba->resize(size_t(w) * h);
    if (FAILED(converter->CopyPixels(nullptr, w * 4, w * 4 * h, reinterpret_cast<BYTE*>(rgba->data())))) return false;
    swap_red_blue(rgba->data(), rgba->size());
    *width = int(w);
    *height = int(h);
    return true;
}

}  // namespace nax
