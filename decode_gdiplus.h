#pragma once

#include "image.h"

void InitGdiplus();
void ShutdownGdiplus();
bool DecodeWithGdiplus(const wchar_t* path, Image& img, std::wstring& info);
