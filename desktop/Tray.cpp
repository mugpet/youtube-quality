#include "Tray.h"
#include <shellapi.h>
#include <string>

namespace {
constexpr UINT iconId = 1;
constexpr UINT startupCommand = 0x5102;

enum class Startup { Off, On, Unavailable };

#ifndef YTQ_TEST_BUILD
constexpr auto runKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr auto runName = L"YouTube Quality (1440p)";

std::wstring StartupCommandLine() {
    // Windows limits a Run entry's entire command line to 260 characters.
    wchar_t path[MAX_PATH + 1]{};
    DWORD length = GetModuleFileNameW(nullptr, path, _countof(path));
    if (!length || length >= _countof(path)) return {};
    std::wstring commandLine = L"\"" + std::wstring(path, length) + L"\" --startup";
    return commandLine.size() <= MAX_PATH ? commandLine : std::wstring{};
}

Startup ReadStartup(const std::wstring& expected) {
    if (expected.empty()) return Startup::Unavailable;
    wchar_t saved[MAX_PATH + 1]{};
    DWORD size = sizeof(saved);
    auto status = RegGetValueW(HKEY_CURRENT_USER, runKey, runName, RRF_RT_REG_SZ,
                               nullptr, saved, &size);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) return Startup::Off;
    if (status == ERROR_SUCCESS && _wcsicmp(saved, expected.c_str()) == 0) return Startup::On;
    return Startup::Unavailable;
}

void ChangeStartup(HWND hwnd, Startup shown) {
    const auto expected = StartupCommandLine();
    // Only change a value that still has the state the person chose to toggle.
    if (shown == Startup::Unavailable || ReadStartup(expected) != shown) return;
    LSTATUS status;
    if (shown == Startup::On) {
        status = RegDeleteKeyValueW(HKEY_CURRENT_USER, runKey, runName);
    } else {
        const auto bytes = static_cast<DWORD>((expected.size() + 1) * sizeof(wchar_t));
        status = RegSetKeyValueW(HKEY_CURRENT_USER, runKey, runName, REG_SZ, expected.c_str(), bytes);
    }
    if (status != ERROR_SUCCESS)
        MessageBoxW(hwnd, L"Windows could not update the sign-in setting.", L"YouTube Quality",
                    MB_OK | MB_ICONWARNING);
}
#endif

NOTIFYICONDATAW IconData(HWND hwnd) {
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = hwnd;
    icon.uID = iconId;
    return icon;
}

POINT MenuPosition(HWND hwnd) {
    NOTIFYICONIDENTIFIER identity{};
    identity.cbSize = sizeof(identity);
    identity.hWnd = hwnd;
    identity.uID = iconId;
    RECT bounds{};
    if (SUCCEEDED(Shell_NotifyIconGetRect(&identity, &bounds)) && !IsRectEmpty(&bounds))
        return {(bounds.left + bounds.right) / 2, bounds.top};
    POINT point{};
    GetCursorPos(&point);
    return point;
}
}

bool AddTrayIcon(HWND hwnd) {
    auto icon = IconData(hwnd);
    icon.uCallbackMessage = kTrayNotification;
    icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    icon.hIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1),
        IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    if (!icon.hIcon) return false;
    wcscpy_s(icon.szTip, L"YouTube Quality\nChrome and Edge \u00b7 up to 1440p");
    if (!Shell_NotifyIconW(NIM_ADD, &icon)) return false;
    icon.uVersion = NOTIFYICON_VERSION_4;
    if (Shell_NotifyIconW(NIM_SETVERSION, &icon)) return true;
    RemoveTrayIcon(hwnd);
    return false;
}

void RemoveTrayIcon(HWND hwnd) {
    auto icon = IconData(hwnd);
    Shell_NotifyIconW(NIM_DELETE, &icon);
}

bool IsTrayActivation(LPARAM notification) {
    const auto event = LOWORD(notification);
    if (HIWORD(notification) != iconId ||
        (event != WM_CONTEXTMENU && event != NIN_SELECT && event != NIN_KEYSELECT &&
         event != WM_LBUTTONUP && event != WM_RBUTTONUP)) return false;
    // Explorer may report both a mouse release and the matching activation.
    static DWORD previous = 0;
    const DWORD time = static_cast<DWORD>(GetMessageTime());
    if (previous && time - previous < 250) return false;
    previous = time;
    return true;
}

void ShowTrayMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"YouTube Quality \u00b7 up to 1440p");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
#ifdef YTQ_TEST_BUILD
    const auto startup = Startup::Unavailable;
#else
    const auto startup = ReadStartup(StartupCommandLine());
#endif
    const UINT startupFlags = startup == Startup::Unavailable ? MF_GRAYED
        : (startup == Startup::On ? MF_CHECKED : MF_UNCHECKED);
    AppendMenuW(menu, MF_STRING | startupFlags, startupCommand, L"&Start with Windows");
    AppendMenuW(menu, MF_STRING, kTrayExitCommand, L"E&xit YouTube Quality");
    const POINT point = MenuPosition(hwnd);
    // A tray menu must own the foreground for outside clicks to dismiss it.
    SetForegroundWindow(hwnd);
    const UINT command = TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN |
        TPM_CENTERALIGN | TPM_RETURNCMD | TPM_NONOTIFY, point.x, point.y, hwnd, nullptr);
    DestroyMenu(menu);
    if (command == kTrayExitCommand) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    } else {
#ifndef YTQ_TEST_BUILD
        if (command == startupCommand) ChangeStartup(hwnd, startup);
#endif
        PostMessageW(hwnd, WM_NULL, 0, 0);
        auto icon = IconData(hwnd);
        Shell_NotifyIconW(NIM_SETFOCUS, &icon);
    }
}
