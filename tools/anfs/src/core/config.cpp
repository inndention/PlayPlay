// core/config.cpp - anfs.json: the flat Config schema, ConfigStore, ConfigWatcher and writeFileAtomic (ANFS-F1b).
//
// A port of C# Config.cs (FromDict / ToDict / ToSafeDict / ConfigStore / ConfigWatcher) and PathUtil.WriteAtomic, so
// the C# and C++ binaries can share one data folder:
// - fromJval reads every key with the C# Json.Str/Int/Bool leniency and keeps the whole object in `raw`;
// - toJval starts from raw and overwrites the typed keys in place (new keys append), removing the absent-when-default
//   ones (port, tlsPort, test, bridgeUrl, bridgeFallbacks, agentToken, tls), so unknown keys keep their position;
// - rev and statusPort (mgmt-integration.md 1.2) are C++ additions: written when > 0, otherwise removed. The C#
//   binary keeps them in its Raw, so a rollback does not lose them.
// Differences from C#: a port outside int range is "port out of range" (C# wrapped it with an int cast); toJval does
// not normalise the mode in place (it is const; C# ToDict called Validate()); a gate rule's method list loses its
// whitespace ("get, head" -> "GET,HEAD"), so the written "METHOD pattern" text reads back as the same rule.
#include "core/config.h"

#include "core/clock.h"
#include "core/ids.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <thread>

namespace anfs {

namespace {

constexpr int64_t kMaxConfigBytes = 16LL << 20;   // anfs.json is a few KB; refuse to slurp a runaway file
constexpr int kReplaceAttempts = 40;              // MoveFileEx retries on sharing violations (40 x 25 ms)

std::wstring widen(std::string_view s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::string narrow(std::wstring_view w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

std::string winError(DWORD code)
{
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::string text = n ? narrow(std::wstring_view(buf, n)) : std::string();
    if (buf) LocalFree(buf);
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ' || text.back() == '.'))
        text.pop_back();
    return (text.empty() ? std::string("error") : text) + " (" + std::to_string(code) + ")";
}

bool isWs(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

std::string trim(std::string_view s)
{
    size_t b = 0, e = s.size();
    while (b < e && isWs(s[b])) ++b;
    while (e > b && isWs(s[e - 1])) --e;
    return std::string(s.substr(b, e - b));
}

char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string toLower(std::string_view s)
{
    std::string out(s);
    for (char& c : out) c = lowerAscii(c);
    return out;
}

std::string toUpper(std::string_view s)
{
    std::string out(s);
    for (char& c : out)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return out;
}

// C# OrdinalIgnoreCase for the ASCII keys and paths used here (folder keys, gate literals, methods).
bool iequals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (lowerAscii(a[i]) != lowerAscii(b[i])) return false;
    return true;
}

// C# Config.StrList: a string (trimmed, non-empty) or an array of strings; anything else gives nothing.
std::vector<std::string> strList(const JVal& v)
{
    std::vector<std::string> out;
    if (v.isString()) {
        std::string t = trim(v.asString());
        if (!t.empty()) out.push_back(std::move(t));
    } else if (v.isArray()) {
        for (const JVal& x : v.arr()) {
            if (!x.isString()) continue;
            std::string t = trim(x.asString());
            if (!t.empty()) out.push_back(std::move(t));
        }
    }
    return out;
}

JVal strArray(const std::vector<std::string>& v)
{
    JVal a = JVal::array();
    for (const std::string& s : v) a.push(s);
    return a;
}

// Uri.UnescapeDataString for a path segment: %XX becomes the byte, anything malformed stays as written.
std::string percentDecode(std::string_view s)
{
    auto nib = [](char c) {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    };
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && nib(s[i + 1]) >= 0 && nib(s[i + 2]) >= 0) {
            out += static_cast<char>((nib(s[i + 1]) << 4) | nib(s[i + 2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::vector<std::string_view> segments(std::string_view p)
{
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i <= p.size()) {
        const size_t j = std::min(p.find('/', i), p.size());
        if (j > i) out.push_back(p.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

// C# PathPattern.Match (HttpHost.cs:43-76) without parameter capture. A private copy: config.cpp must not need the
// router (ANFS-F4b) to link, and a gate rule only needs the yes/no answer.
bool patternMatches(std::string_view pattern, std::string_view path)
{
    const std::vector<std::string_view> ps = segments(pattern), xs = segments(path);
    for (size_t i = 0; i < ps.size(); ++i) {
        const std::string_view p = ps[i];
        const bool rest = p == "*" || (p.size() >= 3 && p.substr(0, 2) == "{*" && p.back() == '}');
        if (rest) return i == ps.size() - 1;
        if (i >= xs.size()) return false;
        if (p.size() > 2 && p.front() == '{' && p.back() == '}') continue;
        if (!iequals(p, percentDecode(xs[i]))) return false;
    }
    return ps.size() == xs.size();
}

std::string normalizedMode(std::string_view m)
{
    std::string out = toLower(trim(m));
    return out.empty() ? std::string(Config::kModeOff) : out;
}

// Ports are read as int64 and narrowed so an out-of-range value still fails validate() in the C# order.
int narrowPort(int64_t v) { return v < 0 ? -1 : v > 65535 ? 65536 : static_cast<int>(v); }

void checkPorts(int port, int tlsPort, int statusPort)
{
    if (port < 0 || port > 65535) throw ConfigError("port out of range");
    if (tlsPort < 0 || tlsPort > 65535) throw ConfigError("tlsPort out of range");
    if (statusPort < 0 || statusPort > 65535) throw ConfigError("statusPort out of range");
}

// ---- file helpers -----------------------------------------------------------------------------------------------------

struct FileSig {
    bool exists = false;
    int64_t size = -1;
    int64_t mtime = -1;
};

FileSig statFile(const std::wstring& path)
{
    FileSig s;
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return s;
    s.exists = true;
    s.size = (static_cast<int64_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    s.mtime = (static_cast<int64_t>(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
    return s;
}

// Shares read, write and delete, so an atomic replace by another writer is never blocked by this reader (it keeps
// reading the old file, which stays complete until the handle closes).
bool readFileBytes(const std::wstring& path, std::string& out, std::string* err)
{
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = winError(GetLastError());
        return false;
    }
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) != 0;
    if (ok && size.QuadPart > kMaxConfigBytes) {
        if (err) *err = "file too large";
        ok = false;
    }
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        size_t got = 0;
        while (got < out.size()) {
            DWORD n = 0;
            if (!ReadFile(h, &out[got], static_cast<DWORD>(out.size() - got), &n, nullptr)) {
                if (err) *err = winError(GetLastError());
                ok = false;
                break;
            }
            if (n == 0) break;   // the file shrank since GetFileSizeEx
            got += n;
        }
        out.resize(got);
    }
    CloseHandle(h);
    return ok;
}

// StreamReader(UTF8, detectEncodingFromByteOrderMarks) as C# reads the file: a UTF-16 BOM converts to UTF-8; a UTF-8
// BOM is left to json::parse, which skips it.
std::string decodeText(std::string bytes)
{
    const auto b = [&](size_t i) { return static_cast<unsigned char>(bytes[i]); };
    if (bytes.size() >= 2 && ((b(0) == 0xFF && b(1) == 0xFE) || (b(0) == 0xFE && b(1) == 0xFF))) {
        const bool be = b(0) == 0xFE;
        std::wstring w;
        w.reserve(bytes.size() / 2);
        for (size_t i = 2; i + 1 < bytes.size(); i += 2)
            w.push_back(static_cast<wchar_t>(be ? (b(i) << 8) | b(i + 1) : (b(i + 1) << 8) | b(i)));
        return narrow(w);
    }
    return bytes;
}

} // namespace

// ---- PasswordRecord / GateRule ----------------------------------------------------------------------------------------

bool PasswordRecord::isSet() const
{
    std::string a = toLower(algo);
    std::replace(a.begin(), a.end(), '_', '-');
    return iterations > 0 && !salt.empty() && !hash.empty() && (a == "pbkdf2-sha256" || a == "pbkdf2");
}

JVal PasswordRecord::toJval() const
{
    return JVal::object({{"algo", algo}, {"iterations", iterations}, {"salt", salt}, {"hash", hash}});
}

std::optional<PasswordRecord> PasswordRecord::fromJval(const JVal& v)
{
    if (!v.isObject()) return std::nullopt;
    PasswordRecord r;
    r.algo = v.getStr("algo", "");
    r.iterations = std::max<int64_t>(0, std::min<int64_t>(INT_MAX, v.getInt("iterations", 0)));   // C# int clamp
    r.salt = v.getStr("salt", "");
    r.hash = v.getStr("hash", "");
    return r;
}

bool GateRule::matches(std::string_view m, std::string_view path) const
{
    if (method != "*") {
        bool ok = false;
        size_t i = 0;
        while (i <= method.size() && !ok) {
            const size_t j = std::min(method.find(',', i), method.size());
            ok = iequals(trim(std::string_view(method).substr(i, j - i)), m);
            i = j + 1;
        }
        if (!ok) return false;
    }
    return patternMatches(pattern, path);
}

std::string GateRule::toString() const { return method + " " + pattern; }

std::optional<GateRule> GateRule::parse(const JVal& v)
{
    GateRule r;
    if (v.isString()) {
        const std::string s = trim(v.asString());
        const size_t sp = s.find(' ');
        if (sp != std::string::npos && sp > 0 && s[0] != '/' && s[0] != '*') {
            r.method = toUpper(trim(s.substr(0, sp)));
            r.pattern = trim(s.substr(sp + 1));
        } else if (sp != std::string::npos && sp > 0 && s[0] == '*') {
            r.method = "*";
            r.pattern = trim(s.substr(sp + 1));
        } else {
            r.pattern = s;
        }
    } else if (v.isObject()) {
        r.method = toUpper(trim(v.getStr("method", "*")));
        r.pattern = trim(v.getStr("pattern", ""));
    } else {
        return std::nullopt;
    }
    if (r.pattern.empty()) return std::nullopt;
    // Deviation from C#: no whitespace inside the method list. C# kept {"method":"get, head"} as "GET, HEAD", wrote
    // the rule as "GET, HEAD /p" and read that back as method "GET," with pattern "/HEAD /p".
    r.method.erase(std::remove_if(r.method.begin(), r.method.end(), isWs), r.method.end());
    if (r.method.empty()) r.method = "*";
    if (r.pattern[0] != '/') r.pattern = "/" + r.pattern;
    return r;
}

// ---- Config -----------------------------------------------------------------------------------------------------------

std::string Config::bridgeUrl() const { return bridgeUrls.empty() ? std::string() : bridgeUrls.front(); }

std::string Config::folder(std::string_view key) const
{
    for (const auto& kv : folders)
        if (iequals(kv.first, key)) return kv.second;
    return {};
}

void Config::setFolder(std::string_view key, std::string value)
{
    // C# Dictionary(OrdinalIgnoreCase)[key] = v: an existing key keeps its first spelling and position.
    for (auto& kv : folders) {
        if (iequals(kv.first, key)) {
            kv.second = std::move(value);
            return;
        }
    }
    folders.emplace_back(std::string(key), std::move(value));
}

int Config::effectivePort() const
{
    if (port > 0) return port;
    if (mode == kModeServer) return test ? 18080 : 8080;
    return test ? 18899 : 8899;
}

int Config::effectiveTlsPort() const
{
    if (tlsPort > 0) return tlsPort;
    return test ? 18443 : 443;
}

std::string Config::rawStr(std::string_view key, std::string_view def) const { return raw.getStr(key, def); }
bool Config::rawBool(std::string_view key, bool def) const { return raw.getBool(key, def); }
int64_t Config::rawInt(std::string_view key, int64_t def) const { return raw.getInt(key, def); }
const JVal& Config::rawGet(std::string_view key) const { return raw.get(key); }

void Config::validateMode(const std::string& m)
{
    if (m != kModeOff && m != kModePeer && m != kModeServer)
        throw ConfigError("mode must be off, peer or server (got '" + m + "')");
}

void Config::validate()
{
    mode = normalizedMode(mode);
    validateMode(mode);
    checkPorts(port, tlsPort, statusPort);
}

Config Config::fromJson(std::string_view text)
{
    JVal v;
    try {
        v = json::parse(text);
    } catch (const JsonError& e) {
        throw ConfigError(std::string("invalid JSON: ") + e.what());
    }
    return fromJval(v);
}

Config Config::fromJval(const JVal& d)
{
    if (!d.isObject()) throw ConfigError("invalid JSON: expected a JSON object");
    Config c;
    c.raw = d;
    c.mode = d.getStr("mode", kModeOff);
    c.nodeId = d.getStr("nodeId", "");
    c.name = d.getStr("name", "");
    c.port = narrowPort(d.getInt("port", 0));
    c.tlsPort = narrowPort(d.getInt("tlsPort", 0));
    c.statusPort = narrowPort(d.getInt("statusPort", 0));
    c.bindAll = d.getBool("bindAll", true);
    c.bindAddress = d.getStr("bindAddress", "127.0.0.1");
    c.test = d.getBool("test", false);
    c.exposeFullDrives = d.getBool("exposeFullDrives", true);
    c.allowWrite = d.getBool("allowWrite", true);
    c.agentToken = d.getStr("agentToken", "");
    c.bridgeUrls = strList(d.get("bridgeUrl"));
    for (std::string& u : strList(d.get("bridgeFallbacks")))
        if (std::find(c.bridgeUrls.begin(), c.bridgeUrls.end(), u) == c.bridgeUrls.end()) c.bridgeUrls.push_back(std::move(u));
    c.corsAllow = strList(d.get("corsAllow"));
    c.rev = std::max<int64_t>(0, d.getInt("rev", 0));

    if (const JVal* shares = d.getArr("shares")) {
        for (const JVal& o : shares->arr()) {
            if (!o.isObject()) continue;
            Share s{o.getStr("id", ""), o.getStr("name", ""), o.getStr("path", "")};
            if (s.id.empty() && s.name.empty()) continue;
            if (s.id.empty()) s.id = s.name;
            if (s.name.empty()) s.name = s.id;
            c.shares.push_back(std::move(s));
        }
    }
    if (const JVal* folders = d.getObj("folders")) {
        for (const auto& kv : folders->obj())
            if (kv.second.isString()) c.setFolder(kv.first, kv.second.asString());
    }
    if (const JVal* wan = d.getObj("wan")) {
        c.wan.passwordRecord = PasswordRecord::fromJval(wan->get("passwordRecord"));
        if (const JVal* gp = wan->getArr("gatedPaths")) {
            for (const JVal& o : gp->arr())
                if (std::optional<GateRule> r = GateRule::parse(o)) c.wan.gatedPaths.push_back(std::move(*r));
        }
        if (wan->has("trustedProxies")) c.wan.trustedProxies = strList(wan->get("trustedProxies"));
        c.wan.requireTls = wan->getBool("requireTls", false);
    }
    if (const JVal* tls = d.getObj("tls")) {
        c.tls.pfx = tls->getStr("pfx", "");
        c.tls.passwordEnv = tls->getStr("passwordEnv", "");
    }
    c.validate();
    return c;
}

JVal Config::toJval() const
{
    const std::string m = normalizedMode(mode);
    validateMode(m);
    checkPorts(port, tlsPort, statusPort);

    JVal d = raw.isObject() ? raw : JVal::object();
    auto putOrErase = [&d](std::string_view key, bool put, JVal v) {
        if (put) d.set(key, std::move(v));
        else d.erase(key);
    };
    d.set("mode", m);
    d.set("nodeId", nodeId);
    d.set("name", name);
    putOrErase("port", port > 0, port);
    putOrErase("tlsPort", tlsPort > 0, tlsPort);
    putOrErase("statusPort", statusPort > 0, statusPort);
    d.set("bindAll", bindAll);
    if (!bindAll) d.set("bindAddress", bindAddress);
    putOrErase("test", test, true);
    d.set("exposeFullDrives", exposeFullDrives);
    d.set("allowWrite", allowWrite);
    putOrErase("bridgeUrl", !bridgeUrls.empty(), bridgeUrl());
    putOrErase("bridgeFallbacks", bridgeUrls.size() > 1,
               bridgeUrls.size() > 1 ? strArray(std::vector<std::string>(bridgeUrls.begin() + 1, bridgeUrls.end())) : JVal());
    putOrErase("agentToken", !agentToken.empty(), agentToken);
    d.set("corsAllow", strArray(corsAllow));

    JVal sh = JVal::array();
    for (const Share& s : shares) sh.push(JVal::object({{"id", s.id}, {"name", s.name}, {"path", s.path}}));
    d.set("shares", std::move(sh));

    // Copies first: a pointer into d dies when d.set() inserts.
    JVal f = d.getObj("folders") ? *d.getObj("folders") : JVal::object();
    for (const auto& kv : folders) f.set(kv.first, kv.second);
    d.set("folders", std::move(f));

    JVal w = d.getObj("wan") ? *d.getObj("wan") : JVal::object();
    if (wan.passwordRecord) w.set("passwordRecord", wan.passwordRecord->toJval());
    else w.erase("passwordRecord");
    JVal gp = JVal::array();
    for (const GateRule& r : wan.gatedPaths) gp.push(r.toString());
    w.set("gatedPaths", std::move(gp));
    w.set("trustedProxies", strArray(wan.trustedProxies));
    w.set("requireTls", wan.requireTls);
    d.set("wan", std::move(w));

    if (!tls.pfx.empty() || !tls.passwordEnv.empty()) {
        JVal t = d.getObj("tls") ? *d.getObj("tls") : JVal::object();
        t.set("pfx", tls.pfx);
        t.set("passwordEnv", tls.passwordEnv);
        d.set("tls", std::move(t));
    } else {
        d.erase("tls");
    }
    putOrErase("rev", rev > 0, rev);
    return d;
}

std::string Config::toJsonText() const { return json::stringifyPretty(toJval(), 2) + "\r\n"; }

JVal Config::toSafeJval() const
{
    JVal d = toJval();
    if (d.has("agentToken")) d.set("agentToken", "[set]");
    if (JVal* w = d.find("wan"); w && w->isObject() && w->has("passwordRecord")) w->set("passwordRecord", "[set]");
    if (JVal* t = d.find("tls"); t && t->isObject()) t->erase("passwordEnv");
    return d;
}

// ---- data dir / atomic write ------------------------------------------------------------------------------------------

std::string defaultDataDir()
{
    std::wstring base;
    wchar_t buf[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, SHGFP_TYPE_CURRENT, buf))) base = buf;
    if (base.empty()) {
        const DWORD n = GetEnvironmentVariableW(L"ProgramData", buf, MAX_PATH);
        if (n > 0 && n < MAX_PATH) base = buf;
    }
    if (base.empty()) base = L"C:\\ProgramData";
    while (base.size() > 3 && (base.back() == L'\\' || base.back() == L'/')) base.pop_back();
    return narrow(base) + "\\StarRoot\\anfs";
}

bool writeFileAtomic(const std::string& path, std::string_view data, std::string* err)
{
    auto fail = [&](std::string msg) {
        if (err) *err = std::move(msg);
        return false;
    };
    if (path.empty()) return fail("empty path");
    const std::wstring target = widen(path);
    const std::filesystem::path parent = std::filesystem::path(target).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }
    const std::wstring tmp = target + L"." + widen(ids::randomHex(4)) + L".tmp";   // the C# temp name form
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return fail("cannot create " + narrow(tmp) + ": " + winError(GetLastError()));
    size_t off = 0;
    bool ok = true;
    DWORD code = 0;
    while (ok && off < data.size()) {
        const DWORD piece = static_cast<DWORD>(std::min<size_t>(data.size() - off, 1u << 24));
        DWORD n = 0;
        ok = WriteFile(h, data.data() + off, piece, &n, nullptr) && n > 0;
        if (!ok) code = GetLastError();
        off += n;
    }
    if (ok && !FlushFileBuffers(h)) {
        ok = false;
        code = GetLastError();
    }
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return fail("cannot write " + narrow(tmp) + ": " + winError(code));
    }
    for (int attempt = 0;; ++attempt) {
        if (MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            if (err) err->clear();
            return true;
        }
        code = GetLastError();
        // A reader that opened the target without FILE_SHARE_DELETE (File.ReadAllText, an editor) blocks the
        // replace for as long as it holds the handle; wait it out briefly, as C# WriteAtomic does.
        const bool transient = code == ERROR_SHARING_VIOLATION || code == ERROR_ACCESS_DENIED || code == ERROR_LOCK_VIOLATION;
        if (!transient || attempt + 1 >= kReplaceAttempts) break;
        Sleep(25);
    }
    DeleteFileW(tmp.c_str());
    return fail("cannot replace " + path + ": " + winError(code));
}

// ---- ConfigStore ------------------------------------------------------------------------------------------------------

struct ConfigStore::Impl {
    std::string path;
    std::wstring wpath;

    // Readers: a short lock to copy the snapshot pointer, never held across file I/O.
    std::mutex snapMu;
    std::shared_ptr<const Config> cur = std::make_shared<const Config>();
    int64_t version = 0;
    std::string lastError;

    // Load/save: the file and what was last loaded or saved from it.
    std::mutex sync;
    FileSig sig;            // starts as "no file" (C# sigTicks = -1): an absent file is not a change
    std::string sigSha;
    bool sigValid = true;   // false after a read error or a vanished file: every poll retries (C# sigTicks = -2)
    bool hadFile = false;

    std::mutex updateMu;    // one read-modify-write at a time

    std::mutex listenMu;
    std::vector<std::pair<int, Listener>> listeners;
    int nextId = 1;

    void setError(std::string e)
    {
        std::lock_guard<std::mutex> g(snapMu);
        lastError = std::move(e);
    }

    std::shared_ptr<const Config> publish(Config c)
    {
        auto p = std::make_shared<const Config>(std::move(c));
        std::lock_guard<std::mutex> g(snapMu);
        cur = p;
        ++version;
        lastError.clear();
        return p;
    }

    // Writes c when the store is still at expectVersion (or unconditionally for -1). Returns the published snapshot,
    // or null when another load/save got in between. Throws ConfigError on an invalid config or a write failure.
    std::shared_ptr<const Config> saveCore(const Config& c, int64_t expectVersion)
    {
        const std::string text = c.toJsonText();
        Config reparsed = Config::fromJson(text);   // exactly what the next load of this file will see
        std::lock_guard<std::mutex> g(sync);
        {
            std::lock_guard<std::mutex> s(snapMu);
            if (expectVersion >= 0 && version != expectVersion) return nullptr;
        }
        std::string err;
        if (!writeFileAtomic(path, text, &err)) {
            setError("cannot write config: " + err);
            throw ConfigError("cannot write config: " + err);
        }
        sig = statFile(wpath);
        sigSha = ids::sha256Hex(text);
        sigValid = true;
        hadFile = true;
        return publish(std::move(reparsed));
    }

    void notify()
    {
        std::vector<Listener> ls;
        {
            std::lock_guard<std::mutex> g(listenMu);
            for (const auto& l : listeners) ls.push_back(l.second);
        }
        if (ls.empty()) return;
        std::shared_ptr<const Config> snap;
        {
            std::lock_guard<std::mutex> g(snapMu);
            snap = cur;   // the newest, never a stale one from a racing save
        }
        for (const Listener& l : ls) {
            try {
                l(snap);
            } catch (...) {
                // a failing subscriber must not undo a load or save that already happened
            }
        }
    }
};

ConfigStore::ConfigStore(std::string path) : d_(std::make_unique<Impl>())
{
    d_->wpath = widen(path);
    d_->path = std::move(path);
}

ConfigStore::~ConfigStore() = default;

const std::string& ConfigStore::path() const { return d_->path; }

std::shared_ptr<const Config> ConfigStore::current() const
{
    std::lock_guard<std::mutex> g(d_->snapMu);
    return d_->cur;
}

std::string ConfigStore::lastError() const
{
    std::lock_guard<std::mutex> g(d_->snapMu);
    return d_->lastError;
}

int64_t ConfigStore::version() const
{
    std::lock_guard<std::mutex> g(d_->snapMu);
    return d_->version;
}

bool ConfigStore::hasChangedOnDisk() const
{
    Impl& d = *d_;
    std::lock_guard<std::mutex> g(d.sync);
    const FileSig now = statFile(d.wpath);
    if (!d.sigValid) return true;
    if (now.exists != d.sig.exists) return true;
    if (!now.exists) return false;
    if (now.size != d.sig.size || now.mtime != d.sig.mtime) return true;
    // Same size and mtime: two writes inside one timestamp tick keep both, so compare the content hash (the file is
    // small). A read that fails now is retried by the next poll rather than reported as a change.
    std::string bytes;
    if (!readFileBytes(d.wpath, bytes, nullptr)) return false;
    return ids::sha256Hex(bytes) != d.sigSha;
}

bool ConfigStore::load()
{
    Impl& d = *d_;
    {
        std::lock_guard<std::mutex> g(d.sync);
        FileSig st;
        std::string bytes, readErr;
        bool haveText = false;
        for (int attempt = 0; attempt < 5; ++attempt) {
            st = statFile(d.wpath);
            if (st.exists) {
                if (readFileBytes(d.wpath, bytes, &readErr)) {
                    haveText = true;
                    readErr.clear();
                    break;
                }
            } else {
                readErr.clear();
                if (!d.hadFile) break;   // never had a file: defaults, no retry
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));   // I/O retry, real time on purpose
        }
        if (!readErr.empty()) {
            d.sigValid = false;
            d.setError("cannot read config: " + readErr);
            return false;
        }
        if (!haveText && d.hadFile) {
            d.sigValid = false;
            d.setError("config file is missing, keeping previous settings");
            return false;
        }
        d.sig = st;
        d.sigSha = haveText ? ids::sha256Hex(bytes) : std::string();
        d.sigValid = true;
        Config c;
        if (haveText) {
            d.hadFile = true;
            try {
                c = Config::fromJson(decodeText(std::move(bytes)));
            } catch (const ConfigError& e) {
                d.setError(e.what());   // keeps the previous config; the bad file's signature is recorded
                return false;
            }
        }
        d.publish(std::move(c));
    }
    d.notify();
    return true;
}

void ConfigStore::save(const Config& c)
{
    d_->saveCore(c, -1);
    d_->notify();
}

std::shared_ptr<const Config> ConfigStore::update(const std::function<void(Config&)>& mutate, bool bumpRev)
{
    Impl& d = *d_;
    std::shared_ptr<const Config> result;
    {
        std::lock_guard<std::mutex> u(d.updateMu);
        for (int attempt = 0; !result; ++attempt) {
            std::shared_ptr<const Config> base;
            int64_t v;
            {
                std::lock_guard<std::mutex> g(d.snapMu);
                base = d.cur;
                v = d.version;
            }
            Config c = *base;
            mutate(c);
            if (bumpRev) ++c.rev;
            // Conditional on the version read above: a load or save in between re-runs mutate on the fresh config.
            // After 100 lost races the write goes through unconditionally (C# does the same).
            result = d.saveCore(c, attempt < 100 ? v : -1);
        }
    }
    d.notify();
    return result;
}

int ConfigStore::subscribe(Listener l)
{
    std::lock_guard<std::mutex> g(d_->listenMu);
    const int id = d_->nextId++;
    d_->listeners.emplace_back(id, std::move(l));
    return id;
}

void ConfigStore::unsubscribe(int id)
{
    std::lock_guard<std::mutex> g(d_->listenMu);
    auto& ls = d_->listeners;
    ls.erase(std::remove_if(ls.begin(), ls.end(), [id](const auto& p) { return p.first == id; }), ls.end());
}

// ---- ConfigWatcher ----------------------------------------------------------------------------------------------------

struct ConfigWatcher::Impl {
    ConfigStore& store;
    int intervalMs;

    std::mutex mu;
    std::condition_variable cv;
    bool stopFlag = false;
    std::thread th;

    std::mutex pollMu;       // one poll at a time (the thread and a test calling poll())
    std::string lastReported;
    std::mutex cbMu;
    std::function<void(const std::string&)> onError;

    Impl(ConfigStore& s, int ms) : store(s), intervalMs(ms > 0 ? ms : 2000) {}

    void loop()
    {
        std::unique_lock<std::mutex> lk(mu);
        while (!stopFlag) {
            // anfs::clock() time: a ManualClock advanced by intervalMs wakes this at once.
            anfs::clock().waitFor(cv, lk, intervalMs, [this] { return stopFlag; });
            if (stopFlag) break;
            lk.unlock();
            pollOnce();
            lk.lock();
        }
    }

    void pollOnce()
    {
        std::unique_lock<std::mutex> p(pollMu, std::try_to_lock);
        if (!p.owns_lock()) return;
        try {
            if (!store.hasChangedOnDisk()) return;
            if (store.load()) {
                lastReported.clear();
                return;
            }
            const std::string err = store.lastError();
            if (err == lastReported) return;   // one report per distinct error text
            lastReported = err;
            std::function<void(const std::string&)> cb;
            {
                std::lock_guard<std::mutex> g(cbMu);
                cb = onError;
            }
            if (cb) cb(err);
        } catch (...) {
            // the poller must survive anything a subscriber or the file system throws
        }
    }
};

ConfigWatcher::ConfigWatcher(ConfigStore& store, int intervalMs) : d_(std::make_unique<Impl>(store, intervalMs)) {}

ConfigWatcher::~ConfigWatcher() { stop(); }

void ConfigWatcher::start()
{
    std::lock_guard<std::mutex> g(d_->mu);
    if (d_->th.joinable()) return;
    d_->stopFlag = false;
    d_->th = std::thread([this] { d_->loop(); });
}

void ConfigWatcher::stop()
{
    std::thread t;
    {
        std::lock_guard<std::mutex> g(d_->mu);
        d_->stopFlag = true;
        t = std::move(d_->th);
    }
    d_->cv.notify_all();
    if (t.joinable()) {
        if (t.get_id() == std::this_thread::get_id()) t.detach();   // stop() from inside an onError callback
        else t.join();
    }
}

void ConfigWatcher::poll() { d_->pollOnce(); }

void ConfigWatcher::onError(std::function<void(const std::string&)> cb)
{
    std::lock_guard<std::mutex> g(d_->cbMu);
    d_->onError = std::move(cb);
}

} // namespace anfs
