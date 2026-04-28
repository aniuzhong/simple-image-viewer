#pragma once

#include <vector>
#include <string>
#include <cwchar>

struct Image
{
    int width  = 0;
    int height = 0;
    int stride = 0;
    std::vector<unsigned char> pixels; // BGRA, top-down
};

inline std::wstring GetFileExt(const wchar_t* path)
{
    const wchar_t* dot = wcsrchr(path, L'.');
    if (!dot) return {};
    std::wstring ext(dot);
    for (auto& c : ext) c = towlower(c);
    return ext;
}
