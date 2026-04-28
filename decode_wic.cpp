#include <windows.h>
#include <wincodec.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d2d1_3.h>
#include <d2d1effects_2.h>
#include <combaseapi.h>
#include <wrl/client.h>

#include <vector>
#include <cmath>

#include "decode_wic.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dxguid.lib")

using Microsoft::WRL::ComPtr;

struct CoInit
{
    CoInit()  { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~CoInit() { CoUninitialize(); }
};

// Constants (mirror Microsoft D2DAdvancedColorImages sample) 
static constexpr float  sc_nominalRefWhite   = 80.0f;     // scRGB 1.0 = 80 nits
static constexpr float  sc_sdrPeakNits       = 80.0f;     // SDR display target
static constexpr float  sc_histMaxNits       = 1.0e6f;    // histogram top end
static constexpr float  sc_histGamma         = 0.1f;      // bin allocation gamma
static constexpr UINT32 sc_histNumBins       = 400;
static constexpr float  sc_maxCLLPercentile  = 0.9999f;   // p99.99
static constexpr float  sc_defaultMaxCLLNits = 1000.0f;   // fallback if histogram fails

// D3D11 + D2D init 
static HRESULT CreateD2DContextForDecode(
    ComPtr<ID3D11Device>&         outD3D,
    ComPtr<ID2D1Factory3>&        outFactory,
    ComPtr<ID2D1Device2>&         outDevice,
    ComPtr<ID2D1DeviceContext2>&  outContext)
{
    UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        createFlags, featureLevels, ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION, &outD3D, nullptr, nullptr);

    if (FAILED(hr))
    {
        // Fall back to WARP (software rasterizer, still supports compute).
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            createFlags, featureLevels, ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION, &outD3D, nullptr, nullptr);
        if (FAILED(hr)) return hr;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    hr = outD3D.As(&dxgiDevice);
    if (FAILED(hr)) return hr;

    D2D1_FACTORY_OPTIONS opts = {};
    hr = D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory3),
        &opts,
        reinterpret_cast<void**>(outFactory.GetAddressOf()));
    if (FAILED(hr)) return hr;

    hr = outFactory->CreateDevice(dxgiDevice.Get(), &outDevice);
    if (FAILED(hr)) return hr;

    return outDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &outContext);
}

// Histogram-based MaxCLL
//
// Mirrors D2DAdvancedColorImagesRenderer::CreateHistogramResources +
// ComputeHdrMetadata:
//   colorMgmt(scRGB) → Scale 0.5 → ColorMatrix(Y / 12500) → GammaTransfer 0.1
//                    → Histogram(400 bins)
// Bin index scanned top-down; first bin where cumulative fraction reaches
// (1 - p99.99) is the peak. Inverse-mapped back to nits.
static bool ComputeMaxCLLNitsViaHistogram(
    ID2D1DeviceContext2* ctx,
    ID2D1Effect*         upstream,   // outputs scRGB
    float&               outNits)
{
    ComPtr<ID2D1Effect> scale;
    if (FAILED(ctx->CreateEffect(CLSID_D2D1Scale, &scale))) return false;
    scale->SetValue(D2D1_SCALE_PROP_SCALE, D2D1::Vector2F(0.5f, 0.5f));
    scale->SetInputEffect(0, upstream);

    ComPtr<ID2D1Effect> matrix;
    if (FAILED(ctx->CreateEffect(CLSID_D2D1ColorMatrix, &matrix))) return false;
    matrix->SetInputEffect(0, scale.Get());

    const float scaleFactor = sc_histMaxNits / sc_nominalRefWhite;  // ~12500
    D2D1_MATRIX_5X4_F m = D2D1::Matrix5x4F(
        0.2126f / scaleFactor, 0, 0, 0,
        0.7152f / scaleFactor, 0, 0, 0,
        0.0722f / scaleFactor, 0, 0, 0,
        0,                     0, 0, 1,
        0,                     0, 0, 0);
    matrix->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, m);

    ComPtr<ID2D1Effect> gamma;
    if (FAILED(ctx->CreateEffect(CLSID_D2D1GammaTransfer, &gamma))) return false;
    gamma->SetInputEffect(0, matrix.Get());
    gamma->SetValue(D2D1_GAMMATRANSFER_PROP_RED_EXPONENT,  sc_histGamma);
    gamma->SetValue(D2D1_GAMMATRANSFER_PROP_GREEN_DISABLE, TRUE);
    gamma->SetValue(D2D1_GAMMATRANSFER_PROP_BLUE_DISABLE,  TRUE);
    gamma->SetValue(D2D1_GAMMATRANSFER_PROP_ALPHA_DISABLE, TRUE);

    ComPtr<ID2D1Effect> histogram;
    HRESULT hr = ctx->CreateEffect(CLSID_D2D1Histogram, &histogram);
    if (FAILED(hr)) return false;   // includes D2DERR_INSUFFICIENT_DEVICE_CAPABILITIES

    histogram->SetValue(D2D1_HISTOGRAM_PROP_NUM_BINS, sc_histNumBins);
    histogram->SetInputEffect(0, gamma.Get());

    ctx->BeginDraw();
    ctx->DrawImage(histogram.Get());
    if (FAILED(ctx->EndDraw())) return false;

    std::vector<float> histData(sc_histNumBins, 0.0f);
    hr = histogram->GetValue(
        D2D1_HISTOGRAM_PROP_HISTOGRAM_OUTPUT,
        reinterpret_cast<BYTE*>(histData.data()),
        sc_histNumBins * sizeof(float));
    if (FAILED(hr)) return false;

    const float tail = 1.0f - sc_maxCLLPercentile;
    float runningSum = 0.0f;
    int   maxBin     = 0;
    for (int i = static_cast<int>(sc_histNumBins) - 1; i >= 0; --i)
    {
        runningSum += histData[i];
        maxBin = i;
        if (runningSum >= tail) break;
    }

    float binNorm = static_cast<float>(maxBin) / static_cast<float>(sc_histNumBins);
    float nits    = std::pow(binNorm, 1.0f / sc_histGamma) * sc_histMaxNits;
    if (nits <= 0.0f) return false;     // some drivers return all zeros
    if (nits < sc_nominalRefWhite) nits = sc_nominalRefWhite;

    outNits = nits;
    return true;
}

// HDR path: WIC → D2D effect chain → BGRA8
static bool DecodeHdrViaD2D(
    IWICImagingFactory*    wicFactory,
    IWICBitmapFrameDecode* frame,
    UINT width, UINT height,
    Image& img, std::wstring& info)
{
    // 1. WIC format-convert to FP16 PRGBA — the lightest format the D2D
    //    ImageSource can ingest while preserving HDR float data.
    ComPtr<IWICFormatConverter> formatConvert;
    HRESULT hr = wicFactory->CreateFormatConverter(&formatConvert);
    if (FAILED(hr)) return false;
    hr = formatConvert->Initialize(
        frame, GUID_WICPixelFormat64bppPRGBAHalf,
        WICBitmapDitherTypeNone, nullptr, 0.0,
        WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return false;

    // 2. D3D11 + D2D device.
    ComPtr<ID3D11Device>          d3dDevice;
    ComPtr<ID2D1Factory3>         d2dFactory;
    ComPtr<ID2D1Device2>          d2dDevice;
    ComPtr<ID2D1DeviceContext2>   d2dContext;
    hr = CreateD2DContextForDecode(d3dDevice, d2dFactory, d2dDevice, d2dContext);
    if (FAILED(hr))
    {
        MessageBoxW(nullptr, L"Failed to create D2D device.", L"Error", MB_ICONERROR);
        return false;
    }

    // 3. Wrap WIC source as ID2D1ImageSource (ID2D1DeviceContext2, Win 10).
    ComPtr<ID2D1ImageSourceFromWic> imageSource;
    hr = d2dContext->CreateImageSourceFromWic(formatConvert.Get(), &imageSource);
    if (FAILED(hr)) return false;

    // 4. Read embedded ICC if present (most HDR JXR don't have one).
    ComPtr<IWICColorContext> wicColorContext;
    UINT actualNumProfiles = 0;
    if (SUCCEEDED(wicFactory->CreateColorContext(&wicColorContext)))
    {
        frame->GetColorContexts(1, wicColorContext.GetAddressOf(), &actualNumProfiles);
    }

    // 5. Source color context: embedded profile or scRGB (FP source default).
    ComPtr<ID2D1ColorContext> srcColorContext;
    if (actualNumProfiles >= 1)
    {
        hr = d2dContext->CreateColorContextFromWicColorContext(
            wicColorContext.Get(), &srcColorContext);
    }
    else
    {
        hr = d2dContext->CreateColorContext(
            D2D1_COLOR_SPACE_SCRGB, nullptr, 0, &srcColorContext);
    }
    if (FAILED(hr)) return false;

    // 6. ColorManagement #1: source → scRGB.
    ComPtr<ID2D1Effect> colorMgmtIn;
    hr = d2dContext->CreateEffect(CLSID_D2D1ColorManagement, &colorMgmtIn);
    if (FAILED(hr)) return false;
    colorMgmtIn->SetValue(D2D1_COLORMANAGEMENT_PROP_QUALITY,
                          D2D1_COLORMANAGEMENT_QUALITY_BEST);
    colorMgmtIn->SetValue(D2D1_COLORMANAGEMENT_PROP_SOURCE_COLOR_CONTEXT,
                          srcColorContext.Get());

    ComPtr<ID2D1ColorContext> scRgbContext;
    hr = d2dContext->CreateColorContext(D2D1_COLOR_SPACE_SCRGB, nullptr, 0, &scRgbContext);
    if (FAILED(hr)) return false;
    colorMgmtIn->SetValue(D2D1_COLORMANAGEMENT_PROP_DESTINATION_COLOR_CONTEXT,
                          scRgbContext.Get());
    colorMgmtIn->SetInput(0, imageSource.Get());

    // 7. BGRA8 render target bitmap.
    D2D1_BITMAP_PROPERTIES1 targetProps = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1Bitmap1> targetBitmap;
    hr = d2dContext->CreateBitmap(
        D2D1::SizeU(width, height), nullptr, 0, targetProps, &targetBitmap);
    if (FAILED(hr)) return false;
    d2dContext->SetTarget(targetBitmap.Get());

    // 8. Compute MaxCLL via D2D Histogram (GPU compute).
    float maxCLLNits  = sc_defaultMaxCLLNits;
    bool  computedCLL = ComputeMaxCLLNitsViaHistogram(d2dContext.Get(),
                                                      colorMgmtIn.Get(),
                                                      maxCLLNits);

    // 9. CLSID_D2D1HdrToneMap — the actual built-in HDR → SDR curve.
    ComPtr<ID2D1Effect> toneMap;
    hr = d2dContext->CreateEffect(CLSID_D2D1HdrToneMap, &toneMap);
    if (FAILED(hr))
    {
        MessageBoxW(nullptr,
            L"CLSID_D2D1HdrToneMap unavailable. Requires Windows 10 1809 or later.",
            L"Error", MB_ICONERROR);
        return false;
    }
    toneMap->SetInputEffect(0, colorMgmtIn.Get());
    toneMap->SetValue(D2D1_HDRTONEMAP_PROP_INPUT_MAX_LUMINANCE,  maxCLLNits);
    toneMap->SetValue(D2D1_HDRTONEMAP_PROP_OUTPUT_MAX_LUMINANCE, sc_sdrPeakNits);
    toneMap->SetValue(D2D1_HDRTONEMAP_PROP_DISPLAY_MODE,
                      D2D1_HDRTONEMAP_DISPLAY_MODE_SDR);

    // 10. CLSID_D2D1WhiteLevelAdjustment — identity for SDR target (in == out).
    ComPtr<ID2D1Effect> whiteLevel;
    hr = d2dContext->CreateEffect(CLSID_D2D1WhiteLevelAdjustment, &whiteLevel);
    if (FAILED(hr)) return false;
    whiteLevel->SetInputEffect(0, toneMap.Get());
    whiteLevel->SetValue(D2D1_WHITELEVELADJUSTMENT_PROP_INPUT_WHITE_LEVEL,
                         sc_nominalRefWhite);
    whiteLevel->SetValue(D2D1_WHITELEVELADJUSTMENT_PROP_OUTPUT_WHITE_LEVEL,
                         sc_sdrPeakNits);

    // 11. ColorManagement #2: scRGB → sRGB (gamma + 8-bit-ready encoding).
    ComPtr<ID2D1Effect> colorMgmtOut;
    hr = d2dContext->CreateEffect(CLSID_D2D1ColorManagement, &colorMgmtOut);
    if (FAILED(hr)) return false;
    colorMgmtOut->SetValue(D2D1_COLORMANAGEMENT_PROP_QUALITY,
                           D2D1_COLORMANAGEMENT_QUALITY_BEST);
    colorMgmtOut->SetValue(D2D1_COLORMANAGEMENT_PROP_SOURCE_COLOR_CONTEXT,
                           scRgbContext.Get());

    ComPtr<ID2D1ColorContext> sRgbContext;
    hr = d2dContext->CreateColorContext(D2D1_COLOR_SPACE_SRGB, nullptr, 0, &sRgbContext);
    if (FAILED(hr)) return false;
    colorMgmtOut->SetValue(D2D1_COLORMANAGEMENT_PROP_DESTINATION_COLOR_CONTEXT,
                           sRgbContext.Get());
    colorMgmtOut->SetInputEffect(0, whiteLevel.Get());

    // 12. Render the final pipeline into the BGRA8 target.
    d2dContext->BeginDraw();
    d2dContext->Clear(D2D1::ColorF(0, 0, 0, 0));
    d2dContext->DrawImage(colorMgmtOut.Get());
    hr = d2dContext->EndDraw();
    if (FAILED(hr))
    {
        MessageBoxW(nullptr, L"D2D EndDraw failed.", L"Error", MB_ICONERROR);
        return false;
    }

    // 13. GPU → CPU readback via staging bitmap.
    D2D1_BITMAP_PROPERTIES1 stagingProps = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1Bitmap1> staging;
    hr = d2dContext->CreateBitmap(
        D2D1::SizeU(width, height), nullptr, 0, stagingProps, &staging);
    if (FAILED(hr)) return false;

    D2D1_POINT_2U dstOrigin = D2D1::Point2U(0, 0);
    D2D1_RECT_U   srcRect   = D2D1::RectU(0, 0, width, height);
    hr = staging->CopyFromBitmap(&dstOrigin, targetBitmap.Get(), &srcRect);
    if (FAILED(hr)) return false;

    D2D1_MAPPED_RECT mapped = {};
    hr = staging->Map(D2D1_MAP_OPTIONS_READ, &mapped);
    if (FAILED(hr)) return false;

    int outStride = static_cast<int>(width) * 4;
    img.width  = static_cast<int>(width);
    img.height = static_cast<int>(height);
    img.stride = outStride;
    img.pixels.resize(static_cast<size_t>(outStride) * height);

    for (UINT y = 0; y < height; ++y)
    {
        memcpy(img.pixels.data() + static_cast<size_t>(y) * outStride,
               mapped.bits + static_cast<size_t>(y) * mapped.pitch,
               outStride);
    }
    staging->Unmap();

    wchar_t tmInfo[160];
    if (computedCLL)
    {
        swprintf_s(tmInfo,
            L"JXR \xB7 WIC \xB7 D2D HdrToneMap (peak %.0f nits, p%.2f)",
            maxCLLNits, sc_maxCLLPercentile * 100.0f);
    }
    else
    {
        swprintf_s(tmInfo,
            L"JXR \xB7 WIC \xB7 D2D HdrToneMap (peak %.0f nits, default)",
            maxCLLNits);
    }
    info = tmInfo;

    return true;
}

// SDR path: convert to BGRA8 directly
static bool DecodeSdrViaWic(
    IWICImagingFactory*    factory,
    IWICBitmapFrameDecode* frame,
    UINT width, UINT height,
    const wchar_t* path,
    Image& img, std::wstring& info)
{
    ComPtr<IWICFormatConverter> converter;
    HRESULT hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr)) return false;

    hr = converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                               WICBitmapDitherTypeNone, nullptr, 0.0,
                               WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return false;

    int stride = static_cast<int>(width) * 4;
    img.width  = static_cast<int>(width);
    img.height = static_cast<int>(height);
    img.stride = stride;
    img.pixels.resize(static_cast<size_t>(stride) * height);

    hr = converter->CopyPixels(nullptr, static_cast<UINT>(stride),
                               static_cast<UINT>(img.pixels.size()),
                               img.pixels.data());
    if (FAILED(hr)) return false;

    std::wstring ext = GetFileExt(path);
    if (ext == L".jxr" || ext == L".wdp" || ext == L".hdp")
        info = L"JXR \xB7 WIC";
    else
        info = L"WIC";
    return true;
}

bool DecodeWithWIC(const wchar_t* path, Image& img, std::wstring& info)
{
    CoInit com;

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        MessageBoxW(nullptr, L"Failed to create WIC factory.", L"Error", MB_ICONERROR);
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
                                             WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr))
    {
        MessageBoxW(nullptr, L"WIC cannot decode this file.", L"Error", MB_ICONERROR);
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr))
    {
        MessageBoxW(nullptr, L"WIC GetFrame failed.", L"Error", MB_ICONERROR);
        return false;
    }

    UINT width = 0, height = 0;
    frame->GetSize(&width, &height);

    WICPixelFormatGUID srcFmt{};
    frame->GetPixelFormat(&srcFmt);

    bool isHDR = (srcFmt == GUID_WICPixelFormat128bppRGBAFloat ||
                  srcFmt == GUID_WICPixelFormat128bppRGBFloat  ||
                  srcFmt == GUID_WICPixelFormat96bppRGBFloat   ||
                  srcFmt == GUID_WICPixelFormat64bppRGBAHalf   ||
                  srcFmt == GUID_WICPixelFormat48bppRGBHalf);

    if (isHDR)
        return DecodeHdrViaD2D(factory.Get(), frame.Get(), width, height, img, info);
    else
        return DecodeSdrViaWic(factory.Get(), frame.Get(), width, height, path, img, info);
}
