#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <cstring>

#include "decode_gdiplus.h"

static ULONG_PTR g_gdiplusToken = 0;

void InitGdiplus()
{
    Gdiplus::GdiplusStartupInput si;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &si, nullptr);
}

void ShutdownGdiplus()
{
    Gdiplus::GdiplusShutdown(g_gdiplusToken);
}

bool DecodeWithGdiplus(const wchar_t* path, Image& img, std::wstring& info)
{
    auto* bmp = Gdiplus::Bitmap::FromFile(path);
    if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok)
    {
        delete bmp;
        MessageBoxW(nullptr, L"GDI+ cannot decode this image.", L"Error", MB_ICONERROR);
        return false;
    }

    int width  = static_cast<int>(bmp->GetWidth());
    int height = static_cast<int>(bmp->GetHeight());
    int stride = width * 4;

    Gdiplus::Rect rect(0, 0, width, height);
    Gdiplus::BitmapData bd{};

    if (bmp->LockBits(&rect, Gdiplus::ImageLockModeRead,
                       PixelFormat32bppARGB, &bd) != Gdiplus::Ok)
    {
        delete bmp;
        MessageBoxW(nullptr, L"GDI+ LockBits failed.", L"Error", MB_ICONERROR);
        return false;
    }

    img.width  = width;
    img.height = height;
    img.stride = stride;
    img.pixels.resize(static_cast<size_t>(stride) * height);

    // Copy row-by-row (GDI+ stride may include extra padding)
    const auto* src = static_cast<const unsigned char*>(bd.Scan0);
    for (int y = 0; y < height; ++y)
        std::memcpy(&img.pixels[static_cast<size_t>(y) * stride],
                    src + static_cast<size_t>(y) * bd.Stride,
                    stride);

    bmp->UnlockBits(&bd);
    delete bmp;

    std::wstring ext = GetFileExt(path);
    if      (ext == L".png")                    info = L"PNG \xB7 GDI+";
    else if (ext == L".bmp")                    info = L"BMP \xB7 GDI+";
    else if (ext == L".gif")                    info = L"GIF \xB7 GDI+";
    else if (ext == L".tiff" || ext == L".tif") info = L"TIFF \xB7 GDI+";
    else if (ext == L".ico")                    info = L"ICO \xB7 GDI+";
    else                                        info = L"GDI+";

    return true;
}
