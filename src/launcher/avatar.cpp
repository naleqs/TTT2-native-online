#include "avatar.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace tt2::launcher
{
    namespace
    {
        IWICImagingFactory* factory()
        {
            static ComPtr<IWICImagingFactory> f = [] {
                CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                ComPtr<IWICImagingFactory> made;
                CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&made));
                return made;
            }();
            return f.Get();
        }

        // The first frame of a decoder as 32-bit RGBA.
        ComPtr<IWICBitmapSource> rgba_source(IWICBitmapDecoder* decoder)
        {
            ComPtr<IWICBitmapFrameDecode> frame;
            if (FAILED(decoder->GetFrame(0, &frame)))
                return nullptr;
            ComPtr<IWICFormatConverter> converter;
            if (FAILED(factory()->CreateFormatConverter(&converter)) ||
                FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                             WICBitmapPaletteTypeCustom)))
                return nullptr;
            return converter;
        }

        bool to_rgba(IWICBitmapSource* source, Rgba& out)
        {
            UINT w = 0, h = 0;
            if (!source || FAILED(source->GetSize(&w, &h)) || !w || !h)
                return false;
            out.width = int(w);
            out.height = int(h);
            out.pixels.resize(size_t(w) * h * 4);
            return SUCCEEDED(source->CopyPixels(nullptr, w * 4, UINT(out.pixels.size()), out.pixels.data()));
        }
    }

    fs::path pick_image_file(void* owner_hwnd)
    {
        wchar_t file[MAX_PATH] = {};
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = static_cast<HWND>(owner_hwnd);
        ofn.lpstrFilter = L"Images (*.png;*.jpg;*.jpeg;*.bmp;*.gif)\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0All files\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrTitle = L"Choose a profile picture";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&ofn))
            return {};
        return fs::path(file);
    }

    bool import_avatar(const fs::path& source, const fs::path& destination, std::string* error)
    {
        auto fail = [&](const char* what) {
            if (error)
                *error = what;
            return false;
        };
        IWICImagingFactory* f = factory();
        if (!f)
            return fail("Windows Imaging Component is not available.");
        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(f->CreateDecoderFromFilename(source.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)))
            return fail("That file is not an image Windows can read.");
        ComPtr<IWICBitmapSource> rgba = rgba_source(decoder.Get());
        if (!rgba)
            return fail("Could not decode the image.");
        UINT w = 0, h = 0;
        rgba->GetSize(&w, &h);
        // Centred square crop, then scale down.
        const UINT side = std::min(w, h);
        ComPtr<IWICBitmapClipper> clipper;
        const WICRect rect{ INT((w - side) / 2), INT((h - side) / 2), INT(side), INT(side) };
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(f->CreateBitmapClipper(&clipper)) || FAILED(clipper->Initialize(rgba.Get(), &rect)) || FAILED(f->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(clipper.Get(), kAvatarSize, kAvatarSize, WICBitmapInterpolationModeFant)))
            return fail("Could not resize the image.");

        std::error_code ec;
        fs::create_directories(destination.parent_path(), ec);
        const fs::path tmp = destination.wstring() + L".tmp";
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        if (FAILED(f->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(tmp.c_str(), GENERIC_WRITE)) ||
            FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) || FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
            FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)))
            return fail("Could not write the picture.");
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
        frame->SetSize(kAvatarSize, kAvatarSize);
        frame->SetPixelFormat(&format);
        if (FAILED(frame->WriteSource(scaler.Get(), nullptr)) || FAILED(frame->Commit()) || FAILED(encoder->Commit()))
            return fail("Could not write the picture.");
        stream.Reset();
        encoder.Reset();
        frame.Reset();
        fs::rename(tmp, destination, ec);
        if (ec)
            return fail("Could not save the picture.");
        return true;
    }

    bool decode_image_file(const fs::path& file, Rgba& out)
    {
        IWICImagingFactory* f = factory();
        ComPtr<IWICBitmapDecoder> decoder;
        if (!f || FAILED(f->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)))
            return false;
        return to_rgba(rgba_source(decoder.Get()).Get(), out);
    }

    bool decode_image_bytes(const std::string& bytes, Rgba& out)
    {
        IWICImagingFactory* f = factory();
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;
        if (!f || bytes.empty() || bytes.size() > 256 * 1024 || FAILED(f->CreateStream(&stream)) ||
            FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data())), DWORD(bytes.size()))) ||
            FAILED(f->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)))
            return false;
        // Other players' pictures: never larger than an avatar.
        ComPtr<IWICBitmapSource> rgba = rgba_source(decoder.Get());
        UINT w = 0, h = 0;
        if (!rgba || FAILED(rgba->GetSize(&w, &h)) || w > 256 || h > 256)
            return false;
        return to_rgba(rgba.Get(), out);
    }

    std::string read_file_bytes(const fs::path& file)
    {
        std::ifstream in(file, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), {});
    }
}
