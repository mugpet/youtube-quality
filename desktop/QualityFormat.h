#pragma once

#include <cwchar>
#include <string>

namespace quality {
struct ReportedFormat {
    int height = 0, fps = 0;
    bool premium = false, standard = false;
};

// The menu reader supplies its normalized, lowercase accessible label.
inline ReportedFormat ParseReportedFormat(const std::wstring& text) {
    ReportedFormat format;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] < L'0' || text[i] > L'9') continue;
        wchar_t* end = nullptr;
        const long height = std::wcstol(text.c_str() + i, &end, 10);
        i = static_cast<std::size_t>(end - text.c_str()) - 1;
        while (*end == L' ') ++end;
        if (*end != L'p' || height < 1 || height > 16384) continue;
        format.height = static_cast<int>(height);
        if (end[1] >= L'0' && end[1] <= L'9') {
            const long fps = std::wcstol(end + 1, nullptr, 10);
            if (fps > 0 && fps <= 1000) format.fps = static_cast<int>(fps);
        }
        break;
    }
    auto has = [&](const wchar_t* label) { return text.find(label) != std::wstring::npos; };
    format.premium = has(L"premium") || has(L"enhanced bitrate") || has(L"forbedret bithastighed");
    format.standard = has(L"standard") || has(L"normal bitrate");
    return format;
}

inline bool PlayingAtBest(const ReportedFormat& current, const ReportedFormat& best) {
    if (current.height > 1440 || current.height < best.height) return false;
    // The available list can briefly omit the higher stream already playing.
    if (current.height > best.height) return true;
    // An omitted bitrate or frame rate can still match the best rendition.
    if (best.premium && current.standard && !current.premium) return false;
    return !current.fps || !best.fps || current.fps >= best.fps;
}

inline bool ManualSummaryDisagrees(const ReportedFormat& summary, const ReportedFormat& selected) {
    if (!selected.height) return true;
    return (summary.height && summary.height != selected.height) ||
           (summary.fps && selected.fps && summary.fps != selected.fps) ||
           (summary.premium && !selected.premium) || (summary.standard && selected.premium);
}
}  // namespace quality
