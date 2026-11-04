// core/nodepath.cpp - Node path.win32 on UTF-8 strings, plus the AN-FS path rules every module shares (ANFS-F2a).
//
// resolve/normalize/join/basename/dirname/extname/isAbsolute and normalizeString are a line-by-line port of
// lib/path.js of Node 22.13.1, the node.exe installed on this PC, which test_anfs_core_fs compares against live:
// - Node walks UTF-16 code units. Every character the algorithms test ('\\', '/', ':', '.', ASCII drive letters) is
//   ASCII and UTF-8 never puts an ASCII byte inside a multi-byte character, so running them on bytes gives the same
//   result. Lengths only matter in basename(p, ext), whose two comparisons count UTF-16 units as JS does.
// - 22.13.1 carries the CVE-2025-23084 normalize() fix (a relative path that would come out as "X:..." or holds a
//   ':' followed by a separator gets a ".\\" prefix). The reserved-device-name rule of 22.17.1 is not in it.
// - resolve() reads the process cwd and, for "X:rel", the hidden "=X:" variable, like process.env[`=${device}`].
//   The live Node peer runs Node 18 (pkg), which differs only in those colon cases; AN-FS callers pass absolute paths.
// toLowerInvariant is C# ToLowerInvariant on .NET Framework (the C# port's runtime), which asks Windows for invariant
// casing: LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE). The test compares it with powershell.exe 5.1.
#include "core/nodepath.h"

#include "core/fsutil.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstddef>
#include <vector>

namespace anfs::nodepath {

namespace {

using ssize = std::ptrdiff_t;

bool isSep(char c) { return c == '\\' || c == '/'; }
bool isDeviceRoot(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool startsWithDrive(std::string_view p) { return p.size() >= 2 && isDeviceRoot(p[0]) && p[1] == ':'; }

// String length in UTF-16 code units (continuation bytes do not count; a 4-byte character is a surrogate pair).
size_t utf16Len(std::string_view s)
{
    size_t n = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) ++n;
        if (c >= 0xF0) ++n;
    }
    return n;
}

std::string asciiLower(std::string_view s)
{
    std::string r(s);
    for (char& c : r)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return r;
}

// normalizeString(path, allowAboveRoot, '\\', isPathSeparator): folds '.' and '..'; never emits a leading or trailing
// separator. lastSegmentLength and the res.length tests behave the same on bytes (see the file comment).
std::string normalizeString(std::string_view path, bool allowAboveRoot)
{
    std::string res;
    size_t lastSegmentLength = 0;
    ssize lastSlash = -1;
    int dots = 0;
    char code = 0;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i < path.size()) code = path[i];
        else if (isSep(code)) break;
        else code = '/';

        if (isSep(code)) {
            if (lastSlash == static_cast<ssize>(i) - 1 || dots == 1) {
                // NOOP
            } else if (dots == 2) {
                if (res.size() < 2 || lastSegmentLength != 2 || res[res.size() - 1] != '.' ||
                    res[res.size() - 2] != '.') {
                    if (res.size() > 2) {
                        const size_t lastSlashIndex = res.rfind('\\');
                        if (lastSlashIndex == std::string::npos) {
                            res.clear();
                            lastSegmentLength = 0;
                        } else {
                            res.resize(lastSlashIndex);
                            const size_t ls = res.rfind('\\');
                            lastSegmentLength = ls == std::string::npos ? res.size() : res.size() - 1 - ls;
                        }
                        lastSlash = static_cast<ssize>(i);
                        dots = 0;
                        continue;
                    } else if (!res.empty()) {
                        res.clear();
                        lastSegmentLength = 0;
                        lastSlash = static_cast<ssize>(i);
                        dots = 0;
                        continue;
                    }
                }
                if (allowAboveRoot) {
                    res += res.empty() ? ".." : "\\..";
                    lastSegmentLength = 2;
                }
            } else {
                const std::string_view seg = path.substr(static_cast<size_t>(lastSlash + 1), i - static_cast<size_t>(lastSlash + 1));
                if (!res.empty()) {
                    res += '\\';
                    res += seg;
                } else {
                    res = std::string(seg);
                }
                lastSegmentLength = i - static_cast<size_t>(lastSlash + 1);
            }
            lastSlash = static_cast<ssize>(i);
            dots = 0;
        } else if (code == '.' && dots != -1) {
            ++dots;
        } else {
            dots = -1;
        }
    }
    return res;
}

// process.env['=X:'] (the per-drive cwd cmd.exe and the CRT keep); "" when unset.
std::string driveCwd(std::string_view device)
{
    std::wstring name = L"=" + fs::widen(device);
    wchar_t buf[MAX_PATH + 1];
    DWORD n = GetEnvironmentVariableW(name.c_str(), buf, MAX_PATH + 1);
    if (n == 0) return {};
    if (n <= MAX_PATH) return fs::narrow(std::wstring_view(buf, n));
    std::wstring big(n, L'\0');
    n = GetEnvironmentVariableW(name.c_str(), &big[0], n);
    big.resize(n);
    return fs::narrow(big);
}

std::string resolveArgs(const std::string_view* args, int count)
{
    std::string resolvedDevice;
    std::string resolvedTail;
    bool resolvedAbsolute = false;
    std::string holder;   // owns the cwd text for the i == -1 step

    for (int i = count - 1; i >= -1; --i) {
        std::string_view path;
        if (i >= 0) {
            path = args[i];
            if (path.empty()) continue;   // skip empty entries
        } else if (resolvedDevice.empty()) {
            holder = fs::cwd();
            path = holder;
        } else {
            // Windows keeps a cwd per drive. Take that drive's cwd, else the process cwd; if that names another drive,
            // use the drive root (the device is never UNC here: UNC paths are absolute).
            holder = driveCwd(resolvedDevice);
            if (holder.empty()) holder = fs::cwd();
            if (asciiLower(std::string_view(holder).substr(0, 2)) != asciiLower(resolvedDevice) && holder.size() > 2 &&
                holder[2] == '\\')
                holder = resolvedDevice + "\\";
            path = holder;
        }

        const size_t len = path.size();
        size_t rootEnd = 0;
        std::string device;
        bool isAbs = false;
        const char code = path.empty() ? '\0' : path[0];

        if (len == 1) {
            if (isSep(code)) {
                rootEnd = 1;   // just a separator
                isAbs = true;
            }
        } else if (len > 1 && isSep(code)) {
            isAbs = true;      // UNC or rooted
            if (isSep(path[1])) {
                size_t j = 2, last = j;
                while (j < len && !isSep(path[j])) ++j;
                if (j < len && j != last) {
                    const std::string_view firstPart = path.substr(last, j - last);
                    last = j;
                    while (j < len && isSep(path[j])) ++j;
                    if (j < len && j != last) {
                        last = j;
                        while (j < len && !isSep(path[j])) ++j;
                        if (j == len || j != last) {
                            device = "\\\\" + std::string(firstPart) + "\\" + std::string(path.substr(last, j - last));
                            rootEnd = j;
                        }
                    }
                }
            } else {
                rootEnd = 1;
            }
        } else if (len > 1 && isDeviceRoot(code) && path[1] == ':') {
            device = std::string(path.substr(0, 2));
            rootEnd = 2;
            if (len > 2 && isSep(path[2])) {
                isAbs = true;   // a separator after the drive makes it absolute
                rootEnd = 3;
            }
        }

        if (!device.empty()) {
            if (!resolvedDevice.empty()) {
                if (toLowerInvariant(device) != toLowerInvariant(resolvedDevice)) continue;   // another device
            } else {
                resolvedDevice = device;
            }
        }

        if (resolvedAbsolute) {
            if (!resolvedDevice.empty()) break;
        } else {
            resolvedTail = std::string(path.substr(rootEnd)) + "\\" + resolvedTail;
            resolvedAbsolute = isAbs;
            if (isAbs && !resolvedDevice.empty()) break;
        }
    }

    resolvedTail = normalizeString(resolvedTail, !resolvedAbsolute);
    if (resolvedAbsolute) return resolvedDevice + "\\" + resolvedTail;
    std::string r = resolvedDevice + resolvedTail;
    return r.empty() ? std::string(".") : r;
}

} // namespace

std::string resolve(std::string_view p)
{
    return resolveArgs(&p, 1);
}

std::string resolve(std::string_view base, std::string_view p)
{
    const std::string_view args[2] = {base, p};
    return resolveArgs(args, 2);
}

std::string normalize(std::string_view path)
{
    const size_t len = path.size();
    if (len == 0) return ".";
    size_t rootEnd = 0;
    std::string device;
    bool hasDevice = false;
    bool isAbs = false;
    const char code = path[0];

    if (len == 1) return code == '/' ? std::string("\\") : std::string(path);   // a single char: exit early
    if (isSep(code)) {
        isAbs = true;   // UNC or rooted
        if (isSep(path[1])) {
            size_t j = 2, last = j;
            while (j < len && !isSep(path[j])) ++j;
            if (j < len && j != last) {
                const std::string_view firstPart = path.substr(last, j - last);
                last = j;
                while (j < len && isSep(path[j])) ++j;
                if (j < len && j != last) {
                    last = j;
                    while (j < len && !isSep(path[j])) ++j;
                    if (j == len) {
                        // Only a UNC root: return it normalized, nothing is left to process.
                        return "\\\\" + std::string(firstPart) + "\\" + std::string(path.substr(last)) + "\\";
                    }
                    if (j != last) {
                        device = "\\\\" + std::string(firstPart) + "\\" + std::string(path.substr(last, j - last));
                        hasDevice = true;
                        rootEnd = j;
                    }
                }
            }
        } else {
            rootEnd = 1;
        }
    } else if (isDeviceRoot(code) && path[1] == ':') {
        device = std::string(path.substr(0, 2));
        hasDevice = true;
        rootEnd = 2;
        if (len > 2 && isSep(path[2])) {
            isAbs = true;
            rootEnd = 3;
        }
    }

    std::string tail = rootEnd < len ? normalizeString(path.substr(rootEnd), !isAbs) : std::string();
    if (tail.empty() && !isAbs) tail = ".";
    if (!tail.empty() && isSep(path[len - 1])) tail += '\\';
    if (!isAbs && !hasDevice && path.find(':') != std::string_view::npos) {
        // CVE-2025-23084: a relative path must not come out as something Windows reads as absolute.
        if (startsWithDrive(tail)) return ".\\" + tail;
        size_t index = path.find(':');
        do {
            if (index == len - 1 || isSep(path[index + 1])) return ".\\" + tail;
        } while ((index = path.find(':', index + 1)) != std::string_view::npos);
    }
    if (!hasDevice) return isAbs ? "\\" + tail : tail;
    return isAbs ? device + "\\" + tail : device + tail;
}

std::string join(std::string_view a, std::string_view b)
{
    std::string joined;
    std::string_view firstPart;
    bool any = false;
    for (std::string_view arg : {a, b}) {
        if (arg.empty()) continue;
        if (!any) {
            joined = std::string(arg);
            firstPart = arg;
            any = true;
        } else {
            joined += '\\';
            joined += arg;
        }
    }
    if (!any) return ".";

    // The joined path must not start with two separators (normalize would read a UNC root), unless the first part
    // clearly is one: exactly two separators followed by a non-separator ("//server" + "share" -> "\\\\server\\share\\").
    bool needsReplace = true;
    size_t slashCount = 0;
    if (isSep(firstPart[0])) {
        ++slashCount;
        const size_t firstLen = firstPart.size();
        if (firstLen > 1 && isSep(firstPart[1])) {
            ++slashCount;
            if (firstLen > 2) {
                if (isSep(firstPart[2])) ++slashCount;
                else needsReplace = false;   // a UNC path in the first part
            }
        }
    }
    if (needsReplace) {
        while (slashCount < joined.size() && isSep(joined[slashCount])) ++slashCount;
        if (slashCount >= 2) joined = "\\" + joined.substr(slashCount);
    }
    return normalize(joined);
}

std::string basename(std::string_view path)
{
    ssize start = 0;
    ssize end = -1;
    bool matchedSlash = true;
    if (startsWithDrive(path)) start = 2;   // the separator after "X:" is not a trailing one
    for (ssize i = static_cast<ssize>(path.size()) - 1; i >= start; --i) {
        if (isSep(path[static_cast<size_t>(i)])) {
            if (!matchedSlash) {
                start = i + 1;
                break;
            }
        } else if (end == -1) {
            matchedSlash = false;
            end = i + 1;
        }
    }
    if (end == -1) return {};
    return std::string(path.substr(static_cast<size_t>(start), static_cast<size_t>(end - start)));
}

std::string basename(std::string_view path, std::string_view suffix)
{
    ssize start = 0;
    ssize end = -1;
    bool matchedSlash = true;
    if (startsWithDrive(path)) start = 2;

    if (!suffix.empty() && utf16Len(suffix) <= utf16Len(path)) {
        if (suffix == path) return {};
        ssize extIdx = static_cast<ssize>(suffix.size()) - 1;
        ssize firstNonSlashEnd = -1;
        for (ssize i = static_cast<ssize>(path.size()) - 1; i >= start; --i) {
            const char code = path[static_cast<size_t>(i)];
            if (isSep(code)) {
                if (!matchedSlash) {
                    start = i + 1;
                    break;
                }
            } else {
                if (firstNonSlashEnd == -1) {
                    matchedSlash = false;
                    firstNonSlashEnd = i + 1;
                }
                if (extIdx >= 0) {
                    if (code == suffix[static_cast<size_t>(extIdx)]) {
                        if (--extIdx == -1) end = i;   // the whole suffix matched: the component ends here
                    } else {
                        extIdx = -1;                   // no match: the result is the whole component
                        end = firstNonSlashEnd;
                    }
                }
            }
        }
        if (start == end) end = firstNonSlashEnd;
        else if (end == -1) end = static_cast<ssize>(path.size());
        if (end < start) return {};
        return std::string(path.substr(static_cast<size_t>(start), static_cast<size_t>(end - start)));
    }
    return basename(path);
}

std::string dirname(std::string_view path)
{
    const size_t len = path.size();
    if (len == 0) return ".";
    ssize rootEnd = -1;
    ssize offset = 0;
    const char code = path[0];
    if (len == 1) return isSep(code) ? std::string(path) : std::string(".");

    if (isSep(code)) {
        rootEnd = offset = 1;   // possible UNC root
        if (isSep(path[1])) {
            size_t j = 2, last = j;
            while (j < len && !isSep(path[j])) ++j;
            if (j < len && j != last) {
                last = j;
                while (j < len && isSep(path[j])) ++j;
                if (j < len && j != last) {
                    last = j;
                    while (j < len && !isSep(path[j])) ++j;
                    if (j == len) return std::string(path);   // only a UNC root
                    // A UNC root with leftovers: the separator after it counts as part of the root.
                    if (j != last) rootEnd = offset = static_cast<ssize>(j) + 1;
                }
            }
        }
    } else if (isDeviceRoot(code) && path[1] == ':') {
        rootEnd = len > 2 && isSep(path[2]) ? 3 : 2;
        offset = rootEnd;
    }

    ssize end = -1;
    bool matchedSlash = true;
    for (ssize i = static_cast<ssize>(len) - 1; i >= offset; --i) {
        if (isSep(path[static_cast<size_t>(i)])) {
            if (!matchedSlash) {
                end = i;
                break;
            }
        } else {
            matchedSlash = false;
        }
    }
    if (end == -1) {
        if (rootEnd == -1) return ".";
        end = rootEnd;
    }
    return std::string(path.substr(0, static_cast<size_t>(end)));
}

std::string extname(std::string_view path)
{
    ssize start = 0;
    ssize startDot = -1;
    ssize startPart = 0;
    ssize end = -1;
    bool matchedSlash = true;
    int preDotState = 0;   // characters seen before the first dot after a separator
    if (startsWithDrive(path)) start = startPart = 2;

    for (ssize i = static_cast<ssize>(path.size()) - 1; i >= start; --i) {
        const char code = path[static_cast<size_t>(i)];
        if (isSep(code)) {
            if (!matchedSlash) {
                startPart = i + 1;
                break;
            }
            continue;
        }
        if (end == -1) {
            matchedSlash = false;
            end = i + 1;
        }
        if (code == '.') {
            if (startDot == -1) startDot = i;
            else if (preDotState != 1) preDotState = 1;
        } else if (startDot != -1) {
            preDotState = -1;
        }
    }
    if (startDot == -1 || end == -1 ||
        preDotState == 0 ||   // a non-dot character right before the dot (".bashrc")
        (preDotState == 1 && startDot == end - 1 && startDot == startPart + 1))   // the component is ".."
        return {};
    return std::string(path.substr(static_cast<size_t>(startDot), static_cast<size_t>(end - startDot)));
}

bool isAbsolute(std::string_view path)
{
    const size_t len = path.size();
    if (len == 0) return false;
    const char code = path[0];
    return isSep(code) || (len > 2 && isDeviceRoot(code) && path[1] == ':' && isSep(path[2]));
}

std::string toLowerInvariant(std::string_view s)
{
    bool ascii = true;
    for (unsigned char c : s)
        if (c >= 0x80) {
            ascii = false;
            break;
        }
    if (ascii) return asciiLower(s);
    const std::wstring w = fs::widen(s);
    const int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                nullptr, nullptr, 0);
    if (n <= 0) return asciiLower(s);
    std::wstring out(static_cast<size_t>(n), L'\0');
    LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, w.data(), static_cast<int>(w.size()), &out[0], n, nullptr,
                  nullptr, 0);
    return fs::narrow(out);
}

std::string key(std::string_view p)
{
    return toLowerInvariant(resolve(p));
}

bool isUnder(std::string_view rootKey, std::string_view pKey)
{
    if (rootKey.empty()) return false;
    if (pKey == rootKey) return true;
    if (rootKey.back() == '\\') return pKey.size() > rootKey.size() && pKey.substr(0, rootKey.size()) == rootKey;
    return pKey.size() > rootKey.size() && pKey[rootKey.size()] == '\\' && pKey.substr(0, rootKey.size()) == rootKey;
}

std::string safeRelDir(std::string_view rel)
{
    // String(relPath).replace(/^[a-zA-Z]:/, '').split(/[\\/]+/).filter(seg => seg && seg !== '.' && seg !== '..' &&
    //   !/^[a-zA-Z]:$/.test(seg)).join(path.sep)
    if (startsWithDrive(rel)) rel.remove_prefix(2);
    std::string out;
    size_t i = 0;
    while (i < rel.size()) {
        while (i < rel.size() && isSep(rel[i])) ++i;
        const size_t b = i;
        while (i < rel.size() && !isSep(rel[i])) ++i;
        const std::string_view seg = rel.substr(b, i - b);
        if (seg.empty() || seg == "." || seg == ".." || (seg.size() == 2 && startsWithDrive(seg))) continue;
        if (!out.empty()) out += '\\';
        out += seg;
    }
    return out;
}

std::string conflictName(std::string_view fullPath)
{
    // agent.js resolveConflict, rename policy: dirname / extname / basename(p, ext), then path.join(dir, `${base} (${c})${ext}`).
    const std::string full(fullPath);
    if (!fs::exists(full)) return full;
    const std::string dir = dirname(fullPath);
    const std::string ext = extname(fullPath);
    const std::string base = basename(fullPath, ext);
    std::string candidate = full;
    for (long long c = 1; fs::exists(candidate); ++c) candidate = join(dir, base + " (" + std::to_string(c) + ")" + ext);
    return candidate;
}

} // namespace anfs::nodepath
