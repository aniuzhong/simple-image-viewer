# **Image Viewer**

轻量 Windows 桌面图像查看器，主要是验证**测试色彩管理**与 **HDR tonemap** 算法。

- [**Image Viewer**](#image-viewer)
  - [开发环境](#开发环境)
  - [ICC 切换 (目前专门给 JPEG 使用)](#icc-切换-目前专门给-jpeg-使用)
  - [消除闪烁](#消除闪烁)
  - [CMYK JPEG 链路效率分析](#cmyk-jpeg-链路效率分析)
    - [能否更快？](#能否更快)
  - [GdiplusDecoder 做了什么](#gdiplusdecoder-做了什么)
  - [HDR 图片的显示做了什么](#hdr-图片的显示做了什么)
    - [解码层 — **不算很难（WIC 已内建支持）**](#解码层--不算很难wic-已内建支持)
    - [色调映射（Tone Mapping）— **真正难的地方**](#色调映射tone-mapping-真正难的地方)

```
                  File → Open ...
                        │
                        ▼
              ┌────────────────────┐
              │  GetFileExt(path)  │
              └────────┬───────────┘
                       │
       ┌───────────────┼────────────────┐
       │               │                │
   .jpg/.jpeg      .jxr/.wdp        其它
   .jfif           .hdp             (.png/.bmp/.gif
       │               │             /.tif/.ico …)
       ▼               ▼                │
  DecodeJpeg()   DecodeWithWIC()        ▼
  (TurboJPEG    (WIC + D2D effect   DecodeWithGdiplus()
   + lcms2)      chain)                 │
       │               │                │
       └───────┬───────┴────────────────┘
               ▼
        Image{ BGRA8 }
               │
               ▼
        WM_PAINT (双缓冲 GDI)
               │
               ▼
   StretchDIBits → 屏幕（HALFTONE 缩放，居中适配）
```

- **JPEG / CMYK** → libjpeg-turbo + lcms2（可自选 ICC，做 CMYK -> sRGB/Monitor profile 转换）
    - 这套流程实际工程上已经使用。
- **JXR / HDR** → WIC 解码 + D2D 内建 effect chain（HdrToneMap + WhiteLevel + ColorManagement，MaxCLL 走 D2D Histogram 自动估算）
    - 仅作为算法验证，Windows 打开 HDR 图片推荐 [HDR + WCG Image Viewer](https://13thsymphony.github.io/hdrimageviewer)。
- **PNG / BMP / GIF / TIFF / ICO** → GDI+（保底通用解码）

## 开发环境

- vcpkg：`lcms2`、`libjpeg-turbo`
    - 对于 libjpeg-turbo 这种在 Windows 上编译比较繁琐（涉及汇编器）的库，用 vcpkg 是最省事。
- 系统：最低 Windows 10 1809（`CLSID_D2D1HdrToneMap` 要求）

## ICC 切换 (目前专门给 JPEG 使用)

菜单 `Color Profile` 改 `g_outputIccPath` 后会触发当前文件**重新解码**：

- **JPEG**：lcms2 transform 的 destination profile 切换，CMYK 也会重走 transform
- **WIC HDR**：当前实现以 sRGB 作为最终输出，**不使用** `g_outputIccPath`（D2D 第二个 ColorManagement 的 destination 永远是 sRGB）
- **GDI+**：不接 ICC


## 消除闪烁

- **双缓冲**：`WM_PAINT` 中先绘制到 off-screen `memDC`，最后 `BitBlt` 一次性拷贝到屏幕 — 背景和图片不再交替可见
- **`InvalidateRect(..., FALSE)`**：不再请求系统擦除背景
- **移除 `CS_HREDRAW | CS_VREDRAW`**：不再由系统强制带擦除的全区域重绘，改为 `WM_SIZE` 中自己 invalidate
## CMYK JPEG 链路效率分析

当前（`/O2` Release 编译 + lcms2 fastfloat/threaded 插件）：

| 步骤 | 操作 | 耗时占比 | 优化状态 |
|---|---|---|---|
| (1) 文件读取 | `fread` 整文件到内存 | ~5% | 已最优（可用 `CreateFileMapping` 零拷贝，但对 JPEG 文件大小收益微小） |
| (2) JPEG 解码 | `tj3Decompress8` → CMYK 8bit | ~60% | TurboJPEG 内部 SIMD (SSE2/AVX2)，已极致 |
| (3) CMYK 反转 | `255 - b` 逐字节 | ~2% | MSVC `/O2` 自动向量化为 `vpxor ymm`，无需手写 SIMD |
| (4) 色彩转换 | `cmsDoTransform` CMYK→BGRA | ~30% | fastfloat 插件用 SIMD，threaded 插件多核并行 |
| (5) Alpha 修复 | 每 4 字节写 `0xFF` | ~1% | 编译器自动向量化 |
| (6) ICC 解析 | `cmsOpenProfileFromMem` | ~2% | 可缓存但收益极小 |

### 能否更快？

- **步骤(4)和(5)无法合并**：创建自定义 ToneCurve 嵌入 profile 理论可行但很难且收益很小
- **步骤(2)是最大瓶颈**：JPEG 熵解码本身 CPU 密集，TurboJPEG 已是最快的纯 CPU 方案
- **GPU 加速**（NVJPEG）可绕过 CPU 瓶颈，但复杂度非常大且依赖 GPU 驱动
- **内存分配**：两次 `resize`（rawBuf + pixels）各分配 `W×H×4` 字节，可用对象池复用，但收益很小

> 当前方案在 CPU 软解码路线上**已经接近极致**。唯一有意义的进一步优化是 GPU 硬件解码，但那完全是另一回事。

## GdiplusDecoder 做了什么

核心就是 `Bitmap::FromFile()` → `LockBits(PixelFormat32bppRGB)` → `memcpy` 到 BGRA buffer。支持 **JPEG / PNG / BMP / ICO**。

## HDR 图片的显示做了什么

NVIDIA App 在开启 HDR 模式下进行游戏截图时，默认使用的是 .jxr 格式。测试图片是我在 `刺客信条 - 起源` 中截的图。

| 属性 | 值 |
|---|---|
| 分辨率 | 2560 × 1440 |
| 像素格式 | **Rgba128Float** (每通道 32-bit float，共 128-bit/pixel) |
| 解码器 | WmpBitmapDecoder（Windows Media Photo = JPEG XR） |

### 解码层 — **不算很难（WIC 已内建支持）**

Windows Imaging Component (WIC) 原生支持 JPEG XR，且是 **C/COM API**，可直接从 C++ 调用。GDI+ 不支持，但 WIC 是 Windows Vista+ 内建组件，**无需任何外部库**。

核心流程：
```cpp
IWICBitmapDecoder → IWICBitmapFrameDecode → IWICFormatConverter → CopyPixels
```

### 色调映射（Tone Mapping）— **真正难的地方**

`Rgba128Float` 意味着这是 **HDR 内容**，像素值范围 `[0.0, +INF)`，不能直接截断到 `[0, 255]`。需要色调映射。