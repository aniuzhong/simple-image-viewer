#include <windows.h>
#include <vector>
#include <memory>
#include <cstdio>

#include <turbojpeg.h>

#include <lcms2.h>
#include <lcms2_fast_float.h>
#include <lcms2_threaded.h>

#include "decode_jpeg.h"

void InitLcmsPlugins()
{
    cmsPlugin(cmsFastFloatExtensions());
    cmsPlugin(cmsThreadedExtensions(CMS_THREADED_GUESS_MAX_THREADS, 0));
}

struct TjDestroyer
{
    void operator()(void* h) const { if (h) tj3Destroy(static_cast<tjhandle>(h)); }
};
using TjHandle = std::unique_ptr<void, TjDestroyer>;
using ProfileGuard = std::unique_ptr<void, decltype(&cmsCloseProfile)>;
using TransformGuard = std::unique_ptr<void, decltype(&cmsDeleteTransform)>;

static ProfileGuard   MakeProfileGuard(cmsHPROFILE h)     { return {h, cmsCloseProfile}; }
static TransformGuard MakeTransformGuard(cmsHTRANSFORM h) { return {h, cmsDeleteTransform}; }

static cmsHPROFILE OpenProfileFromWPath(const wchar_t* iccPath)
{
    FILE* f = nullptr;
    _wfopen_s(&f, iccPath, L"rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    std::vector<unsigned char> buf(size);
    fread(buf.data(), 1, size, f);
    fclose(f);
    return cmsOpenProfileFromMem(buf.data(), static_cast<cmsUInt32Number>(size));
}

bool DecodeJpeg(const wchar_t* path, Image& img, std::wstring& info,
                const wchar_t* outputIccPath)
{
    FILE* f = nullptr;
    _wfopen_s(&f, path, L"rb");
    if (!f)
    {
        MessageBoxW(nullptr, L"Cannot open file.", L"Error", MB_ICONERROR);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    rewind(f);
    std::vector<unsigned char> jpegBuf(fileSize);
    fread(jpegBuf.data(), 1, fileSize, f);
    fclose(f);

    TjHandle tj(tj3Init(TJINIT_DECOMPRESS));
    if (!tj) return false;

    if (tj3DecompressHeader(tj.get(), jpegBuf.data(), jpegBuf.size()) != 0)
    {
        MessageBoxA(nullptr, tj3GetErrorStr(tj.get()), "TurboJPEG", MB_ICONERROR);
        return false;
    }

    int width      = tj3Get(tj.get(), TJPARAM_JPEGWIDTH);
    int height     = tj3Get(tj.get(), TJPARAM_JPEGHEIGHT);
    int colorspace = tj3Get(tj.get(), TJPARAM_COLORSPACE);

    bool isCMYK = (colorspace == TJCS_CMYK || colorspace == TJCS_YCCK);
    int  tjFmt  = isCMYK ? TJPF_CMYK : TJPF_BGRA;
    int  stride = width * 4;

    std::vector<unsigned char> rawBuf(static_cast<size_t>(stride) * height);

    if (tj3Decompress8(tj.get(), jpegBuf.data(), jpegBuf.size(),
                       rawBuf.data(), stride, tjFmt) != 0)
    {
        MessageBoxA(nullptr, tj3GetErrorStr(tj.get()), "TurboJPEG", MB_ICONERROR);
        return false;
    }

    img.width  = width;
    img.height = height;
    img.stride = stride;

    if (isCMYK)
    {
        unsigned char* iccBuf  = nullptr;
        size_t         iccSize = 0;

        cmsHPROFILE srcProfile = nullptr;
        if (tj3GetICCProfile(tj.get(), &iccBuf, &iccSize) == 0 && iccBuf && iccSize)
        {
            srcProfile = cmsOpenProfileFromMem(iccBuf, static_cast<cmsUInt32Number>(iccSize));
            tj3Free(iccBuf);
        }
        if (!srcProfile)
        {
            MessageBoxW(nullptr, L"No usable ICC profile in CMYK image.", L"Error", MB_ICONERROR);
            return false;
        }
        ProfileGuard srcGuard = MakeProfileGuard(srcProfile);

        cmsHPROFILE dstProfile = nullptr;
        if (outputIccPath && outputIccPath[0])
            dstProfile = OpenProfileFromWPath(outputIccPath);
        if (!dstProfile)
            dstProfile = cmsCreate_sRGBProfile();
        if (!dstProfile) return false;
        ProfileGuard dstGuard = MakeProfileGuard(dstProfile);

        cmsHTRANSFORM xform = cmsCreateTransform(
            srcProfile, TYPE_CMYK_8,
            dstProfile, TYPE_BGRA_8,
            INTENT_PERCEPTUAL, 0);
        if (!xform) return false;
        TransformGuard xfGuard = MakeTransformGuard(xform);

        // Invert CMYK (YCCK / Photoshop convention)
        for (auto& b : rawBuf) b = static_cast<unsigned char>(255 - b);

        // CMYK -> BGRA directly via lcms2
        img.pixels.resize(static_cast<size_t>(stride) * height);
        cmsDoTransform(xform, rawBuf.data(), img.pixels.data(), width * height);

        // Force alpha = 255 (lcms2 outputs 0 for the extra channel)
        for (size_t i = 3; i < img.pixels.size(); i += 4)
            img.pixels[i] = 255;

        info = L"JPEG \xB7 TurboJPEG + lcms2 CMYK";
    }
    else
    {
        img.pixels = std::move(rawBuf);
        info = L"JPEG \xB7 TurboJPEG";
    }
    return true;
}
