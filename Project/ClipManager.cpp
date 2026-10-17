#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "resource.h"
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <shlobj.h>
#include <winreg.h>
#include <string>
#include <vector>
#include <deque>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <climits>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "Advapi32.lib")

// ClipManager: a small Windows clipboard history utility.
// Hold Ctrl and tap V: each V press/release advances one item. Releasing both
// Ctrl and V commits and pastes the selected item. Esc/Ctrl+Z cancels.

static constexpr wchar_t kClassName[] = L"ClipManagerHiddenWindow";
static constexpr wchar_t kMainClassName[] = L"ClipManagerMainScreenWindow";


// Support and bug reporting.
static constexpr wchar_t kTelegramChannelUrl[] = L"https://t.me/MyTelegramRepository";
static constexpr wchar_t kBugReportUrl[] = L"https://t.me/MyTelegramRepository/2";

static constexpr wchar_t kRunRegistryPath[] =
L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static constexpr wchar_t kRunRegistryValueName[] = L"ClipManager";

enum : int {
    ID_PICKER_LIST = 100,
    ID_PICKER_HINT = 101,
    ID_MAIN_TELEGRAM = 201,
    ID_MAIN_BUG_REPORT = 202,
    ID_MAIN_STARTUP = 203,
    ID_MAIN_START = 204,
    ID_MAIN_CLOSE = 205
};
static constexpr size_t kMaxItems = 100;
static constexpr size_t kMaxPayload = 32 * 1024 * 1024;

enum class Kind { Text, Files, Image };
struct Item {
    Kind kind{};
    std::wstring preview;
    std::wstring sizeText;
    std::wstring text;
    std::vector<std::wstring> files;
    std::vector<BYTE> imageDib;
    DWORD imageFormat = CF_DIB;
    ULONGLONG stamp = 0;
};

static HINSTANCE g_instance;
static HWND g_hwnd = nullptr, g_list = nullptr;
static HWND g_pickerHint = nullptr;
static HWND g_mainHwnd = nullptr;
static HWND g_startupCheckbox = nullptr;
static HWND g_previousForegroundWindow = nullptr;
static HHOOK g_hook = nullptr;
static std::deque<Item> g_history;
static bool g_pickerOpen = false, g_vDown = false, g_ownClipboardWrite = false;
static bool g_openedOnCurrentTap = false;
static bool g_ctrlDown = false, g_leftCtrlDown = false, g_rightCtrlDown = false, g_genericCtrlDown = false;
static bool g_commitPending = false;
static bool g_suppressZUntilKeyUp = false;
static bool g_suppressAltUntilKeyUp = false;
static bool g_suppressMUntilKeyUp = false;
static bool g_openMainRequested = false;
static HWND g_targetWindow = nullptr;
static int g_selected = 0;
static DWORD g_lastClipboardSequence = 0;
static HFONT g_font = nullptr;
static HFONT g_hintFont = nullptr;
static HFONT g_mainFont = nullptr;
static HFONT g_mainTitleFont = nullptr;

static std::wstring FormatBytes(size_t bytes) {
    std::wostringstream out;
    if (bytes < 1024) out << bytes << L" B";
    else if (bytes < 1024 * 1024) out << std::fixed << std::setprecision(1) << (bytes / 1024.0) << L" KB";
    else if (bytes < 1024ull * 1024 * 1024) out << std::fixed << std::setprecision(1) << (bytes / (1024.0 * 1024)) << L" MB";
    else out << std::fixed << std::setprecision(1) << (bytes / (1024.0 * 1024 * 1024)) << L" GB";
    return out.str();
}

static std::wstring ClipText(std::wstring s, size_t maxChars = 110) {
    for (auto& c : s) if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
    if (s.size() > maxChars) { s.resize(maxChars > 1 ? maxChars - 1 : maxChars); s += L"\x2026"; }
    return s;
}

static bool ReadClipboardText(Item& item) {
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return false;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (!h) return false;
    const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h));
    if (!p) return false;
    item.text.assign(p);
    GlobalUnlock(h);
    if (item.text.empty()) return false;
    item.kind = Kind::Text;
    item.preview = ClipText(item.text);
    item.sizeText = FormatBytes(item.text.size() * sizeof(wchar_t));
    return true;
}

static bool ReadClipboardFiles(Item& item) {
    if (!IsClipboardFormatAvailable(CF_HDROP)) return false;
    HDROP drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
    if (!drop) return false;
    UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    if (!count) return false;
    size_t total = 0;
    for (UINT i = 0; i < count; ++i) {
        UINT len = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring path(len + 1, L'\0');
        DragQueryFileW(drop, i, path.data(), len + 1);
        path.resize(len);
        total += path.size() * sizeof(wchar_t);
        item.files.push_back(std::move(path));
    }
    item.kind = Kind::Files;
    // Show only each item's basename, never its parent directories. If a path
    // has no basename (for example, it ends in a slash), show its item type.
    std::wstring names;
    for (const auto& path : item.files) {
        size_t slash = path.find_last_of(L"\\/");
        std::wstring name = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
        if (name.empty()) {
            DWORD attrs = path.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(path.c_str());
            name = (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
                ? L"Folder" : L"File";
        }
        if (!names.empty()) names += L", ";
        names += name;
    }
    item.preview = ClipText(names.empty() ? L"File" : names);
    item.sizeText = FormatBytes(total);
    return true;
}

static bool ReadClipboardImage(Item& item) {
    UINT fmt = IsClipboardFormatAvailable(CF_DIBV5) ? CF_DIBV5 : CF_DIB;
    if (!IsClipboardFormatAvailable(fmt)) return false;
    HANDLE h = GetClipboardData(fmt);
    if (!h) return false;
    SIZE_T bytes = GlobalSize(h);
    if (!bytes || bytes > kMaxPayload) return false;
    void* p = GlobalLock(h);
    if (!p) return false;
    item.imageDib.assign(static_cast<BYTE*>(p), static_cast<BYTE*>(p) + bytes);
    GlobalUnlock(h);
    item.kind = Kind::Image;
    item.imageFormat = fmt;
    item.preview = L"Image";
    item.sizeText = FormatBytes(item.imageDib.size());
    return true;
}

static void RefreshList();

static void CaptureClipboard() {
    if (g_ownClipboardWrite || !OpenClipboard(g_hwnd)) return;
    Item item;
    bool ok = ReadClipboardImage(item);
    if (!ok) ok = ReadClipboardFiles(item);
    if (!ok) ok = ReadClipboardText(item);
    CloseClipboard();
    if (!ok) return;

    // Skip an identical newest item to avoid duplicate notifications.
    if (!g_history.empty()) {
        const Item& last = g_history.front();
        bool same = last.kind == item.kind;
        if (same && item.kind == Kind::Text) same = last.text == item.text;
        else if (same && item.kind == Kind::Files) same = last.files == item.files;
        else if (same && item.kind == Kind::Image) same = last.imageDib == item.imageDib;
        if (same) return;
    }
    item.stamp = GetTickCount64();
    g_history.push_front(std::move(item));
    while (g_history.size() > kMaxItems) g_history.pop_back();
    if (g_pickerOpen) {
        if (g_selected < INT_MAX) ++g_selected; // preserve the same item as new entries arrive
        RefreshList();
    }
}

static bool WriteHDrop(const Item& item) {
    size_t chars = 1;
    for (const auto& f : item.files) chars += f.size() + 1;
    SIZE_T bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GHND | GMEM_SHARE, bytes);
    if (!h) return false;
    auto* df = static_cast<DROPFILES*>(GlobalLock(h));
    if (!df) { GlobalFree(h); return false; }
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    wchar_t* out = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(df) + sizeof(DROPFILES));
    for (const auto& f : item.files) {
        memcpy(out, f.c_str(), (f.size() + 1) * sizeof(wchar_t));
        out += f.size() + 1;
    }
    *out = L'\0';
    GlobalUnlock(h);
    if (!SetClipboardData(CF_HDROP, h)) { GlobalFree(h); return false; }
    return true;
}

static bool WriteDib(const Item& item) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, item.imageDib.size());
    if (!h) return false;
    void* p = GlobalLock(h);
    if (!p) { GlobalFree(h); return false; }
    memcpy(p, item.imageDib.data(), item.imageDib.size());
    GlobalUnlock(h);
    if (!SetClipboardData(item.imageFormat, h)) { GlobalFree(h); return false; }
    return true;
}

static bool RestoreClipboard(const Item& item) {
    if (!OpenClipboard(g_hwnd)) return false;
    EmptyClipboard();
    bool ok = false;
    if (item.kind == Kind::Text) {
        SIZE_T bytes = (item.text.size() + 1) * sizeof(wchar_t);
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (h) {
            void* p = GlobalLock(h);
            if (p) {
                memcpy(p, item.text.c_str(), bytes);
                GlobalUnlock(h);
                ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
                if (!ok) GlobalFree(h);
            }
            else GlobalFree(h);
        }
    }
    else if (item.kind == Kind::Files) ok = WriteHDrop(item);
    else if (item.kind == Kind::Image) ok = WriteDib(item);
    g_ownClipboardWrite = true;
    CloseClipboard();
    // Clipboard listener notifications arrive asynchronously; reset after the queue drains.
    PostMessageW(g_hwnd, WM_APP + 2, 0, 0);
    return ok;
}

static void PasteTarget() {
    if (g_history.empty()) return;
    int index = std::max(0, std::min(g_selected, static_cast<int>(g_history.size()) - 1));
    Item item = g_history[index];
    HWND target = g_targetWindow;
    if (!RestoreClipboard(item)) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    g_pickerOpen = false;
    g_openedOnCurrentTap = false;
    ShowWindow(g_hwnd, SW_HIDE);
    g_commitPending = false;
    if (target && IsWindow(target)) {
        SetForegroundWindow(target);
        Sleep(35);
        // Commit happens only after both physical keys are released, so synthesize
        // a complete Ctrl+V chord for the target application.
        INPUT paste[4]{};
        paste[0].type = INPUT_KEYBOARD;
        paste[0].ki.wVk = VK_CONTROL;
        paste[1].type = INPUT_KEYBOARD;
        paste[1].ki.wVk = 'V';
        paste[2].type = INPUT_KEYBOARD;
        paste[2].ki.wVk = 'V';
        paste[2].ki.dwFlags = KEYEVENTF_KEYUP;
        paste[3].type = INPUT_KEYBOARD;
        paste[3].ki.wVk = VK_CONTROL;
        paste[3].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(4, paste, sizeof(INPUT));
    }
}

static void QueueCommit() {
    if (!g_pickerOpen ||
        g_history.empty() ||
        g_commitPending ||
        g_ctrlDown ||
        g_vDown) {
        return;
    }
    g_commitPending = true;
    PostMessageW(g_hwnd, WM_APP + 3, 0, 0);
}

static void CancelPicker() {
    g_pickerOpen = false;
    g_commitPending = false;
    g_openedOnCurrentTap = false;
    ShowWindow(g_hwnd, SW_HIDE);
}

static void ShowMainScreen();

static bool IsRunAtStartupEnabled() {
    HKEY key = nullptr;
    LONG status = RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegistryPath, 0,
        KEY_QUERY_VALUE, &key);
    if (status != ERROR_SUCCESS) return false;

    DWORD type = 0;
    DWORD bytes = 0;
    status = RegQueryValueExW(key, kRunRegistryValueName, nullptr, &type, nullptr, &bytes);
    RegCloseKey(key);
    return status == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ);
}

static LONG SetRunAtStartup(bool enabled) {
    HKEY key = nullptr;
    LONG status = ERROR_SUCCESS;

    if (enabled) {
        status = RegCreateKeyExW(HKEY_CURRENT_USER, kRunRegistryPath, 0, nullptr,
            REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr);
        if (status != ERROR_SUCCESS) return status;

        wchar_t exePath[MAX_PATH]{};
        DWORD pathLength = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (pathLength == 0 || pathLength >= MAX_PATH) {
            RegCloseKey(key);
            return ERROR_INSUFFICIENT_BUFFER;
        }

        std::wstring command = L"\"" + std::wstring(exePath, pathLength) + L"\" --background";
        status = RegSetValueExW(key, kRunRegistryValueName, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
        return status;
    }

    status = RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegistryPath, 0, KEY_SET_VALUE, &key);
    if (status == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
    if (status != ERROR_SUCCESS) return status;

    status = RegDeleteValueW(key, kRunRegistryValueName);
    RegCloseKey(key);
    return status == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : status;
}

static void OpenConfiguredUrl(HWND owner, const wchar_t* url, const wchar_t* settingName) {
    if (!url || !url[0]) {
        std::wstring message = L"Set ";
        message += settingName;
        message += L" near the top of main.cpp to configure this link.";
        MessageBoxW(owner, message.c_str(), L"ClipManager", MB_OK | MB_ICONINFORMATION);
        return;
    }

    HINSTANCE result = ShellExecuteW(owner, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        MessageBoxW(owner, L"Windows could not open this link.", L"ClipManager",
            MB_OK | MB_ICONWARNING);
    }
}

static void HideMainScreen(HWND hwnd) {
    ShowWindow(hwnd, SW_HIDE);
    if (g_previousForegroundWindow && IsWindow(g_previousForegroundWindow))
        SetForegroundWindow(g_previousForegroundWindow);
}

static void ShowMainScreen() {
    if (!g_mainHwnd || !IsWindow(g_mainHwnd)) return;

    HWND foreground = GetForegroundWindow();
    if (g_pickerOpen && g_targetWindow && IsWindow(g_targetWindow) &&
        g_targetWindow != g_hwnd && g_targetWindow != g_mainHwnd) {
        g_previousForegroundWindow = g_targetWindow;
    }
    else if (foreground && foreground != g_hwnd && foreground != g_mainHwnd) {
        g_previousForegroundWindow = foreground;
    }

    if (g_pickerOpen) CancelPicker();

    if (!IsWindowVisible(g_mainHwnd)) {
        RECT workArea{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
        const int width = 500;
        const int height = 500;
        int x = workArea.left + ((workArea.right - workArea.left) - width) / 2;
        int y = workArea.top + ((workArea.bottom - workArea.top) - height) / 2;
        x = std::max(static_cast<int>(workArea.left), x);
        y = std::max(static_cast<int>(workArea.top), y);
        SetWindowPos(g_mainHwnd, HWND_TOP, x, y, width, height, SWP_SHOWWINDOW);
    }
    else {
        ShowWindow(g_mainHwnd, SW_RESTORE);
        SetWindowPos(g_mainHwnd, HWND_TOP, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    }
    SetForegroundWindow(g_mainHwnd);
    SetActiveWindow(g_mainHwnd);
    SetFocus(g_mainHwnd);
}

static void ShowPicker() {
    if (g_history.empty()) CaptureClipboard();
    if (g_history.empty()) { MessageBeep(MB_ICONASTERISK); return; }
    g_targetWindow = GetForegroundWindow();
    if (g_targetWindow == g_hwnd) g_targetWindow = GetWindow(g_hwnd, GW_OWNER);
    g_selected = 0;
    g_pickerOpen = true;
    RefreshList();

    POINT p; GetCursorPos(&p);
    int x = p.x + 12, y = p.y + 16;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    if (x + 560 > sw) x = std::max(0, sw - 570);
    if (y + 340 > sh) y = std::max(0, sh - 350);
    SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, 560, 330, SWP_SHOWWINDOW);
    SetForegroundWindow(g_hwnd);
    SetFocus(g_list);
    ListView_SetItemState(g_list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}

static void MoveSelection(int delta) {
    if (!g_pickerOpen || g_history.empty()) return;
    g_selected = (g_selected + delta + static_cast<int>(g_history.size())) % static_cast<int>(g_history.size());
    ListView_SetItemState(g_list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemState(g_list, g_selected, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(g_list, g_selected, FALSE);
}

static LRESULT CALLBACK KeyboardProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const KBDLLHOOKSTRUCT* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        // Ignore injected keystrokes from SendInput so our synthetic paste cannot
        // re-enter the Ctrl+V state machine.
        if (k->flags & LLKHF_INJECTED) return CallNextHookEx(g_hook, code, wp, lp);

        bool down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
        bool up = (wp == WM_KEYUP || wp == WM_SYSKEYUP);

        // Track left/right Ctrl separately so releasing one Ctrl key does not
        // end the selection session while the other Ctrl key is still held.
        if (k->vkCode == VK_LCONTROL || k->vkCode == VK_RCONTROL || k->vkCode == VK_CONTROL) {
            if (k->vkCode == VK_LCONTROL) {
                if (down) g_leftCtrlDown = true;
                if (up) g_leftCtrlDown = false;
            }
            else if (k->vkCode == VK_RCONTROL) {
                if (down) g_rightCtrlDown = true;
                if (up) g_rightCtrlDown = false;
            }
            else {
                // Fallback for drivers that report generic VK_CONTROL.
                if (down) g_genericCtrlDown = true;
                if (up) g_genericCtrlDown = false;
            }
            g_ctrlDown = g_leftCtrlDown || g_rightCtrlDown || g_genericCtrlDown;
            if (up && !g_ctrlDown && g_pickerOpen && !g_vDown && !g_openMainRequested) QueueCommit();
            return CallNextHookEx(g_hook, code, wp, lp);
        }

        //Consume both M key event
        // Ctrl+M opens the Main Screen only while the picker is open.
        if (k->vkCode == 'M') {
            if (g_suppressMUntilKeyUp) {
                if (up)
                    g_suppressMUntilKeyUp = false;

                return 1;
            }

            if (down && g_ctrlDown && g_pickerOpen) {
                g_suppressMUntilKeyUp = true;
                g_openMainRequested = true;

                if (!PostMessageW(g_hwnd, WM_APP + 4, 0, 0))
                    g_openMainRequested = false;

                return 1;
            }
        }

        // Ctrl+Z cancels the current selection without pasting.
        if (k->vkCode == 'Z') {
            if (down && g_ctrlDown && g_pickerOpen) {
                g_suppressZUntilKeyUp = true;
                CancelPicker();
                return 1;
            }

            // Consume the matching key-up and any repeated key-down events.
            if (g_suppressZUntilKeyUp) {
                if (up)
                    g_suppressZUntilKeyUp = false;

                return 1;
            }
        }

        // Ctrl+Alt clears the clipboard history.
        const bool isAltKey =
            k->vkCode == VK_MENU ||
            k->vkCode == VK_LMENU ||
            k->vkCode == VK_RMENU;

        if (isAltKey) {
            if (down && g_ctrlDown && g_pickerOpen) {
                // Clear only once for the current Alt press.
                if (!g_suppressAltUntilKeyUp) {
                    g_history.clear();
                    g_selected = 0;
                    g_commitPending = false;

                    // Immediately update the table.
                    RefreshList();
                    CancelPicker();
                }

                g_suppressAltUntilKeyUp = true;
                return 1;
            }

            // Consume the matching Alt key-up to avoid activating
            // Windows menus or leaving the shortcut half-handled.
            if (g_suppressAltUntilKeyUp) {
                if (up)
                    g_suppressAltUntilKeyUp = false;

                return 1;
            }
        }
        if (k->vkCode == VK_UP && down && g_pickerOpen) { MoveSelection(-1); return 1; }
        if (k->vkCode == VK_DOWN && down && g_pickerOpen) { MoveSelection(1); return 1; }
        if (k->vkCode == 'V') {
            if (down) {
                // Suppress V repeat events while a Ctrl+V tap is in progress;
                // navigation happens only once, on the matching key-up.
                if (g_vDown) return 1;
                if (g_ctrlDown) {
                    g_vDown = true;
                    g_openedOnCurrentTap = !g_pickerOpen;
                    if (g_openedOnCurrentTap) ShowPicker();
                    // The initial Ctrl+V tap opens the picker; later V taps navigate.
                    return 1; // keep the original Ctrl+V away from the target
                }
            }
            if (up && g_vDown) {
                g_vDown = false;
                bool wasOpeningTap = g_openedOnCurrentTap;
                g_openedOnCurrentTap = false;
                if (g_pickerOpen) {
                    if (g_ctrlDown) {
                        if (!wasOpeningTap) MoveSelection(1); // one item per navigation tap
                    }
                    else QueueCommit(); // both keys are now up
                }
                return 1;
            }
        }
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

static bool IsImageFilePath(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    const size_t dot = path.find_last_of(L'.');

    // Ensure the extension belongs to the filename, not a directory.
    if (dot == std::wstring::npos ||
        (slash != std::wstring::npos && dot < slash)) {
        return false;
    }

    const std::wstring ext = path.substr(dot);

    // Common raster, vector, animated, graphics, and RAW image formats.
    static const wchar_t* const imageExtensions[] = {
        // Common raster formats
        L".png", L".apng",
        L".jpg", L".jpeg", L".jpe", L".jfif",
        L".gif",
        L".bmp", L".dib", L".rle",
        L".tif", L".tiff",
        L".webp",
        L".avif",
        L".heic", L".heif", L".hif",
        L".jxl",

        // JPEG 2000 and related formats
        L".jp2", L".j2k", L".jpf", L".jpx",
        L".jpm", L".mj2", L".jng", L".mng",

        // Older and specialized raster formats
        L".pcx", L".tga", L".targa",
        L".dds", L".qoi",
        L".pbm", L".pgm", L".ppm", L".pnm", L".pam", L".pfm",
        L".xbm", L".xpm", L".xwd",
        L".ras", L".sun",
        L".sgi", L".rgb", L".rgba", L".bw",
        L".iff", L".ilbm", L".lbm",
        L".pict", L".pct", L".pic", L".mac", L".pntg",
        L".wbmp",
        L".exr", L".hdr",
        L".fits", L".fit", L".fts",
        L".ktx", L".ktx2", L".astc",

        // Icons and cursor images
        L".ico", L".cur", L".icns",

        // Vector and illustration formats
        L".svg", L".svgz",
        L".eps", L".ai",
        L".emf", L".wmf",

        // Image-editing and graphics project formats
        L".psd", L".psb",
        L".xcf",
        L".kra", L".ora",

        // Camera RAW formats
        L".raw", L".dng",
        L".crw", L".cr2", L".cr3",
        L".nef", L".nrw",
        L".arw", L".srf", L".sr2",
        L".orf",
        L".rw2", L".rwl",
        L".raf",
        L".pef", L".ptx",
        L".srw",
        L".3fr", L".ari", L".bay",
        L".cap", L".dcs", L".dcr", L".drf",
        L".eip", L".erf", L".fff",
        L".gpr", L".iiq",
        L".k25", L".kdc",
        L".mdc", L".mef", L".mos", L".mrw",
        L".obm", L".pxn", L".r3d",
        L".sti", L".x3f"
    };

    // Compare extensions case-insensitively.
    for (const wchar_t* imageExt : imageExtensions) {
        if (CompareStringOrdinal(
            ext.c_str(), -1,
            imageExt, -1,
            TRUE) == CSTR_EQUAL) {
            return true;
        }
    }

    return false;
}

static void RefreshList() {
    if (!g_list) return;

    ListView_DeleteAllItems(g_list);

    for (int i = 0; i < static_cast<int>(g_history.size()); ++i) {
        const Item& it = g_history[i];

        // Order column
        std::wstring order = std::to_wstring(i + 1);

        LVITEMW row{};
        row.mask = LVIF_TEXT;
        row.iItem = i;
        row.iSubItem = 0;
        row.pszText = order.data();

        int idx = ListView_InsertItem(g_list, &row);

        // Type column
        std::wstring type;

        switch (it.kind) {
        case Kind::Text:
            type = L"Text";
            break;

        case Kind::Files: {
            const bool allImages =
                !it.files.empty() &&
                std::all_of(
                    it.files.begin(),
                    it.files.end(),
                    IsImageFilePath
                );

            type = allImages ? L"Image" : L"Files";
            break;
        }

        case Kind::Image:
            type = L"Image";
            break;
        }

        // Preview and Size columns
        std::wstring preview = it.preview;
        std::wstring size = it.sizeText;

        ListView_SetItemText(g_list, idx, 1, preview.data());
        ListView_SetItemText(g_list, idx, 2, type.data());
        ListView_SetItemText(g_list, idx, 3, size.data());
    }

    // Preserve the selected item and its highlight.
    if (!g_history.empty()) {
        g_selected = std::max(
            0,
            std::min(
                g_selected,
                static_cast<int>(g_history.size()) - 1
            )
        );

        ListView_SetItemState(
            g_list,
            g_selected,
            LVIS_SELECTED | LVIS_FOCUSED,
            LVIS_SELECTED | LVIS_FOCUSED
        );
    }
}

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            8, 8, 540, 270, hwnd, reinterpret_cast<HMENU>(ID_PICKER_LIST), g_instance, nullptr);
        ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;

        col.pszText = const_cast<wchar_t*>(L"Order");
        col.cx = 55;
        ListView_InsertColumn(g_list, 0, &col);

        col.pszText = const_cast<wchar_t*>(L"Preview");
        col.cx = 340;
        ListView_InsertColumn(g_list, 1, &col);

        col.pszText = const_cast<wchar_t*>(L"Type");
        col.cx = 70;
        ListView_InsertColumn(g_list, 2, &col);

        col.pszText = const_cast<wchar_t*>(L"Size");
        col.cx = 70;
        ListView_InsertColumn(g_list, 3, &col);

        g_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        g_hintFont = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        SendMessageW(g_list, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        g_pickerHint = CreateWindowExW(0, L"STATIC", L"Ctrl+M \u2014 Open Main Screen",
            WS_CHILD | WS_VISIBLE | SS_CENTER, 8, 286, 540, 22, hwnd,
            reinterpret_cast<HMENU>(ID_PICKER_HINT), g_instance, nullptr);
        if (g_pickerHint && g_hintFont)
            SendMessageW(g_pickerHint, WM_SETFONT, reinterpret_cast<WPARAM>(g_hintFont), TRUE);
        DragAcceptFiles(hwnd, TRUE);
        return 0;
    }
    case WM_CLIPBOARDUPDATE:
        if (!g_ownClipboardWrite) CaptureClipboard();
        return 0;
    case WM_APP + 2:
        g_ownClipboardWrite = false;
        g_lastClipboardSequence = GetClipboardSequenceNumber();
        return 0;
    case WM_APP + 3:
        if (g_commitPending && g_pickerOpen && !g_ctrlDown && !g_vDown) PasteTarget();
        else g_commitPending = false;
        return 0;
    case WM_APP + 4:
        g_openMainRequested = false;
        ShowMainScreen();
        return 0;
    case WM_APP + 5:
        DestroyWindow(hwnd);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { CancelPicker(); return 0; }
        if (wp == VK_UP) { MoveSelection(-1); return 0; }
        if (wp == VK_DOWN) { MoveSelection(1); return 0; }
        break;
    case WM_NOTIFY: {
        auto* hdr = reinterpret_cast<NMHDR*>(lp);
        if (hdr->hwndFrom == g_list && hdr->code == LVN_ITEMCHANGED) {
            int selected = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
            if (selected >= 0) g_selected = selected;
        }
        break;
    }
    case WM_SIZE: {
        const int clientWidth = LOWORD(lp);
        const int clientHeight = HIWORD(lp);
        if (g_list)
            MoveWindow(g_list, 8, 8, std::max(0, clientWidth - 16),
                std::max(40, clientHeight - 42), TRUE);
        if (g_pickerHint)
            MoveWindow(g_pickerHint, 8, std::max(8, clientHeight - 28),
                std::max(0, clientWidth - 16), 20, TRUE);
        return 0;
    }
    case WM_CLOSE: CancelPicker(); return 0;
    case WM_DESTROY:
        RemoveClipboardFormatListener(hwnd);
        if (g_hook) {
            UnhookWindowsHookEx(g_hook);
            g_hook = nullptr;
        }
        if (g_mainHwnd && IsWindow(g_mainHwnd)) DestroyWindow(g_mainHwnd);
        g_mainHwnd = nullptr;
        if (g_font) { DeleteObject(g_font); g_font = nullptr; }
        if (g_hintFont) { DeleteObject(g_hintFont); g_hintFont = nullptr; }
        if (g_mainFont) { DeleteObject(g_mainFont); g_mainFont = nullptr; }
        if (g_mainTitleFont) { DeleteObject(g_mainTitleFont); g_mainTitleFont = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK MainWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        auto makeControl = [&](DWORD exStyle, const wchar_t* klass, const wchar_t* text,
            DWORD style, int x, int y, int width, int height,
            int id, HFONT font) -> HWND {
                HWND control = CreateWindowExW(exStyle, klass, text, style,
                    x, y, width, height, hwnd,
                    id ? reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)) : nullptr,
                    g_instance, nullptr);
                if (control && font)
                    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
                return control;
            };

        makeControl(0, L"STATIC", L"ClipManager", WS_CHILD | WS_VISIBLE | SS_LEFT,
            20, 16, 440, 32, 0, g_mainTitleFont);
        makeControl(0, L"STATIC", L"A lightweight clipboard history manager", WS_CHILD | WS_VISIBLE | SS_LEFT,
            22, 49, 440, 22, 0, g_mainFont);
        makeControl(0, L"STATIC", L"HOW TO USE", WS_CHILD | WS_VISIBLE | SS_LEFT,
            20, 82, 440, 22, 0, g_mainFont);
        const wchar_t* instructions =
            L"1. Copy text, files, or images as usual.\r\n"
            L"2. Hold Ctrl and tap V to open clipboard history.\r\n"
            L"3. Keep Ctrl held and tap V again to move to the next item.\r\n"
            L"4. Release both Ctrl and V to paste the selected item.\r\n"
            L"5. Use the arrow keys to navigate, Esc or Ctrl+Z to cancel.\r\n"
            L"6. While the picker is open, Ctrl+Alt clears its history.";
        makeControl(0, L"STATIC", instructions, WS_CHILD | WS_VISIBLE | SS_LEFT,
            22, 108, 440, 138, 0, g_mainFont);
        makeControl(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
            20, 253, 440, 2, 0, nullptr);
        makeControl(0, L"STATIC", L"LINKS & SUPPORT", WS_CHILD | WS_VISIBLE | SS_LEFT,
            20, 266, 440, 22, 0, g_mainFont);
        makeControl(0, L"BUTTON", L"Open Telegram channel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            20, 292, 214, 34, ID_MAIN_TELEGRAM, g_mainFont);
        makeControl(0, L"BUTTON", L"Report a bug", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            244, 292, 216, 34, ID_MAIN_BUG_REPORT, g_mainFont);
        makeControl(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
            20, 339, 440, 2, 0, nullptr);
        g_startupCheckbox = makeControl(0, L"BUTTON",
            L"Start automatically with Windows (run in the background without showing this screen)",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX | BS_MULTILINE,
            20, 352, 440, 44, ID_MAIN_STARTUP, g_mainFont);
        if (g_startupCheckbox) {
            SendMessageW(g_startupCheckbox, BM_SETCHECK,
                IsRunAtStartupEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        makeControl(0, L"BUTTON", L"Close",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            190, 411, 100, 34, ID_MAIN_CLOSE, g_mainFont);

        makeControl(0, L"BUTTON", L"Start ClipManager", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            300, 411, 160, 34, ID_MAIN_START, g_mainFont);
        return 0;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id == ID_MAIN_TELEGRAM && HIWORD(wp) == BN_CLICKED) {
            OpenConfiguredUrl(hwnd, kTelegramChannelUrl, L"kTelegramChannelUrl");
            return 0;
        }
        if (id == ID_MAIN_BUG_REPORT && HIWORD(wp) == BN_CLICKED) {
            OpenConfiguredUrl(hwnd, kBugReportUrl, L"kBugReportUrl");
            return 0;
        }
        if (id == ID_MAIN_STARTUP && HIWORD(wp) == BN_CLICKED) {
            const bool enabled = SendMessageW(g_startupCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
            LONG status = SetRunAtStartup(enabled);
            if (status != ERROR_SUCCESS) {
                SendMessageW(g_startupCheckbox, BM_SETCHECK,
                    enabled ? BST_UNCHECKED : BST_CHECKED, 0);
                MessageBoxW(hwnd,
                    L"Could not update the Windows startup setting. Check your Windows user permissions and try again.",
                    L"ClipManager", MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        if (id == ID_MAIN_CLOSE && HIWORD(wp) == BN_CLICKED) {
            PostMessageW(g_hwnd, WM_APP + 5, 0, 0);
            return 0;
        }
        if (id == ID_MAIN_START && HIWORD(wp) == BN_CLICKED) {
            HideMainScreen(hwnd);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        HideMainScreen(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    g_instance = instance;
    HICON appIcon = LoadIconW(
        instance,
        MAKEINTRESOURCEW(IDI_ICON1)
    );
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kClassName;
    wc.hIcon = appIcon;
    wc.hIconSm = appIcon;
    RegisterClassExW(&wc);

    WNDCLASSEXW mainWc{ sizeof(mainWc) };
    mainWc.lpfnWndProc = MainWindowProc;
    mainWc.hInstance = instance;
    mainWc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    mainWc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    mainWc.lpszClassName = kMainClassName;
    mainWc.hIcon = appIcon;
    mainWc.hIconSm = appIcon;
    RegisterClassExW(&mainWc);

    g_mainFont = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_mainTitleFont = CreateFontW(-22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kClassName, L"ClipManager",
        WS_POPUP | WS_BORDER, CW_USEDEFAULT, CW_USEDEFAULT, 560, 330,
        nullptr, nullptr, instance, nullptr);
    if (!g_hwnd) return 1;
    AddClipboardFormatListener(g_hwnd);
    CaptureClipboard();
    g_leftCtrlDown = (GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0;
    g_rightCtrlDown = (GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0;
    g_genericCtrlDown = false;
    g_ctrlDown = g_leftCtrlDown || g_rightCtrlDown || g_genericCtrlDown;
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, instance, 0);
    if (!g_hook) {
        MessageBoxW(nullptr, L"Could not install the keyboard hook.", L"ClipManager", MB_ICONERROR);
        return 2;
    }

    // Tray icon lets the user exit the background utility.
    NOTIFYICONDATAW nid{}; nid.cbSize = sizeof(nid); nid.hWnd = g_hwnd; nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE; nid.uCallbackMessage = WM_APP + 10;
    nid.hIcon = appIcon; wcscpy_s(nid.szTip, L"ClipManager - hold Ctrl, tap V, release Ctrl to paste");
    Shell_NotifyIconW(NIM_ADD, &nid);

    g_mainHwnd = CreateWindowExW(0, kMainClassName, L"ClipManager - Main Screen",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 500, 500, nullptr, nullptr, instance, nullptr);
    if (!g_mainHwnd) {
        Shell_NotifyIconW(NIM_DELETE, &nid);
        DestroyWindow(g_hwnd);
        return 3;
    }

    // A Run-key launch includes --background and must not show the Main Screen.
    const bool launchedInBackground = commandLine && wcsstr(commandLine, L"--background") != nullptr;
    if (!launchedInBackground) ShowMainScreen();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_APP + 10 && msg.lParam == WM_RBUTTONUP) {
            if (MessageBoxW(nullptr, L"Exit ClipManager ?", L"ClipManager", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                Shell_NotifyIconW(NIM_DELETE, &nid);
                DestroyWindow(g_hwnd);
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Shell_NotifyIconW(NIM_DELETE, &nid);
    return 0;
}
