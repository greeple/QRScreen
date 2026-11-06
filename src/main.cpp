#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <shellapi.h>
#include <windowsx.h>
#include <wingdi.h>

#include <ReadBarcode.h>
#include <BarcodeFormat.h>
#include <ImageView.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kMainClass[] = L"QRScreen.MainWindow";
constexpr wchar_t kSelectorClass[] = L"QRScreen.RegionSelector";
constexpr wchar_t kSettingsClass[] = L"QRScreen.SettingsWindow";
constexpr wchar_t kHotkeyBoxClass[] = L"QRScreen.HotkeyBox";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kCaptureMessage = WM_APP + 2;
constexpr UINT kDecodeMessage = WM_APP + 3;
constexpr UINT kCancelMessage = WM_APP + 4;
constexpr UINT kHotkeyRecordedMessage = WM_APP + 5;
constexpr UINT kTrayIconId = 1;
constexpr UINT kMenuCapture = 1001;
constexpr UINT kMenuExit = 1002;
constexpr UINT kMenuSettings = 1003;
constexpr UINT kButtonRecognize = 2001;
constexpr UINT kSettingsHotkeyBox = 2101;
constexpr UINT kSettingsSave = 2102;
constexpr UINT kSettingsCancel = 2103;
constexpr UINT kSettingsDefault = 2104;
constexpr UINT kHotkeyIdA = 3001;
constexpr UINT kHotkeyIdB = 3002;

HINSTANCE gInstance = nullptr;
HWND gMainWindow = nullptr;
HWND gSelector = nullptr;
HWND gRecognizeButton = nullptr;
HWND gSettingsWindow = nullptr;
HWND gHotkeyBox = nullptr;
HHOOK gKeyboardHook = nullptr;
HANDLE gSingleInstanceMutex = nullptr;
UINT gRegisteredHotkeyId = 0;
UINT gHotkeyModifiers = 0;
UINT gHotkeyVk = VK_SNAPSHOT;
UINT gPendingHotkeyModifiers = 0;
UINT gPendingHotkeyVk = VK_SNAPSHOT;

HBITMAP gScreenBitmap = nullptr;
HDC gScreenDC = nullptr;
HGDIOBJ gOldScreenBitmap = nullptr;
void* gScreenPixels = nullptr;
int gScreenLeft = 0;
int gScreenTop = 0;
int gScreenWidth = 0;
int gScreenHeight = 0;

POINT gDragStart{};
RECT gSelection{};
bool gDragging = false;
bool gHasSelection = false;
bool gEnterDown = false;
bool gEscapeDown = false;
bool gCustomHotkeyKeyDown = false;
bool gSuppressHotkeyKeyUp = false;
UINT gSettingsCapturedVk = 0;
bool gRecordingLeftWinDown = false;
bool gRecordingRightWinDown = false;
HFONT gButtonFont = nullptr;

std::wstring FormatHotkey(UINT modifiers, UINT vk);
void ShowSettingsWindow();

RECT NormalizeRect(POINT a, POINT b)
{
    RECT r{};
    // POINT/RECT coordinates are LONG in WinAPI. Keep all std::clamp
    // arguments the same type; mixing LONG with int prevents template
    // argument deduction in MSVC.
    const LONG maxX = static_cast<LONG>(gScreenWidth);
    const LONG maxY = static_cast<LONG>(gScreenHeight);
    r.left = std::clamp<LONG>(std::min(a.x, b.x), LONG{0}, maxX);
    r.top = std::clamp<LONG>(std::min(a.y, b.y), LONG{0}, maxY);
    r.right = std::clamp<LONG>(std::max(a.x, b.x), LONG{0}, maxX);
    r.bottom = std::clamp<LONG>(std::max(a.y, b.y), LONG{0}, maxY);
    return r;
}

int SelectionWidth()
{
    return gSelection.right - gSelection.left;
}

int SelectionHeight()
{
    return gSelection.bottom - gSelection.top;
}

void ReleaseDesktopImage()
{
    if (gScreenDC) {
        if (gOldScreenBitmap)
            SelectObject(gScreenDC, gOldScreenBitmap);
        DeleteDC(gScreenDC);
    }
    if (gScreenBitmap)
        DeleteObject(gScreenBitmap);

    gScreenDC = nullptr;
    gOldScreenBitmap = nullptr;
    gScreenBitmap = nullptr;
    gScreenPixels = nullptr;
    gScreenWidth = 0;
    gScreenHeight = 0;
    gScreenLeft = 0;
    gScreenTop = 0;
}

bool CaptureVirtualDesktop()
{
    ReleaseDesktopImage();

    gScreenLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    gScreenTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
    gScreenWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    gScreenHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (gScreenWidth <= 0 || gScreenHeight <= 0)
        return false;

    HDC screenDC = GetDC(nullptr);
    if (!screenDC)
        return false;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = gScreenWidth;
    bmi.bmiHeader.biHeight = -gScreenHeight; // top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    gScreenBitmap = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS,
                                     &gScreenPixels, nullptr, 0);
    if (!gScreenBitmap || !gScreenPixels) {
        ReleaseDC(nullptr, screenDC);
        ReleaseDesktopImage();
        return false;
    }

    gScreenDC = CreateCompatibleDC(screenDC);
    if (!gScreenDC) {
        ReleaseDC(nullptr, screenDC);
        ReleaseDesktopImage();
        return false;
    }

    gOldScreenBitmap = SelectObject(gScreenDC, gScreenBitmap);
    const BOOL copied = BitBlt(gScreenDC, 0, 0, gScreenWidth, gScreenHeight,
                                screenDC, gScreenLeft, gScreenTop,
                                SRCCOPY | CAPTUREBLT);
    ReleaseDC(nullptr, screenDC);

    if (!copied) {
        ReleaseDesktopImage();
        return false;
    }

    return true;
}

void DestroyRecognizeButton()
{
    if (gRecognizeButton && IsWindow(gRecognizeButton))
        DestroyWindow(gRecognizeButton);
    gRecognizeButton = nullptr;
}

void DestroySelector()
{
    if (gSelector && IsWindow(gSelector)) {
        DestroyWindow(gSelector);
        return;
    }
    DestroyRecognizeButton();
    ReleaseDesktopImage();
    gSelector = nullptr;
}

void ShowTrayMenu()
{
    HMENU menu = CreatePopupMenu();
    if (!menu)
        return;

    std::wstring captureLabel = L"Распознать QR-код...";
    if (gRegisteredHotkeyId)
        captureLabel += L"  (" + FormatHotkey(gHotkeyModifiers, gHotkeyVk) + L")";
    AppendMenuW(menu, MF_STRING, kMenuCapture, captureLabel.c_str());
    AppendMenuW(menu, MF_STRING, kMenuSettings, L"Настройки сочетания клавиш...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Выход");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(gMainWindow);
    const UINT command = TrackPopupMenu(menu,
        TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
        cursor.x, cursor.y, 0, gMainWindow, nullptr);
    PostMessageW(gMainWindow, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (command == kMenuCapture)
        PostMessageW(gMainWindow, kCaptureMessage, 0, 0);
    else if (command == kMenuSettings)
        ShowSettingsWindow();
    else if (command == kMenuExit)
        PostMessageW(gMainWindow, WM_CLOSE, 0, 0);
}

void AddTrayIcon()
{
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = gMainWindow;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kTrayMessage;
    nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"QR Screen — Print Screen");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

void RemoveTrayIcon()
{
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = gMainWindow;
    nid.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}


std::wstring GetIniPath()
{
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return L"QRScreen.ini";

    std::wstring result(path, length);
    const size_t slash = result.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        result.resize(slash + 1);
    else
        result.clear();
    result += L"QRScreen.ini";
    return result;
}

void SaveHotkeySettings(UINT modifiers, UINT vk)
{
    const std::wstring path = GetIniPath();
    const std::wstring modifierText = std::to_wstring(modifiers);
    const std::wstring keyText = std::to_wstring(vk);
    WritePrivateProfileStringW(L"Hotkey", L"Modifiers", modifierText.c_str(), path.c_str());
    WritePrivateProfileStringW(L"Hotkey", L"Key", keyText.c_str(), path.c_str());
}

std::wstring GetKeyName(UINT vk)
{
    if (vk == VK_SNAPSHOT)
        return L"Print Screen";

    const UINT scanCode = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LONG keyData = static_cast<LONG>(scanCode << 16);
    switch (vk) {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
    case VK_PRIOR: case VK_NEXT: case VK_LEFT: case VK_RIGHT:
    case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_NUMLOCK:
        keyData |= (1L << 24);
        break;
    default:
        break;
    }

    wchar_t name[128]{};
    if (GetKeyNameTextW(keyData, name, static_cast<int>(std::size(name))) > 0)
        return name;
    return L"Key " + std::to_wstring(vk);
}

std::wstring FormatHotkey(UINT modifiers, UINT vk)
{
    std::wstring result;
    if (modifiers & MOD_CONTROL) result += L"Ctrl + ";
    if (modifiers & MOD_ALT)     result += L"Alt + ";
    if (modifiers & MOD_SHIFT)   result += L"Shift + ";
    if (modifiers & MOD_WIN)     result += L"Win + ";

    if (vk == 0) {
        if (result.empty())
            return L"Нажмите сочетание клавиш...";
        return result + L"нажмите клавишу";
    }

    result += GetKeyName(vk);
    return result;
}

void UpdateTrayIconTooltip()
{
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = gMainWindow;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_TIP;
    const std::wstring tip = gRegisteredHotkeyId
        ? L"QR Screen — " + FormatHotkey(gHotkeyModifiers, gHotkeyVk)
        : L"QR Screen — горячая клавиша не назначена";
    wcscpy_s(nid.szTip, tip.c_str());
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void ShowSimpleMessage(const std::wstring& title, const std::wstring& message,
                      UINT icon = MB_ICONINFORMATION, HWND owner = nullptr)
{
    MessageBoxW(owner, message.c_str(), title.c_str(),
                MB_OK | icon | MB_SETFOREGROUND | MB_TOPMOST);
}

bool IsModifierKey(UINT vk)
{
    return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
           vk == VK_LSHIFT || vk == VK_RSHIFT ||
           vk == VK_LCONTROL || vk == VK_RCONTROL ||
           vk == VK_LMENU || vk == VK_RMENU ||
           vk == VK_LWIN || vk == VK_RWIN;
}

bool IsValidHotkey(UINT modifiers, UINT vk)
{
    constexpr UINT allowedModifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN;
    if (vk == 0 || vk > 0xFF || IsModifierKey(vk) ||
        vk == VK_ESCAPE || vk == VK_RETURN || (modifiers & ~allowedModifiers) != 0)
        return false;

    // Avoid globally stealing ordinary typing keys. An unmodified key is
    // permitted only for Print Screen, which is the default screenshot key.
    return modifiers != 0 || vk == VK_SNAPSHOT;
}

bool ApplyGlobalHotkey(UINT modifiers, UINT vk, DWORD* errorCode = nullptr)
{
    if (!IsValidHotkey(modifiers, vk)) {
        if (errorCode) *errorCode = ERROR_INVALID_PARAMETER;
        return false;
    }

    if (gRegisteredHotkeyId && modifiers == gHotkeyModifiers && vk == gHotkeyVk) {
        if (errorCode) *errorCode = ERROR_SUCCESS;
        return true;
    }

    const UINT newId = (gRegisteredHotkeyId == kHotkeyIdA) ? kHotkeyIdB : kHotkeyIdA;
    if (!RegisterHotKey(gMainWindow, static_cast<int>(newId),
                        modifiers | MOD_NOREPEAT, vk)) {
        if (errorCode) *errorCode = GetLastError();
        return false;
    }

    // Register the new combination first. If it is busy, the current one
    // remains active and the user can choose another without losing it.
    if (gRegisteredHotkeyId)
        UnregisterHotKey(gMainWindow, static_cast<int>(gRegisteredHotkeyId));

    gRegisteredHotkeyId = newId;
    gHotkeyModifiers = modifiers;
    gHotkeyVk = vk;
    UpdateTrayIconTooltip();
    if (errorCode) *errorCode = ERROR_SUCCESS;
    return true;
}

bool RegisterStartupHotkey()
{
    const std::wstring ini = GetIniPath();
    const UINT preferredModifiers = GetPrivateProfileIntW(
        L"Hotkey", L"Modifiers", 0, ini.c_str());
    const UINT preferredVk = GetPrivateProfileIntW(
        L"Hotkey", L"Key", VK_SNAPSHOT, ini.c_str());

    if (IsValidHotkey(preferredModifiers, preferredVk) &&
        RegisterHotKey(gMainWindow, static_cast<int>(kHotkeyIdA),
                       preferredModifiers | MOD_NOREPEAT, preferredVk)) {
        gRegisteredHotkeyId = kHotkeyIdA;
        gHotkeyModifiers = preferredModifiers;
        gHotkeyVk = preferredVk;
        UpdateTrayIconTooltip();
        return true;
    }

    const DWORD preferredError = IsValidHotkey(preferredModifiers, preferredVk)
        ? GetLastError() : ERROR_INVALID_PARAMETER;
    struct Candidate { UINT modifiers; UINT vk; };
    const Candidate alternatives[] = {
        { 0, VK_SNAPSHOT },
        { MOD_CONTROL, VK_SNAPSHOT },
        { MOD_SHIFT, VK_SNAPSHOT },
        { MOD_ALT, VK_SNAPSHOT },
        { MOD_CONTROL | MOD_SHIFT, VK_SNAPSHOT },
        { MOD_CONTROL | MOD_ALT, VK_SNAPSHOT },
        { MOD_ALT | MOD_SHIFT, VK_SNAPSHOT },
        { MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_SNAPSHOT },
        { MOD_CONTROL | MOD_ALT, VK_F11 },
        { MOD_CONTROL | MOD_ALT, VK_F10 },
        { MOD_CONTROL | MOD_ALT, VK_F9 },
        { MOD_CONTROL | MOD_ALT, VK_F8 },
        { MOD_CONTROL | MOD_SHIFT, VK_F11 },
        { MOD_CONTROL | MOD_SHIFT, VK_F10 },
        { MOD_CONTROL | MOD_SHIFT, VK_F9 }
    };

    for (const Candidate& candidate : alternatives) {
        if (candidate.modifiers == preferredModifiers && candidate.vk == preferredVk)
            continue;
        if (!RegisterHotKey(gMainWindow, static_cast<int>(kHotkeyIdA),
                            candidate.modifiers | MOD_NOREPEAT, candidate.vk))
            continue;

        gRegisteredHotkeyId = kHotkeyIdA;
        gHotkeyModifiers = candidate.modifiers;
        gHotkeyVk = candidate.vk;
        SaveHotkeySettings(gHotkeyModifiers, gHotkeyVk);
        UpdateTrayIconTooltip();

        const std::wstring oldName = FormatHotkey(preferredModifiers, preferredVk);
        const std::wstring newName = FormatHotkey(gHotkeyModifiers, gHotkeyVk);
        std::wstring message;
        if (preferredError == ERROR_HOTKEY_ALREADY_REGISTERED) {
            message = oldName + L" занято другим приложением. Для QR Screen автоматически назначено " +
                      newName + L". Сочетание можно изменить в настройках.";
        } else {
            message = L"Не удалось зарегистрировать " + oldName + L". Для QR Screen назначено " +
                      newName + L". Сочетание можно изменить в настройках.";
        }
        ShowSimpleMessage(L"QR Screen: изменена горячая клавиша", message, MB_ICONWARNING);
        return true;
    }

    gRegisteredHotkeyId = 0;
    UpdateTrayIconTooltip();
    ShowSimpleMessage(L"QR Screen: нет горячей клавиши",
        L"Не удалось зарегистрировать сочетание клавиш. Запуск распознавания доступен через меню значка в трее.",
        MB_ICONWARNING);
    return false;
}

UINT GetModifiersForKeyboardEvent(UINT eventVk, bool eventDown)
{
    auto pressed = [eventVk, eventDown](UINT generic, UINT left, UINT right) {
        bool value = (GetAsyncKeyState(generic) & 0x8000) != 0 ||
                     (GetAsyncKeyState(left) & 0x8000) != 0 ||
                     (GetAsyncKeyState(right) & 0x8000) != 0;
        if (eventVk == generic || eventVk == left || eventVk == right)
            value = eventDown;
        return value;
    };

    UINT modifiers = 0;
    if (pressed(VK_CONTROL, VK_LCONTROL, VK_RCONTROL)) modifiers |= MOD_CONTROL;
    if (pressed(VK_MENU, VK_LMENU, VK_RMENU)) modifiers |= MOD_ALT;
    if (pressed(VK_SHIFT, VK_LSHIFT, VK_RSHIFT)) modifiers |= MOD_SHIFT;

    bool winDown = gRecordingLeftWinDown || gRecordingRightWinDown ||
                   (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
                   (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
    if (eventVk == VK_LWIN)
        winDown = eventDown || gRecordingRightWinDown;
    else if (eventVk == VK_RWIN)
        winDown = eventDown || gRecordingLeftWinDown;
    if (winDown) modifiers |= MOD_WIN;
    return modifiers;
}

LRESULT CALLBACK HotkeyBoxWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS;

    case WM_SETFOCUS:
    case WM_LBUTTONDOWN:
        // Start a fresh recording session as soon as the user enters the field.
        gPendingHotkeyModifiers = 0;
        gPendingHotkeyVk = 0;
        gSettingsCapturedVk = 0;
        gRecordingLeftWinDown = false;
        gRecordingRightWinDown = false;
        SetFocus(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        // Fallback for keyboards that deliver ordinary key messages rather
        // than being intercepted by the low-level hook.
        const UINT vk = static_cast<UINT>(wParam);
        if (!IsModifierKey(vk) && vk != VK_ESCAPE && vk != VK_RETURN) {
            gPendingHotkeyModifiers = GetModifiersForKeyboardEvent(vk, true);
            gPendingHotkeyVk = vk;
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;
    }

    case WM_CHAR:
        return 0;

    case kHotkeyRecordedMessage:
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, GetSysColorBrush(COLOR_WINDOW));
        DrawEdge(dc, &rc, (GetFocus() == hwnd) ? EDGE_SUNKEN : EDGE_RAISED, BF_RECT);
        InflateRect(&rc, -8, -2);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        const std::wstring label = FormatHotkey(gPendingHotkeyModifiers, gPendingHotkeyVk);
        DrawTextW(dc, label.c_str(), -1, &rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (GetFocus() == hwnd) {
            RECT focus = rc;
            InflateRect(&focus, -2, -3);
            DrawFocusRect(dc, &focus);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE: {
        gSettingsWindow = hwnd;
        CreateWindowExW(0, L"STATIC", L"Сейчас используется:",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
            24, 20, 600, 24, hwnd, nullptr, gInstance, nullptr);
        const std::wstring current = gRegisteredHotkeyId
            ? FormatHotkey(gHotkeyModifiers, gHotkeyVk)
            : L"Не назначено";
        CreateWindowExW(0, L"STATIC", current.c_str(),
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
            24, 49, 600, 28, hwnd, nullptr, gInstance, nullptr);
        CreateWindowExW(0, L"STATIC", L"Нажмите нужное сочетание в поле ниже:",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
            24, 91, 600, 25, hwnd, nullptr, gInstance, nullptr);
        gHotkeyBox = CreateWindowExW(0, kHotkeyBoxClass, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            24, 122, 600, 48,
            hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSettingsHotkeyBox)),
            gInstance, nullptr);
        CreateWindowExW(0, L"STATIC",
            L"Щёлкните поле и нажмите клавиши одновременно.\r\n"
            L"Print Screen можно использовать отдельно или с Ctrl, Shift, Alt и Win.\r\n"
            L"Для других клавиш нужен модификатор. Esc и Enter не назначаются.",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
            24, 184, 600, 72, hwnd, nullptr, gInstance, nullptr);
        CreateWindowExW(0, L"BUTTON", L"По умолчанию: Print Screen",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            24, 302, 245, 38, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSettingsDefault)), gInstance, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Сохранить",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            416, 302, 100, 38, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSettingsSave)), gInstance, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Отмена",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            526, 302, 100, 38, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSettingsCancel)), gInstance, nullptr);

        // Use the standard Windows dialog font for consistent text metrics.
        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        for (HWND child = GetWindow(hwnd, GW_CHILD); child;
             child = GetWindow(child, GW_HWNDNEXT)) {
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
        if (gHotkeyBox)
            SetFocus(gHotkeyBox);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kSettingsDefault:
            gPendingHotkeyModifiers = 0;
            gPendingHotkeyVk = VK_SNAPSHOT;
            if (gHotkeyBox) InvalidateRect(gHotkeyBox, nullptr, TRUE);
            return 0;

        case kSettingsSave: {
            DWORD error = ERROR_SUCCESS;
            if (!ApplyGlobalHotkey(gPendingHotkeyModifiers, gPendingHotkeyVk, &error)) {
                const wchar_t* warningText = (error == ERROR_HOTKEY_ALREADY_REGISTERED)
                    ? L"Это сочетание уже занято другой программой.\r\nВыберите другое сочетание клавиш."
                    : (error == ERROR_INVALID_PARAMETER)
                        ? L"Для клавиш кроме Print Screen нужен модификатор Ctrl, Alt, Shift или Win.\r\nEsc и Enter зарезервированы."
                        : L"Не удалось зарегистрировать это сочетание клавиш.\r\nПопробуйте другое сочетание.";
                MessageBoxW(hwnd, warningText, L"QR Screen",
                            MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
                if (gHotkeyBox) SetFocus(gHotkeyBox);
                return 0;
            }
            SaveHotkeySettings(gHotkeyModifiers, gHotkeyVk);
            const std::wstring confirmationText = L"Горячая клавиша установлена: " +
                FormatHotkey(gHotkeyModifiers, gHotkeyVk);
            MessageBoxW(hwnd, confirmationText.c_str(), L"QR Screen",
                        MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
            DestroyWindow(hwnd);
            return 0;
        }

        case kSettingsCancel:
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        gSettingsWindow = nullptr;
        gHotkeyBox = nullptr;
        gSettingsCapturedVk = 0;
        gRecordingLeftWinDown = false;
        gRecordingRightWinDown = false;
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool EnsureSettingsClasses()
{
    WNDCLASSEXW box{};
    box.cbSize = sizeof(box);
    box.lpfnWndProc = HotkeyBoxWndProc;
    box.hInstance = gInstance;
    box.hCursor = LoadCursorW(nullptr, IDC_HAND);
    box.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    box.lpszClassName = kHotkeyBoxClass;
    if (!RegisterClassExW(&box) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    WNDCLASSEXW settings{};
    settings.cbSize = sizeof(settings);
    settings.lpfnWndProc = SettingsWndProc;
    settings.hInstance = gInstance;
    settings.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    settings.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    settings.lpszClassName = kSettingsClass;
    if (!RegisterClassExW(&settings) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;
    return true;
}

void ShowSettingsWindow()
{
    if (gSettingsWindow && IsWindow(gSettingsWindow)) {
        ShowWindow(gSettingsWindow, SW_SHOWNORMAL);
        SetForegroundWindow(gSettingsWindow);
        if (gHotkeyBox) SetFocus(gHotkeyBox);
        return;
    }
    if (!EnsureSettingsClasses()) {
        MessageBoxW(gMainWindow, L"Не удалось создать окно настроек.",
                    L"QR Screen", MB_OK | MB_ICONERROR);
        return;
    }

    gPendingHotkeyModifiers = gHotkeyModifiers;
    gPendingHotkeyVk = gHotkeyVk;
    constexpr int windowWidth = 660;
    constexpr int windowHeight = 420;
    POINT cursor{};
    GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    GetMonitorInfoW(monitor, &monitorInfo);
    const RECT work = monitorInfo.rcWork;
    // RECT coordinates are LONG (WinAPI); keep all std::max operands the same type.
    const LONG offsetX = std::max<LONG>(0L,
        (work.right - work.left - static_cast<LONG>(windowWidth)) / 2);
    const LONG offsetY = std::max<LONG>(0L,
        (work.bottom - work.top - static_cast<LONG>(windowHeight)) / 2);
    const int windowX = static_cast<int>(work.left + offsetX);
    const int windowY = static_cast<int>(work.top + offsetY);

    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_DLGMODALFRAME,
        kSettingsClass, L"Настройки QR Screen",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        windowX, windowY, windowWidth, windowHeight,
        gMainWindow, nullptr, gInstance, nullptr);
    if (!window) {
        MessageBoxW(gMainWindow, L"Не удалось открыть настройки.",
                    L"QR Screen", MB_OK | MB_ICONERROR);
        return;
    }
    ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window);
    SetForegroundWindow(window);
    if (gHotkeyBox) SetFocus(gHotkeyBox);
}

void DrawDimLayer(HDC target, int width, int height)
{
    HDC blackDC = CreateCompatibleDC(target);
    if (!blackDC)
        return;

    HBITMAP pixel = CreateCompatibleBitmap(target, 1, 1);
    if (!pixel) {
        DeleteDC(blackDC);
        return;
    }

    HGDIOBJ old = SelectObject(blackDC, pixel);
    PatBlt(blackDC, 0, 0, 1, 1, BLACKNESS);

    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 145;
    blend.AlphaFormat = 0;
    AlphaBlend(target, 0, 0, width, height, blackDC, 0, 0, 1, 1, blend);

    SelectObject(blackDC, old);
    DeleteObject(pixel);
    DeleteDC(blackDC);
}

void DrawSelection(HDC target)
{
    if (!gHasSelection && !gDragging)
        return;

    const RECT r = gSelection;
    const int width = r.right - r.left;
    const int height = r.bottom - r.top;
    if (width <= 0 || height <= 0)
        return;

    // Restore the original, undimmed pixels inside the selected rectangle.
    BitBlt(target, r.left, r.top, width, height,
           gScreenDC, r.left, r.top, SRCCOPY);

    HPEN pen = CreatePen(PS_SOLID, 2, RGB(0, 180, 235));
    HGDIOBJ oldPen = SelectObject(target, pen);
    HGDIOBJ oldBrush = SelectObject(target, GetStockObject(HOLLOW_BRUSH));
    Rectangle(target, r.left, r.top, r.right, r.bottom);
    SelectObject(target, oldBrush);
    SelectObject(target, oldPen);
    DeleteObject(pen);

    wchar_t sizeText[64]{};
    swprintf_s(sizeText, L"%d × %d", width, height);
    RECT label{ r.left, (r.top >= 28) ? r.top - 26 : r.top + 4,
                r.left + 90, (r.top >= 28) ? r.top - 4 : r.top + 22 };
    HBRUSH labelBrush = CreateSolidBrush(RGB(28, 30, 34));
    FillRect(target, &label, labelBrush);
    DeleteObject(labelBrush);
    SetBkMode(target, TRANSPARENT);
    SetTextColor(target, RGB(255, 255, 255));
    DrawTextW(target, sizeText, -1, &label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void CreateRecognizeButton(HWND parent)
{
    DestroyRecognizeButton();

    constexpr int buttonWidth = 156;
    constexpr int buttonHeight = 38;
    constexpr int margin = 10;

    int x = gSelection.left;
    int y = gSelection.bottom + margin;
    if (y + buttonHeight > gScreenHeight - 8)
        y = gSelection.top - buttonHeight - margin;

    x = std::clamp(x, 8, std::max(8, gScreenWidth - buttonWidth - 8));
    y = std::clamp(y, 8, std::max(8, gScreenHeight - buttonHeight - 8));

    gRecognizeButton = CreateWindowExW(
        0, L"BUTTON", L"Распознать QR",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        x, y, buttonWidth, buttonHeight,
        parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonRecognize)),
        gInstance, nullptr);

    if (gRecognizeButton) {
        gButtonFont = CreateFontW(-16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        if (gButtonFont)
            SendMessageW(gRecognizeButton, WM_SETFONT,
                         reinterpret_cast<WPARAM>(gButtonFont), TRUE);
    }
}

std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty())
        return {};

    const int inputLength = static_cast<int>(text.size());
    int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                       text.data(), inputLength, nullptr, 0);
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (required == 0) {
        flags = 0;
        required = MultiByteToWideChar(CP_UTF8, flags,
                                       text.data(), inputLength, nullptr, 0);
    }
    if (required <= 0)
        return {};

    std::wstring result(static_cast<size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, flags, text.data(), inputLength,
                            result.data(), required) <= 0)
        return {};
    return result;
}

HGLOBAL CreateClipboardMemory(const void* source, SIZE_T bytes)
{
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory)
        return nullptr;
    void* destination = GlobalLock(memory);
    if (!destination) {
        GlobalFree(memory);
        return nullptr;
    }
    std::memcpy(destination, source, bytes);
    GlobalUnlock(memory);
    return memory;
}

bool CopyTextToClipboard(HWND owner, const std::wstring& text)
{
    const SIZE_T textBytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL textMemory = CreateClipboardMemory(text.c_str(), textBytes);
    if (!textMemory)
        return false;

    if (!OpenClipboard(owner)) {
        GlobalFree(textMemory);
        return false;
    }

    EmptyClipboard();
    const bool textOk = SetClipboardData(CF_UNICODETEXT, textMemory) != nullptr;
    if (textOk)
        textMemory = nullptr; // Ownership transferred to the clipboard.

    CloseClipboard();
    if (textMemory)
        GlobalFree(textMemory);
    return textOk;
}

void DecodeSelection(HWND owner)
{
    if (!gScreenPixels || !gHasSelection || SelectionWidth() < 8 || SelectionHeight() < 8)
        return;

    try {
        const auto* pixels = static_cast<const uint8_t*>(gScreenPixels);
        const ZXing::ImageView desktop(pixels, gScreenWidth, gScreenHeight,
                                       ZXing::ImageFormat::BGR, gScreenWidth * 4, 4);
        const ZXing::ImageView cropped = desktop.cropped(
            gSelection.left, gSelection.top, SelectionWidth(), SelectionHeight());

        const ZXing::ReaderOptions options = ZXing::ReaderOptions()
            .formats(ZXing::BarcodeFormat::QRCode)
            .tryHarder(true)
            .tryRotate(true)
            .tryInvert(true)
            .tryDownscale(true)
            .maxNumberOfSymbols(32);

        const ZXing::Barcodes barcodes = ZXing::ReadBarcodes(cropped, options);
        std::vector<std::wstring> decodedValues;
        std::unordered_set<std::wstring> uniqueValues;

        for (const auto& barcode : barcodes) {
            if (!barcode.isValid())
                continue;
            std::wstring value = Utf8ToWide(barcode.text());
            if (!value.empty() && uniqueValues.insert(value).second)
                decodedValues.push_back(std::move(value));
        }

        if (decodedValues.empty()) {
            MessageBoxW(owner,
                L"QR-код не найден. Попробуйте выделить область целиком, включая белые поля вокруг кода.",
                L"QR Screen", MB_OK | MB_ICONWARNING);
            return;
        }

        // If several QR codes are inside the selected rectangle, copy all of
        // them in reading order, separated by a blank line.
        std::wstring clipboardText;
        for (const auto& value : decodedValues) {
            if (!clipboardText.empty())
                clipboardText += L"\r\n\r\n";
            clipboardText += value;
        }

        if (!CopyTextToClipboard(owner, clipboardText)) {
            MessageBoxW(owner,
                L"QR-код распознан, но Windows не разрешила записать данные в буфер обмена.",
                L"QR Screen", MB_OK | MB_ICONERROR);
            return;
        }

        const std::wstring message = L"Найдено QR-кодов: " +
            std::to_wstring(decodedValues.size()) +
            L".\nТекст скопирован в буфер обмена.";
        MessageBoxW(owner, message.c_str(), L"QR Screen",
                    MB_OK | MB_ICONINFORMATION);

        if (gSelector && IsWindow(gSelector))
            DestroyWindow(gSelector);
    }
    catch (...) {
        MessageBoxW(owner,
            L"Во время распознавания QR-кода произошла ошибка.",
            L"QR Screen", MB_OK | MB_ICONERROR);
    }
}

LRESULT CALLBACK SelectorWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_ERASEBKGND:
        return 1;

    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, (gDragging || !gHasSelection) ? IDC_CROSS : IDC_ARROW));
        return TRUE;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC target = BeginPaint(hwnd, &ps);
        if (gScreenDC) {
            BitBlt(target, 0, 0, gScreenWidth, gScreenHeight,
                   gScreenDC, 0, 0, SRCCOPY);
            DrawDimLayer(target, gScreenWidth, gScreenHeight);
            DrawSelection(target);

            if (!gHasSelection && !gDragging) {
                const wchar_t* hint = L"Выделите область с QR-кодом мышью.   Esc — отмена";
                RECT hintRect{ 18, std::max(8, gScreenHeight - 48),
                               std::min(gScreenWidth - 18, 620), gScreenHeight - 14 };
                SetBkMode(target, TRANSPARENT);
                SetTextColor(target, RGB(255, 255, 255));
                DrawTextW(target, hint, -1, &hintRect,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        DestroyRecognizeButton();
        if (gButtonFont) {
            DeleteObject(gButtonFont);
            gButtonFont = nullptr;
        }
        gHasSelection = false;
        gDragging = true;
        gDragStart = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        gSelection = { gDragStart.x, gDragStart.y, gDragStart.x, gDragStart.y };
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_MOUSEMOVE:
        if (gDragging) {
            const POINT current{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            gSelection = NormalizeRect(gDragStart, current);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONUP:
        if (gDragging) {
            const POINT current{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            gSelection = NormalizeRect(gDragStart, current);
            gDragging = false;
            if (GetCapture() == hwnd)
                ReleaseCapture();

            if (SelectionWidth() >= 12 && SelectionHeight() >= 12) {
                gHasSelection = true;
                CreateRecognizeButton(hwnd);
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            } else {
                gHasSelection = false;
                SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == kButtonRecognize && HIWORD(wParam) == BN_CLICKED) {
            DecodeSelection(hwnd);
            return 0;
        }
        break;

    case kDecodeMessage:
        DecodeSelection(hwnd);
        return 0;

    case kCancelMessage:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (GetCapture() == hwnd)
            ReleaseCapture();
        DestroyRecognizeButton();
        if (gButtonFont) {
            DeleteObject(gButtonFont);
            gButtonFont = nullptr;
        }
        gDragging = false;
        gHasSelection = false;
        ReleaseDesktopImage();
        gSelector = nullptr;
        return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool EnsureSelectorClass()
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = SelectorWndProc;
    wc.hInstance = gInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    wc.lpszClassName = kSelectorClass;

    if (RegisterClassExW(&wc))
        return true;
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

void StartCapture()
{
    if (gSelector)
        DestroySelector();

    if (!CaptureVirtualDesktop()) {
        MessageBoxW(gMainWindow,
            L"Не удалось получить изображение рабочего стола.",
            L"QR Screen", MB_OK | MB_ICONERROR);
        return;
    }

    if (!EnsureSelectorClass()) {
        ReleaseDesktopImage();
        MessageBoxW(gMainWindow, L"Не удалось создать окно выделения области.",
                    L"QR Screen", MB_OK | MB_ICONERROR);
        return;
    }

    gSelection = {};
    gDragging = false;
    gHasSelection = false;

    gSelector = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kSelectorClass, L"QR Screen — выберите QR-код",
        WS_POPUP,
        gScreenLeft, gScreenTop, gScreenWidth, gScreenHeight,
        gMainWindow, nullptr, gInstance, nullptr);

    if (!gSelector) {
        ReleaseDesktopImage();
        MessageBoxW(gMainWindow, L"Не удалось открыть экран выделения.",
                    L"QR Screen", MB_OK | MB_ICONERROR);
        return;
    }

    ShowWindow(gSelector, SW_SHOWNORMAL);
    SetWindowPos(gSelector, HWND_TOPMOST, gScreenLeft, gScreenTop,
                 gScreenWidth, gScreenHeight, SWP_SHOWWINDOW);
    UpdateWindow(gSelector);
    SetForegroundWindow(gSelector);
    SetFocus(gSelector);
}

LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION) {
        const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        const bool keyDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        const bool keyUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);

        // While the shortcut field is focused, record every key directly in
        // the low-level hook. This gives immediate visual feedback and handles
        // Print Screen even on keyboards that do not send WM_KEYDOWN to controls.
        const bool recordingShortcut = gSettingsWindow && gHotkeyBox &&
            GetForegroundWindow() == gSettingsWindow && GetFocus() == gHotkeyBox;
        if (recordingShortcut) {
            const UINT vk = key->vkCode;
            if (keyDown) {
                if (vk == VK_LWIN) gRecordingLeftWinDown = true;
                if (vk == VK_RWIN) gRecordingRightWinDown = true;

                const UINT modifiers = GetModifiersForKeyboardEvent(vk, true);
                if (IsModifierKey(vk)) {
                    gPendingHotkeyModifiers = modifiers;
                    gPendingHotkeyVk = 0;
                    if (gHotkeyBox)
                        PostMessageW(gHotkeyBox, kHotkeyRecordedMessage, 0, 0);
                    // Let Ctrl/Alt/Shift reach Windows so GetAsyncKeyState sees
                    // them for the following key. Suppress Win to avoid opening Start.
                    if (vk == VK_LWIN || vk == VK_RWIN)
                        return 1;
                    return CallNextHookEx(gKeyboardHook, code, wParam, lParam);
                }

                if (vk != VK_ESCAPE && vk != VK_RETURN) {
                    gPendingHotkeyModifiers = modifiers;
                    gPendingHotkeyVk = vk;
                    gSettingsCapturedVk = vk;
                    if (gHotkeyBox)
                        PostMessageW(gHotkeyBox, kHotkeyRecordedMessage, 0, 0);
                }
                return 1;
            }
            if (keyUp) {
                if (vk == VK_LWIN) gRecordingLeftWinDown = false;
                if (vk == VK_RWIN) gRecordingRightWinDown = false;

                if (IsModifierKey(vk) && gPendingHotkeyVk == 0) {
                    gPendingHotkeyModifiers = GetModifiersForKeyboardEvent(vk, false);
                    if (gHotkeyBox)
                        PostMessageW(gHotkeyBox, kHotkeyRecordedMessage, 0, 0);
                } else if (!IsModifierKey(vk) && vk != VK_ESCAPE && vk != VK_RETURN &&
                           gSettingsCapturedVk != vk) {
                    // A few keyboard drivers report Print Screen only on key-up.
                    // Record it here as a fallback, without delaying normal key-down capture.
                    gPendingHotkeyModifiers = GetModifiersForKeyboardEvent(vk, false);
                    gPendingHotkeyVk = vk;
                    if (gHotkeyBox)
                        PostMessageW(gHotkeyBox, kHotkeyRecordedMessage, 0, 0);
                }
                if (gSettingsCapturedVk == vk)
                    gSettingsCapturedVk = 0;

                if (vk == VK_LWIN || vk == VK_RWIN)
                    return 1;
                if (IsModifierKey(vk))
                    return CallNextHookEx(gKeyboardHook, code, wParam, lParam);
                return 1;
            }
        }

        // RegisterHotKey reserves the selected combination so Windows can
        // report conflicts. The low-level hook then suppresses that key from
        // reaching other screenshot tools and starts our capture directly.
        // Do not swallow it while the settings window is open: the user must
        // be able to record the current combination in the hotkey editor.
        if (gRegisteredHotkeyId && !gSettingsWindow && key->vkCode == gHotkeyVk) {
            if (keyDown) {
                if (!gCustomHotkeyKeyDown) {
                    gCustomHotkeyKeyDown = true;
                    UINT modifiers = 0;
                    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) modifiers |= MOD_CONTROL;
                    if (GetAsyncKeyState(VK_MENU) & 0x8000) modifiers |= MOD_ALT;
                    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) modifiers |= MOD_SHIFT;
                    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) ||
                        (GetAsyncKeyState(VK_RWIN) & 0x8000)) modifiers |= MOD_WIN;

                    if (modifiers == gHotkeyModifiers) {
                        gSuppressHotkeyKeyUp = true;
                        if (gMainWindow && !gSelector)
                            PostMessageW(gMainWindow, kCaptureMessage, 0, 0);
                        return 1;
                    }
                }
                if (gSuppressHotkeyKeyUp)
                    return 1;
            }
            if (keyUp) {
                gCustomHotkeyKeyDown = false;
                if (gSuppressHotkeyKeyUp) {
                    gSuppressHotkeyKeyUp = false;
                    return 1;
                }
            }
        }

        // While the region selector is active, Enter confirms and Esc cancels.
        if (key->vkCode == VK_ESCAPE) {
            if (keyUp)
                gEscapeDown = false;
            else if (keyDown && gSelector && GetForegroundWindow() == gSelector && !gEscapeDown) {
                gEscapeDown = true;
                PostMessageW(gSelector, kCancelMessage, 0, 0);
                return 1;
            }
        }
        if (key->vkCode == VK_RETURN) {
            if (keyUp)
                gEnterDown = false;
            else if (keyDown && gSelector && GetForegroundWindow() == gSelector &&
                     gHasSelection && !gEnterDown) {
                gEnterDown = true;
                PostMessageW(gSelector, kDecodeMessage, 0, 0);
                return 1;
            }
        }
    }

    return CallNextHookEx(gKeyboardHook, code, wParam, lParam);
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case kTrayMessage:
        if (wParam == kTrayIconId) {
            if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU)
                ShowTrayMenu();
            else if (lParam == WM_LBUTTONDBLCLK)
                PostMessageW(hwnd, kCaptureMessage, 0, 0);
        }
        return 0;

    case kCaptureMessage:
        StartCapture();
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (gSelector)
            DestroySelector();
        if (gSettingsWindow)
            DestroyWindow(gSettingsWindow);
        if (gRegisteredHotkeyId) {
            UnregisterHotKey(hwnd, static_cast<int>(gRegisteredHotkeyId));
            gRegisteredHotkeyId = 0;
        }
        RemoveTrayIcon();
        if (gKeyboardHook) {
            UnhookWindowsHookEx(gKeyboardHook);
            gKeyboardHook = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void EnablePerMonitorDpiAwareness()
{
    // Use Per-Monitor V2 when available; fall back to system DPI awareness.
    using SetDpiContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    const auto setContext = reinterpret_cast<SetDpiContextFn>(
        GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (!setContext || !setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        SetProcessDPIAware();
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int)
{
    gInstance = instance;
    EnablePerMonitorDpiAwareness();

    gSingleInstanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\QRScreen.SingleInstance");
    if (!gSingleInstanceMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (gSingleInstanceMutex)
            CloseHandle(gSingleInstanceMutex);
        return 0;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = gInstance;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = kMainClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"Не удалось зарегистрировать окно программы.",
                    L"QR Screen", MB_OK | MB_ICONERROR);
        CloseHandle(gSingleInstanceMutex);
        return 1;
    }

    gMainWindow = CreateWindowExW(
        WS_EX_TOOLWINDOW, kMainClass, L"QR Screen",
        WS_POPUP, 0, 0, 1, 1,
        nullptr, nullptr, gInstance, nullptr);
    if (!gMainWindow) {
        MessageBoxW(nullptr, L"Не удалось запустить QR Screen.",
                    L"QR Screen", MB_OK | MB_ICONERROR);
        CloseHandle(gSingleInstanceMutex);
        return 1;
    }

    AddTrayIcon();
    RegisterStartupHotkey();
    gKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                                      gInstance, 0);
    if (!gKeyboardHook) {
        RemoveTrayIcon();
        MessageBoxW(nullptr,
            L"Не удалось установить обработчик клавиш Enter и Esc.\n"
            L"Проверьте, не запрещает ли это защитное ПО.",
            L"QR Screen", MB_OK | MB_ICONERROR);
        DestroyWindow(gMainWindow);
        CloseHandle(gSingleInstanceMutex);
        return 1;
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (gSettingsWindow && IsDialogMessageW(gSettingsWindow, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (gSingleInstanceMutex)
        CloseHandle(gSingleInstanceMutex);
    return static_cast<int>(msg.wParam);
}
