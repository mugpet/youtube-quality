#pragma once
#include <windows.h>

inline constexpr UINT kTrayNotification = WM_APP + 0x31;
inline constexpr UINT kTrayExitCommand = 0x5101;

bool AddTrayIcon(HWND hwnd);
void RemoveTrayIcon(HWND hwnd);
bool IsTrayActivation(LPARAM notification);
void ShowTrayMenu(HWND hwnd);
