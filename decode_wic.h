#pragma once

#include "image.h"

bool DecodeWithWIC(const wchar_t* path, Image& img, std::wstring& info);
