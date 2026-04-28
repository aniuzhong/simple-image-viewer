
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <chrono>

#include "image.h"
#include "decode_jpeg.h"
#include "decode_gdiplus.h"
#include "decode_wic.h"

static Image g_image;
static HWND g_hwndStatus = nullptr;
static HMENU g_hProfileMenu  = nullptr;
static std::wstring g_outputIccPath;
static std::wstring g_currentFilePath;
static constexpr const wchar_t* APP_TITLE = L"Image Viewer";

static bool IsJpegExt(const std::wstring& ext)
{
    return ext == L".jpg" || ext == L".jpeg" || ext == L".jfif";
}

static bool IsWicExt(const std::wstring& ext)
{
    return ext == L".jxr" || ext == L".wdp" || ext == L".hdp";
}

static void SetStatus(const wchar_t* text)
{
    if (g_hwndStatus)
        SendMessageW(g_hwndStatus, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(text));
}

static bool DecodeImage(const wchar_t* path, Image& img,
                        double& elapsedMs, std::wstring& info)
{
    using Clock = std::chrono::high_resolution_clock;
    auto t0 = Clock::now();

    std::wstring ext = GetFileExt(path);
    const wchar_t* icc = g_outputIccPath.empty() ? nullptr : g_outputIccPath.c_str();
    bool ok;
    if (IsJpegExt(ext))
        ok = DecodeJpeg(path, img, info, icc);
    else if (IsWicExt(ext))
        ok = DecodeWithWIC(path, img, info);
    else
        ok = DecodeWithGdiplus(path, img, info);

    elapsedMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    return ok;
}

enum { IDM_OPEN = 1001,
       IDM_PROFILE_SRGB = 2001, IDM_PROFILE_MONITOR, IDM_PROFILE_CUSTOM };

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        HMENU hMenu = CreateMenu();
        HMENU hFile = CreatePopupMenu();
        AppendMenuW(hFile, MF_STRING, IDM_OPEN, L"&Open...\tCtrl+O");
        AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hFile), L"&File");

        g_hProfileMenu = CreatePopupMenu();
        AppendMenuW(g_hProfileMenu, MF_STRING, IDM_PROFILE_SRGB,    L"sRGB (&Default)");
        AppendMenuW(g_hProfileMenu, MF_STRING, IDM_PROFILE_MONITOR, L"&Monitor Profile");
        AppendMenuW(g_hProfileMenu, MF_STRING, IDM_PROFILE_CUSTOM,  L"&Custom ICC...");
        CheckMenuRadioItem(g_hProfileMenu, IDM_PROFILE_SRGB, IDM_PROFILE_CUSTOM,
                           IDM_PROFILE_SRGB, MF_BYCOMMAND);
        AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_hProfileMenu), L"Color &Profile");

        SetMenu(hwnd, hMenu);

        g_hwndStatus = CreateWindowExW(
            0, STATUSCLASSNAMEW, nullptr,
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
            0, 0, 0, 0, hwnd, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        SetStatus(L"  Ready");
        return 0;
    }
    case WM_COMMAND:
    {
        WORD id = LOWORD(wp);

        if (id == IDM_OPEN)
        {
            wchar_t filePath[MAX_PATH]{};
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner   = hwnd;
            ofn.lpstrFilter =
                L"All Images\0*.jpg;*.jpeg;*.jfif;*.png;*.bmp;*.gif;*.tiff;*.tif;*.ico;*.jxr;*.wdp;*.hdp\0"
                L"JPEG\0*.jpg;*.jpeg;*.jfif\0"
                L"PNG\0*.png\0"
                L"BMP\0*.bmp\0"
                L"All Files\0*.*\0";
            ofn.lpstrFile   = filePath;
            ofn.nMaxFile    = MAX_PATH;
            ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

            if (GetOpenFileNameW(&ofn))
            {
                double ms = 0;
                std::wstring info;
                if (DecodeImage(filePath, g_image, ms, info))
                {
                    g_currentFilePath = filePath;
                    SetWindowTextW(hwnd,
                        (std::wstring(APP_TITLE) + L" \u2013 " + filePath).c_str());

                    wchar_t buf[512];
                    swprintf_s(buf,
                        L"  %.1f ms  \u2502  %d \u00D7 %d  \u2502  %ls",
                        ms, g_image.width, g_image.height, info.c_str());
                    SetStatus(buf);

                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
        }
        else if (id == IDM_PROFILE_SRGB)
        {
            g_outputIccPath.clear();
            CheckMenuRadioItem(g_hProfileMenu, IDM_PROFILE_SRGB, IDM_PROFILE_CUSTOM,
                               IDM_PROFILE_SRGB, MF_BYCOMMAND);
        }
        else if (id == IDM_PROFILE_MONITOR)
        {
            wchar_t iccPath[MAX_PATH]{};
            DWORD len = MAX_PATH;
            HDC hdc = GetDC(hwnd);
            BOOL ok = GetICMProfileW(hdc, &len, iccPath);
            ReleaseDC(hwnd, hdc);
            if (ok)
            {
                g_outputIccPath = iccPath;
                CheckMenuRadioItem(g_hProfileMenu, IDM_PROFILE_SRGB, IDM_PROFILE_CUSTOM,
                                   IDM_PROFILE_MONITOR, MF_BYCOMMAND);
            }
            else
            {
                MessageBoxW(hwnd, L"No ICC profile associated with this monitor.",
                            L"Color Profile", MB_ICONWARNING);
            }
        }
        else if (id == IDM_PROFILE_CUSTOM)
        {
            wchar_t iccFile[MAX_PATH]{};
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner   = hwnd;
            ofn.lpstrFilter = L"ICC Profiles\0*.icc;*.icm\0All Files\0*.*\0";
            ofn.lpstrFile   = iccFile;
            ofn.nMaxFile    = MAX_PATH;
            ofn.Flags       = OFN_FILEMUSTEXIST;
            if (GetOpenFileNameW(&ofn))
            {
                g_outputIccPath = iccFile;
                CheckMenuRadioItem(g_hProfileMenu, IDM_PROFILE_SRGB, IDM_PROFILE_CUSTOM,
                                   IDM_PROFILE_CUSTOM, MF_BYCOMMAND);
            }
        }

        // Re-decode current image when output profile changes
        if ((id == IDM_PROFILE_SRGB || id == IDM_PROFILE_MONITOR || id == IDM_PROFILE_CUSTOM)
            && !g_currentFilePath.empty())
        {
            double ms = 0;
            std::wstring info;
            if (DecodeImage(g_currentFilePath.c_str(), g_image, ms, info))
            {
                wchar_t buf[512];
                swprintf_s(buf,
                    L"  %.1f ms  \u2502  %d \u00D7 %d  \u2502  %ls",
                    ms, g_image.width, g_image.height, info.c_str());
                SetStatus(buf);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (wp == 'O' && (GetKeyState(VK_CONTROL) & 0x8000))
            SendMessageW(hwnd, WM_COMMAND, IDM_OPEN, 0);
        return 0;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rc;
        GetClientRect(hwnd, &rc);

        // Exclude status-bar area
        if (g_hwndStatus)
        {
            RECT rcs;
            GetWindowRect(g_hwndStatus, &rcs);
            rc.bottom -= (rcs.bottom - rcs.top);
        }

        int cw = rc.right, ch = rc.bottom;

        //  Double buffer
        HDC     memDC  = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, cw, ch);
        HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(memDC, memBmp));

        RECT memRc = {0, 0, cw, ch};
        FillRect(memDC, &memRc, static_cast<HBRUSH>(GetStockObject(DKGRAY_BRUSH)));

        if (g_image.width > 0 && g_image.height > 0)
        {
            float sx = static_cast<float>(cw) / g_image.width;
            float sy = static_cast<float>(ch) / g_image.height;
            float s  = (sx < sy) ? sx : sy;
            if (s > 1.0f) s = 1.0f;

            int dw = static_cast<int>(g_image.width  * s);
            int dh = static_cast<int>(g_image.height * s);
            int dx = (cw - dw) / 2;
            int dy = (ch - dh) / 2;

            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth       = g_image.width;
            bmi.bmiHeader.biHeight      = -g_image.height;   // top-down
            bmi.bmiHeader.biPlanes      = 1;
            bmi.bmiHeader.biBitCount    = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            SetStretchBltMode(memDC, HALFTONE);
            SetBrushOrgEx(memDC, 0, 0, nullptr);
            StretchDIBits(memDC, dx, dy, dw, dh,
                          0, 0, g_image.width, g_image.height,
                          g_image.pixels.data(), &bmi,
                          DIB_RGB_COLORS, SRCCOPY);
        }
        else
        {
            SetBkMode(memDC, TRANSPARENT);
            SetTextColor(memDC, RGB(200, 200, 200));
            DrawTextW(memDC, L"File \u2192 Open to load an image", -1, &memRc,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        BitBlt(hdc, 0, 0, cw, ch, memDC, 0, 0, SRCCOPY);

        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        if (g_hwndStatus) SendMessageW(g_hwndStatus, WM_SIZE, 0, 0);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nShow)
{
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    InitGdiplus();
    InitLcmsPlugins();

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance      = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"ImageViewerClass";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, APP_TITLE,
                                WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1024, 768,
                                nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    ShutdownGdiplus();
    return static_cast<int>(msg.wParam);
}