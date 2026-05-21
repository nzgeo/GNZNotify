// =============================================================================
// gnz_notification_agent.cpp
// GNZ Notification Agent  —  system-tray application
// =============================================================================
//
// Build (MSVC x64, Visual Studio Developer Command Prompt):
//   cl /EHsc /std:c++17 /W3 /O2 /DUNICODE /D_UNICODE gnz_notification_agent.cpp ^
//      /link winhttp.lib comctl32.lib shell32.lib user32.lib gdi32.lib ^
//      /SUBSYSTEM:WINDOWS
//
// No third-party headers required; uses WinHTTP (winhttp.dll, ships with
// Windows 7 and later) for all outbound HTTP requests.
//
// Registry (HKCU\SOFTWARE\GNZ\NotificationAgent):
//   ServerAddress  REG_SZ     default "localhost"
//   ApiPort        REG_DWORD  default 8081
//   PollInterval   REG_DWORD  default 5000  (milliseconds)
//   ShowBanner     REG_DWORD  default 1
//   EmitSound      REG_DWORD  default 1
//   ChangeIcon     REG_DWORD  default 0
//   Active         REG_DWORD  default 1
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commctrl.h>
#include <winhttp.h>

#include <string>
#include <vector>
#include <sstream>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include "resource.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

// ---------------------------------------------------------------------------
// Control / menu / message IDs
// ---------------------------------------------------------------------------

// Alerts window controls
#define IDC_LIST_ALERTS    100
#define IDC_BTN_DEL_SEL    101
#define IDC_BTN_CLEAR_ALL  102
#define IDC_BTN_REFRESH    103

// Config window controls
#define IDC_EDIT_SERVER    200
#define IDC_EDIT_PORT      201
#define IDC_EDIT_INTERVAL  202
#define IDC_CHK_BANNER     203
#define IDC_CHK_SOUND      204
#define IDC_CHK_ICON       205
#define IDC_CHK_ACTIVE     208
#define IDC_CHK_ADDRESSED  209
#define IDC_CFG_OK         206
#define IDC_CFG_CANCEL     207

// Tray context menu
#define IDM_SHOW           300
#define IDM_CONFIGURE      301
#define IDM_EXIT           302

// Banner close button
#define IDC_BANNER_CLOSE   400

// Custom window messages
#define WM_TRAYICON        (WM_USER + 1)   // tray icon callback
#define WM_NEW_ALERTS      (WM_USER + 2)   // poll thread → main thread
#define WM_POLL_STATUS     (WM_USER + 3)   // wParam: 1=connected, 0=disconnected

// Window class names
#define WC_MAIN    L"GNZ_NotifyMain"
#define WC_ALERTS  L"GNZ_Alerts"
#define WC_CONFIG  L"GNZ_Config"
#define WC_BANNER  L"GNZ_Banner"

static constexpr wchar_t REG_PATH[] = L"SOFTWARE\\GNZ\\NotificationAgent";
static constexpr int BANNER_TIMER = 1;
static constexpr DWORD BANNER_MS = 7000;   // auto-close delay

// ---------------------------------------------------------------------------
// Data types
// ---------------------------------------------------------------------------

struct Alert {
    int          id = 0;
    std::string  timestamp;
    std::string  event;
    std::string  issue_key;
    std::string  summary;
    std::string  project;
    std::string  priority;
    std::string  reporter;
    std::string  status;
    std::string  link;      // Jira browse URL
    std::string  user;      // assignee login name (empty if unassigned)
};

struct AgentConfig {
    std::wstring server = L"localhost";
    int          port = 8081;
    int          interval = 5000;
    bool         banner = true;
    bool         sound = true;
    bool         icon_chg = false;
    bool         active = true;
    bool         addressed_only = true;   // notify only when assignee == current user
};

// Data marshalled from poll thread to main thread
struct PendingUpdate {
    std::vector<Alert> alerts;
    bool               full_refresh = false;  // true = clear local list first
};

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

static HINSTANCE g_hInst = nullptr;
static HWND      g_hwndMain = nullptr;   // hidden host window
static HWND      g_hwndAlerts = nullptr;   // alerts list window (nullptr if closed)
static HWND      g_hwndConfig = nullptr;   // config window     (nullptr if closed)
static HWND      g_hwndBanner = nullptr;   // current banner    (nullptr if none)

static HICON g_iconNormal = nullptr;        // default tray icon
static HICON g_iconAlert = nullptr;        // alert-pending tray icon
static bool  g_hasAlert = false;          // true while unacknowledged alerts exist

static AgentConfig g_cfg;
static std::vector<Alert> g_alerts;         // local alert cache (main thread only)
static std::wstring g_windowsUser;          // current logged-on username (for addressed-only filter)

// Poll thread
static std::atomic<bool> g_pollActive{ false };
static HANDLE            g_pollWake = nullptr;   // auto-reset, signals stop/wake
static HANDLE            g_pollThread = nullptr;
static bool              g_initialLoadDone = false; // set by main thread after first full refresh

// Pending update queue (poll thread writes, main thread drains)
static PendingUpdate     g_pending;
static CRITICAL_SECTION  g_pendingCS;

// Banner content (set before creating banner window)
static std::wstring g_bannerLine1;   // e.g. "PROJ-123  [High]"
static std::wstring g_bannerLine2;   // summary text
static std::wstring g_bannerLink;    // Jira browse URL — opened on click (empty = no link)

// ---------------------------------------------------------------------------
// String conversion
// ---------------------------------------------------------------------------

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1,
        nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1,
        &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring Utf8ToWide(const std::string& u) {
    if (u.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u.c_str(), -1, &w[0], n);
    return w;
}

// ---------------------------------------------------------------------------
// HTTP client helper (WinHTTP — ships with Windows 7+, no extra headers needed)
// ---------------------------------------------------------------------------

struct HttpResult {
    bool        ok = false;
    int         status = 0;
    std::string body;
};

// Issues an HTTP request using WinHTTP. host is a narrow (UTF-8/ASCII) string.
// verb is "GET" or "DELETE". path is the request-target (e.g. "/api/alerts").
// Timeouts: connect 3 s, receive 5 s.
static HttpResult DoHttpRequest(const std::string& host, int port,
    const std::string& verb,
    const std::string& path) {
    HttpResult result;

    std::wstring whost = Utf8ToWide(host);
    std::wstring wverb = Utf8ToWide(verb);
    std::wstring wpath = Utf8ToWide(path);

    HINTERNET hSession = WinHttpOpen(
        L"GNZ-Agent/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!hSession) return result;

    DWORD connectTimeout = 3000, receiveTimeout = 5000;
    WinHttpSetOption(hSession, WINHTTP_OPTION_CONNECT_TIMEOUT,
        &connectTimeout, sizeof(connectTimeout));
    WinHttpSetOption(hSession, WINHTTP_OPTION_RECEIVE_TIMEOUT,
        &receiveTimeout, sizeof(receiveTimeout));

    HINTERNET hConnect = WinHttpConnect(
        hSession, whost.c_str(),
        static_cast<INTERNET_PORT>(port), 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return result;
    }

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, wverb.c_str(), wpath.c_str(),
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }

    if (WinHttpSendRequest(hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, nullptr)) {
        // Read HTTP status code
        DWORD statusCode = 0, statusSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &statusCode, &statusSize,
            WINHTTP_NO_HEADER_INDEX);
        result.status = static_cast<int>(statusCode);

        // Read response body
        DWORD bytesAvail = 0;
        while (WinHttpQueryDataAvailable(hRequest, &bytesAvail) &&
            bytesAvail > 0) {
            std::vector<char> buf(bytesAvail + 1);
            DWORD bytesRead = 0;
            if (WinHttpReadData(hRequest, buf.data(),
                bytesAvail, &bytesRead) && bytesRead > 0)
                result.body.append(buf.data(), bytesRead);
        }
        result.ok = true;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return result;
}

// ---------------------------------------------------------------------------
// Minimal JSON helpers (agent-side, mirrors server helpers)
// ---------------------------------------------------------------------------

static std::string JsonGet(const std::string& j, const std::string& key) {
    const std::string pat = "\"" + key + "\"";
    size_t p = j.find(pat);
    while (p != std::string::npos) {
        size_t c = j.find_first_not_of(" \t\r\n", p + pat.size());
        if (c == std::string::npos || j[c] != ':') {
            p = j.find(pat, p + 1);
            continue;
        }
        size_t v = j.find_first_not_of(" \t\r\n", c + 1);
        if (v == std::string::npos || j[v] != '"') break;
        ++v;
        std::string val;
        for (; v < j.size() && j[v] != '"'; ++v) {
            if (j[v] == '\\' && v + 1 < j.size()) { ++v; }
            val += j[v];
        }
        return val;
    }
    return {};
}

static int JsonGetInt(const std::string& j, const std::string& key) {
    const std::string pat = "\"" + key + "\"";
    size_t p = j.find(pat);
    if (p == std::string::npos) return 0;
    size_t c = j.find(':', p);
    if (c == std::string::npos) return 0;
    size_t v = j.find_first_not_of(" \t\r\n", c + 1);
    if (v == std::string::npos) return 0;
    try { return std::stoi(j.substr(v)); }
    catch (...) { return 0; }
}

// Parses the {"count":N,"alerts":[...]} response from the server.
static std::vector<Alert> ParseAlertsResponse(const std::string& json) {
    std::vector<Alert> result;

    // Find the start of the "alerts" array
    size_t arr = json.find("\"alerts\"");
    if (arr == std::string::npos) return result;
    size_t bracket = json.find('[', arr);
    if (bracket == std::string::npos) return result;

    // Walk the array: extract top-level objects {…}
    int    depth = 0;
    size_t objStart = std::string::npos;
    bool   inStr = false;

    for (size_t i = bracket + 1; i < json.size(); ++i) {
        char ch = json[i];
        if (ch == '"' && (i == 0 || json[i - 1] != '\\'))
            inStr = !inStr;
        if (inStr) continue;

        if (ch == '{') {
            if (depth == 0) objStart = i;
            ++depth;
        }
        else if (ch == '}') {
            --depth;
            if (depth == 0 && objStart != std::string::npos) {
                std::string obj = json.substr(objStart, i - objStart + 1);
                Alert a;
                a.id = JsonGetInt(obj, "id");
                a.timestamp = JsonGet(obj, "timestamp");
                a.event = JsonGet(obj, "event");
                a.issue_key = JsonGet(obj, "issue_key");
                a.summary = JsonGet(obj, "summary");
                a.project = JsonGet(obj, "project");
                a.priority = JsonGet(obj, "priority");
                a.reporter = JsonGet(obj, "reporter");
                a.status = JsonGet(obj, "status");
                a.link = JsonGet(obj, "link");
                a.user = JsonGet(obj, "user");
                if (a.id > 0) result.push_back(std::move(a));
                objStart = std::string::npos;
            }
        }
        else if (ch == ']' && depth == 0) {
            break;
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Registry: load / save config
// ---------------------------------------------------------------------------

static void LoadConfig() {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_PATH, 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE,
        nullptr, &hKey, nullptr) != ERROR_SUCCESS)
        return;

    auto readDword = [&](const wchar_t* name, DWORD def) -> DWORD {
        DWORD v = def, sz = sizeof(v), type = REG_DWORD;
        RegQueryValueExW(hKey, name, nullptr, &type,
            reinterpret_cast<LPBYTE>(&v), &sz);
        return v;
        };

    wchar_t buf[256] = {};
    DWORD sz = sizeof(buf), type = REG_SZ;
    if (RegQueryValueExW(hKey, L"ServerAddress", nullptr, &type,
        reinterpret_cast<LPBYTE>(buf), &sz) == ERROR_SUCCESS)
        g_cfg.server = buf;

    g_cfg.port = static_cast<int>(readDword(L"ApiPort", 8081));
    g_cfg.interval = static_cast<int>(readDword(L"PollInterval", 5000));
    g_cfg.banner = readDword(L"ShowBanner", 1) != 0;
    g_cfg.sound = readDword(L"EmitSound", 1) != 0;
    g_cfg.icon_chg = readDword(L"ChangeIcon", 0) != 0;
    g_cfg.active = readDword(L"Active", 1) != 0;
    g_cfg.addressed_only = readDword(L"AddressedOnly", 1) != 0;

    RegCloseKey(hKey);
}

static void SaveConfig() {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_PATH, 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_WRITE,
        nullptr, &hKey, nullptr) != ERROR_SUCCESS)
        return;

    RegSetValueExW(hKey, L"ServerAddress", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(g_cfg.server.c_str()),
        static_cast<DWORD>((g_cfg.server.size() + 1) * sizeof(wchar_t)));

    auto writeDword = [&](const wchar_t* name, DWORD val) {
        RegSetValueExW(hKey, name, 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&val), sizeof(DWORD));
        };
    writeDword(L"ApiPort", static_cast<DWORD>(g_cfg.port));
    writeDword(L"PollInterval", static_cast<DWORD>(g_cfg.interval));
    writeDword(L"ShowBanner", g_cfg.banner ? 1 : 0);
    writeDword(L"EmitSound", g_cfg.sound ? 1 : 0);
    writeDword(L"ChangeIcon", g_cfg.icon_chg ? 1 : 0);
    writeDword(L"Active", g_cfg.active ? 1 : 0);
    writeDword(L"AddressedOnly", g_cfg.addressed_only ? 1 : 0);

    RegCloseKey(hKey);
}

// ---------------------------------------------------------------------------
// Tray icon management
// ---------------------------------------------------------------------------

static void AddTrayIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwndMain;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = g_iconNormal;
    wcscpy_s(nid.szTip, L"GNZ Notification Agent");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

static void UpdateTrayIcon(bool alert) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwndMain;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP;
    nid.hIcon = alert ? g_iconAlert : g_iconNormal;
    wcscpy_s(nid.szTip, alert
        ? L"GNZ Notification Agent  [New alerts]"
        : L"GNZ Notification Agent");
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void UpdateTrayTip(bool connected) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwndMain;
    nid.uID = 1;
    nid.uFlags = NIF_TIP;
    wcscpy_s(nid.szTip, connected
        ? L"GNZ Notification Agent  [connected]"
        : L"GNZ Notification Agent  [disconnected]");
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void RemoveTrayIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwndMain;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

// ---------------------------------------------------------------------------
// Poll thread
// ---------------------------------------------------------------------------

static void StartPoll();
static void StopPoll();

DWORD WINAPI PollThreadProc(LPVOID) {
    // Capture config snapshot; config only changes when this thread is restarted.
    const std::string addr = WideToUtf8(g_cfg.server);
    const int         port = g_cfg.port;
    const DWORD       interval = static_cast<DWORD>(g_cfg.interval);

    bool was_connected = false;
    int  last_id = 0;

    while (g_pollActive) {
        bool connected = false;

        auto res = DoHttpRequest(addr, port, "GET", "/api/alerts");
        if (res.ok && res.status == 200) {
            connected = true;
            const bool is_reconnect = !was_connected;
            auto all = ParseAlertsResponse(res.body);

            // Determine new alerts
            std::vector<Alert> fresh;
            if (is_reconnect) {
                fresh = all;   // full refresh on (re)connect
            }
            else {
                for (auto& a : all)
                    if (a.id > last_id) fresh.push_back(a);
            }

            // Update high-water mark
            for (const auto& a : all)
                if (a.id > last_id) last_id = a.id;

            // Post to main thread if there is anything to report
            if (!fresh.empty() || is_reconnect) {
                EnterCriticalSection(&g_pendingCS);
                g_pending.alerts = std::move(fresh);
                g_pending.full_refresh = is_reconnect;
                LeaveCriticalSection(&g_pendingCS);
                PostMessage(g_hwndMain, WM_NEW_ALERTS, 0, 0);
            }
        }

        if (connected != was_connected)
            PostMessage(g_hwndMain, WM_POLL_STATUS,
                connected ? 1 : 0, 0);
        was_connected = connected;

        // Sleep until interval elapses or we're woken (stop/config change)
        WaitForSingleObject(g_pollWake, interval);
    }
    return 0;
}

static void StartPoll() {
    g_pollActive = true;
    ResetEvent(g_pollWake);
    g_pollThread = CreateThread(nullptr, 0, PollThreadProc,
        nullptr, 0, nullptr);
}

static void StopPoll() {
    g_pollActive = false;
    if (g_pollWake) SetEvent(g_pollWake);
    if (g_pollThread) {
        WaitForSingleObject(g_pollThread, 6000);
        CloseHandle(g_pollThread);
        g_pollThread = nullptr;
    }
}

static void RestartPoll() {
    StopPoll();
    StartPoll();
}

// ---------------------------------------------------------------------------
// Alerts list window helpers (called from main thread only)
// ---------------------------------------------------------------------------

// Adds one alert row at the top of the ListView (newest-first order).
static void AddAlertToListView(HWND hList, const Alert& a) {
    std::wstring wid = std::to_wstring(a.id);
    std::wstring wtime = Utf8ToWide(a.timestamp);
    std::wstring wlink = Utf8ToWide(a.link);
    std::wstring wev = Utf8ToWide(a.event);
    std::wstring wkey = Utf8ToWide(a.issue_key);
    std::wstring wuser = Utf8ToWide(a.user);
    std::wstring wsum = Utf8ToWide(a.summary);

    LVITEMW lvi{};
    lvi.mask = LVIF_TEXT | LVIF_PARAM;
    lvi.iItem = 0;   // insert at top
    lvi.pszText = const_cast<wchar_t*>(wid.c_str());
    lvi.lParam = static_cast<LPARAM>(a.id);
    int row = ListView_InsertItem(hList, &lvi);
    if (row < 0) return;

    ListView_SetItemText(hList, row, 1, const_cast<wchar_t*>(wtime.c_str()));
    ListView_SetItemText(hList, row, 2, const_cast<wchar_t*>(wlink.c_str()));
    ListView_SetItemText(hList, row, 3, const_cast<wchar_t*>(wev.c_str()));
    ListView_SetItemText(hList, row, 4, const_cast<wchar_t*>(wkey.c_str()));
    ListView_SetItemText(hList, row, 5, const_cast<wchar_t*>(wuser.c_str()));
    ListView_SetItemText(hList, row, 6, const_cast<wchar_t*>(wsum.c_str()));
}

// Repopulates the ListView from g_alerts (newest at top).
// Iterate oldest-to-newest, each inserted at position 0 → newest ends at row 0.
static void PopulateAlertsList(HWND hList) {
    ListView_DeleteAllItems(hList);
    for (const auto& a : g_alerts)
        AddAlertToListView(hList, a);
}

// ---------------------------------------------------------------------------
// Banner window
// ---------------------------------------------------------------------------

static LRESULT CALLBACK BannerWndProc(HWND hwnd, UINT msg,
    WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, BANNER_TIMER, BANNER_MS, nullptr);
        break;

    case WM_TIMER:
        if (wParam == BANNER_TIMER) DestroyWindow(hwnd);
        break;

    case WM_SETCURSOR:
        // Show a hand cursor when the banner has a clickable link
        if (!g_bannerLink.empty()) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;

    case WM_LBUTTONDOWN:
        if (!g_bannerLink.empty())
            ShellExecuteW(nullptr, L"open", g_bannerLink.c_str(),
                nullptr, nullptr, SW_SHOWNORMAL);
        DestroyWindow(hwnd);
        break;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_BANNER_CLOSE)
            DestroyWindow(hwnd);
        break;

    case WM_DESTROY:
        KillTimer(hwnd, BANNER_TIMER);
        g_hwndBanner = nullptr;
        break;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// Creates (or replaces) the banner popup. Call only from the main thread.
static void ShowBanner(const Alert& a) {
    if (g_hwndBanner) DestroyWindow(g_hwndBanner);

    // Build display strings
    std::wstring key = Utf8ToWide(a.issue_key);
    std::wstring pri = Utf8ToWide(a.priority);
    std::wstring sum = Utf8ToWide(a.summary);

    g_bannerLine1 = key;
    if (!pri.empty()) g_bannerLine1 += L"  [" + pri + L"]";
    g_bannerLine2 = sum.size() > 60
        ? sum.substr(0, 57) + L"..."
        : sum;
    g_bannerLink = Utf8ToWide(a.link);

    // Position at bottom-right of work area
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    const int W = 390, H = 110;
    int x = wa.right - W - 12;
    int y = wa.bottom - H - 12;

    // Title bar text: event type
    std::wstring title = L"\u26A0 GNZ Alert: " + Utf8ToWide(a.event);

    g_hwndBanner = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        WC_BANNER,
        title.c_str(),
        WS_CAPTION | WS_SYSMENU | WS_POPUP,
        x, y, W, H,
        nullptr, nullptr, g_hInst, nullptr);

    if (!g_hwndBanner) return;

    // Line 1 — issue key + priority
    CreateWindowExW(0, L"STATIC", g_bannerLine1.c_str(),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        8, 4, W - 20, 24,
        g_hwndBanner, nullptr, g_hInst, nullptr);

    // Line 2 — summary
    CreateWindowExW(0, L"STATIC", g_bannerLine2.c_str(),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        8, 30, W - 20, 40,
        g_hwndBanner, nullptr, g_hInst, nullptr);

    // Line 3 — click hint shown only when a link is available
    if (!g_bannerLink.empty())
        CreateWindowExW(0, L"STATIC", L"Click to open in Jira",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            8, 74, W - 20, 18,
            g_hwndBanner, nullptr, g_hInst, nullptr);

    ShowWindow(g_hwndBanner, SW_SHOWNOACTIVATE);
    UpdateWindow(g_hwndBanner);
}

// ---------------------------------------------------------------------------
// Config window
// ---------------------------------------------------------------------------

static LRESULT CALLBACK ConfigWndProc(HWND hwnd, UINT msg,
    WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        auto lbl = [&](const wchar_t* text, int x, int y, int w) {
            CreateWindowExW(0, L"STATIC", text,
                WS_CHILD | WS_VISIBLE | SS_RIGHT,
                x, y, w, 20, hwnd, nullptr, g_hInst, nullptr);
            };
        auto edt = [&](int id, const wchar_t* init, int x, int y, int w) {
            HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", init,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                x, y, w, 22, hwnd,
                reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                g_hInst, nullptr);
            return h;
            };
        auto chk = [&](int id, const wchar_t* text, bool checked, int y) {
            HWND h = CreateWindowExW(0, L"BUTTON", text,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                20, y, 360, 22, hwnd,
                reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                g_hInst, nullptr);
            Button_SetCheck(h, checked ? BST_CHECKED : BST_UNCHECKED);
            };

        lbl(L"Server address:", 10, 14, 130);
        wchar_t portBuf[16], intBuf[16];
        swprintf_s(portBuf, L"%d", g_cfg.port);
        swprintf_s(intBuf, L"%d", g_cfg.interval);
        edt(IDC_EDIT_SERVER, g_cfg.server.c_str(), 148, 12, 220);
        lbl(L"API port:", 10, 44, 130);
        edt(IDC_EDIT_PORT, portBuf, 148, 42, 70);
        lbl(L"Poll interval:", 10, 74, 130);
        HWND hInt = edt(IDC_EDIT_INTERVAL, intBuf, 148, 72, 70);
        CreateWindowExW(0, L"STATIC", L"ms", WS_CHILD | WS_VISIBLE,
            224, 74, 30, 20, hwnd, nullptr, g_hInst, nullptr);
        (void)hInt;

        chk(IDC_CHK_BANNER, L"Show banner notification on new alert",
            g_cfg.banner, 110);
        chk(IDC_CHK_SOUND, L"Play sound on new alert",
            g_cfg.sound, 136);
        chk(IDC_CHK_ICON, L"Change tray icon when alerts are pending",
            g_cfg.icon_chg, 162);
        chk(IDC_CHK_ACTIVE, L"Active (receive and display alerts)",
            g_cfg.active, 188);
        chk(IDC_CHK_ADDRESSED, L"Addressed to me only (notify only if assignee = current user)",
            g_cfg.addressed_only, 214);

        CreateWindowExW(0, L"BUTTON", L"OK",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            220, 252, 80, 28, hwnd,
            reinterpret_cast<HMENU>(IDC_CFG_OK), g_hInst, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Cancel",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            316, 252, 80, 28, hwnd,
            reinterpret_cast<HMENU>(IDC_CFG_CANCEL), g_hInst, nullptr);
        break;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_CFG_OK: {
            // Read and validate inputs
            wchar_t srv[256] = {}; GetDlgItemTextW(hwnd, IDC_EDIT_SERVER, srv, 256);
            wchar_t prt[16] = {}; GetDlgItemTextW(hwnd, IDC_EDIT_PORT, prt, 16);
            wchar_t itv[16] = {}; GetDlgItemTextW(hwnd, IDC_EDIT_INTERVAL, itv, 16);

            if (wcslen(srv) == 0) {
                MessageBoxW(hwnd, L"Server address cannot be empty.",
                    L"Validation", MB_OK | MB_ICONWARNING);
                break;
            }
            int port = _wtoi(prt), intv = _wtoi(itv);
            if (port < 1 || port > 65535) {
                MessageBoxW(hwnd, L"Port must be between 1 and 65535.",
                    L"Validation", MB_OK | MB_ICONWARNING);
                break;
            }
            if (intv < 1000 || intv > 300000) {
                MessageBoxW(hwnd,
                    L"Poll interval must be between 1000 and 300000 ms.",
                    L"Validation", MB_OK | MB_ICONWARNING);
                break;
            }

            g_cfg.server = srv;
            g_cfg.port = port;
            g_cfg.interval = intv;
            g_cfg.banner = (IsDlgButtonChecked(hwnd, IDC_CHK_BANNER) == BST_CHECKED);
            g_cfg.sound = (IsDlgButtonChecked(hwnd, IDC_CHK_SOUND) == BST_CHECKED);
            g_cfg.icon_chg = (IsDlgButtonChecked(hwnd, IDC_CHK_ICON) == BST_CHECKED);
            g_cfg.active = (IsDlgButtonChecked(hwnd, IDC_CHK_ACTIVE) == BST_CHECKED);
            g_cfg.addressed_only = (IsDlgButtonChecked(hwnd, IDC_CHK_ADDRESSED) == BST_CHECKED);

            SaveConfig();
            RestartPoll();   // apply new server/port/interval
            DestroyWindow(hwnd);
            break;
        }
        case IDC_CFG_CANCEL:
            DestroyWindow(hwnd);
            break;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        break;

    case WM_DESTROY:
        g_hwndConfig = nullptr;
        break;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

static void ShowConfigWindow() {
    if (g_hwndConfig) { SetForegroundWindow(g_hwndConfig); return; }

    g_hwndConfig = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        WC_CONFIG, L"GNZ Agent Configuration",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 420, 340,
        g_hwndMain, nullptr, g_hInst, nullptr);

    if (g_hwndConfig) {
        ShowWindow(g_hwndConfig, SW_SHOW);
        UpdateWindow(g_hwndConfig);
    }
}

// ---------------------------------------------------------------------------
// Alerts list — sort state and comparator
// ---------------------------------------------------------------------------

static int g_sortCol = -1;   // -1 = unsorted
static int g_sortDir = 1;   //  1 = ascending, -1 = descending

// Callback for ListView_SortItemsEx; lp1/lp2 are item indices.
static int CALLBACK AlertSortProc(LPARAM lp1, LPARAM lp2, LPARAM data) {
    HWND hList = reinterpret_cast<HWND>(data);
    wchar_t buf1[1024] = {}, buf2[1024] = {};
    ListView_GetItemText(hList, static_cast<int>(lp1), g_sortCol, buf1, 1024);
    ListView_GetItemText(hList, static_cast<int>(lp2), g_sortCol, buf2, 1024);
    // Column 0 (ID) sorts numerically; all others sort lexicographically.
    if (g_sortCol == 0) {
        int n1 = _wtoi(buf1), n2 = _wtoi(buf2);
        return g_sortDir * (n1 - n2);
    }
    return g_sortDir * wcscmp(buf1, buf2);
}

// ---------------------------------------------------------------------------
// Alerts list window
// ---------------------------------------------------------------------------

static LRESULT CALLBACK AlertsWndProc(HWND hwnd, UINT msg,
    WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        // ListView
        HWND hList = CreateWindowExW(
            WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | WS_TABSTOP,
            0, 0, 0, 0,
            hwnd, reinterpret_cast<HMENU>(IDC_LIST_ALERTS), g_hInst, nullptr);
        ListView_SetExtendedListViewStyle(
            hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        // Columns: 0=ID 1=Time 2=Link 3=Event 4=Key 5=User 6=Summary
        struct { const wchar_t* name; int w; } cols[] = {
            { L"ID",      50  }, { L"Time",    162 }, { L"Link",    120 },
            { L"Event",  130  }, { L"Key",      80 }, { L"User",    100 },
            { L"Summary", 260 }
        };
        for (int i = 0; i < 7; ++i) {
            LVCOLUMNW lvc{};
            lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            lvc.iSubItem = i;
            lvc.pszText = const_cast<wchar_t*>(cols[i].name);
            lvc.cx = cols[i].w;
            ListView_InsertColumn(hList, i, &lvc);
        }

        // Restore column widths saved at last close
        {
            HKEY hKey = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH, 0,
                KEY_READ, &hKey) == ERROR_SUCCESS) {
                for (int i = 0; i < 7; ++i) {
                    wchar_t name[32];
                    swprintf_s(name, L"ColWidth%d", i);
                    DWORD w = 0, sz = sizeof(w), type = REG_DWORD;
                    if (RegQueryValueExW(hKey, name, nullptr, &type,
                        reinterpret_cast<LPBYTE>(&w), &sz) == ERROR_SUCCESS
                        && w > 0)
                        ListView_SetColumnWidth(hList, i, static_cast<int>(w));
                }
                RegCloseKey(hKey);
            }
        }

        // Buttons
        CreateWindowExW(0, L"BUTTON", L"Delete Selected",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            4, 0, 130, 26, hwnd,
            reinterpret_cast<HMENU>(IDC_BTN_DEL_SEL), g_hInst, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Clear All",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            140, 0, 90, 26, hwnd,
            reinterpret_cast<HMENU>(IDC_BTN_CLEAR_ALL), g_hInst, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Refresh",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            236, 0, 80, 26, hwnd,
            reinterpret_cast<HMENU>(IDC_BTN_REFRESH), g_hInst, nullptr);

        PopulateAlertsList(hList);
        break;
    }

    case WM_SIZE: {
        HWND hList = GetDlgItem(hwnd, IDC_LIST_ALERTS);
        RECT r; GetClientRect(hwnd, &r);
        const int BTN_H = 30;
        MoveWindow(hList, 0, BTN_H, r.right, r.bottom - BTN_H, TRUE);
        // Re-anchor buttons to top
        MoveWindow(GetDlgItem(hwnd, IDC_BTN_DEL_SEL), 4, 2, 130, 26, TRUE);
        MoveWindow(GetDlgItem(hwnd, IDC_BTN_CLEAR_ALL), 140, 2, 90, 26, TRUE);
        MoveWindow(GetDlgItem(hwnd, IDC_BTN_REFRESH), 236, 2, 80, 26, TRUE);
        break;
    }

    case WM_COMMAND: {
        HWND hList = GetDlgItem(hwnd, IDC_LIST_ALERTS);

        if (LOWORD(wParam) == IDC_BTN_DEL_SEL) {
            // Collect selected items (iterate in reverse to preserve indices)
            std::vector<int> rows;
            int sel = -1;
            while ((sel = ListView_GetNextItem(hList, sel, LVNI_SELECTED)) != -1)
                rows.push_back(sel);

            if (rows.empty()) break;

            // Build server + port snapshot for HTTP calls
            std::string addr = WideToUtf8(g_cfg.server);
            int         port = g_cfg.port;

            for (int i = static_cast<int>(rows.size()) - 1; i >= 0; --i) {
                LVITEMW lvi{};
                lvi.mask = LVIF_PARAM;
                lvi.iItem = rows[static_cast<size_t>(i)];
                if (!ListView_GetItem(hList, &lvi)) continue;
                int alertId = static_cast<int>(lvi.lParam);

                // Ask the server to delete it
                DoHttpRequest(addr, port, "DELETE",
                    "/api/alerts/" + std::to_string(alertId));

                // Remove from local cache and ListView
                g_alerts.erase(
                    std::remove_if(g_alerts.begin(), g_alerts.end(),
                        [alertId](const Alert& a) { return a.id == alertId; }),
                    g_alerts.end());
                ListView_DeleteItem(hList, rows[static_cast<size_t>(i)]);
            }
            break;
        }

        if (LOWORD(wParam) == IDC_BTN_CLEAR_ALL) {
            if (MessageBoxW(hwnd, L"Clear all alerts from the server?",
                L"Confirm", MB_YESNO | MB_ICONQUESTION) != IDYES)
                break;
            DoHttpRequest(WideToUtf8(g_cfg.server), g_cfg.port,
                "DELETE", "/api/alerts");
            g_alerts.clear();
            ListView_DeleteAllItems(hList);
            break;
        }

        if (LOWORD(wParam) == IDC_BTN_REFRESH) {
            // Wake poll thread immediately so it fetches fresh data
            if (g_pollWake) SetEvent(g_pollWake);
            break;
        }
        break;
    }

    case WM_NOTIFY: {
        NMHDR* pnm = reinterpret_cast<NMHDR*>(lParam);
        if (pnm->idFrom != IDC_LIST_ALERTS) break;

        if (pnm->code == NM_DBLCLK) {
            // Single-click on the Link column opens the URL in the browser.
            NMITEMACTIVATE* nia = reinterpret_cast<NMITEMACTIVATE*>(lParam);
            if (nia->iSubItem == 2 && nia->iItem >= 0) {
                wchar_t url[2048] = {};
                ListView_GetItemText(pnm->hwndFrom, nia->iItem, 2, url, 2048);
                if (wcslen(url) > 0)
                    ShellExecuteW(nullptr, L"open", url,
                        nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
        else if (pnm->code == LVN_COLUMNCLICK) {
            NMLISTVIEW* nlv = reinterpret_cast<NMLISTVIEW*>(lParam);
            if (nlv->iSubItem == g_sortCol)
                g_sortDir = -g_sortDir;   // same column: reverse direction
            else {
                g_sortCol = nlv->iSubItem;
                g_sortDir = 1;
            }
            ListView_SortItemsEx(pnm->hwndFrom, AlertSortProc,
                reinterpret_cast<LPARAM>(pnm->hwndFrom));
        }
        break;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        break;

    case WM_DESTROY: {
        HKEY hKey = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_PATH, 0, nullptr,
            REG_OPTION_NON_VOLATILE, KEY_WRITE,
            nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
            // Save column widths
            HWND hList = GetDlgItem(hwnd, IDC_LIST_ALERTS);
            for (int i = 0; i < 7; ++i) {
                wchar_t name[32];
                swprintf_s(name, L"ColWidth%d", i);
                DWORD w = static_cast<DWORD>(ListView_GetColumnWidth(hList, i));
                RegSetValueExW(hKey, name, 0, REG_DWORD,
                    reinterpret_cast<const BYTE*>(&w), sizeof(DWORD));
            }
            // Save window position and size
            RECT r{};
            GetWindowRect(hwnd, &r);
            DWORD ww = static_cast<DWORD>(r.right - r.left);
            DWORD wh = static_cast<DWORD>(r.bottom - r.top);
            DWORD wx = static_cast<DWORD>(r.left);   // stored as DWORD (two's complement safe)
            DWORD wy = static_cast<DWORD>(r.top);
            RegSetValueExW(hKey, L"AlertsWidth", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&ww), sizeof(DWORD));
            RegSetValueExW(hKey, L"AlertsHeight", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&wh), sizeof(DWORD));
            RegSetValueExW(hKey, L"AlertsLeft", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&wx), sizeof(DWORD));
            RegSetValueExW(hKey, L"AlertsTop", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&wy), sizeof(DWORD));
            RegCloseKey(hKey);
        }
        g_hwndAlerts = nullptr;
        g_hasAlert = false;
        if (g_cfg.icon_chg) UpdateTrayIcon(false);
        break;
    }

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

static void ShowAlertsWindow() {
    if (g_hwndAlerts) { SetForegroundWindow(g_hwndAlerts); return; }

    // Restore saved position and size; fall back to defaults if not saved yet.
    int wx = CW_USEDEFAULT, wy = CW_USEDEFAULT, ww = 760, wh = 480;
    {
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH, 0,
            KEY_READ, &hKey) == ERROR_SUCCESS) {
            auto rd = [&](const wchar_t* name) -> DWORD {
                DWORD v = 0, sz = sizeof(v), type = REG_DWORD;
                RegQueryValueExW(hKey, name, nullptr, &type,
                    reinterpret_cast<LPBYTE>(&v), &sz);
                return v;
                };
            DWORD savedW = rd(L"AlertsWidth");
            if (savedW > 0) {
                int rx = static_cast<int>(rd(L"AlertsLeft"));
                int ry = static_cast<int>(rd(L"AlertsTop"));
                int rw = static_cast<int>(savedW);
                int rh = static_cast<int>(rd(L"AlertsHeight"));

                // Validate: coordinates 0–9999, dimensions 100–9999.
                // Any value outside these ranges indicates corrupt/stale data.
                if (rx >= 0 && rx <= 9999 &&
                    ry >= 0 && ry <= 9999 &&
                    rw >= 100 && rw <= 9999 &&
                    rh >= 100 && rh <= 9999) {
                    wx = rx; wy = ry; ww = rw; wh = rh;
                }
                // If validation fails, wx/wy/ww/wh keep their default values.
            }
            RegCloseKey(hKey);
        }
    }

    g_hwndAlerts = CreateWindowExW(
        0, WC_ALERTS, L"GNZ Alerts",
        WS_OVERLAPPEDWINDOW,
        wx, wy, ww, wh,
        nullptr, nullptr, g_hInst, nullptr);

    if (g_hwndAlerts) {
        ShowWindow(g_hwndAlerts, SW_SHOW);
        UpdateWindow(g_hwndAlerts);
    }
}

// ---------------------------------------------------------------------------
// Tray context menu
// ---------------------------------------------------------------------------

static void ShowTrayMenu() {
    POINT pt{}; GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();
    InsertMenuW(hMenu, (UINT)-1, MF_BYPOSITION | MF_STRING, IDM_SHOW, L"Show Alerts");
    InsertMenuW(hMenu, (UINT)-1, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    InsertMenuW(hMenu, (UINT)-1, MF_BYPOSITION | MF_STRING, IDM_CONFIGURE, L"Configure...");
    InsertMenuW(hMenu, (UINT)-1, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    InsertMenuW(hMenu, (UINT)-1, MF_BYPOSITION | MF_STRING, IDM_EXIT, L"Exit");

    // Required so the menu closes if the user clicks elsewhere
    SetForegroundWindow(g_hwndMain);
    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN,
        pt.x, pt.y, 0, g_hwndMain, nullptr);
    DestroyMenu(hMenu);
}

// ---------------------------------------------------------------------------
// Main (hidden) window
// ---------------------------------------------------------------------------

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg,
    WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g_hwndMain = hwnd;
        AddTrayIcon();
        break;

    case WM_TRAYICON:
        switch (LOWORD(lParam)) {
        case WM_LBUTTONDBLCLK:
            ShowAlertsWindow();
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowTrayMenu();
            break;
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_SHOW:      ShowAlertsWindow(); break;
        case IDM_CONFIGURE: ShowConfigWindow(); break;
        case IDM_EXIT:      DestroyWindow(hwnd); break;
        }
        break;

    case WM_NEW_ALERTS: {
        // Drain pending update from poll thread
        PendingUpdate upd;
        EnterCriticalSection(&g_pendingCS);
        upd = std::move(g_pending);
        g_pending.alerts.clear();
        g_pending.full_refresh = false;
        LeaveCriticalSection(&g_pendingCS);

        // When inactive, discard silently — no list update, no notifications
        if (!g_cfg.active) break;

        if (upd.full_refresh) {
            // ── Initial load / reconnect ─────────────────────────────────────
            // Populate the list with existing data silently. Never notify here
            // regardless of what alerts are present — they are not new events.
            g_alerts.clear();
            for (auto& a : upd.alerts) g_alerts.push_back(a);
            if (g_hwndAlerts)
                PopulateAlertsList(GetDlgItem(g_hwndAlerts, IDC_LIST_ALERTS));
            g_initialLoadDone = true;   // allow notifications from now on
            break;
        }

        // ── Incremental update ───────────────────────────────────────────────
        for (auto& a : upd.alerts) g_alerts.push_back(a);
        if (g_hwndAlerts) {
            HWND hList = GetDlgItem(g_hwndAlerts, IDC_LIST_ALERTS);
            for (const auto& a : upd.alerts)
                AddAlertToListView(hList, a);
        }

        // Notify only after the initial load and only for genuinely new alerts.
        // The alerts list window always shows everything unconditionally above.
        if (g_initialLoadDone && !upd.alerts.empty()) {
            // Find the most-recent alert that passes the addressed-to-me filter.
            const Alert* notify = nullptr;
            for (auto it = upd.alerts.rbegin(); it != upd.alerts.rend(); ++it) {
                if (!g_cfg.addressed_only ||
                    _wcsicmp(Utf8ToWide(it->user).c_str(),
                        g_windowsUser.c_str()) == 0) {
                    notify = &(*it);
                    break;
                }
            }
            if (notify) {
                if (g_cfg.sound)    MessageBeep(MB_ICONINFORMATION);
                if (g_cfg.icon_chg) { g_hasAlert = true; UpdateTrayIcon(true); }
                if (g_cfg.banner)   ShowBanner(*notify);
            }
        }
        break;
    }

    case WM_POLL_STATUS:
        UpdateTrayTip(wParam == 1);
        break;

    case WM_DESTROY:
        StopPoll();
        RemoveTrayIcon();
        if (g_hwndAlerts) { DestroyWindow(g_hwndAlerts); g_hwndAlerts = nullptr; }
        if (g_hwndConfig) { DestroyWindow(g_hwndConfig); g_hwndConfig = nullptr; }
        if (g_hwndBanner) { DestroyWindow(g_hwndBanner); g_hwndBanner = nullptr; }
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Auto-start (HKCU Run key)
// ---------------------------------------------------------------------------

static bool InstallAutoRun() {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS)
        return false;

    LONG rc = RegSetValueExW(hKey, L"GNZNotificationAgent", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(exePath),
        static_cast<DWORD>((wcslen(exePath) + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);
    return rc == ERROR_SUCCESS;
}

static bool UninstallAutoRun() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS)
        return false;

    LONG rc = RegDeleteValueW(hKey, L"GNZNotificationAgent");
    RegCloseKey(hKey);
    // ERROR_FILE_NOT_FOUND means it was already absent — treat as success.
    return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
}

// ---------------------------------------------------------------------------
// WinMain
// ---------------------------------------------------------------------------

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    // Handle install / uninstall command-line arguments before any UI is set up.
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv && argc >= 2) {
            if (_wcsicmp(argv[1], L"install") == 0) {
                LocalFree(argv);
                if (InstallAutoRun())
                    MessageBoxW(nullptr,
                        L"GNZ Notification Agent has been registered to start automatically on login.",
                        L"GNZ Agent — Installed", MB_OK | MB_ICONINFORMATION);
                else
                    MessageBoxW(nullptr,
                        L"Failed to register auto-start.\n"
                        L"Check that the executable path is accessible.",
                        L"GNZ Agent — Error", MB_OK | MB_ICONERROR);
                return 0;
            }
            if (_wcsicmp(argv[1], L"uninstall") == 0) {
                LocalFree(argv);
                if (UninstallAutoRun())
                    MessageBoxW(nullptr,
                        L"GNZ Notification Agent has been removed from auto-start.",
                        L"GNZ Agent — Uninstalled", MB_OK | MB_ICONINFORMATION);
                else
                    MessageBoxW(nullptr,
                        L"Failed to remove auto-start registration.",
                        L"GNZ Agent — Error", MB_OK | MB_ICONERROR);
                return 0;
            }
        }
        if (argv) LocalFree(argv);
    }

    g_hInst = hInst;

    // Enable DPI awareness for clean rendering on high-DPI displays
    SetProcessDPIAware();

    // Initialise Common Controls (required for ListView)
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);

    InitializeCriticalSection(&g_pendingCS);

    // Load config from registry (create keys + defaults if missing)
    // Capture the logged-on username once for the addressed-to-me filter.
    {
        wchar_t uname[256] = {};
        DWORD   len = 256;
        if (GetUserNameW(uname, &len)) g_windowsUser = uname;
    }

    LoadConfig();

    // Load icons: system stock icons used to avoid needing a .rc file.
    // Replace with LoadImageW from your own .ico for a custom icon.
    g_iconNormal = static_cast<HICON>(
        LoadImageW(hInst, MAKEINTRESOURCEW(IDI_GNZNOTIFICATIONAGENT),
            IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    g_iconAlert = static_cast<HICON>(
        LoadImageW(hInst, MAKEINTRESOURCEW(IDI_GNZNOTIFICATIONAGENT),
            IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));

    // Register window classes
    auto reg = [&](const wchar_t* cls, WNDPROC proc, HBRUSH bg) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = proc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = bg;
        wc.hIcon = static_cast<HICON>(LoadImageW(
            hInst, MAKEINTRESOURCEW(IDI_GNZNOTIFICATIONAGENT),
            IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
        wc.lpszClassName = cls;
        RegisterClassExW(&wc);
        };

    reg(WC_MAIN, MainWndProc, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
    reg(WC_ALERTS, AlertsWndProc, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
    reg(WC_CONFIG, ConfigWndProc, reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1));
    reg(WC_BANNER, BannerWndProc, reinterpret_cast<HBRUSH>(COLOR_INFOBK + 1));

    // Create the hidden main window (hosts tray icon and message loop target)
    g_hwndMain = CreateWindowExW(
        0, WC_MAIN, L"GNZ Notification Agent",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1, 1,
        nullptr, nullptr, hInst, nullptr);
    if (!g_hwndMain) return 1;
    // Window stays hidden; ShowWindow is intentionally not called.

    // Create wake event and start poll thread
    g_pollWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    StartPoll();

    // Message loop — IsDialogMessageW enables Tab navigation in modeless windows
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (g_hwndConfig && IsDialogMessageW(g_hwndConfig, &msg)) continue;
        if (g_hwndAlerts && IsDialogMessageW(g_hwndAlerts, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Cleanup
    if (g_pollWake) { CloseHandle(g_pollWake); g_pollWake = nullptr; }
    DeleteCriticalSection(&g_pendingCS);

    return static_cast<int>(msg.wParam);
}

