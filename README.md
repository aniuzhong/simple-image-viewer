# Image Viewer

轻量 Windows 桌面图像查看器，主要是验证**测试色彩管理**与 **HDR tonemap** 算法。

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

## ICC 切换 (目前专门给 JPEG 使用)

菜单 `Color Profile` 改 `g_outputIccPath` 后会触发当前文件**重新解码**：

- **JPEG**：lcms2 transform 的 destination profile 切换，CMYK 也会重走 transform
- **WIC HDR**：当前实现以 sRGB 作为最终输出，**不使用** `g_outputIccPath`（D2D 第二个 ColorManagement 的 destination 永远是 sRGB）
- **GDI+**：不接 ICC

## 依赖

- vcpkg：`lcms2`、`libjpeg-turbo`
    - 对于 libjpeg-turbo 这种在 Windows 上编译比较繁琐（涉及汇编器）的库，用 vcpkg 是最省事。
- 系统：最低 Windows 10 1809（`CLSID_D2D1HdrToneMap` 要求）
