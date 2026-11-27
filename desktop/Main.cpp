#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <oleacc.h>
#include <wrl/client.h>
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>
#include "QualityMenu.h"
#include "Tray.h"

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {
#ifdef YTQ_TEST_BUILD
constexpr auto controlClass = L"YouTubeQuality.TestTray";
constexpr auto instanceName = L"Local\\YouTubeQuality.TestTray.v1";
#else
constexpr auto controlClass = L"YouTubeQuality.Tray";
constexpr auto instanceName = L"Local\\YouTubeQuality.Tray.v1";
#endif
constexpr UINT refreshMessage = WM_APP + 1;
constexpr UINT resultMessage = WM_APP + 2;
constexpr UINT showMenuMessage = WM_APP + 3;
constexpr UINT_PTR checkTimer = 1;
constexpr UINT_PTR trayRetryTimer = 2;
constexpr UINT checkInterval = 120000;

struct VideoWindow {
    HWND window = nullptr;
    DWORD pid = 0;
    HWND renderer = nullptr;
    std::wstring title;
    bool operator==(const VideoWindow& other) const {
        return window == other.window && pid == other.pid && renderer == other.renderer && title == other.title;
    }
};
struct LastCheck { VideoWindow target; ULONGLONG time; };
struct Monitor {
    HWND control = nullptr;
    UINT taskbarCreated = 0;
    HWINEVENTHOOK foregroundHook = nullptr, titleHook = nullptr, tabHook = nullptr;
    HWND foreground = nullptr;
    DWORD browserPid = 0;
    VideoWindow current;
    VideoWindow inFlight;
    std::vector<LastCheck> recent;
    std::vector<std::wstring> browsers;
    std::thread worker;
    UINT_PTR generation = 0;
    ULONGLONG timerDue = 0;
    unsigned retries = 0;
    unsigned trayRetries = 0;
    bool refreshPosted = false, tabChanged = false, working = false, confirmedUnsupported = false, stopping = false;
#ifdef YTQ_TEST_BUILD
    std::wstring traceFile;
    std::vector<DWORD> allowedPids;
#endif
} monitor;

std::wstring Environment(const wchar_t* name) {
    DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size) return {};
    std::wstring value(size, L'\0');
    DWORD length = GetEnvironmentVariableW(name, value.data(), size);
    if (!length || length >= size) return {};
    value.resize(length);
    return value;
}

#ifdef YTQ_TEST_BUILD
void Trace(const char* message) {
    if (monitor.traceFile.empty()) return;
    HANDLE file = CreateFileW(monitor.traceFile.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    const std::string line = std::to_string(GetTickCount64()) + " " + message + "\r\n";
    DWORD written;
    WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(file);
}
bool SetTestScope() {
    monitor.traceFile = Environment(L"YTQ_TRACE_FILE");
    const std::wstring list = Environment(L"YTQ_TEST_BROWSER_PIDS");
    size_t start = 0;
    while (start < list.size()) {
        const size_t end = list.find(L',', start);
        auto token = list.substr(start, end - start);
        if (token.empty() || token.find_first_not_of(L"0123456789") != token.npos) return false;
        const auto pid = std::stoull(token);
        if (!pid || pid > MAXDWORD) return false;
        monitor.allowedPids.push_back(static_cast<DWORD>(pid));
        if (end == list.npos) break;
        start = end + 1;
    }
    return !monitor.allowedPids.empty();
}
#else
void Trace(const char*) {}
#endif

void FindBrowsers() {
    for (auto exe : {L"chrome.exe", L"msedge.exe"}) {
        const auto key = std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\") + exe;
        for (auto hive : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
            wchar_t buffer[MAX_PATH * 4]{};
            DWORD size = sizeof(buffer);
            if (RegGetValueW(hive, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, buffer, &size) == ERROR_SUCCESS)
                monitor.browsers.emplace_back(buffer);
        }
    }
    for (auto name : {L"ProgramW6432", L"ProgramFiles", L"ProgramFiles(x86)", L"LOCALAPPDATA"}) {
        auto base = Environment(name);
        if (base.empty()) continue;
        for (auto relative : {L"Google/Chrome/Application/chrome.exe", L"Microsoft/Edge/Application/msedge.exe"})
            monitor.browsers.push_back((fs::path(base) / relative).make_preferred().wstring());
    }
}

DWORD BrowserPid(HWND window) {
    wchar_t cls[64]{};
    if (!window || !IsWindowVisible(window) || IsIconic(window) ||
        !GetClassNameW(window, cls, 64) || wcscmp(cls, L"Chrome_WidgetWin_1") != 0 ||
        GetWindow(window, GW_OWNER)) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
#ifdef YTQ_TEST_BUILD
    if (std::find(monitor.allowedPids.begin(), monitor.allowedPids.end(), pid) == monitor.allowedPids.end()) return 0;
#endif
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return 0;
    wchar_t path[32768];
    DWORD count = static_cast<DWORD>(std::size(path));
    const bool found = QueryFullProcessImageNameW(process, 0, path, &count) &&
        std::any_of(monitor.browsers.begin(), monitor.browsers.end(),
            [&](const auto& candidate) { return _wcsicmp(candidate.c_str(), path) == 0; });
    CloseHandle(process);
    return found ? pid : 0;
}

HWND ForegroundFrame() {
    return GetAncestor(GetForegroundWindow(), GA_ROOT);
}

HWND PaintedRenderer(HWND frame) {
    RECT bounds{};
    if (!GetClientRect(frame, &bounds)) return nullptr;
    POINT point{bounds.right / 2, bounds.bottom / 2};
    if (!ClientToScreen(frame, &point)) return nullptr;
    const HWND hit = WindowFromPoint(point);
    wchar_t cls[64]{};
    return hit && GetAncestor(hit, GA_ROOT) == frame && GetClassNameW(hit, cls, 64) &&
        wcscmp(cls, L"Chrome_RenderWidgetHostHWND") == 0 ? hit : nullptr;
}

VideoWindow CurrentVideo() {
    if (!monitor.browserPid || ForegroundFrame() != monitor.foreground) return {};
    wchar_t buffer[1024]{};
    GetWindowTextW(monitor.foreground, buffer, static_cast<int>(std::size(buffer)));
    std::wstring title(buffer);
    std::transform(title.begin(), title.end(), title.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    // This title filter avoids opening the accessibility tree on unrelated tabs.
    // QualityMenu separately verifies the foreground tab's actual YouTube origin.
    if (title.find(L"youtube") == title.npos) return {};
    return {monitor.foreground, monitor.browserPid, PaintedRenderer(monitor.foreground), std::move(title)};
}

void StopTimer() {
    KillTimer(monitor.control, checkTimer);
    monitor.timerDue = 0;
}
void CheckIn(UINT delay) {
    monitor.timerDue = GetTickCount64() + delay;
    SetTimer(monitor.control, checkTimer, std::max<UINT>(delay, USER_TIMER_MINIMUM), nullptr);
}
void RequestRefresh(bool selectedTab = false) {
    monitor.tabChanged |= selectedTab;
    if (!monitor.refreshPosted) monitor.refreshPosted = PostMessageW(monitor.control, refreshMessage, 0, 0) != FALSE;
}

void CALLBACK WindowEvent(HWINEVENTHOOK, DWORD event, HWND window, LONG object, LONG child, DWORD, DWORD) {
    if (event == EVENT_SYSTEM_FOREGROUND) {
        RequestRefresh();
    } else if (window == monitor.foreground && event == EVENT_OBJECT_NAMECHANGE &&
               object == OBJID_WINDOW && child == CHILDID_SELF) {
        RequestRefresh();
    } else if (window == monitor.foreground && monitor.current.window &&
               event == EVENT_OBJECT_SELECTION && object == OBJID_CLIENT) {
        // A tab can change without changing the window title (two tabs on the same video).
        // Restrict this lookup to a native selection event in the active browser's top-level window.
        ComPtr<IAccessible> item;
        VARIANT id{}, role{};
        if (SUCCEEDED(AccessibleObjectFromEvent(window, object, child, &item, &id)) && item &&
            SUCCEEDED(item->get_accRole(id, &role)) && role.vt == VT_I4 && role.lVal == ROLE_SYSTEM_PAGETAB)
            RequestRefresh(true);
        VariantClear(&role);
        VariantClear(&id);
    }
}

void ClearHook(HWINEVENTHOOK& hook) {
    if (hook) UnhookWinEvent(hook);
    hook = nullptr;
}

void Refresh() {
    monitor.refreshPosted = false;
    const bool tabChanged = monitor.tabChanged;
    monitor.tabChanged = false;
    const HWND active = ForegroundFrame();
    if (active != monitor.foreground) {
        ClearHook(monitor.titleHook);
        ClearHook(monitor.tabHook);
        monitor.foreground = active;
        monitor.browserPid = BrowserPid(active);
        if (monitor.browserPid) {
            const DWORD thread = GetWindowThreadProcessId(active, nullptr);
            constexpr DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
            monitor.titleHook = SetWinEventHook(EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE, nullptr,
                                                WindowEvent, monitor.browserPid, thread, flags);
            monitor.tabHook = SetWinEventHook(EVENT_OBJECT_SELECTION, EVENT_OBJECT_SELECTION, nullptr,
                                               WindowEvent, monitor.browserPid, thread, flags);
        }
    }
    auto next = CurrentVideo();
    if (next == monitor.current && !tabChanged) return;
    monitor.current = std::move(next);
    ++monitor.generation;
    monitor.retries = 0;
    monitor.confirmedUnsupported = false;
    StopTimer();
    if (!monitor.current.window) return;
    const auto now = GetTickCount64();
    // Returning to a recently verified renderer does not need another menu visit.
    // If Chromium supplies no renderer handle, the title alone is a weaker match.
    const UINT revisitDelay = monitor.current.renderer ? checkInterval : 10000;
    if (!tabChanged) {
        for (const auto& last : monitor.recent) {
            if (last.target == monitor.current && last.time + revisitDelay > now) {
                CheckIn(static_cast<UINT>(last.time + revisitDelay - now));
                return;
            }
        }
    }
    CheckIn(850);
}

void BeginCheck() {
    if (!monitor.timerDue) return;
    const auto now = GetTickCount64();
    if (now < monitor.timerDue) { CheckIn(static_cast<UINT>(monitor.timerDue - now)); return; }
    StopTimer();
    if (!monitor.current.window || monitor.working) return;
    if (!(CurrentVideo() == monitor.current)) { RequestRefresh(); return; }
    LASTINPUTINFO input{sizeof(input), 0};
    if (GetLastInputInfo(&input) && GetTickCount() - input.dwTime < 900) {
        CheckIn(1500);
        return;
    }
    const HWND target = monitor.current.window, control = monitor.control;
    const UINT_PTR generation = monitor.generation;
    monitor.inFlight = monitor.current;
    monitor.working = true;
    monitor.worker = std::thread([target, control, generation] {
        MenuResult result = MenuResult::NotReady;
        if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
            {
                ComPtr<IUIAutomation> automation;
                if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                               IID_PPV_ARGS(&automation)))) {
                    try { result = ApplyBestQuality(automation.Get(), target); }
                    catch (const std::exception&) { result = MenuResult::NotReady; }
                }
            }
            CoUninitialize();
        }
        PostMessageW(control, resultMessage, generation, static_cast<LPARAM>(result));
    });
}

void FinishCheck(UINT_PTR generation, MenuResult result) {
    if (!monitor.working) return;
    monitor.worker.join();
    monitor.working = false;
    const auto completed = std::move(monitor.inFlight);
    const bool success = result == MenuResult::Applied || result == MenuResult::AlreadySet;
    if (success) {
        Trace(result == MenuResult::Applied ? "Applied" : "AlreadySet");
        const auto now = GetTickCount64();
        auto& recent = monitor.recent;
        recent.erase(std::remove_if(recent.begin(), recent.end(), [&](const auto& entry) {
            return now - entry.time >= checkInterval || entry.target == completed;
        }), recent.end());
        if (recent.size() == 32) recent.erase(recent.begin());
        recent.push_back({completed, now});
    }
    if (generation != monitor.generation || !monitor.current.window ||
        !(CurrentVideo() == monitor.current)) {
        RequestRefresh();
        if (success && monitor.current.window == completed.window && monitor.current.title == completed.title) {
            // Chromium may replace its renderer or briefly focus a menu during our selection.
            // Give the player time to apply the completed request before checking the same video again.
            CheckIn(10000);
        } else if (monitor.current.window && !monitor.timerDue) CheckIn(850);
        return;
    }
    switch (result) {
    case MenuResult::Applied:
    case MenuResult::AlreadySet:
        monitor.retries = 0;
        CheckIn(checkInterval);
        break;
    case MenuResult::Unsupported:
        Trace("Unsupported");
        // A tab's new title can arrive just before its new document.
        if (!monitor.confirmedUnsupported) {
            monitor.confirmedUnsupported = true;
            CheckIn(2800);
        }
        break;
    case MenuResult::Busy:
    case MenuResult::NotReady: {
        Trace(result == MenuResult::Busy ? "Busy" : "NotReady");
        constexpr UINT retry[] = {2500, 5500, 10000, 20000, 30000};
        CheckIn(monitor.retries < std::size(retry) ? retry[monitor.retries++] : checkInterval);
        break;
    }
    }
}

LRESULT CALLBACK ControlProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (monitor.stopping && message == resultMessage) {
        if (monitor.worker.joinable()) monitor.worker.join();
        monitor.working = false;
        DestroyWindow(hwnd);
        return 0;
    }
    if (monitor.stopping && message == WM_CLOSE) return 0;
    if (monitor.stopping && message != WM_DESTROY) return DefWindowProcW(hwnd, message, wParam, lParam);
    if (monitor.taskbarCreated && message == monitor.taskbarCreated) {
        monitor.trayRetries = 0;
        if (!AddTrayIcon(hwnd)) SetTimer(hwnd, trayRetryTimer, 1500, nullptr);
        return 0;
    }
    switch (message) {
    case refreshMessage: Refresh(); return 0;
    case WM_TIMER:
        if (wParam == checkTimer) BeginCheck();
        else if (wParam == trayRetryTimer && (AddTrayIcon(hwnd) || ++monitor.trayRetries >= 4))
            KillTimer(hwnd, trayRetryTimer);
        return 0;
    case resultMessage: FinishCheck(wParam, static_cast<MenuResult>(lParam)); return 0;
    case kTrayNotification: if (IsTrayActivation(lParam)) ShowTrayMenu(hwnd); return 0;
    case showMenuMessage: ShowTrayMenu(hwnd); return 0;
    case WM_CLOSE:
        monitor.stopping = true;
        SetPropW(hwnd, L"YouTubeQuality.Closing", reinterpret_cast<HANDLE>(1));
        StopTimer();
        KillTimer(hwnd, trayRetryTimer);
        ClearHook(monitor.foregroundHook);
        ClearHook(monitor.titleHook);
        ClearHook(monitor.tabHook);
        // Keep this HWND valid until the in-flight UIA call posts its result.
        if (!monitor.working) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        RemovePropW(hwnd, L"YouTubeQuality.Closing");
        RemoveTrayIcon(hwnd);
        PostQuitMessage(0);
        return 0;
    default: return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

int Run(HINSTANCE instance) {
#ifdef YTQ_TEST_BUILD
    if (!SetTestScope()) return 2;
#endif
    FindBrowsers();
    WNDCLASSW wc{};
    wc.lpfnWndProc = ControlProc;
    wc.hInstance = instance;
    wc.lpszClassName = controlClass;
    if (!RegisterClassW(&wc)) return 1;
    monitor.control = CreateWindowExW(WS_EX_TOOLWINDOW, controlClass, L"YouTube Quality", WS_POPUP,
                                      0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!monitor.control) return 1;
    monitor.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    monitor.foregroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                                             WindowEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
    if (!monitor.foregroundHook || !AddTrayIcon(monitor.control)) {
        ClearHook(monitor.foregroundHook);
        DestroyWindow(monitor.control);
        MessageBoxW(nullptr, L"Windows could not start the tray monitor. Please open YouTube Quality again.",
                    L"YouTube Quality", MB_OK | MB_ICONERROR);
        return 1;
    }
    Trace("TrayReady");
    Refresh();
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    ClearHook(monitor.foregroundHook);
    ClearHook(monitor.titleHook);
    ClearHook(monitor.tabHook);
    if (monitor.worker.joinable()) monitor.worker.join();
    Trace("Closed");
    return 0;
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const bool quit = _wcsicmp(commandLine, L"--exit") == 0;
    const bool startup = _wcsicmp(commandLine, L"--startup") == 0;
    HANDLE singleton = CreateMutexW(nullptr, FALSE, instanceName);
    if (!singleton) return 1;
    DWORD lock = WaitForSingleObject(singleton, 0);
    // A previous process may be creating its tray window or finishing a quality check on exit.
    for (unsigned attempt = 0; lock == WAIT_TIMEOUT && attempt < 80; ++attempt) {
        HWND control = FindWindowW(controlClass, nullptr);
        if (control && (quit || !GetPropW(control, L"YouTubeQuality.Closing"))) {
            if (!startup) PostMessageW(control, quit ? WM_CLOSE : showMenuMessage, 0, 0);
            CloseHandle(singleton);
            return 0;
        }
        lock = WaitForSingleObject(singleton, 50);
    }
    if (lock != WAIT_OBJECT_0 && lock != WAIT_ABANDONED) { CloseHandle(singleton); return 1; }
    int result = 0;
    if (!quit) {
        if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
            try { result = Run(instance); } catch (const std::exception&) { result = 1; }
            CoUninitialize();
        } else result = 1;
    }
    ReleaseMutex(singleton);
    CloseHandle(singleton);
    return result;
}
