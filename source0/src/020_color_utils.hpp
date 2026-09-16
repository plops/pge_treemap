#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "olcPixelGameEngine3.h"

namespace pge_treemap {

inline Pixel HSLtoPixel(const float h, const float s, const float l)
{
    auto f = [h, s, l](const float n) {
        const float k = std::fmod(n + h * 12.0f, 12.0f);
        const float a = s * std::min(l, 1.0f - l);
        return l - a * std::max(-1.0f, std::min({k - 3.0f, 9.0f - k, 1.0f}));
    };
    return {
        static_cast<uint8_t>(f(0) * 255.0f),
        static_cast<uint8_t>(f(8) * 255.0f),
        static_cast<uint8_t>(f(4) * 255.0f)};
}

inline Pixel GetColorForFilename(const std::string_view name)
{
    const auto dotPos = name.rfind('.');
    if (dotPos == std::string_view::npos) return {120, 130, 140};

    std::string ext(name.substr(dotPos));
    for (char& c: ext) c = static_cast<char>(tolower(c));

    if (ext == ".mp4" || ext == ".mkv" || ext == ".avi" || ext == ".mov") return {185, 60, 220};
    if (ext == ".mp3" || ext == ".flac" || ext == ".wav" || ext == ".ogg") return {240, 205, 35};
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" || ext == ".gif") return {30, 190, 230};
    if (ext == ".zip" || ext == ".rar" || ext == ".7z" || ext == ".tar" || ext == ".gz") return {235, 50, 50};
    if (ext == ".cpp" || ext == ".h" || ext == ".rs" || ext == ".py" || ext == ".js" || ext == ".txt" || ext == ".md")
        return {40, 210, 110};
    if (ext == ".exe" || ext == ".dll" || ext == ".so" || ext == ".bin") return {60, 100, 240};

    const size_t hash = std::hash<std::string>{}(ext);
    return HSLtoPixel(static_cast<float>(hash % 360) / 360.0f, 0.70f, 0.55f);
}

inline std::string FormatBytes(const uintmax_t bytes)
{
    const char* suffixes[] = {"B", "KB", "MB", "GB", "TB"};
    int         i          = 0;
    auto        dBytes     = static_cast<double>(bytes);
    while (dBytes >= 1024.0 && i < 4)
    {
        dBytes /= 1024.0;
        i++;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f %s", dBytes, suffixes[i]);
    return {buf};
}

} // namespace pge_treemap
