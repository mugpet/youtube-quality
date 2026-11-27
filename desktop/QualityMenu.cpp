#include "QualityMenu.h"
#include "QualityFormat.h"

#include <oleacc.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <cwctype>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
using quality::ManualSummaryDisagrees;
using quality::ParseReportedFormat;
using quality::PlayingAtBest;
using quality::ReportedFormat;
using Element = ComPtr<IUIAutomationElement>;
using Condition = ComPtr<IUIAutomationCondition>;

#ifdef YTQ_TEST_BUILD
void MenuTrace(const char* stage) {
    wchar_t path[MAX_PATH]{};
    DWORD size = GetEnvironmentVariableW(L"YTQ_TRACE_FILE", path, MAX_PATH);
    if (!size || size >= MAX_PATH) return;
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    std::string message = std::to_string(GetTickCount64()) + " menu " + stage + "\r\n";
    DWORD written;
    WriteFile(file, message.data(), static_cast<DWORD>(message.size()), &written, nullptr);
    CloseHandle(file);
}
#else
void MenuTrace(const char*) {}
#endif

std::wstring Text(IUIAutomationElement* item, PROPERTYID property) {
    VARIANT value{};
    std::wstring result;
    if (item && SUCCEEDED(item->GetCurrentPropertyValue(property, &value)) &&
        value.vt == VT_BSTR && value.bstrVal)
        result.assign(value.bstrVal, SysStringLen(value.bstrVal));
    VariantClear(&value);
    return result;
}

std::wstring Lower(std::wstring value) {
    for (auto& c : value) c = static_cast<wchar_t>(towlower(c));
    return value;
}

bool Contains(const std::wstring& value, const wchar_t* word) {
    return value.find(word) != std::wstring::npos;
}

Condition StringCondition(IUIAutomation* uia, PROPERTYID id, const wchar_t* text,
                          PropertyConditionFlags flags = PropertyConditionFlags_None) {
    VARIANT value{};
    value.vt = VT_BSTR;
    value.bstrVal = SysAllocString(text);
    Condition condition;
    if (value.bstrVal) uia->CreatePropertyConditionEx(id, value, flags, &condition);
    VariantClear(&value);
    return condition;
}

Condition TypeCondition(IUIAutomation* uia, CONTROLTYPEID id) {
    VARIANT value{};
    value.vt = VT_I4;
    value.lVal = id;
    Condition condition;
    uia->CreatePropertyCondition(UIA_ControlTypePropertyId, value, &condition);
    return condition;
}

Element First(IUIAutomationElement* parent, IUIAutomationCondition* condition) {
    Element item;
    if (parent && condition) parent->FindFirst(TreeScope_Descendants, condition, &item);
    return item;
}

// Edge may keep background tab renderer HWNDs visible and stacked at the same
// coordinates. A read-only point query identifies the content actually painted
// in this foreground window. Walk outward if the point falls in an iframe.
Element DocumentAtPoint(IUIAutomation* uia, IUIAutomationTreeWalker* documents, HWND window) {
    RECT bounds{};
    if (!GetClientRect(window, &bounds)) return {};
    POINT point{(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2};
    if (!ClientToScreen(window, &point) || GetAncestor(GetForegroundWindow(), GA_ROOT) != window ||
        GetAncestor(WindowFromPoint(point), GA_ROOT) != window) return {};
    Element hit, doc;
    if (FAILED(uia->ElementFromPoint(point, &hit)) || !hit ||
        FAILED(documents->NormalizeElement(hit.Get(), &doc)) || !doc) return {};
    for (unsigned depth = 0; depth < 32; ++depth) {
        Element outer;
        if (FAILED(documents->GetParentElement(doc.Get(), &outer))) return {};
        if (!outer) return Text(doc.Get(), UIA_AutomationIdPropertyId) == L"RootWebArea" ? doc : Element{};
        doc = outer;
    }
    return {};
}

Element BrowserAddressBar(IUIAutomation* uia, HWND window) {
    Element root;
    if (FAILED(uia->ElementFromHandle(window, &root)) || !root) return {};
    auto address = First(root.Get(), StringCondition(uia, UIA_ClassNamePropertyId, L"OmniboxViewViews").Get());
    // Only a native browser control can confirm the active tab. A webpage can
    // give its own text field exactly the same CSS class or accessible name.
    ComPtr<IUIAutomationTreeWalker> raw;
    if (!address || FAILED(uia->get_RawViewWalker(&raw)) || !raw) return {};
    for (Element ancestor = address; ancestor;) {
        CONTROLTYPEID type{};
        BOOL isRoot = FALSE;
        if (FAILED(ancestor->get_CurrentControlType(&type)) || type == UIA_DocumentControlTypeId) break;
        if (SUCCEEDED(uia->CompareElements(root.Get(), ancestor.Get(), &isRoot)) && isRoot) return address;
        Element parent;
        if (FAILED(raw->GetParentElement(ancestor.Get(), &parent))) break;
        ancestor = parent;
    }
    return {};
}

// Chrome can elide the scheme and www in its address bar. Only the full
// document URL, provided by the browser, is allowed to establish HTTPS.
std::wstring YoutubePath(const std::wstring& url, bool addressBar) {
    std::size_t start = 0;
    if (url.compare(0, 8, L"https://") == 0) start = 8;
    else if (!addressBar) return {};
    if (url.compare(start, 4, L"www.") == 0) start += 4;
    constexpr wchar_t host[] = L"youtube.com/";
    if (url.compare(start, _countof(host) - 1, host) != 0) return {};
    return url.substr(start + _countof(host) - 1);
}

bool FullscreenOrYoutubeApp(HWND window) {
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    RECT frame{};
    if (GetWindowRect(window, &frame) &&
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor) &&
        EqualRect(&frame, &monitor.rcMonitor)) return true;

    ComPtr<IPropertyStore> store;
    if (FAILED(SHGetPropertyStoreForWindow(window, IID_PPV_ARGS(&store)))) return false;
    PROPVARIANT app{};
    if (FAILED(store->GetValue(PKEY_AppUserModel_ID, &app))) return false;
    std::wstring id = app.vt == VT_LPWSTR && app.pwszVal ? Lower(app.pwszVal) : L"";
    PropVariantClear(&app);
    // Installed YouTube PWA, or Chromium's URL-app identity (which can be
    // shortened when the profile name makes the AppUserModelID too long).
    return Contains(id, L"._crx_agimnkijcaahngcdmfeangaknmldooml") ||
           Contains(id, L".www.youtube.com_") || Contains(id, L".www.youtubcom_");
}
bool SameElement(IUIAutomation* uia, IUIAutomationElement* a, IUIAutomationElement* b) {
    BOOL same = FALSE;
    return a && b && SUCCEEDED(uia->CompareElements(a, b, &same)) && same;
}

bool BelongsToDocument(IUIAutomation* uia, IUIAutomationTreeWalker* documents,
                       IUIAutomationElement* item, IUIAutomationElement* doc) {
    Element nearest;
    return item && SUCCEEDED(documents->NormalizeElement(item, &nearest)) &&
           SameElement(uia, nearest.Get(), doc);
}

bool EditingText(IUIAutomation* uia) {
    Element focus;
    if (FAILED(uia->GetFocusedElement(&focus)) || !focus) return false;
    CONTROLTYPEID type{};
    focus->get_CurrentControlType(&type);
    if (type == UIA_EditControlTypeId || type == UIA_ComboBoxControlTypeId) return true;
    const auto role = Text(focus.Get(), UIA_AriaRolePropertyId);
    return role == L"textbox" || role == L"searchbox" || role == L"combobox";
}

std::vector<Element> All(IUIAutomationElement* parent, IUIAutomationCondition* condition) {
    ComPtr<IUIAutomationElementArray> matches;
    std::vector<Element> result;
    if (!parent || !condition || FAILED(parent->FindAll(TreeScope_Descendants, condition, &matches)) || !matches)
        return result;
    int count = 0;
    matches->get_Length(&count);
    for (int i = 0; i < count && i < 50; ++i) {
        Element item;
        if (SUCCEEDED(matches->GetElement(i, &item)) && item) result.push_back(item);
    }
    return result;
}

std::wstring Url(IUIAutomationElement* doc) {
    auto value = Text(doc, UIA_ValueValuePropertyId);
    if (value.empty()) value = Text(doc, UIA_LegacyIAccessibleValuePropertyId);
    return value;
}

bool PlaybackPage(const std::wstring& url) {
    const auto path = YoutubePath(url, false);
    return path.compare(0, 6, L"watch?") == 0 || path.compare(0, 5, L"live/") == 0 ||
           path.compare(0, 7, L"shorts/") == 0;
}

bool Enabled(IUIAutomationElement* item) {
    BOOL enabled = FALSE;
    return item && SUCCEEDED(item->get_CurrentIsEnabled(&enabled)) && enabled;
}

bool Visible(IUIAutomationElement* item) {
    BOOL offscreen = TRUE;
    return item && SUCCEEDED(item->get_CurrentIsOffscreen(&offscreen)) && !offscreen;
}

bool Invoke(IUIAutomationElement* item) {
    if (!Enabled(item)) return false;
    ComPtr<IUIAutomationInvokePattern> action;
    if (SUCCEEDED(item->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&action))) && action &&
        SUCCEEDED(action->Invoke())) return true;
    ComPtr<IUIAutomationLegacyIAccessiblePattern> legacy;
    if (SUCCEEDED(item->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId, IID_PPV_ARGS(&legacy))) && legacy &&
        SUCCEEDED(legacy->DoDefaultAction())) return true;
    ComPtr<IUIAutomationSelectionItemPattern> selection;
    return SUCCEEDED(item->GetCurrentPatternAs(UIA_SelectionItemPatternId, IID_PPV_ARGS(&selection))) &&
           selection && SUCCEEDED(selection->Select());
}

bool SetMenuOpen(IUIAutomationElement* item, bool open) {
    if (!Enabled(item)) return false;
    // Chrome exposes its player gear and Quality submenu through this pattern.
    // Legacy.DoDefaultAction can return S_OK without opening the gear.
    ComPtr<IUIAutomationExpandCollapsePattern> menu;
    if (SUCCEEDED(item->GetCurrentPatternAs(UIA_ExpandCollapsePatternId, IID_PPV_ARGS(&menu))) && menu &&
        SUCCEEDED(open ? menu->Expand() : menu->Collapse())) return true;
    return Invoke(item);
}

enum class OpenState { Unknown, Closed, Open };
OpenState MenuState(IUIAutomationElement* settings) {
    const auto aria = Text(settings, UIA_AriaPropertiesPropertyId);
    if (Contains(aria, L"expanded=true")) return OpenState::Open;
    if (Contains(aria, L"expanded=false")) return OpenState::Closed;
    ComPtr<IUIAutomationExpandCollapsePattern> pattern;
    ExpandCollapseState state{};
    if (SUCCEEDED(settings->GetCurrentPatternAs(UIA_ExpandCollapsePatternId, IID_PPV_ARGS(&pattern))) &&
        pattern && SUCCEEDED(pattern->get_CurrentExpandCollapseState(&state))) {
        if (state == ExpandCollapseState_Expanded) return OpenState::Open;
        if (state == ExpandCollapseState_Collapsed) return OpenState::Closed;
    }
    return OpenState::Unknown;
}

bool PlayingAd(IUIAutomationElement* player) {
    const auto classes = Text(player, UIA_ClassNamePropertyId);
    return Contains(classes, L"ad-showing") || Contains(classes, L"ad-interrupting");
}

enum class RadioState { Unknown, Off, On };

RadioState SelectedState(IUIAutomationElement* item) {
    const auto aria = L";" + Text(item, UIA_AriaPropertiesPropertyId) + L";";
    if (Contains(aria, L";checked=true;")) return RadioState::On;
    if (Contains(aria, L";checked=false;")) return RadioState::Off;
    if (Contains(aria, L";checked=mixed;")) return RadioState::Unknown;
    // No-default-value queries preserve "unsupported" instead of turning a
    // missing provider property into an otherwise valid false or zero.
    VARIANT value{};
    item->GetCurrentPropertyValueEx(UIA_SelectionItemIsSelectedPropertyId, TRUE, &value);
    if (value.vt == VT_BOOL) return value.boolVal ? RadioState::On : RadioState::Off;
    VariantClear(&value);
    item->GetCurrentPropertyValueEx(UIA_ToggleToggleStatePropertyId, TRUE, &value);
    if (value.vt == VT_I4) {
        if (value.lVal == ToggleState_On) return RadioState::On;
        if (value.lVal == ToggleState_Off) return RadioState::Off;
    }
    VariantClear(&value);
    item->GetCurrentPropertyValueEx(UIA_LegacyIAccessibleStatePropertyId, TRUE, &value);
    RadioState state = RadioState::Unknown;
    if (value.vt == VT_I4 || value.vt == VT_UI4) {
        const auto flags = value.vt == VT_UI4 ? value.ulVal : static_cast<ULONG>(value.lVal);
        if (flags & STATE_SYSTEM_CHECKED) state = RadioState::On;
        else if (!(flags & STATE_SYSTEM_MIXED)) state = RadioState::Off;
    }
    VariantClear(&value);
    return state;
}

bool PremiumBrand(IUIAutomation* uia, IUIAutomationTreeWalker* documents, IUIAutomationElement* doc) {
    // A Premium row can also be an advertisement in the free player. Only
    // treat it as an entitlement when the site's home link confirms Premium.
    auto logos = All(doc, StringCondition(uia, UIA_AutomationIdPropertyId, L"logo").Get());
    for (const auto& logo : logos) {
        if (!BelongsToDocument(uia, documents, logo.Get(), doc)) continue;
        const auto brand = Lower(Text(logo.Get(), UIA_NamePropertyId) + L" " +
                                 Text(logo.Get(), UIA_HelpTextPropertyId));
        if (Contains(brand, L"youtube premium") || Contains(brand, L"premium youtube")) return true;
    }
    return false;
}

struct Option { Element item; int height = 0; bool premium = false; int fps = 0; bool checked = false; };
struct Choices { Option best; Element selected; bool ambiguous = false; };

std::wstring FormatKey(int height, int fps, bool premium, bool standard) {
    return std::to_wstring(height) + L"/" + std::to_wstring(fps) +
           (premium ? L"/premium" : standard ? L"/standard" : L"/unknown");
}

std::vector<int> DocumentId(IUIAutomationElement* doc) {
    SAFEARRAY* array = nullptr;
    if (FAILED(doc->GetRuntimeId(&array)) || !array) return {};
    std::vector<int> id;
    LONG low = 0, high = -1;
    int* values = nullptr;
    if (SafeArrayGetDim(array) == 1 && SUCCEEDED(SafeArrayGetLBound(array, 1, &low)) &&
        SUCCEEDED(SafeArrayGetUBound(array, 1, &high)) && high >= low && high - low < 64 &&
        SUCCEEDED(SafeArrayAccessData(array, reinterpret_cast<void**>(&values)))) {
        if (values) id.assign(values, values + (high - low + 1));
        SafeArrayUnaccessData(array);
    }
    SafeArrayDestroy(array);
    return id;
}

Choices ReadChoices(IUIAutomation* uia, IUIAutomationTreeWalker* documents, IUIAutomationElement* doc,
                    const std::vector<Element>& rows) {
    Choices choices;
    auto& best = choices.best;
    bool lookedUpBrand = false, premiumAccount = false;
    for (const auto& row : rows) {
        // Include Auto when reading the current setting, even though it is
        // never a candidate for the explicit resolution preference.
        const bool checked = SelectedState(row.Get()) == RadioState::On;
        if (checked) {
            if (choices.selected) choices.ambiguous = true;
            choices.selected = row;
        }
        if (!Enabled(row.Get())) continue;
        const auto label = Lower(Text(row.Get(), UIA_NamePropertyId));
        // Auto can describe what is playing, but has no fixed resolution
        // and cannot serve as a candidate when a change is necessary.
        if (label.empty() || label.front() < L'0' || label.front() > L'9') continue;
        wchar_t* end = nullptr;
        long height = wcstol(label.c_str(), &end, 10);
        if (height < 1 || height > 1440 || *end != L'p') continue;
        const int fps = ParseReportedFormat(label).fps;
        const auto details = label + L" " + Lower(Text(row.Get(), UIA_HelpTextPropertyId)) +
                             L" " + Lower(Text(row.Get(), UIA_FullDescriptionPropertyId));
        if (Contains(details, L"upgrade") || Contains(details, L"get premium") ||
            Contains(details, L"opgrader") || Contains(details, L"abonner") ||
            Contains(details, L"subscribe") || Contains(details, L"lock") ||
            Contains(details, L"unavailable")) continue;
        const bool premium = Contains(details, L"premium") || Contains(details, L"bitrate");
        if (premium && !checked) {
            if (!lookedUpBrand) { premiumAccount = PremiumBrand(uia, documents, doc); lookedUpBrand = true; }
            if (!premiumAccount) continue;
        }
        if (!best.item || std::tie(height, premium, fps, checked) >
                          std::tie(best.height, best.premium, best.fps, best.checked))
            best = {row, static_cast<int>(height), premium, fps, checked};
    }
    return choices;
}
}  // namespace

MenuResult ApplyBestQuality(IUIAutomation* uia, HWND window, AutoQualityObservation& previousAuto) {
    const AutoQualityObservation prior = std::move(previousAuto);
    previousAuto = {};
    if (!uia || !IsWindow(window)) return MenuResult::NotReady;
    if (GetAncestor(GetForegroundWindow(), GA_ROOT) != window || IsIconic(window)) return MenuResult::Busy;
    ComPtr<IUIAutomationTreeWalker> documents;
    auto docType = TypeCondition(uia, UIA_DocumentControlTypeId);
    if (!docType || FAILED(uia->CreateTreeWalker(docType.Get(), &documents)) || !documents)
        return MenuResult::NotReady;
    auto address = BrowserAddressBar(uia, window);
    const auto addressValue = Url(address.Get());
    const bool withoutAddress = addressValue.empty() && FullscreenOrYoutubeApp(window);
    if (addressValue.empty() && !withoutAddress) { MenuTrace("address bar unavailable"); return MenuResult::NotReady; }
    const auto activePath = YoutubePath(addressValue, true);
    if (!withoutAddress && activePath.empty()) return MenuResult::Unsupported;
    if (EditingText(uia)) { MenuTrace("editing text"); return MenuResult::Busy; }
    auto doc = DocumentAtPoint(uia, documents.Get(), window);
    if (!doc) {
        Sleep(100);  // Chromium's on-demand accessibility tree may still be starting.
        doc = DocumentAtPoint(uia, documents.Get(), window);
    }
    if (!doc) { MenuTrace("document pending"); return MenuResult::NotReady; }
    const auto page = Url(doc.Get());
    if (page.empty()) { MenuTrace("URL pending"); return MenuResult::NotReady; }
    if (!PlaybackPage(page)) return MenuResult::Unsupported;
    if (!withoutAddress && YoutubePath(page, false) != activePath) {
        MenuTrace("active tab pending"); return MenuResult::NotReady;
    }
    auto ownElement = [&](IUIAutomationElement* item) {
        return BelongsToDocument(uia, documents.Get(), item, doc.Get());
    };
    const bool shorts = Contains(page.substr(0, page.find(L'?')), L"/shorts/");
    auto player = First(doc.Get(), StringCondition(uia, UIA_AutomationIdPropertyId,
                                                  shorts ? L"shorts-player" : L"movie_player").Get());
    if (!ownElement(player.Get()) || PlayingAd(player.Get())) { MenuTrace("player unavailable"); return MenuResult::NotReady; }
    const auto dialog = StringCondition(uia, UIA_AriaRolePropertyId, L"dialog");
    auto modalVisible = [&] {
        for (const auto& item : All(doc.Get(), dialog.Get())) {
            BOOL hidden = TRUE;
            if (SUCCEEDED(item->get_CurrentIsOffscreen(&hidden)) && !hidden) return true;
        }
        return false;
    };
    if (modalVisible()) { MenuTrace("dialog present"); return MenuResult::Busy; }
    auto settings = First(player.Get(), StringCondition(uia, UIA_ClassNamePropertyId,
                          L"ytp-settings-button", PropertyConditionFlags_MatchSubstring).Get());
    if (!settings) {
        for (const auto& button : All(player.Get(), TypeCondition(uia, UIA_ButtonControlTypeId).Get())) {
            const auto name = Lower(Text(button.Get(), UIA_NamePropertyId));
            if ((name == L"settings" || name == L"indstillinger") && MenuState(button.Get()) != OpenState::Unknown) {
                settings = button;
                break;
            }
        }
    }
    if (!ownElement(settings.Get())) return shorts ? MenuResult::Unsupported : MenuResult::NotReady;
    auto state = MenuState(settings.Get());
    if (state == OpenState::Open) { MenuTrace("gear already open"); return MenuResult::Busy; }
    if (state == OpenState::Unknown || !Enabled(settings.Get())) {
        MenuTrace("gear state unavailable"); return MenuResult::NotReady;
    }
    auto samePlayer = [&] {
        const bool sameAddress = withoutAddress ? FullscreenOrYoutubeApp(window) :
                                                 YoutubePath(Url(address.Get()), true) == activePath;
        if (GetAncestor(GetForegroundWindow(), GA_ROOT) != window || !sameAddress ||
            !SameElement(uia, doc.Get(), DocumentAtPoint(uia, documents.Get(), window).Get()) || Url(doc.Get()) != page) {
            MenuTrace("tab or page changed"); return false;
        }
        if (EditingText(uia)) { MenuTrace("editing text"); return false; }
        if (PlayingAd(player.Get())) { MenuTrace("ad started"); return false; }
        if (modalVisible()) { MenuTrace("dialog appeared"); return false; }
        return true;
    };
    LASTINPUTINFO lastInput{sizeof(LASTINPUTINFO), 0};
    GetLastInputInfo(&lastInput);
    POINT initialCursor{};
    GetCursorPos(&initialCursor);
    auto stillOurs = [&] {
        if (GetAncestor(GetForegroundWindow(), GA_ROOT) != window || IsIconic(window)) { MenuTrace("window deactivated"); return false; }
        LASTINPUTINFO current{sizeof(LASTINPUTINFO), 0};
        if (!GetLastInputInfo(&current)) return false;
        if (current.dwTime == lastInput.dwTime) return true;
        // Cursor motion alone must not strand a panel opened by the helper.
        POINT point{};
        if (GetCursorPos(&point) && (point.x != initialCursor.x || point.y != initialCursor.y) &&
            !(GetAsyncKeyState(VK_LBUTTON) & 0x8000) && !(GetAsyncKeyState(VK_RBUTTON) & 0x8000))
            return true;
        MenuTrace("new input"); return false;
    };
    if (!samePlayer() || !stillOurs()) return MenuResult::Busy;
    if (!SetMenuOpen(settings.Get(), true)) { MenuTrace("gear expand failed"); return MenuResult::NotReady; }
    MenuTrace("gear expand sent");

    const auto menuItems = StringCondition(uia, UIA_AriaRolePropertyId, L"menuitem");
    const auto radioItems = StringCondition(uia, UIA_AriaRolePropertyId, L"menuitemradio");
    auto menuRole = StringCondition(uia, UIA_AriaRolePropertyId, L"menu");
    auto menuClass = StringCondition(uia, UIA_ClassNamePropertyId, L"ytp-panel-menu", PropertyConditionFlags_MatchSubstring);
    Condition menuPanel;
    ComPtr<IUIAutomationTreeWalker> panels;
    if (menuRole && menuClass && SUCCEEDED(uia->CreateOrCondition(menuRole.Get(), menuClass.Get(), &menuPanel)))
        uia->CreateTreeWalker(menuPanel.Get(), &panels);
    auto run = [&]() -> MenuResult {
        Element quality;
        std::wstring initialSummary;
        const auto mainDeadline = GetTickCount64() + 1500;
        do {
            if (MenuState(settings.Get()) == OpenState::Open) {
                for (const auto& row : All(player.Get(), menuItems.Get())) {
                    if (!Visible(row.Get())) continue;
                    const auto name = Lower(Text(row.Get(), UIA_NamePropertyId));
                    if (name.rfind(L"quality", 0) == 0 || name.rfind(L"kvalitet", 0) == 0) {
                        initialSummary = name;
                        quality = row;
                        break;
                    }
                }
            }
            if (!quality) Sleep(50);
        } while (!quality && GetTickCount64() < mainDeadline);
        if (!ownElement(quality.Get())) { MenuTrace("visible quality submenu missing"); return MenuResult::NotReady; }
        if (!samePlayer() || !stillOurs() || MenuState(settings.Get()) != OpenState::Open) return MenuResult::Busy;
        if (!SetMenuOpen(quality.Get(), true)) { MenuTrace("submenu expand failed"); return MenuResult::NotReady; }
        MenuTrace("submenu expand sent");
        Choices choices;
        const auto offersDeadline = GetTickCount64() + 1500;
        do {
            // During the panel animation some rows are clipped. The visible
            // radio identifies the active panel; its full list determines
            // the best format, including higher tiers above the scroll area.
            Element available;
            for (const auto& row : All(player.Get(), radioItems.Get())) {
                if (!Visible(row.Get())) continue;
                const auto name = Lower(Text(row.Get(), UIA_NamePropertyId));
                if (name.empty() || name.front() < L'0' || name.front() > L'9' || !Contains(name, L"p")) continue;
                if (panels && SUCCEEDED(panels->NormalizeElement(row.Get(), &available)) && available) break;
            }
            if (available && ownElement(available.Get()))
                choices = ReadChoices(uia, documents.Get(), doc.Get(), All(available.Get(), radioItems.Get()));
            if (choices.best.item && choices.selected && !choices.ambiguous) break;
            Sleep(50);
        } while (GetTickCount64() < offersDeadline);
        const auto& best = choices.best;
        if (!ownElement(best.item.Get())) { MenuTrace("visible quality choices missing"); return MenuResult::NotReady; }
        if (choices.ambiguous || !ownElement(choices.selected.Get())) {
            MenuTrace("current quality unknown"); return MenuResult::NotReady;
        }
#ifdef YTQ_TEST_BUILD
        MenuTrace(("target " + std::to_string(best.height) + "p " + std::to_string(best.fps) + "fps" +
                   (best.premium ? " premium" : " standard") + (best.checked ? " checked" : " unchecked")).c_str());
#endif
        if (best.checked) return MenuResult::AlreadySet;
        const ReportedFormat bestFormat{best.height, best.fps, best.premium, !best.premium};
        const bool autoSelected = Lower(Text(choices.selected.Get(), UIA_NamePropertyId)).rfind(L"auto", 0) == 0;
        auto assessPlayback = [&]() -> std::optional<MenuResult> {
            const auto radio = Lower(Text(choices.selected.Get(), UIA_NamePropertyId));
            const auto liveSummary = Lower(Text(quality.Get(), UIA_NamePropertyId));
            // Chromium can discard the parent menu item when it opens the
            // submenu. Its immediately preceding visible label is then our
            // observation. The checked Auto radio sometimes gives more detail.
            const auto& summary = liveSummary.empty() ? initialSummary : liveSummary;
            const auto fromMenu = ParseReportedFormat(summary);
            const auto fromRadio = ParseReportedFormat(radio);
            if (!autoSelected) {
                // A mismatch between the two menus can occur during a switch.
                if (Contains(summary, L"auto") || ManualSummaryDisagrees(fromMenu, fromRadio)) {
                    MenuTrace("manual quality changing"); return MenuResult::NotReady;
                }
                return std::nullopt;
            }
            if (radio.rfind(L"auto", 0) != 0) return MenuResult::NotReady;
            if (fromMenu.height && !Contains(summary, L"auto")) return MenuResult::NotReady;
            if ((fromMenu.height && PlayingAtBest(fromMenu, bestFormat)) ||
                (fromRadio.height && PlayingAtBest(fromRadio, bestFormat))) {
                MenuTrace("automatic quality already best"); return MenuResult::AlreadySet;
            }
            if (fromMenu.height && fromRadio.height && fromMenu.height != fromRadio.height)
                return MenuResult::NotReady;
            const auto& current = fromRadio.height ? fromRadio : fromMenu;
            if (!current.height) { MenuTrace("automatic quality unknown"); return MenuResult::NotReady; }
            auto id = DocumentId(doc.Get());
            if (id.empty()) return MenuResult::NotReady;
            const auto reported = FormatKey(current.height, current.fps, current.premium, current.standard);
            const auto target = FormatKey(best.height, best.fps, best.premium, !best.premium);
            const auto now = GetTickCount64();
            const bool matches = prior.firstSeen && now >= prior.firstSeen && now - prior.firstSeen <= 30000 &&
                prior.documentId == id && prior.pageUrl == page && prior.reportedQuality == reported &&
                prior.targetQuality == target;
            if (matches && now - prior.firstSeen >= autoSettleDelayMs) return std::nullopt;
            previousAuto = {std::move(id), page, reported, target, matches ? prior.firstSeen : now};
            MenuTrace("automatic quality settling"); return MenuResult::AutoSettling;
        };
        if (const auto decision = assessPlayback()) return *decision;
        if (MenuState(settings.Get()) != OpenState::Open || !samePlayer() || !stillOurs()) return MenuResult::Busy;
        if (!Visible(best.item.Get())) {
            ComPtr<IUIAutomationScrollItemPattern> scrolling;
            if (FAILED(best.item->GetCurrentPatternAs(UIA_ScrollItemPatternId, IID_PPV_ARGS(&scrolling))) ||
                !scrolling || FAILED(scrolling->ScrollIntoView())) return MenuResult::NotReady;
            const auto deadline = GetTickCount64() + 400;
            while (!Visible(best.item.Get()) && GetTickCount64() < deadline) Sleep(40);
            if (!Visible(best.item.Get()) || !samePlayer() || !stillOurs()) return MenuResult::NotReady;
        }
        // UIA menu updates are asynchronous. Only select if the target is
        // still different and the previous radio is still positively checked.
        if (SelectedState(best.item.Get()) == RadioState::On) return MenuResult::AlreadySet;
        if (SelectedState(choices.selected.Get()) != RadioState::On) {
            MenuTrace("current quality changed"); return MenuResult::NotReady;
        }
        if (const auto decision = assessPlayback()) return *decision;
        if (!Invoke(best.item.Get())) { MenuTrace("quality select failed"); return MenuResult::NotReady; }
        MenuTrace("quality selected"); return MenuResult::Applied;
    };
    const auto result = run();
    // Selecting a quality normally closes the panel. Only toggle a panel we
    // opened if it is still open on this same video after the UI update.
    if (result == MenuResult::Applied) {
        const auto deadline = GetTickCount64() + 350;
        while (MenuState(settings.Get()) == OpenState::Open && GetTickCount64() < deadline) Sleep(40);
    }
    if (samePlayer() && stillOurs() && MenuState(settings.Get()) == OpenState::Open) {
        MenuTrace("closing owned menu");
        SetMenuOpen(settings.Get(), false);
        const auto deadline = GetTickCount64() + 350;
        while (MenuState(settings.Get()) == OpenState::Open && GetTickCount64() < deadline) Sleep(40);
        if (MenuState(settings.Get()) == OpenState::Open) MenuTrace("menu still open after collapse");
    }
    return result;
}
