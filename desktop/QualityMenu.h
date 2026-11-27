#pragma once
#include <windows.h>
#include <UIAutomation.h>

enum class MenuResult { Applied, AlreadySet, NotReady, Busy, Unsupported };

// Run on a COM-initialized worker for a verified Chrome or Edge foreground window.
MenuResult ApplyBestQuality(IUIAutomation* automation, HWND window);
