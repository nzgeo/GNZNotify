// =============================================================================
// gnz_notification_server.cpp
// GNZ Notification Service  —  Windows Service
// =============================================================================
//
// Build (MSVC x64, from a Visual Studio Developer Command Prompt):
//   cl /EHsc /std:c++17 /W3 /O2 gnz_notification_server.cpp ^
//      /link ws2_32.lib advapi32.lib
//
// No third-party headers required; uses raw Winsock2 for HTTP.
//
// Usage:
//   gnz_notification_server.exe install    – install service + write registry defaults
//   gnz_notification_server.exe uninstall  – stop + remove service
//   gnz_notification_server.exe run        – run interactively for debugging
//   (no args)                              – called by SCM when starting as service
//
// Registry (HKLM\SOFTWARE\GNZ\NotificationService):
//   CacheSize    REG_DWORD   default 300   – max alerts kept in memory
//   WebhookPort  REG_DWORD   default 8080  – Jira webhook listener port
//   ApiPort      REG_DWORD   default 8081  – client-facing API port
//   JiraPath     REG_SZ      default ""    – Jira base URL, e.g. http://jira.example.com
//                                            Used to build /browse/<key> links in alerts.
//
// Webhook endpoint (WebhookPort):
//   POST /webhook   – Jira sends here; body is JSON
//
// API endpoints (ApiPort), no authentication:
//   GET  /api/count          – {"count": N}
//   GET  /api/alerts         – all alerts as JSON array
//   GET  /api/alerts?from=N1&to=N2  – 0-based index range, inclusive
//   DELETE /api/alerts       – clear all alerts
//   DELETE /api/alerts/:id   – remove one alert by its ID
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0601     // Windows 7+

#include <winsock2.h>           // MUST precede windows.h
#include <ws2tcpip.h>
#include <windows.h>
#include <winsvc.h>

#include <deque>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <sstream>
#include <atomic>
#include <algorithm>
#include <cstdio>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(linker, "/SUBSYSTEM:CONSOLE")

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr wchar_t SVC_NAME[] = L"GNZ_Notification_Service";
static constexpr wchar_t SVC_DISPLAY[] = L"GNZ Notification Service";
static constexpr wchar_t SVC_DESC[] = L"Receives Jira webhooks and relays alerts to GNZ agents";
static constexpr wchar_t REG_PATH[] = L"SOFTWARE\\GNZ\\NotificationService";

static constexpr DWORD DEF_CACHE_SIZE = 300;
static constexpr DWORD DEF_WEBHOOK_PORT = 8080;
static constexpr DWORD DEF_API_PORT = 8081;

// ---------------------------------------------------------------------------
// Data types
// ---------------------------------------------------------------------------

struct Alert {
    int         id = 0;
    std::string timestamp;
    std::string event;
    std::string issue_key;
    std::string summary;
    std::string project;
    std::string priority;
    std::string reporter;
    std::string status;
    std::string link;      // full Jira browse URL, e.g. http://jira/browse/PROJ-1
    std::string user;      // assignee login name (empty if unassigned)
};

struct ServerConfig {
    DWORD       cache_size = DEF_CACHE_SIZE;
    DWORD       webhook_port = DEF_WEBHOOK_PORT;
    DWORD       api_port = DEF_API_PORT;
    std::string jira_path;   // e.g. "http://jira.example.com"  (no trailing slash)
};

static ServerConfig g_cfg;

// ---------------------------------------------------------------------------
// Registry helpers
// ---------------------------------------------------------------------------

static DWORD RegReadDword(HKEY hKey, const wchar_t* name, DWORD def) {
    DWORD val = def, sz = sizeof(val), type = REG_DWORD;
    RegQueryValueExW(hKey, name, nullptr, &type,
        reinterpret_cast<LPBYTE>(&val), &sz);
    return val;
}

static void RegWriteDword(HKEY hKey, const wchar_t* name, DWORD val) {
    RegSetValueExW(hKey, name, 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&val), sizeof(DWORD));
}

// Reads a REG_SZ value and returns it as a UTF-8 std::string.
static std::string RegReadString(HKEY hKey, const wchar_t* name) {
    wchar_t buf[1024] = {};
    DWORD sz = sizeof(buf), type = REG_SZ;
    if (RegQueryValueExW(hKey, name, nullptr, &type,
        reinterpret_cast<LPBYTE>(buf), &sz) != ERROR_SUCCESS)
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string result(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, &result[0], n, nullptr, nullptr);
    return result;
}

static void LoadConfig() {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, REG_PATH, 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_READ,
        nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        g_cfg.cache_size = RegReadDword(hKey, L"CacheSize", DEF_CACHE_SIZE);
        g_cfg.webhook_port = RegReadDword(hKey, L"WebhookPort", DEF_WEBHOOK_PORT);
        g_cfg.api_port = RegReadDword(hKey, L"ApiPort", DEF_API_PORT);
        g_cfg.jira_path = RegReadString(hKey, L"JiraPath");
        // Strip trailing slash so we can always append /browse/KEY safely
        while (!g_cfg.jira_path.empty() && g_cfg.jira_path.back() == '/')
            g_cfg.jira_path.pop_back();
        RegCloseKey(hKey);
    }
}

// Writes defaults only for values that don't already exist.
static void WriteDefaultsIfMissing() {
    HKEY  hKey = nullptr;
    DWORD disp = 0;
    LONG  rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, REG_PATH, 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE,
        nullptr, &hKey, &disp);
    if (rc != ERROR_SUCCESS) return;

    auto writeIfAbsent = [&](const wchar_t* name, DWORD val) {
        DWORD tmp, sz = sizeof(tmp);
        if (RegQueryValueExW(hKey, name, nullptr, nullptr,
            reinterpret_cast<LPBYTE>(&tmp), &sz) != ERROR_SUCCESS)
            RegWriteDword(hKey, name, val);
        };
    writeIfAbsent(L"CacheSize", DEF_CACHE_SIZE);
    writeIfAbsent(L"WebhookPort", DEF_WEBHOOK_PORT);
    writeIfAbsent(L"ApiPort", DEF_API_PORT);
    // JiraPath is optional — not written here.
    // If absent, the server auto-detects the Jira base URL from the webhook payload.
    // Set it manually: HKLM\SOFTWARE\GNZ\NotificationService\JiraPath (REG_SZ).
    RegCloseKey(hKey);
}

// ---------------------------------------------------------------------------
// Thread-safe alert store
// ---------------------------------------------------------------------------

class AlertStore {
public:
    AlertStore() { InitializeCriticalSection(&cs_); }
    ~AlertStore() { DeleteCriticalSection(&cs_); }

    // Appends an alert, trims to cache size. Returns the assigned ID.
    int Push(Alert a) {
        EnterCriticalSection(&cs_);
        a.id = next_id_++;
        store_.push_back(std::move(a));
        while (store_.size() > static_cast<size_t>(g_cfg.cache_size))
            store_.pop_front();
        int id = store_.back().id;
        LeaveCriticalSection(&cs_);
        return id;
    }

    int Count() {
        EnterCriticalSection(&cs_);
        int n = static_cast<int>(store_.size());
        LeaveCriticalSection(&cs_);
        return n;
    }

    std::vector<Alert> GetAll() {
        EnterCriticalSection(&cs_);
        std::vector<Alert> r(store_.begin(), store_.end());
        LeaveCriticalSection(&cs_);
        return r;
    }

    // 0-based index range [from, to], inclusive.
    std::vector<Alert> GetRange(int from, int to) {
        EnterCriticalSection(&cs_);
        int sz = static_cast<int>(store_.size());
        from = (std::max)(0, from);
        to = (std::min)(to, sz - 1);
        std::vector<Alert> r;
        for (int i = from; i <= to; ++i)
            r.push_back(store_[static_cast<size_t>(i)]);
        LeaveCriticalSection(&cs_);
        return r;
    }

    void Clear() {
        EnterCriticalSection(&cs_);
        store_.clear();
        LeaveCriticalSection(&cs_);
    }

    // Returns true if an alert with this ID was found and removed.
    bool RemoveById(int id) {
        EnterCriticalSection(&cs_);
        bool found = false;
        for (auto it = store_.begin(); it != store_.end(); ++it) {
            if (it->id == id) { store_.erase(it); found = true; break; }
        }
        LeaveCriticalSection(&cs_);
        return found;
    }

private:
    CRITICAL_SECTION  cs_;
    std::deque<Alert> store_;
    int               next_id_ = 1;
};

static AlertStore g_store;

// ---------------------------------------------------------------------------
// Minimal JSON helpers
// ---------------------------------------------------------------------------

// Escapes a UTF-8 string for embedding in a JSON value.
static std::string JsonEsc(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n";  break;
        case '\r': o += "\\r";  break;
        case '\t': o += "\\t";  break;
        default:
            if (c < 0x20) {
                char b[8];
                snprintf(b, sizeof(b), "\\u%04x", c);
                o += b;
            }
            else {
                o += static_cast<char>(c);
            }
        }
    }
    return o;
}

// Extracts the string value for a JSON key. Not a full parser; handles the
// flat / one-level-nested patterns that Jira webhooks produce.
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

static std::string AlertToJson(const Alert& a) {
    std::ostringstream ss;
    ss << '{'
        << "\"id\":" << a.id << ','
        << "\"timestamp\":\"" << JsonEsc(a.timestamp) << "\","
        << "\"event\":\"" << JsonEsc(a.event) << "\","
        << "\"issue_key\":\"" << JsonEsc(a.issue_key) << "\","
        << "\"summary\":\"" << JsonEsc(a.summary) << "\","
        << "\"project\":\"" << JsonEsc(a.project) << "\","
        << "\"priority\":\"" << JsonEsc(a.priority) << "\","
        << "\"reporter\":\"" << JsonEsc(a.reporter) << "\","
        << "\"status\":\"" << JsonEsc(a.status) << "\","
        << "\"link\":\"" << JsonEsc(a.link) << "\","
        << "\"user\":\"" << JsonEsc(a.user) << "\""
        << '}';
    return ss.str();
}

static std::string AlertsToJson(const std::vector<Alert>& v) {
    std::ostringstream ss;
    ss << "{\"count\":" << v.size() << ",\"alerts\":[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) ss << ',';
        ss << AlertToJson(v[i]);
    }
    ss << "]}";
    return ss.str();
}

static std::string LocalTimestamp() {
    SYSTEMTIME st; GetLocalTime(&st);
    char b[32];
    snprintf(b, sizeof(b), "%04d-%02d-%02dT%02d:%02d:%02d",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);
    return b;
}

// ---------------------------------------------------------------------------
// Jira webhook parsing
// ---------------------------------------------------------------------------
//
// Standard Jira webhook structure (Server, Data Center, and Cloud):
//
//   {
//     "webhookEvent": "jira:issue_updated",
//     "user":  { "key": "JIRAUSER10071", ... },   ← user who triggered it
//     "issue": {
//       "key": "PROJ-123",                         ← the issue key we want
//       "fields": {
//         "summary":  "...",
//         "priority": { "name": "High" },
//         "status":   { "name": "In Progress" },
//         "reporter": { "displayName": "Jane Smith" },
//         "project":  { "key": "PROJ", ... }
//       }
//     }
//   }
//
// All parsing is scoped to the "issue" object (and "fields" sub-object) to
// avoid false matches on identically-named keys in "user" or other objects.

static Alert ParseJiraBody(const std::string& body) {
    Alert a;
    a.timestamp = LocalTimestamp();
    a.event = JsonGet(body, "webhookEvent");

    // Locate the opening brace of the "issue" object.
    size_t ip = body.find("\"issue\"");
    if (ip != std::string::npos) {
        size_t brace = body.find('{', ip + 7);   // skip past "issue":
        if (brace != std::string::npos) {
            std::string issue = body.substr(brace);

            // "key" is a direct field of the issue object (e.g. "PROJ-123").
            a.issue_key = JsonGet(issue, "key");

            // Everything else lives inside issue.fields.
            size_t fp = issue.find("\"fields\"");
            if (fp != std::string::npos) {
                std::string fields = issue.substr(fp);

                a.summary = JsonGet(fields, "summary");

                size_t pp = fields.find("\"priority\"");
                if (pp != std::string::npos)
                    a.priority = JsonGet(fields.substr(pp), "name");

                size_t sp = fields.find("\"status\"");
                if (sp != std::string::npos)
                    a.status = JsonGet(fields.substr(sp), "name");

                size_t rp = fields.find("\"reporter\"");
                if (rp != std::string::npos)
                    a.reporter = JsonGet(fields.substr(rp), "displayName");

                size_t projp = fields.find("\"project\"");
                if (projp != std::string::npos)
                    a.project = JsonGet(fields.substr(projp), "key");

                // assignee.name — empty string if the issue is unassigned
                size_t ap = fields.find("\"assignee\"");
                if (ap != std::string::npos)
                    a.user = JsonGet(fields.substr(ap), "name");
            }
        }
    }

    if (a.event.empty())   a.event = "jira:webhook";
    if (a.summary.empty()) a.summary = "(no summary)";

    return a;
}

// ---------------------------------------------------------------------------
// Minimal HTTP/1.1 server  (raw Winsock, thread-per-connection)
// ---------------------------------------------------------------------------
//
// HttpRequest / HttpResponse mirror the subset of the httplib API used by
// the route handler lambdas below, so those lambdas are unchanged.

struct HttpMatch {
    std::string val;
    std::string str() const { return val; }
};

struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
    std::map<std::string, std::string> params;    // parsed query-string
    std::map<std::string, std::string> headers;   // lowercase header names
    HttpMatch   matches[2];  // matches[1] = path suffix for prefix routes

    bool has_param(const std::string& k) const {
        return params.find(k) != params.end();
    }
    std::string get_param_value(const std::string& k) const {
        auto it = params.find(k);
        return it != params.end() ? it->second : "";
    }
};

struct HttpResponse {
    int         status = 200;
    std::string body;
    std::string content_type;

    void set_content(const std::string& b, const std::string& ct) {
        body = b; content_type = ct;
    }
};

using RouteHandler = std::function<void(const HttpRequest&, HttpResponse&)>;

struct Route {
    std::string  method;
    std::string  pattern;
    bool         prefix_match;  // true → path must start with pattern
    RouteHandler handler;
};

class HttpServer {
public:
    HttpServer() = default;
    ~HttpServer() { Stop(); }

    // Route registration — mirrors httplib method names.
    void Get(const std::string& p, RouteHandler h) { Add("GET", p, false, std::move(h)); }
    void Post(const std::string& p, RouteHandler h) { Add("POST", p, false, std::move(h)); }
    void Delete(const std::string& p, RouteHandler h) { Add("DELETE", p, false, std::move(h)); }
    // Prefix variant used for /api/alerts/:id (replaces httplib regex route).
    void DeletePrefix(const std::string& p, RouteHandler h) { Add("DELETE", p, true, std::move(h)); }

    // Start listening (non-blocking). Returns false if bind/listen fails.
    bool Start(int port);

    // Stop and join the accept thread (blocks at most ~500 ms).
    void Stop();

private:
    void Add(const std::string& method, const std::string& pattern,
        bool prefix, RouteHandler h) {
        m_routes.push_back({ method, pattern, prefix, std::move(h) });
    }

    void AcceptLoop();
    void HandleClient(SOCKET sock);

    static DWORD WINAPI AcceptProc(LPVOID p) {
        static_cast<HttpServer*>(p)->AcceptLoop();
        return 0;
    }

    struct ClientCtx { HttpServer* srv; SOCKET sock; };
    static DWORD WINAPI ClientProc(LPVOID p) {
        auto* ctx = static_cast<ClientCtx*>(p);
        ctx->srv->HandleClient(ctx->sock);
        delete ctx;
        return 0;
    }

    SOCKET             m_sock = INVALID_SOCKET;
    HANDLE             m_thread = nullptr;
    volatile bool      m_stop = false;
    std::vector<Route> m_routes;
};

bool HttpServer::Start(int port) {
    m_stop = false;
    m_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_sock == INVALID_SOCKET) return false;

    // Allow quick re-bind during TIME_WAIT.
    BOOL reuse = TRUE;
    setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR,
        reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<u_short>(port));

    if (bind(m_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(m_sock, SOMAXCONN) != 0) {
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
        return false;
    }

    m_thread = CreateThread(nullptr, 0, AcceptProc, this, 0, nullptr);
    if (!m_thread) {
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
        return false;
    }
    return true;
}

void HttpServer::Stop() {
    m_stop = true;
    if (m_thread) {
        // AcceptLoop polls with a 500 ms timeout, so it exits within ~500 ms.
        WaitForSingleObject(m_thread, 3000);
        CloseHandle(m_thread);
        m_thread = nullptr;
    }
}

void HttpServer::AcceptLoop() {
    while (!m_stop) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(m_sock, &fds);
        timeval tv{ 0, 500000 };   // 500 ms poll interval
        int r = select(0, &fds, nullptr, nullptr, &tv);
        if (r > 0 && FD_ISSET(m_sock, &fds)) {
            SOCKET client = accept(m_sock, nullptr, nullptr);
            if (client != INVALID_SOCKET) {
                printf("[http] connection accepted on port\n");
                auto* ctx = new ClientCtx{ this, client };
                HANDLE t = CreateThread(nullptr, 0, ClientProc, ctx, 0, nullptr);
                if (t) CloseHandle(t);   // fire-and-forget
                else { delete ctx; closesocket(client); }
            }
        }
    }
    closesocket(m_sock);
    m_sock = INVALID_SOCKET;
}

static const char* HttpStatusText(int s) {
    switch (s) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 500: return "Internal Server Error";
    default:  return "Unknown";
    }
}

static void ParseQueryString(const std::string& qs,
    std::map<std::string, std::string>& out) {
    size_t pos = 0;
    while (pos <= qs.size()) {
        size_t amp = qs.find('&', pos);
        if (amp == std::string::npos) amp = qs.size();
        if (amp > pos) {
            std::string pair = qs.substr(pos, amp - pos);
            size_t eq = pair.find('=');
            if (eq != std::string::npos)
                out[pair.substr(0, eq)] = pair.substr(eq + 1);
            else
                out[pair] = "";
        }
        pos = amp + 1;
    }
}

// Reads into buf until the CRLFCRLF header terminator appears.
// Returns false on socket error or if headers exceed 64 KB.
static bool RecvHeaders(SOCKET sock, std::string& buf) {
    char tmp[4096];
    while (buf.find("\r\n\r\n") == std::string::npos) {
        int r = recv(sock, tmp, static_cast<int>(sizeof(tmp)), 0);
        if (r <= 0) return false;
        buf.append(tmp, static_cast<size_t>(r));
        if (buf.size() > 65536) return false;
    }
    return true;
}

void HttpServer::HandleClient(SOCKET sock) {
    // Guard against slow/stuck clients.
    DWORD to_ms = 10000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<const char*>(&to_ms), sizeof(to_ms));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
        reinterpret_cast<const char*>(&to_ms), sizeof(to_ms));

    std::string raw;
    if (!RecvHeaders(sock, raw)) { closesocket(sock); return; }

    size_t hend = raw.find("\r\n\r\n");
    if (hend == std::string::npos) { closesocket(sock); return; }

    std::string hdr_section = raw.substr(0, hend);
    std::string body_buf = raw.substr(hend + 4);

    // --- Parse request line ---
    size_t l1 = hdr_section.find("\r\n");
    if (l1 == std::string::npos) { closesocket(sock); return; }
    std::string req_line = hdr_section.substr(0, l1);

    size_t sp1 = req_line.find(' ');
    size_t sp2 = (sp1 != std::string::npos)
        ? req_line.find(' ', sp1 + 1) : std::string::npos;
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        closesocket(sock);
        return;
    }

    HttpRequest req;
    req.method = req_line.substr(0, sp1);
    std::string target = req_line.substr(sp1 + 1, sp2 - sp1 - 1);

    size_t qmark = target.find('?');
    if (qmark != std::string::npos) {
        req.path = target.substr(0, qmark);
        ParseQueryString(target.substr(qmark + 1), req.params);
    }
    else {
        req.path = target;
    }

    // --- Parse header lines (lowercase keys for easy lookup) ---
    size_t pos = l1 + 2;
    while (pos < hdr_section.size()) {
        size_t eol = hdr_section.find("\r\n", pos);
        if (eol == std::string::npos) eol = hdr_section.size();
        std::string line = hdr_section.substr(pos, eol - pos);
        pos = eol + 2;
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            size_t vs = val.find_first_not_of(" \t");
            if (vs != std::string::npos) val = val.substr(vs);
            for (char& c : key)
                c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            req.headers[key] = val;
        }
    }

    // --- Read body according to Content-Length ---
    int content_len = 0;
    {
        auto it = req.headers.find("content-length");
        if (it != req.headers.end()) {
            try { content_len = std::stoi(it->second); }
            catch (...) {}
        }
    }
    if (content_len > 0) {
        req.body = body_buf;
        while (static_cast<int>(req.body.size()) < content_len) {
            char tmp[8192];
            int need = (std::min)(static_cast<int>(sizeof(tmp)),
                content_len - static_cast<int>(req.body.size()));
            int r = recv(sock, tmp, need, 0);
            if (r <= 0) break;
            req.body.append(tmp, static_cast<size_t>(r));
        }
    }

    // --- Dispatch to first matching route ---
    HttpResponse res;
    bool dispatched = false;
    for (auto& route : m_routes) {
        if (route.method != req.method) continue;
        if (route.prefix_match) {
            if (req.path.size() >= route.pattern.size() &&
                req.path.substr(0, route.pattern.size()) == route.pattern) {
                req.matches[1].val = req.path.substr(route.pattern.size());
                route.handler(req, res);
                dispatched = true;
                break;
            }
        }
        else {
            if (req.path == route.pattern) {
                route.handler(req, res);
                dispatched = true;
                break;
            }
        }
    }

    if (!dispatched) {
        res.status = 404;
        res.set_content("{\"error\":\"not found\"}", "application/json");
    }

    // --- Send HTTP/1.1 response ---
    std::ostringstream oss;
    oss << "HTTP/1.1 " << res.status << " " << HttpStatusText(res.status) << "\r\n"
        << "Content-Type: "
        << (res.content_type.empty() ? "application/json" : res.content_type)
        << "\r\n"
        << "Content-Length: " << res.body.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << res.body;
    std::string response = oss.str();
    send(sock, response.c_str(), static_cast<int>(response.size()), 0);

    closesocket(sock);
}

// Reads JiraPath directly from the registry on every call so changes take
// effect without restarting the service.  Falls back to g_cfg.jira_path
// (loaded at startup) if the registry cannot be opened.
static std::string ReadJiraPath() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, REG_PATH, 0,
        KEY_READ, &hKey) == ERROR_SUCCESS) {
        std::string path = RegReadString(hKey, L"JiraPath");
        RegCloseKey(hKey);
        while (!path.empty() && path.back() == '/') path.pop_back();
        return path;
    }
    return g_cfg.jira_path;  // fallback to startup value
}

// ---------------------------------------------------------------------------
// HTTP servers
// ---------------------------------------------------------------------------

static HttpServer* g_wh = nullptr;   // webhook receiver
static HttpServer* g_api = nullptr;   // client API

// Derives the Jira base URL from the "self" field present in every Jira
// webhook payload, e.g. "https://jira.example.com/rest/api/2/issue/10000"
// → "https://jira.example.com".  Returns empty string if not found.
static std::string ExtractJiraBaseUrl(const std::string& body) {
    // "self" appears in multiple nested objects; look for the one inside "issue".
    size_t ip = body.find("\"issue\"");
    const std::string& src = (ip != std::string::npos) ? body.substr(ip) : body;

    std::string self = JsonGet(src, "self");
    if (self.empty()) return {};

    // Strip the /rest/... path to get just scheme + host (+ optional port).
    size_t restPos = self.find("/rest/");
    if (restPos != std::string::npos)
        return self.substr(0, restPos);

    // Fallback: strip everything after the third slash (scheme://host/...).
    size_t schemeEnd = self.find("://");
    if (schemeEnd == std::string::npos) return {};
    size_t pathStart = self.find('/', schemeEnd + 3);
    return (pathStart != std::string::npos) ? self.substr(0, pathStart) : self;
}

static void SetupWebhookServer(HttpServer& s) {
    s.Post("/webhook", [](const HttpRequest& req, HttpResponse& res) {
        printf("[webhook] POST /webhook  body_len=%zu\n", req.body.size());
        Alert a = ParseJiraBody(req.body);

        // Build the browse link.
        // Priority: configured JiraPath (re-read live) → auto-detected from "self".
        if (!a.issue_key.empty()) {
            std::string jiraPath = ReadJiraPath();
            std::string base = jiraPath.empty()
                ? ExtractJiraBaseUrl(req.body)
                : jiraPath;
            if (!base.empty())
                a.link = base + "/browse/" + a.issue_key;
        }

        // Capture fields before moving into the store
        std::string ev = a.event;
        std::string key = a.issue_key;
        std::string link = a.link;
        int id = g_store.Push(std::move(a));
        printf("[webhook] stored alert id=%d  event=%s  key=%s  link=%s\n",
            id, ev.c_str(), key.c_str(), link.c_str());
        res.set_content(
            "{\"received\":true,\"id\":" + std::to_string(id) + "}",
            "application/json");
        });
}

static void SetupApiServer(HttpServer& s) {
    // GET /api/count
    s.Get("/api/count", [](const HttpRequest&, HttpResponse& res) {
        res.set_content(
            "{\"count\":" + std::to_string(g_store.Count()) + "}",
            "application/json");
        });

    // GET /api/alerts[?from=N1&to=N2]
    s.Get("/api/alerts", [](const HttpRequest& req, HttpResponse& res) {
        std::vector<Alert> v;
        if (req.has_param("from") && req.has_param("to")) {
            try {
                int f = std::stoi(req.get_param_value("from"));
                int t = std::stoi(req.get_param_value("to"));
                v = g_store.GetRange(f, t);
            }
            catch (...) {
                res.status = 400;
                res.set_content("{\"error\":\"invalid range\"}",
                    "application/json");
                return;
            }
        }
        else {
            v = g_store.GetAll();
        }
        res.set_content(AlertsToJson(v), "application/json");
        });

    // DELETE /api/alerts  — clear all
    s.Delete("/api/alerts", [](const HttpRequest&, HttpResponse& res) {
        g_store.Clear();
        res.set_content("{\"cleared\":true}", "application/json");
        });

    // DELETE /api/alerts/:id  — remove by alert ID.
    // DeletePrefix stores the trailing path segment in req.matches[1].
    s.DeletePrefix("/api/alerts/", [](const HttpRequest& req, HttpResponse& res) {
        try {
            int  id = std::stoi(req.matches[1].str());
            bool ok = g_store.RemoveById(id);
            if (ok) {
                res.set_content("{\"removed\":true}", "application/json");
            }
            else {
                res.status = 404;
                res.set_content("{\"removed\":false,\"error\":\"not found\"}",
                    "application/json");
            }
        }
        catch (...) {
            res.status = 400;
            res.set_content("{\"error\":\"invalid id\"}", "application/json");
        }
        });
}

// ---------------------------------------------------------------------------
// Windows Service lifecycle
// ---------------------------------------------------------------------------

static SERVICE_STATUS        g_svcStatus{};
static SERVICE_STATUS_HANDLE g_svcHandle = nullptr;
static HANDLE                g_stopEvent = nullptr;

static void ReportSvcStatus(DWORD state, DWORD hint = 0) {
    static DWORD checkpoint = 1;
    g_svcStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_svcStatus.dwCurrentState = state;
    g_svcStatus.dwControlsAccepted = (state == SERVICE_RUNNING)
        ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    g_svcStatus.dwWin32ExitCode = NO_ERROR;
    g_svcStatus.dwCheckPoint =
        (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkpoint++;
    g_svcStatus.dwWaitHint = hint;
    SetServiceStatus(g_svcHandle, &g_svcStatus);
}

VOID WINAPI ServiceCtrlHandler(DWORD ctrl) {
    if (ctrl == SERVICE_CONTROL_STOP || ctrl == SERVICE_CONTROL_SHUTDOWN) {
        ReportSvcStatus(SERVICE_STOP_PENDING, 6000);
        // Stop() is blocking but returns within ~500 ms (one select poll cycle).
        if (g_wh)  g_wh->Stop();
        if (g_api) g_api->Stop();
        SetEvent(g_stopEvent);
    }
}

VOID WINAPI ServiceMain(DWORD, LPTSTR*) {
    g_svcHandle = RegisterServiceCtrlHandlerW(SVC_NAME, ServiceCtrlHandler);
    if (!g_svcHandle) return;

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stopEvent) {
        ReportSvcStatus(SERVICE_STOPPED);
        return;
    }

    ReportSvcStatus(SERVICE_START_PENDING, 3000);
    LoadConfig();

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        CloseHandle(g_stopEvent);
        ReportSvcStatus(SERVICE_STOPPED);
        return;
    }

    g_wh = new HttpServer();
    g_api = new HttpServer();
    SetupWebhookServer(*g_wh);
    SetupApiServer(*g_api);

    if (!g_wh->Start(static_cast<int>(g_cfg.webhook_port)) ||
        !g_api->Start(static_cast<int>(g_cfg.api_port))) {
        delete g_wh;  g_wh = nullptr;
        delete g_api; g_api = nullptr;
        WSACleanup();
        CloseHandle(g_stopEvent);
        ReportSvcStatus(SERVICE_STOPPED);
        return;
    }

    ReportSvcStatus(SERVICE_RUNNING);
    if (g_stopEvent) {
        WaitForSingleObject(g_stopEvent, INFINITE);
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }

    delete g_wh;  g_wh = nullptr;
    delete g_api; g_api = nullptr;

    WSACleanup();
    ReportSvcStatus(SERVICE_STOPPED);
}

// ---------------------------------------------------------------------------
// Service install / uninstall
// ---------------------------------------------------------------------------

static bool InstallService() {
    wchar_t exePath[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) {
        wprintf(L"GetModuleFileName failed: %lu\n", GetLastError());
        return false;
    }

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        wprintf(L"OpenSCManager failed: %lu\n", GetLastError());
        return false;
    }

    SC_HANDLE svc = CreateServiceW(
        scm, SVC_NAME, SVC_DISPLAY,
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        exePath, nullptr, nullptr, nullptr, nullptr, nullptr);

    if (!svc) {
        DWORD e = GetLastError();
        CloseServiceHandle(scm);
        if (e == ERROR_SERVICE_EXISTS) {
            wprintf(L"Service already installed.\n");
            return true;
        }
        wprintf(L"CreateService failed: %lu\n", e);
        return false;
    }

    SERVICE_DESCRIPTIONW desc{ const_cast<wchar_t*>(SVC_DESC) };
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);

    WriteDefaultsIfMissing();

    wprintf(L"Service installed successfully.\n");
    wprintf(L"Registry: HKLM\\%s\n", REG_PATH);
    wprintf(L"  CacheSize=%lu  WebhookPort=%lu  ApiPort=%lu\n",
        DEF_CACHE_SIZE, DEF_WEBHOOK_PORT, DEF_API_PORT);
    return true;
}

static bool UninstallService() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) { wprintf(L"OpenSCManager failed: %lu\n", GetLastError()); return false; }

    SC_HANDLE svc = OpenServiceW(
        scm, SVC_NAME, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (!svc) {
        CloseServiceHandle(scm);
        wprintf(L"Service not found.\n");
        return false;
    }

    // Stop the service if it is running
    SERVICE_STATUS ss{};
    ControlService(svc, SERVICE_CONTROL_STOP, &ss);
    for (int i = 0; i < 40 && ss.dwCurrentState != SERVICE_STOPPED; ++i) {
        Sleep(500);
        if (!QueryServiceStatus(svc, &ss)) break;
    }

    bool ok = DeleteService(svc) != FALSE;
    if (!ok) wprintf(L"DeleteService failed: %lu\n", GetLastError());
    else     wprintf(L"Service uninstalled.\n");

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}

// ---------------------------------------------------------------------------
// Console Ctrl+C handler (console/debug mode only)
// ---------------------------------------------------------------------------

static BOOL WINAPI ConsoleCtrlHandler(DWORD) {
    if (g_wh)  g_wh->Stop();
    if (g_api) g_api->Stop();
    if (g_stopEvent) SetEvent(g_stopEvent);
    return TRUE;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int wmain(int argc, wchar_t* argv[]) {
    if (argc >= 2) {
        if (_wcsicmp(argv[1], L"install") == 0) return InstallService() ? 0 : 1;
        if (_wcsicmp(argv[1], L"uninstall") == 0) return UninstallService() ? 0 : 1;

        if (_wcsicmp(argv[1], L"run") == 0) {
            LoadConfig();
            wprintf(L"GNZ Notification Service [console mode]\n"
                L"  Webhook port : %lu\n"
                L"  API port     : %lu\n"
                L"  Cache size   : %lu\n"
                L"Press Ctrl+C to stop.\n",
                g_cfg.webhook_port, g_cfg.api_port, g_cfg.cache_size);

            WSADATA wsa{};
            if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
                wprintf(L"WSAStartup failed.\n");
                return 1;
            }

            g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

            g_wh = new HttpServer();
            g_api = new HttpServer();
            SetupWebhookServer(*g_wh);
            SetupApiServer(*g_api);
            SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

            if (!g_wh->Start(static_cast<int>(g_cfg.webhook_port)))
                wprintf(L"Warning: failed to start webhook server on port %lu\n",
                    g_cfg.webhook_port);
            if (!g_api->Start(static_cast<int>(g_cfg.api_port)))
                wprintf(L"Warning: failed to start API server on port %lu\n",
                    g_cfg.api_port);

            WaitForSingleObject(g_stopEvent, INFINITE);
            CloseHandle(g_stopEvent);
            g_stopEvent = nullptr;
            delete g_wh;  g_wh = nullptr;
            delete g_api; g_api = nullptr;
            WSACleanup();
            return 0;
        }

        wprintf(L"Usage: %s [install|uninstall|run]\n", argv[0]);
        return 1;
    }

    // No args: start as a Windows service
    SERVICE_TABLE_ENTRYW tbl[] = {
        { const_cast<wchar_t*>(SVC_NAME), ServiceMain },
        { nullptr, nullptr }
    };
    StartServiceCtrlDispatcherW(tbl);
    return 0;
}