#pragma once

#include "image.h"

void InitLcmsPlugins();
bool DecodeJpeg(const wchar_t* path, Image& img, std::wstring& info,
                const wchar_t* outputIccPath = nullptr);
