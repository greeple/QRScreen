#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <shellapi.h>
#include <windowsx.h>
#include <wingdi.h>

#include <ZXing/ReadBarcode.h>
#include <ZXing/BarcodeFormat.h>
#include <ZXing/ImageView.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kMainClass[] = L"QRScreen.MainWindow";
constexpr wchar_t kSelectorClass[] = L"QRScreen.RegionSelector";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kCaptureMessage = WM_APP + 2;
constexpr UINT kDecodeMessage = WM_APP + 3;
constexpr UINT kCancelMessage = WM_APP + 4;
constexpr UINT kTrayIconId = 1;
constexpr UINT kMenuCapture = 1001;
constexpr UINT kMenuExit = 1002;
constexpr UINT kButtonRecognize = 2001;

HINSTANCE gInstance = nullptr;
HWND gMainWindow = nullptr;
HWND gSelector = nullptr;
HWND gRecognizeButton = nullptr;
HHOOK gKeyboardHook = nullptr;
HANDLE gSingleInstanceMutex = nullptr;

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
bool gPrintScreenDown = false;
bool gEnterDown = false;
bool gEscapeDown = false;
HFONT gButtonFont = nullptr;

RECT NormalizeRect(POINT a, POINT b)
{
    RECT r{};
    r.left = std::clamp(std::min(a.x, b.x), 0, gScreenWidth);
    r.top = std::clamp(std::min(a.y, b.y), 0, gScreenHeight);
    r.right = std::clamp(std::max(a.x, b.x), 0, gScreenWidth);
    r.bottom = std::clamp(std::max(a.y, b.y), 0, gScreenHeight);
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

    AppendMenuW(menu, MF_STRING, kMenuCapture, L"Распознать QR-код...  (Print Screen)");
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

bool CopyTextToClipboard(HWND owner, const std::wstring& text)
{
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory)
        return false;

    void* destination = GlobalLock(memory);
    if (!destination) {
        GlobalFree(memory);
        return false;
    }
    std::memcpy(destination, text.c_str(), bytes);
    GlobalUnlock(memory);

    if (!OpenClipboard(owner)) {
        GlobalFree(memory);
        return false;
    }

    EmptyClipboard();
    const bool success = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
    CloseClipboard();
    if (!success)
        GlobalFree(memory);
    return success;
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
            L".\nСодержимое скопировано в буфер обмена.";
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
        SetCursor(LoadCursorW(nullptr, IDC_CROSS));
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
            } else {
                gHasSelection = false;
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

        if (key->vkCode == VK_SNAPSHOT) {
            if (keyDown) {
                if (!gPrintScreenDown) {
                    gPrintScreenDown = true;
                    if (gMainWindow && !gSelector)
                        PostMessageW(gMainWindow, kCaptureMessage, 0, 0);
                }
                return 1; // prevent Windows/other screenshot tools from taking over PrtSc
            }
            if (keyUp) {
                gPrintScreenDown = false;
                return 1;
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
    gKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                                      gInstance, 0);
    if (!gKeyboardHook) {
        RemoveTrayIcon();
        MessageBoxW(nullptr,
            L"Не удалось зарегистрировать глобальный перехват клавиши Print Screen.\n"
            L"Проверьте, не запрещает ли это защитное ПО.",
            L"QR Screen", MB_OK | MB_ICONERROR);
        DestroyWindow(gMainWindow);
        CloseHandle(gSingleInstanceMutex);
        return 1;
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (gSingleInstanceMutex)
        CloseHandle(gSingleInstanceMutex);
    return static_cast<int>(msg.wParam);
}
