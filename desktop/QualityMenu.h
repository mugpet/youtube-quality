#pragma once
#include <windows.h>
#include <UIAutomation.h>
#include <string>
#include <vector>

enum class MenuResult { Applied, AlreadySet, NotReady, Busy, Unsupported, AutoSettling };

inline constexpr UINT autoSettleDelayMs = 4000;

// Kept only while two readings of an automatic stream are being compared.
struct AutoQualityObservation {
    std::vector<int> documentId;
    std::wstring pageUrl, reportedQuality, targetQuality;
    ULONGLONG firstSeen = 0;
};

// Run on a COM-initialized worker for a verified Chrome or Edge foreground window.
MenuResult ApplyBestQuality(IUIAutomation* automation, HWND window, AutoQualityObservation& previousAuto);
