#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <strsafe.h>
#include <string>
#include <cwchar>
#include <cwctype>
#include <algorithm>
#include <dwmapi.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")

#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#define HOTKEY_ID         1
#define TRAY_ID           1
#define WM_TRAYICON       (WM_USER + 1)
#define WM_OPEN_SETTINGS  (WM_USER + 2)

#define IDM_SETTINGS      1001
#define IDM_EXIT          1002

#define IDC_AUTOSTART     2001
#define IDC_ENGINE_URL    2002
#define IDC_HOTKEY        2003
#define IDC_SAVE          2004
#define IDC_CLOSE         2005

#define IDI_APP_ICON      101

#define LAUNCHER_W        650
#define LAUNCHER_H        70

// ---------- Глобальные переменные ----------
HINSTANCE g_hInst;
HWND      g_hMain = NULL;
HWND      g_hLauncher = NULL;
HWND      g_hEdit = NULL;
HWND      g_hSettings = NULL;
WNDPROC   g_oldEditProc;
HBRUSH    g_bgBrush;
HBRUSH    g_editBrush;
HBRUSH    g_settingsBgBrush;
HFONT     g_hFont;
HFONT     g_hFontLarge;
NOTIFYICONDATAW g_nid = {};

bool g_isPlaceholder = true;

struct Config {
    bool         autostart = false;
    bool         configured = false;
    std::wstring engineUrl = L"https://www.google.com/search?q=%s";
    UINT         hotkeyMod = MOD_ALT;
    UINT         hotkeyVk = VK_SPACE;
};
Config g_cfg;

const wchar_t* REG_PATH = L"Software\\QuickFind";

// --- Конвертеры модификаторов ---
UINT ConvertHKModToRegMod(BYTE hkMod) {
    UINT mod = 0;
    if (hkMod & HOTKEYF_ALT) mod |= MOD_ALT;
    if (hkMod & HOTKEYF_CONTROL) mod |= MOD_CONTROL;
    if (hkMod & HOTKEYF_SHIFT) mod |= MOD_SHIFT;
    return mod;
}

BYTE ConvertRegModToHKMod(UINT regMod) {
    BYTE mod = 0;
    if (regMod & MOD_ALT) mod |= HOTKEYF_ALT;
    if (regMod & MOD_CONTROL) mod |= HOTKEYF_CONTROL;
    if (regMod & MOD_SHIFT) mod |= HOTKEYF_SHIFT;
    return mod;
}

// --- Загрузка/Сохранение ---
void LoadConfig() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        DWORD v, sz = sizeof(v);
        if (RegQueryValueExW(k, L"Autostart", NULL, NULL, (LPBYTE)&v, &sz) == ERROR_SUCCESS)
            g_cfg.autostart = v != 0;
        sz = sizeof(v);
        if (RegQueryValueExW(k, L"Configured", NULL, NULL, (LPBYTE)&v, &sz) == ERROR_SUCCESS)
            g_cfg.configured = v != 0;

        sz = sizeof(v);
        if (RegQueryValueExW(k, L"HotkeyMod", NULL, NULL, (LPBYTE)&v, &sz) == ERROR_SUCCESS)
            g_cfg.hotkeyMod = v;
        sz = sizeof(v);
        if (RegQueryValueExW(k, L"HotkeyVk", NULL, NULL, (LPBYTE)&v, &sz) == ERROR_SUCCESS)
            g_cfg.hotkeyVk = v;

        wchar_t buf[1024]; sz = sizeof(buf);
        if (RegQueryValueExW(k, L"EngineUrl", NULL, NULL, (LPBYTE)buf, &sz) == ERROR_SUCCESS)
            g_cfg.engineUrl = buf;
        RegCloseKey(k);
    }
}

void SaveConfig() {
    HKEY k;
    RegCreateKeyExW(HKEY_CURRENT_USER, REG_PATH, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL);
    DWORD v = g_cfg.autostart ? 1 : 0;
    RegSetValueExW(k, L"Autostart", 0, REG_DWORD, (BYTE*)&v, sizeof(v));
    v = g_cfg.configured ? 1 : 0;
    RegSetValueExW(k, L"Configured", 0, REG_DWORD, (BYTE*)&v, sizeof(v));
    RegSetValueExW(k, L"HotkeyMod", 0, REG_DWORD, (BYTE*)&g_cfg.hotkeyMod, sizeof(g_cfg.hotkeyMod));
    RegSetValueExW(k, L"HotkeyVk", 0, REG_DWORD, (BYTE*)&g_cfg.hotkeyVk, sizeof(g_cfg.hotkeyVk));
    RegSetValueExW(k, L"EngineUrl", 0, REG_SZ, (BYTE*)g_cfg.engineUrl.c_str(),
        (g_cfg.engineUrl.size() + 1) * sizeof(wchar_t));
    RegCloseKey(k);
}

void UpdateAutostart() {
    HKEY k;
    RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, NULL, 0, KEY_WRITE, NULL, &k, NULL);
    if (g_cfg.autostart) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(NULL, path, MAX_PATH);
        std::wstring q = L"\"" + std::wstring(path) + L"\"";
        RegSetValueExW(k, L"QuickFind", 0, REG_SZ, (BYTE*)q.c_str(), (q.size() + 1) * sizeof(wchar_t));
    }
    else {
        RegDeleteValueW(k, L"QuickFind");
    }
    RegCloseKey(k);
}

// --- Утилиты ---
std::wstring Trim(const std::wstring& str) {
    size_t first = str.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return L"";
    size_t last = str.find_last_not_of(L" \t\r\n");
    return str.substr(first, (last - first + 1));
}

bool StartsWithCaseInsensitive(const std::wstring& str, const std::wstring& prefix) {
    if (str.length() < prefix.length()) return false;
    for (size_t i = 0; i < prefix.length(); ++i) {
        if (towlower(str[i]) != towlower(prefix[i])) return false;
    }
    return true;
}

std::wstring UrlEncode(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        if (c == L' ') out += L"%20";
        else if ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || wcschr(L"-_.~", c)) out += c;
        else {
            char b[8]; int n = WideCharToMultiByte(CP_UTF8, 0, &c, 1, b, 8, NULL, NULL);
            for (int i = 0; i < n; i++) { wchar_t h[8]; swprintf_s(h, L"%%%02X", (unsigned char)b[i]); out += h; }
        }
    }
    return out;
}

std::wstring BuildSearchUrl(const std::wstring& query) {
    std::wstring url = g_cfg.engineUrl, enc = UrlEncode(query);
    size_t pos = url.find(L"%s");
    if (pos == std::wstring::npos) return url + enc;
    return url.substr(0, pos) + enc + url.substr(pos + 2);
}

// --- УЛУЧШЕННАЯ ПРОВЕРКА ССЫЛОК ---
bool IsUrl(const std::wstring& rawStr) {
    std::wstring str = Trim(rawStr);
    if (str.empty()) return false;

    // Если есть пробелы внутри (после обрезки краев) — это поисковый запрос
    if (str.find(L' ') != std::wstring::npos) return false;

    // 1. Проверяем явные протоколы (http, https, ftp и т.д.) без учета регистра
    if (StartsWithCaseInsensitive(str, L"http://") ||
        StartsWithCaseInsensitive(str, L"https://") ||
        StartsWithCaseInsensitive(str, L"ftp://") ||
        StartsWithCaseInsensitive(str, L"file://")) {
        return true;
    }

    // Любая другая схема с :// (например, myapp://)
    if (str.find(L"://") != std::wstring::npos) return true;

    // 2. Начинается с www.
    if (StartsWithCaseInsensitive(str, L"www.")) return true;

    // 3. Проверяем домен (наличие точки и буквенного окончания)
    size_t dotPos = str.rfind(L'.');
    if (dotPos != std::wstring::npos && dotPos > 0 && dotPos < str.length() - 1) {
        std::wstring tld = str.substr(dotPos + 1);

        // Отрезаем путь, если он есть (например, .com/path -> .com)
        size_t slashPos = tld.find_first_of(L"/\\?");
        if (slashPos != std::wstring::npos) {
            tld = tld.substr(0, slashPos);
        }
        // Отрезаем порт, если он есть (.com:8080 -> .com)
        size_t colonPos = tld.find(L':');
        if (colonPos != std::wstring::npos) {
            tld = tld.substr(0, colonPos);
        }

        // Доменные зоны бывают от 2 до 10 символов (например, .ru, .com, .software)
        if (tld.length() >= 2 && tld.length() <= 10) {
            bool allAlpha = true;
            for (wchar_t c : tld) {
                if (!iswalpha(c)) { allAlpha = false; break; }
            }
            if (allAlpha) return true;
        }
    }
    return false;
}

void ShowLauncher() {
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(g_hLauncher, HWND_TOPMOST, (sw - LAUNCHER_W) / 2, (sh - LAUNCHER_H) / 2, LAUNCHER_W, LAUNCHER_H, SWP_SHOWWINDOW);

    g_isPlaceholder = true;
    SetWindowTextW(g_hEdit, L"Поиск или вставка ссылки...");

    DWORD fg = GetWindowThreadProcessId(GetForegroundWindow(), NULL), me = GetCurrentThreadId();
    AttachThreadInput(me, fg, TRUE);
    SetForegroundWindow(g_hLauncher);
    SetFocus(g_hEdit);
    AttachThreadInput(me, fg, FALSE);
}

void HideLauncher() { ShowWindow(g_hLauncher, SW_HIDE); }

void DoSearch() {
    if (g_isPlaceholder) { HideLauncher(); return; }
    wchar_t buf[1024]; GetWindowTextW(g_hEdit, buf, 1024);

    std::wstring input = Trim(buf); // Очищаем от случайных пробелов по краям
    if (input.empty()) return;

    std::wstring finalUrl = L"";

    if (IsUrl(input)) {
        // Если это ссылка, но без протокола (например, "google.com"), добавляем https://
        if (input.find(L"://") == std::wstring::npos) {
            finalUrl = L"https://" + input;
        }
        else {
            finalUrl = input;
        }
    }
    else {
        finalUrl = BuildSearchUrl(input);
    }

    ShellExecuteW(NULL, L"open", finalUrl.c_str(), NULL, NULL, SW_SHOWNORMAL);
    HideLauncher();
}

// --- Обработчик ввода ---
LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN) {
        if (w == VK_ESCAPE) { HideLauncher(); return 0; }
        if (w == VK_RETURN) { DoSearch(); return 0; }

        // Ctrl + Space для отправки
        if (w == VK_SPACE && (GetKeyState(VK_CONTROL) & 0x8000)) {
            DoSearch();
            return 0;
        }
    }

    if (m == WM_CHAR) {
        if (w == L'\r' || w == L'\n' || w == 27) return 0;
        if (w == L' ' && (GetKeyState(VK_CONTROL) & 0x8000)) return 0;

        if (g_isPlaceholder) {
            if (w >= 32 || w == 8) {
                g_isPlaceholder = false;
                SetWindowTextW(h, L"");
            }
        }
    }

    if (m == WM_SETFOCUS && g_isPlaceholder) SendMessageW(h, EM_SETSEL, 0, -1);
    return CallWindowProcW(g_oldEditProc, h, m, w, l);
}

// --- Окно настроек ---
void OpenSettings() {
    if (g_hSettings && IsWindow(g_hSettings)) {
        ShowWindow(g_hSettings, SW_RESTORE);
    }
    else {
        RECT r = { 0, 0, 460, 300 };
        AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
        g_hSettings = CreateWindowExW(0, L"SettingsWindow", L"Настройки QuickFind",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
            CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
            NULL, NULL, g_hInst, NULL);
    }
    SetWindowPos(g_hSettings, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetWindowPos(g_hSettings, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(g_hSettings);
}

LRESULT CALLBACK SettingsProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    static HWND hAutostart, hEngineUrl, hHotkey, hSave, hClose;

    switch (m) {
    case WM_CREATE: {
        hAutostart = CreateWindowExW(0, L"BUTTON", L"Запускать при старте Windows",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 25, 25, 400, 24, h, (HMENU)IDC_AUTOSTART, g_hInst, NULL);
        SendMessage(hAutostart, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SendMessage(hAutostart, BM_SETCHECK, g_cfg.autostart ? BST_CHECKED : BST_UNCHECKED, 0);

        HWND hLbl = CreateWindowExW(0, L"STATIC", L"URL поиска (вставьте %s на место запроса):",
            WS_CHILD | WS_VISIBLE, 25, 65, 400, 20, h, NULL, g_hInst, NULL);
        SendMessage(hLbl, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        hEngineUrl = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 25, 95, 400, 28, h, (HMENU)IDC_ENGINE_URL, g_hInst, NULL);
        SendMessage(hEngineUrl, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SetWindowTextW(hEngineUrl, g_cfg.engineUrl.c_str());

        HWND hLblHk = CreateWindowExW(0, L"STATIC", L"Горячая клавиша:",
            WS_CHILD | WS_VISIBLE, 25, 135, 200, 20, h, NULL, g_hInst, NULL);
        SendMessage(hLblHk, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        hHotkey = CreateWindowExW(WS_EX_CLIENTEDGE, HOTKEY_CLASS, L"",
            WS_CHILD | WS_VISIBLE, 25, 155, 150, 28, h, (HMENU)IDC_HOTKEY, g_hInst, NULL);
        SendMessage(hHotkey, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        BYTE hkMod = ConvertRegModToHKMod(g_cfg.hotkeyMod);
        SendMessage(hHotkey, HKM_SETHOTKEY, MAKEWORD(g_cfg.hotkeyVk, hkMod), 0);

        hSave = CreateWindowExW(0, L"BUTTON", L"Сохранить", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            92, 210, 130, 36, h, (HMENU)IDC_SAVE, g_hInst, NULL);
        SendMessage(hSave, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        hClose = CreateWindowExW(0, L"BUTTON", L"Закрыть", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            237, 210, 130, 36, h, (HMENU)IDC_CLOSE, g_hInst, NULL);
        SendMessage(hClose, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        return 0;
    }

    case WM_ERASEBKGND: {
        RECT r; GetClientRect(h, &r);
        FillRect((HDC)w, &r, g_settingsBgBrush);
        return 1;
    }

    case WM_CTLCOLORSTATIC: {
        SetBkColor((HDC)w, RGB(255, 255, 255));
        SetTextColor((HDC)w, RGB(30, 30, 30));
        return (LRESULT)g_settingsBgBrush;
    }

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT lpDIS = (LPDRAWITEMSTRUCT)l;
        if (lpDIS->CtlID == IDC_SAVE || lpDIS->CtlID == IDC_CLOSE) {
            HDC hdc = lpDIS->hDC;
            RECT rc = lpDIS->rcItem;
            bool isSave = (lpDIS->CtlID == IDC_SAVE);
            bool isPressed = (lpDIS->itemState & ODS_SELECTED);

            COLORREF bgColor = isSave ? RGB(0, 103, 192) : RGB(240, 240, 240);
            COLORREF textColor = isSave ? RGB(255, 255, 255) : RGB(0, 0, 0);
            if (isPressed) bgColor = isSave ? RGB(0, 80, 160) : RGB(220, 220, 220);

            HBRUSH hBrush = CreateSolidBrush(bgColor);
            HPEN hPen = CreatePen(PS_SOLID, 1, isSave ? bgColor : RGB(200, 200, 200));
            HGDIOBJ oldBrush = SelectObject(hdc, hBrush);
            HGDIOBJ oldPen = SelectObject(hdc, hPen);

            RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 8, 8);

            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, textColor);
            wchar_t text[64];
            GetWindowTextW(lpDIS->hwndItem, text, 64);
            DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(hBrush); DeleteObject(hPen);
            return TRUE;
        }
        break;
    }

    case WM_COMMAND:
        if (LOWORD(w) == IDC_SAVE) {
            bool as = SendMessage(hAutostart, BM_GETCHECK, 0, 0) == BST_CHECKED;
            wchar_t url[1024]; GetWindowTextW(hEngineUrl, url, 1024);

            WORD hk = (WORD)SendMessage(hHotkey, HKM_GETHOTKEY, 0, 0);
            UINT newVk = LOBYTE(hk);
            BYTE hkMod = HIBYTE(hk);

            UINT newMod = ConvertHKModToRegMod(hkMod);

            if (wcslen(url) > 0 && wcsstr(url, L"%s")) {
                UnregisterHotKey(g_hMain, HOTKEY_ID);
                if (RegisterHotKey(g_hMain, HOTKEY_ID, newMod, newVk)) {
                    g_cfg.autostart = as;
                    g_cfg.engineUrl = url;
                    g_cfg.hotkeyMod = newMod;
                    g_cfg.hotkeyVk = newVk;
                    g_cfg.configured = true;
                    SaveConfig(); UpdateAutostart();
                    MessageBoxW(h, L"Настройки успешно сохранены!", L"QuickFind", MB_OK | MB_ICONINFORMATION);
                }
                else {
                    RegisterHotKey(g_hMain, HOTKEY_ID, g_cfg.hotkeyMod, g_cfg.hotkeyVk);
                    MessageBoxW(h, L"Не удалось назначить эту горячую клавишу.\nВозможно, она уже занята.", L"Ошибка", MB_OK | MB_ICONERROR);
                }
            }
            else {
                MessageBoxW(h, L"URL должен содержать %s.\nПример: https://www.google.com/search?q=%s", L"Ошибка", MB_OK | MB_ICONWARNING);
            }
        }
        if (LOWORD(w) == IDC_CLOSE) DestroyWindow(h);
        return 0;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY:
        if (!g_cfg.configured) { g_cfg.configured = true; SaveConfig(); }
        g_hSettings = NULL; return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// --- Диспетчер (трей и хоткей) ---
LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_OPEN_SETTINGS: OpenSettings(); return 0;
    case WM_HOTKEY:
        if (w == HOTKEY_ID) { IsWindowVisible(g_hLauncher) ? HideLauncher() : ShowLauncher(); }
        return 0;
    case WM_TRAYICON:
        if (LOWORD(l) == WM_LBUTTONUP) OpenSettings();
        else if (LOWORD(l) == WM_RBUTTONUP) {
            POINT pt; GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"Настройки");
            AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Выход");
            SetForegroundWindow(h);
            int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, NULL);
            DestroyMenu(menu); PostMessage(h, WM_NULL, 0, 0);
            if (cmd == IDM_SETTINGS) OpenSettings();
            else if (cmd == IDM_EXIT) PostQuitMessage(0);
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// --- Окно поиска ---
LRESULT CALLBACK LauncherProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        HRGN hRgn = CreateRoundRectRgn(0, 0, LAUNCHER_W + 1, LAUNCHER_H + 1, 16, 16);
        SetWindowRgn(h, hRgn, TRUE);

        g_hEdit = CreateWindowExW(0, L"EDIT", L"Поиск или вставка ссылки...",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            20, 20, LAUNCHER_W - 40, LAUNCHER_H - 40, h, NULL, NULL, NULL);

        SendMessageW(g_hEdit, WM_SETFONT, (WPARAM)g_hFontLarge, TRUE);
        g_oldEditProc = (WNDPROC)SetWindowLongPtrW(g_hEdit, GWLP_WNDPROC, (LONG_PTR)EditProc);
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        SetBkMode((HDC)w, TRANSPARENT);
        SetTextColor((HDC)w, RGB(220, 220, 220));
        return (LRESULT)g_editBrush;
    }
    case WM_ERASEBKGND: {
        RECT r; GetClientRect(h, &r);
        HDC hdc = (HDC)w;
        FillRect(hdc, &r, g_bgBrush);

        HPEN hPen = CreatePen(PS_SOLID, 1, RGB(200, 200, 200));
        HGDIOBJ oldPen = SelectObject(hdc, hPen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        RoundRect(hdc, 0, 0, r.right, r.bottom, 16, 16);
        SelectObject(hdc, oldPen); SelectObject(hdc, oldBrush);
        DeleteObject(hPen);
        return 1;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

// --- Точка входа ---
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    g_hInst = hi;

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_HOTKEY_CLASS };
    InitCommonControlsEx(&icc);

    CreateMutexW(NULL, TRUE, L"QuickFindMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND hOld = FindWindowW(L"QuickFindHidden", NULL);
        if (hOld) PostMessageW(hOld, WM_OPEN_SETTINGS, 0, 0);
        return 0;
    }

    LoadConfig();

    g_hFont = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_hFontLarge = CreateFontW(-24, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

    g_bgBrush = CreateSolidBrush(RGB(30, 30, 30));
    g_editBrush = CreateSolidBrush(RGB(30, 30, 30));
    g_settingsBgBrush = CreateSolidBrush(RGB(255, 255, 255));

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm = LoadIconW(hi, MAKEINTRESOURCEW(IDI_APP_ICON));

    wc.lpfnWndProc = LauncherProc;
    wc.lpszClassName = L"QuickFindLauncher";
    wc.style = CS_DROPSHADOW;
    RegisterClassExW(&wc);

    wc.lpfnWndProc = MainProc;
    wc.lpszClassName = L"QuickFindHidden";
    wc.style = 0;
    RegisterClassExW(&wc);

    wc.lpfnWndProc = SettingsProc;
    wc.lpszClassName = L"SettingsWindow";
    wc.style = 0;
    RegisterClassExW(&wc);

    g_hMain = CreateWindowExW(0, L"QuickFindHidden", L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, hi, NULL);
    g_hLauncher = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"QuickFindLauncher", L"", WS_POPUP,
        0, 0, LAUNCHER_W, LAUNCHER_H, NULL, NULL, hi, NULL);

    if (!RegisterHotKey(g_hMain, HOTKEY_ID, g_cfg.hotkeyMod, g_cfg.hotkeyVk)) {
        RegisterHotKey(g_hMain, HOTKEY_ID, MOD_ALT, VK_SPACE);
        g_cfg.hotkeyMod = MOD_ALT;
        g_cfg.hotkeyVk = VK_SPACE;
    }

    g_nid.cbSize = sizeof(g_nid); g_nid.hWnd = g_hMain; g_nid.uID = TRAY_ID;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(IDI_APP_ICON));

    wchar_t tip[128];
    swprintf_s(tip, L"QuickFind (%s)",
        (g_cfg.hotkeyMod == MOD_ALT && g_cfg.hotkeyVk == VK_SPACE) ? L"Alt+Space" : L"Своя клавиша");
    StringCchCopyW(g_nid.szTip, 128, tip);

    Shell_NotifyIconW(NIM_ADD, &g_nid);

    if (!g_cfg.configured) PostMessageW(g_hMain, WM_OPEN_SETTINGS, 0, 0);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    DeleteObject(g_hFont); DeleteObject(g_hFontLarge);
    DeleteObject(g_bgBrush); DeleteObject(g_editBrush); DeleteObject(g_settingsBgBrush);
    return 0;
}