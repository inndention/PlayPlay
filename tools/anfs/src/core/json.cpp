// core/json.cpp - ordered JSON (ANFS-F1a): JObject/JVal, the JSON.stringify-exact writer, the strict parser with the
// Express body rules, ISO-ms dates and the streaming writer. Contract: core/json.h; design: anfs-cpp.md 2.6.
//
// Byte-exactness (checked against Node 22 by tests/anfs/test_anfs_core_json.cpp):
// - Strings are UTF-8 and may carry lone UTF-16 surrogates as 3-byte WTF-8 sequences (ED A0..BF xx), which is how
//   unpaired surrogates of NTFS names survive. The writer prints those as \udxxx (lower-case hex, ES2019 well-formed
//   JSON.stringify). A high and a low half stored next to each other (two lone halves concatenated) are one character
//   in a JS string, so they print as its 4-byte UTF-8 form. Bytes that are not UTF-8 at all cannot exist in a JS
//   string: they print as U+FFFD, one per maximal invalid subpart (the WHATWG decoder Node applies where such bytes
//   would have entered).
// - The parser reads bytes the way body-parser hands them to JSON.parse: UTF-8 decoded with U+FFFD replacement (raw
//   WTF-8 surrogate bytes included, Node's decoder rejects them), a leading BOM dropped, then the JSON.parse grammar.
//   \u escapes may produce lone surrogates; those are stored as WTF-8 and written back as \udxxx.
// - Int prints with to_chars. Double prints as Number#toString: the shortest round-trip digits of
//   to_chars(scientific), laid out with the ECMA-262 Number::toString rules; NaN and +-Infinity print as null.
#include "core/json.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>
#include <memory>
#include <unordered_map>

namespace anfs {
namespace {

constexpr int kMaxDepth = 512;            // json.h: nesting cap of the parser
constexpr size_t kLinearKeys = 8;         // the parser indexes an object's keys once it has more members than this
constexpr int64_t kMaxTimeMs = 8640000000000000LL;   // ECMA-262 time value range (+-100,000,000 days)
constexpr int64_t kDayMs = 86400000;

const JVal& nullValue() { static const JVal v; return v; }
const JObject& emptyObject() { static const JObject o; return o; }
const JVal::Array& emptyArray() { static const JVal::Array a; return a; }
const std::string& emptyString() { static const std::string s; return s; }

inline unsigned char uc(char c) { return static_cast<unsigned char>(c); }
inline bool isDigit(char c) { return c >= '0' && c <= '9'; }
inline bool isJsonWs(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }   // JSON.parse whitespace

bool equalsAsciiNoCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

// ---- UTF-8 ----------------------------------------------------------------------------------------------------------

// One sequence at s[0] (s[0] >= 0x80), WHATWG UTF-8 decoder rules. ok=false: cp is U+FFFD and len is the maximal
// subpart (the lead plus the continuation bytes that were still acceptable), so decoding resumes at the offending
// byte. allowSurrogates accepts a complete ED A0..BF xx (WTF-8 lone surrogates) as D800..DFFF.
struct Utf8Seq {
    uint32_t cp;
    size_t len;
    bool ok;
};

Utf8Seq decodeUtf8(const unsigned char* s, size_t n, bool allowSurrogates)
{
    const unsigned char c = s[0];
    size_t need = 0;
    uint32_t cp = 0;
    unsigned char lo = 0x80, hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) {
        need = 1;
        cp = c & 0x1Fu;
    } else if (c >= 0xE0 && c <= 0xEF) {
        need = 2;
        cp = c & 0x0Fu;
        if (c == 0xE0) lo = 0xA0;
        else if (c == 0xED && !allowSurrogates) hi = 0x9F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        need = 3;
        cp = c & 0x07u;
        if (c == 0xF0) lo = 0x90;
        else if (c == 0xF4) hi = 0x8F;
    } else {
        return {0xFFFD, 1, false};               // 80..C1 (stray continuation, overlong lead) and F5..FF
    }
    for (size_t i = 1; i <= need; ++i) {
        // Only a whole ED A0..BF xx is a stored surrogate; a cut-off ED A0..BF came from bytes, which Node's decoder
        // (ED takes 80..9F only) reads as two subparts: ED, then A0..BF.
        if (i == 2 && c == 0xED && s[1] >= 0xA0 && (i >= n || s[i] < lo || s[i] > hi)) return {0xFFFD, 1, false};
        if (i >= n || s[i] < lo || s[i] > hi) return {0xFFFD, i, false};
        lo = 0x80;
        hi = 0xBF;
        cp = (cp << 6) | (s[i] & 0x3Fu);
    }
    return {cp, need + 1, true};
}

// cp <= 0x10FFFF; D800..DFFF become 3-byte WTF-8.
void appendUtf8(std::string& out, uint32_t cp)
{
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

constexpr char kHex[] = "0123456789abcdef";
constexpr char kReplacement[] = "\xEF\xBF\xBD";   // U+FFFD

void appendU4(std::string& out, uint32_t u)
{
    const char e[6] = {'\\', 'u', kHex[(u >> 12) & 15], kHex[(u >> 8) & 15], kHex[(u >> 4) & 15], kHex[u & 15]};
    out.append(e, 6);
}

// JSON.stringify of one string, quotes included. Unchanged bytes are copied in runs.
void appendQuoted(std::string& out, std::string_view sv)
{
    const auto* s = reinterpret_cast<const unsigned char*>(sv.data());
    const size_t n = sv.size();
    out.push_back('"');
    size_t run = 0, i = 0;
    while (i < n) {
        const unsigned char c = s[i];
        if (c < 0x80) {
            if (c >= 0x20 && c != '"' && c != '\\') { ++i; continue; }
            out.append(sv.data() + run, i - run);
            switch (c) {
            case '"': out.append("\\\"", 2); break;
            case '\\': out.append("\\\\", 2); break;
            case '\b': out.append("\\b", 2); break;
            case '\f': out.append("\\f", 2); break;
            case '\n': out.append("\\n", 2); break;
            case '\r': out.append("\\r", 2); break;
            case '\t': out.append("\\t", 2); break;
            default: appendU4(out, c); break;
            }
            run = ++i;
            continue;
        }
        const Utf8Seq q = decodeUtf8(s + i, n - i, true);
        if (q.ok && (q.cp < 0xD800 || q.cp > 0xDFFF)) { i += q.len; continue; }   // well-formed: stays raw
        out.append(sv.data() + run, i - run);
        if (!q.ok) {
            out.append(kReplacement, 3);
        } else if (q.cp <= 0xDBFF && i + 6 <= n && s[i + 3] == 0xED) {
            const Utf8Seq lo = decodeUtf8(s + i + 3, n - i - 3, true);
            if (lo.ok && lo.cp >= 0xDC00 && lo.cp <= 0xDFFF) {
                appendUtf8(out, 0x10000 + ((q.cp - 0xD800) << 10) + (lo.cp - 0xDC00));
                i += 6;
                run = i;
                continue;
            }
            appendU4(out, q.cp);
        } else {
            appendU4(out, q.cp);
        }
        i += q.len;
        run = i;
    }
    out.append(sv.data() + run, n - run);
    out.push_back('"');
}

void appendInt(std::string& out, int64_t v)
{
    char buf[24];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    out.append(buf, static_cast<size_t>(r.ptr - buf));
}

// JSON.stringify of a number: Number#toString, null for NaN and +-Infinity.
void appendDouble(std::string& out, double d)
{
    if (!std::isfinite(d)) { out.append("null", 4); return; }
    // Integral values below 2^53 print as plain digits either way (n <= 21); skip the shortest-digits search.
    if (d == std::trunc(d) && std::fabs(d) < 9007199254740992.0) { appendInt(out, static_cast<int64_t>(d)); return; }
    out += json::numberToString(d);
}

void writeScalar(std::string& out, const JVal& v)
{
    switch (v.type()) {
    case JVal::Type::Null: out.append("null", 4); break;
    case JVal::Type::Bool: v.asBool() ? out.append("true", 4) : out.append("false", 5); break;
    case JVal::Type::Int: appendInt(out, v.asInt()); break;
    case JVal::Type::Double: appendDouble(out, v.asDouble()); break;
    case JVal::Type::String: appendQuoted(out, v.asString()); break;
    default: break;
    }
}

void writeCompact(std::string& out, const JVal& v)
{
    if (v.isArray()) {
        out.push_back('[');
        bool first = true;
        for (const JVal& e : v.arr()) {
            if (!first) out.push_back(',');
            first = false;
            writeCompact(out, e);
        }
        out.push_back(']');
    } else if (v.isObject()) {
        out.push_back('{');
        bool first = true;
        for (const JObject::Member& m : v.obj()) {
            if (!first) out.push_back(',');
            first = false;
            appendQuoted(out, m.first);
            out.push_back(':');
            writeCompact(out, m.second);
        }
        out.push_back('}');
    } else {
        writeScalar(out, v);
    }
}

// JSON.stringify(v, null, n): members on their own lines, "key": value, empty containers stay "{}" / "[]".
void writePretty(std::string& out, const JVal& v, const std::string& unit, std::string& indent)
{
    const bool isArr = v.isArray();
    if (!isArr && !v.isObject()) { writeScalar(out, v); return; }
    if (v.size() == 0) { out.append(isArr ? "[]" : "{}", 2); return; }
    out.push_back(isArr ? '[' : '{');
    indent += unit;
    bool first = true;
    auto lead = [&] {
        if (!first) out.push_back(',');
        first = false;
        out.push_back('\n');
        out += indent;
    };
    if (isArr) {
        for (const JVal& e : v.arr()) { lead(); writePretty(out, e, unit, indent); }
    } else {
        for (const JObject::Member& m : v.obj()) {
            lead();
            appendQuoted(out, m.first);
            out.append(": ", 2);
            writePretty(out, m.second, unit, indent);
        }
    }
    indent.resize(indent.size() - unit.size());
    out.push_back('\n');
    out += indent;
    out.push_back(isArr ? ']' : '}');
}

// ---- dates ----------------------------------------------------------------------------------------------------------
// Days since 1970-01-01 <-> proleptic Gregorian dates (H. Hinnant's civil algorithms; exact for the whole JS range).

void civilFromDays(int64_t z, int64_t& y, unsigned& m, unsigned& d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<int64_t>(yoe) + era * 400 + (m <= 2 ? 1 : 0);
}

int64_t daysFromCivil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2 ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

// ---- parser ---------------------------------------------------------------------------------------------------------

class Parser {
public:
    explicit Parser(std::string_view t) : b_(t.data()), p_(t.data()), e_(t.data() + t.size()) {}

    JVal document()
    {
        if (e_ - p_ >= 3 && uc(p_[0]) == 0xEF && uc(p_[1]) == 0xBB && uc(p_[2]) == 0xBF) p_ += 3;
        ws();
        JVal v = value();
        ws();
        if (p_ != e_)   // V8 words this one without "in JSON"
            throw JsonError("Unexpected non-whitespace character after JSON at position " + std::to_string(pos()), pos());
        return v;
    }

private:
    // One open object or array. A member or element is attached only once it is complete, so no frame holds an open
    // container and the frames live on the heap (value()).
    struct Frame {
        bool isObject = false;
        JObject obj;
        JVal::Array arr;
        std::string key;                          // the member whose value is being parsed
        // JSON.parse keeps a repeated key at its first position with the last value. Small objects are scanned;
        // bigger ones get a hash -> position index (built lazily: most objects never need it), so a body with
        // 100,000 keys stays linear. Hashes, not views, are stored: member strings move when the vector grows.
        std::unique_ptr<std::unordered_multimap<size_t, size_t>> index;
    };

    size_t pos() const { return static_cast<size_t>(p_ - b_); }

    // The positions are V8's. The wording is V8's older "... in JSON at position N" form (Node 22 phrases most of
    // these differently, e.g. "Unexpected token ']', "[1,]" is not valid JSON"); the texts go to logs only (clients
    // get "Invalid JSON body.") and quote at most one byte of the input.
    [[noreturn]] void fail(const char* what) const
    {
        throw JsonError(std::string(what) + " in JSON at position " + std::to_string(pos()), pos());
    }
    [[noreturn]] void unexpected() const
    {
        if (p_ >= e_) throw JsonError("Unexpected end of JSON input", pos());
        const unsigned char c = uc(*p_);
        char tok[8];
        if (c >= 0x21 && c < 0x7F) std::snprintf(tok, sizeof tok, "%c", c);
        else std::snprintf(tok, sizeof tok, "\\x%02X", c);
        throw JsonError(std::string("Unexpected token ") + tok + " in JSON at position " + std::to_string(pos()), pos());
    }

    void ws() { while (p_ < e_ && isJsonWs(*p_)) ++p_; }

    // Iterative on purpose: open containers are Frames on the heap, not calls. HTTP workers reserve 256 KB of stack
    // (core/http_server.h) and a stack overflow kills the process; the recursive descent this replaces used ~520
    // bytes per object level, so a 2.6 KB body nested 512 deep overflowed a worker. The call depth is now constant;
    // the deepest recursion left is JVal's copy/==/destructor and the writers on an accepted value, at most kMaxDepth
    // levels (test_anfs_core_json deepValuesFitWorkerStack measures them on a small stack).
    JVal value()
    {
        std::vector<Frame> open;
        JVal done;
        for (;;) {
            if (p_ >= e_) unexpected();
            const char c = *p_;
            if (c == '{' || c == '[') {
                if (open.size() >= static_cast<size_t>(kMaxDepth)) fail("Nesting deeper than 512 levels");
                ++p_;
                ws();
                const char close = c == '{' ? '}' : ']';
                if (p_ < e_ && *p_ == close) {
                    ++p_;
                    done = c == '{' ? JVal(JObject{}) : JVal(JVal::Array{});
                } else {
                    open.emplace_back();
                    open.back().isObject = c == '{';
                    if (c == '{') memberKey(open.back());
                    continue;                     // parse the first member's value / the first element
                }
            } else {
                done = scalar();
            }
            // `done` is complete: attach it, and close every container that ends right after it.
            for (;;) {
                if (open.empty()) return done;
                Frame& f = open.back();
                if (f.isObject) addMember(f, std::move(done));
                else f.arr.push_back(std::move(done));
                ws();
                if (p_ < e_ && *p_ == ',') {
                    ++p_;
                    if (f.isObject) memberKey(f);
                    else ws();
                    break;                        // parse the next value
                }
                if (p_ < e_ && *p_ == (f.isObject ? '}' : ']')) {
                    ++p_;
                    done = f.isObject ? JVal(std::move(f.obj)) : JVal(std::move(f.arr));
                    open.pop_back();
                    continue;
                }
                unexpected();
            }
        }
    }

    JVal scalar()
    {
        switch (*p_) {
        case '"': return JVal(string());
        case 't': literal("true", 4); return JVal(true);
        case 'f': literal("false", 5); return JVal(false);
        case 'n': literal("null", 4); return JVal();
        default:
            if (*p_ == '-' || isDigit(*p_)) return number();
            unexpected();
        }
    }

    void literal(const char* lit, size_t n)
    {
        for (size_t i = 0; i < n; ++i, ++p_)
            if (p_ >= e_ || *p_ != lit[i]) unexpected();
    }

    // `"key" :` and the whitespace after it; p_ is then at the member's value.
    void memberKey(Frame& f)
    {
        ws();
        if (p_ >= e_ || *p_ != '"') unexpected();
        f.key = string();
        ws();
        if (p_ >= e_ || *p_ != ':') unexpected();
        ++p_;
        ws();
    }

    static void addMember(Frame& f, JVal&& v)
    {
        const std::hash<std::string_view> hasher;
        const std::vector<JObject::Member>& members = f.obj.members();
        size_t at = members.size();
        if (!f.index) {
            for (size_t i = 0; i < members.size(); ++i)
                if (members[i].first == f.key) { at = i; break; }
        } else {
            auto range = f.index->equal_range(hasher(f.key));
            for (auto it = range.first; it != range.second; ++it)
                if (members[it->second].first == f.key) { at = it->second; break; }
        }
        if (at < members.size()) {
            (f.obj.begin() + static_cast<std::ptrdiff_t>(at))->second = std::move(v);
            return;
        }
        const size_t h = f.index ? hasher(f.key) : 0;
        f.obj.append(std::move(f.key), std::move(v));
        if (f.index) {
            f.index->emplace(h, f.obj.size() - 1);
        } else if (f.obj.size() > kLinearKeys) {
            f.index = std::make_unique<std::unordered_multimap<size_t, size_t>>();
            f.index->reserve(64);
            for (size_t i = 0; i < f.obj.size(); ++i) f.index->emplace(hasher(f.obj.members()[i].first), i);
        }
    }

    int hexDigit(char c) const
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
    // Four hex digits at q, or -1.
    int32_t hex4(const char* q) const
    {
        if (e_ - q < 4) return -1;
        int32_t u = 0;
        for (int i = 0; i < 4; ++i) {
            const int h = hexDigit(q[i]);
            if (h < 0) return -1;
            u = (u << 4) | h;
        }
        return u;
    }

    std::string string()
    {
        ++p_;                                     // opening quote
        std::string out;
        const char* run = p_;
        for (;;) {
            if (p_ >= e_) throw JsonError("Unterminated string in JSON at position " + std::to_string(pos()), pos());
            const unsigned char c = uc(*p_);
            if (c == '"') {
                out.append(run, static_cast<size_t>(p_ - run));
                ++p_;
                return out;
            }
            if (c == '\\') {
                out.append(run, static_cast<size_t>(p_ - run));
                ++p_;
                escape(out);
                run = p_;
                continue;
            }
            if (c < 0x20) fail("Bad control character in string literal");
            if (c < 0x80) { ++p_; continue; }
            const Utf8Seq q = decodeUtf8(reinterpret_cast<const unsigned char*>(p_), static_cast<size_t>(e_ - p_), false);
            if (q.ok) { p_ += q.len; continue; }
            out.append(run, static_cast<size_t>(p_ - run));
            out.append(kReplacement, 3);
            p_ += q.len;
            run = p_;
        }
    }

    void escape(std::string& out)
    {
        if (p_ >= e_) unexpected();
        switch (*p_) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
            const int32_t u = hex4(p_ + 1);
            if (u < 0) {                          // V8 reports the first byte that is not a hex digit
                ++p_;
                for (int i = 0; i < 4 && p_ < e_ && hexDigit(*p_) >= 0; ++i) ++p_;
                if (p_ >= e_) unexpected();
                fail("Bad Unicode escape");
            }
            p_ += 5;
            // A high surrogate followed by an escaped low one is one character; anything else stays a lone half.
            if (u >= 0xD800 && u <= 0xDBFF && e_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
                const int32_t lo = hex4(p_ + 2);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    appendUtf8(out, 0x10000 + ((static_cast<uint32_t>(u) - 0xD800) << 10) + (static_cast<uint32_t>(lo) - 0xDC00));
                    p_ += 6;
                    return;
                }
            }
            appendUtf8(out, static_cast<uint32_t>(u));
            return;
        }
        default: fail("Bad escaped character");
        }
        ++p_;
    }

    JVal number()
    {
        const char* start = p_;
        bool integral = true;
        if (*p_ == '-') {
            ++p_;
            if (p_ >= e_) fail("No number after minus sign");
        }
        if (*p_ == '0') {
            ++p_;
        } else if (*p_ >= '1' && *p_ <= '9') {
            while (p_ < e_ && isDigit(*p_)) ++p_;
        } else {
            unexpected();
        }
        if (p_ < e_ && *p_ == '.') {
            integral = false;
            ++p_;
            if (p_ >= e_ || !isDigit(*p_)) unexpected();
            while (p_ < e_ && isDigit(*p_)) ++p_;
        }
        if (p_ < e_ && (*p_ == 'e' || *p_ == 'E')) {
            integral = false;
            ++p_;
            if (p_ < e_ && (*p_ == '+' || *p_ == '-')) ++p_;
            if (p_ >= e_ || !isDigit(*p_)) unexpected();
            while (p_ < e_ && isDigit(*p_)) ++p_;
        }
        if (integral) {
            int64_t i = 0;
            const auto r = std::from_chars(start, p_, i);
            if (r.ec == std::errc() && r.ptr == p_) return JVal(i);
        }
        double d = 0;
        const auto r = std::from_chars(start, p_, d);
        if (r.ec == std::errc::result_out_of_range) d = outOfRange(start, p_);
        else if (r.ec != std::errc() || r.ptr != p_) fail("Bad number");
        return JVal(d);
    }

    // from_chars leaves the value alone when it does not fit: JSON.parse gives +-Infinity on overflow and +-0 on
    // underflow. The decimal order of magnitude tells which (out of range only happens far beyond 1e308 or below
    // 1e-323, so the estimate cannot be off by the one it might miss).
    static double outOfRange(const char* s, const char* e)
    {
        const bool neg = *s == '-';
        if (neg) ++s;
        int64_t intDigits = 0, index = 0, firstNonZero = -1;
        bool frac = false;
        for (; s < e && *s != 'e' && *s != 'E'; ++s) {
            if (*s == '.') { frac = true; continue; }
            if (!frac) ++intDigits;
            if (*s != '0' && firstNonZero < 0) firstNonZero = index;
            ++index;
        }
        int64_t exp = 0;
        if (s < e) {
            ++s;
            bool eneg = false;
            if (*s == '+' || *s == '-') eneg = *s++ == '-';
            for (; s < e; ++s) exp = std::min<int64_t>(exp * 10 + (*s - '0'), 1000000000);
            if (eneg) exp = -exp;
        }
        const double zero = neg ? -0.0 : 0.0;
        if (firstNonZero < 0) return zero;
        const double inf = std::numeric_limits<double>::infinity();
        return intDigits - firstNonZero + exp > 0 ? (neg ? -inf : inf) : zero;
    }

    const char* b_;
    const char* p_;
    const char* e_;
};

bool parseFixed(std::string_view s, size_t& p, int count, int& out)
{
    if (p + static_cast<size_t>(count) > s.size()) return false;
    int v = 0;
    for (int i = 0; i < count; ++i) {
        const char c = s[p + static_cast<size_t>(i)];
        if (!isDigit(c)) return false;
        v = v * 10 + (c - '0');
    }
    p += static_cast<size_t>(count);
    out = v;
    return true;
}

} // namespace

// ---- JObject --------------------------------------------------------------------------------------------------------

JObject::JObject(std::initializer_list<Member> init)
{
    m_.reserve(init.size());
    for (const Member& m : init) set(m.first, m.second);    // a repeated key behaves like a JS object literal
}

const JVal* JObject::find(std::string_view key) const
{
    for (const Member& m : m_)
        if (m.first == key) return &m.second;
    return nullptr;
}

JVal* JObject::find(std::string_view key)
{
    for (Member& m : m_)
        if (m.first == key) return &m.second;
    return nullptr;
}

JVal& JObject::set(std::string_view key, JVal v)
{
    if (JVal* p = find(key)) {
        *p = std::move(v);
        return *p;
    }
    m_.emplace_back(std::string(key), std::move(v));
    return m_.back().second;
}

JVal& JObject::append(std::string key, JVal v)
{
    m_.emplace_back(std::move(key), std::move(v));
    return m_.back().second;
}

JVal& JObject::operator[](std::string_view key)
{
    if (JVal* p = find(key)) return *p;
    m_.emplace_back(std::string(key), JVal());
    return m_.back().second;
}

bool JObject::erase(std::string_view key)
{
    for (auto it = m_.begin(); it != m_.end(); ++it) {
        if (it->first == key) {
            m_.erase(it);
            return true;
        }
    }
    return false;
}

void JObject::clear() { m_.clear(); }
size_t JObject::size() const { return m_.size(); }
bool JObject::empty() const { return m_.empty(); }
void JObject::reserve(size_t n) { m_.reserve(n); }
JObject::iterator JObject::begin() { return m_.begin(); }
JObject::iterator JObject::end() { return m_.end(); }
JObject::const_iterator JObject::begin() const { return m_.begin(); }
JObject::const_iterator JObject::end() const { return m_.end(); }
bool JObject::operator==(const JObject& o) const { return m_ == o.m_; }

// ---- JVal -----------------------------------------------------------------------------------------------------------

bool JVal::asBool(bool def) const
{
    if (const bool* b = std::get_if<bool>(&v_)) return *b;
    return def;
}

int64_t JVal::asInt(int64_t def) const
{
    if (const int64_t* i = std::get_if<int64_t>(&v_)) return *i;
    if (const double* d = std::get_if<double>(&v_)) {
        // integral and inside int64 (2^63 itself is not); NaN fails every comparison
        if (*d == std::trunc(*d) && *d >= -9223372036854775808.0 && *d < 9223372036854775808.0) return static_cast<int64_t>(*d);
    }
    return def;
}

double JVal::asDouble(double def) const
{
    if (const double* d = std::get_if<double>(&v_)) return *d;
    if (const int64_t* i = std::get_if<int64_t>(&v_)) return static_cast<double>(*i);
    return def;
}

const std::string& JVal::asString() const
{
    if (const std::string* s = std::get_if<std::string>(&v_)) return *s;
    return emptyString();
}

std::string JVal::str(std::string_view def) const
{
    if (const std::string* s = std::get_if<std::string>(&v_)) return *s;
    return std::string(def);
}

bool JVal::truthy() const
{
    switch (type()) {
    case Type::Null: return false;
    case Type::Bool: return std::get<bool>(v_);
    case Type::Int: return std::get<int64_t>(v_) != 0;
    case Type::Double: { const double d = std::get<double>(v_); return d != 0 && !std::isnan(d); }
    case Type::String: return !std::get<std::string>(v_).empty();
    default: return true;                         // arrays and objects, even empty ones
    }
}

const JVal::Array& JVal::arr() const
{
    if (const Array* a = std::get_if<Array>(&v_)) return *a;
    return emptyArray();
}

JVal::Array& JVal::arr()
{
    if (isNull()) v_ = Array{};
    if (Array* a = std::get_if<Array>(&v_)) return *a;
    throw std::logic_error("JVal: not an array");
}

void JVal::push(JVal v) { arr().push_back(std::move(v)); }

size_t JVal::size() const
{
    if (const Array* a = std::get_if<Array>(&v_)) return a->size();
    if (const JObject* o = std::get_if<JObject>(&v_)) return o->size();
    return 0;
}

const JVal& JVal::at(size_t i) const
{
    const Array* a = std::get_if<Array>(&v_);
    return a && i < a->size() ? (*a)[i] : nullValue();
}

const JObject& JVal::obj() const
{
    if (const JObject* o = std::get_if<JObject>(&v_)) return *o;
    return emptyObject();
}

JObject& JVal::obj()
{
    if (isNull()) v_ = JObject{};
    if (JObject* o = std::get_if<JObject>(&v_)) return *o;
    throw std::logic_error("JVal: not an object");
}

const JVal* JVal::find(std::string_view key) const
{
    const JObject* o = std::get_if<JObject>(&v_);
    return o ? o->find(key) : nullptr;
}

JVal* JVal::find(std::string_view key)
{
    JObject* o = std::get_if<JObject>(&v_);
    return o ? o->find(key) : nullptr;
}

const JVal& JVal::get(std::string_view key) const
{
    const JVal* v = find(key);
    return v ? *v : nullValue();
}

JVal& JVal::set(std::string_view key, JVal v) { return obj().set(key, std::move(v)); }
JVal& JVal::operator[](std::string_view key) { return obj()[key]; }

bool JVal::erase(std::string_view key)
{
    JObject* o = std::get_if<JObject>(&v_);
    return o && o->erase(key);
}

// C# Json.Str: strings as they are; numbers and booleans as JS String() prints them; anything else is def.
std::string JVal::getStr(std::string_view key, std::string_view def) const
{
    const JVal* v = find(key);
    if (!v) return std::string(def);
    switch (v->type()) {
    case Type::String: return v->asString();
    case Type::Int: return std::to_string(v->asInt());
    case Type::Double: return json::numberToString(v->asDouble());
    case Type::Bool: return v->asBool() ? "true" : "false";
    default: return std::string(def);
    }
}

// C# Json.Int: Int; a finite Double truncated toward zero; a string holding a (whitespace-padded, signed) integer.
int64_t JVal::getInt(std::string_view key, int64_t def) const
{
    const JVal* v = find(key);
    if (!v) return def;
    if (v->isInt()) return v->asInt();
    if (v->isDouble()) {
        const double d = std::trunc(v->asDouble());
        if (d >= -9223372036854775808.0 && d < 9223372036854775808.0) return static_cast<int64_t>(d);
        return def;
    }
    if (v->isString()) {
        std::string_view s = v->asString();
        while (!s.empty() && (s.front() == ' ' || (s.front() >= '\t' && s.front() <= '\r'))) s.remove_prefix(1);
        while (!s.empty() && (s.back() == ' ' || (s.back() >= '\t' && s.back() <= '\r'))) s.remove_suffix(1);
        const bool plus = !s.empty() && s.front() == '+';
        if (plus) s.remove_prefix(1);
        if (s.empty() || s.front() == '+' || (plus && s.front() == '-')) return def;
        int64_t r = 0;
        const auto res = std::from_chars(s.data(), s.data() + s.size(), r);
        if (res.ec == std::errc() && res.ptr == s.data() + s.size()) return r;
    }
    return def;
}

// C# Json.Bool: a boolean, or the strings "true" / "false" in any case.
bool JVal::getBool(std::string_view key, bool def) const
{
    const JVal* v = find(key);
    if (!v) return def;
    if (v->isBool()) return v->asBool();
    if (v->isString()) {
        if (equalsAsciiNoCase(v->asString(), "true")) return true;
        if (equalsAsciiNoCase(v->asString(), "false")) return false;
    }
    return def;
}

const JVal* JVal::getObj(std::string_view key) const
{
    const JVal* v = find(key);
    return v && v->isObject() ? v : nullptr;
}

const JVal* JVal::getArr(std::string_view key) const
{
    const JVal* v = find(key);
    return v && v->isArray() ? v : nullptr;
}

bool JVal::operator==(const JVal& o) const { return v_ == o.v_; }

// ---- json:: ---------------------------------------------------------------------------------------------------------

namespace json {

std::string stringify(const JVal& v)
{
    std::string out;
    writeCompact(out, v);
    return out;
}

void stringifyTo(std::string& out, const JVal& v) { writeCompact(out, v); }

std::string stringifyPretty(const JVal& v, int indent)
{
    if (indent > 10) indent = 10;                 // JSON.stringify clamps the space argument to 10
    if (indent < 1) return stringify(v);
    std::string out, cur;
    writePretty(out, v, std::string(static_cast<size_t>(indent), ' '), cur);
    return out;
}

JVal parse(std::string_view text) { return Parser(text).document(); }

bool tryParse(std::string_view text, JVal& out, std::string* error)
{
    try {
        out = parse(text);
        return true;
    } catch (const JsonError& e) {
        if (error) *error = e.what();
        return false;
    }
}

std::string quote(std::string_view s)
{
    std::string out;
    out.reserve(s.size() + 2);
    appendQuoted(out, s);
    return out;
}

// ECMA-262 Number::toString(x) for radix 10: k significant digits s, decimal exponent n (x = s * 10^(n-k)).
std::string numberToString(double d)
{
    if (std::isnan(d)) return "NaN";
    if (std::isinf(d)) return d > 0 ? "Infinity" : "-Infinity";
    if (d == 0) return "0";                       // -0 too
    char buf[40];
    const auto r = std::to_chars(buf, buf + sizeof buf, d, std::chars_format::scientific);   // shortest round trip
    std::string_view t(buf, static_cast<size_t>(r.ptr - buf));
    std::string out;
    if (t.front() == '-') {
        out.push_back('-');
        t.remove_prefix(1);
    }
    const size_t ePos = t.find('e');
    std::string digits;
    for (size_t i = 0; i < ePos; ++i)
        if (t[i] != '.') digits.push_back(t[i]);
    int exp = 0;
    std::from_chars(t.data() + ePos + (t[ePos + 1] == '+' ? 2 : 1), t.data() + t.size(), exp);
    const int k = static_cast<int>(digits.size());
    const int n = exp + 1;
    if (k <= n && n <= 21) {
        out += digits;
        out.append(static_cast<size_t>(n - k), '0');
    } else if (0 < n && n <= 21) {
        out.append(digits, 0, static_cast<size_t>(n));
        out.push_back('.');
        out.append(digits, static_cast<size_t>(n), std::string::npos);
    } else if (-6 < n && n <= 0) {
        out.append("0.", 2);
        out.append(static_cast<size_t>(-n), '0');
        out += digits;
    } else {
        out.push_back(digits[0]);
        if (k > 1) {
            out.push_back('.');
            out.append(digits, 1, std::string::npos);
        }
        out.push_back('e');
        out.push_back(n - 1 < 0 ? '-' : '+');
        out += std::to_string(n - 1 < 0 ? 1 - n : n - 1);
    }
    return out;
}

std::string isoMs(int64_t epochMs)
{
    if (epochMs < -kMaxTimeMs || epochMs > kMaxTimeMs) return {};   // JS: RangeError (an invalid Date)
    int64_t days = epochMs / kDayMs;
    int64_t rem = epochMs % kDayMs;
    if (rem < 0) {
        rem += kDayMs;
        --days;
    }
    int64_t y = 0;
    unsigned m = 0, d = 0;
    civilFromDays(days, y, m, d);
    const int ms = static_cast<int>(rem % 1000);
    const int sec = static_cast<int>(rem / 1000 % 60);
    const int min = static_cast<int>(rem / 60000 % 60);
    const int hour = static_cast<int>(rem / 3600000);
    char buf[40];
    if (y >= 0 && y <= 9999)
        std::snprintf(buf, sizeof buf, "%04d-%02u-%02uT%02d:%02d:%02d.%03dZ", static_cast<int>(y), m, d, hour, min, sec, ms);
    else   // expanded years: sign and six digits
        std::snprintf(buf, sizeof buf, "%c%06d-%02u-%02uT%02d:%02d:%02d.%03dZ", y < 0 ? '-' : '+',
                      static_cast<int>(y < 0 ? -y : y), m, d, hour, min, sec, ms);
    return buf;
}

// The ECMA-262 date time string format: YYYY[-MM[-DD]][THH:mm[:ss[.sss]][Z|+HH:mm|-HH:mm]], years also +-YYYYYY.
// Date-only forms are UTC; a time without an offset is local time (as Date.parse). Like V8: more than three fraction
// digits are truncated, an offset may omit its colon, and a day up to 31 rolls into the next month.
int64_t parseIsoMs(std::string_view s)
{
    size_t p = 0;
    int64_t year = 0;
    int v = 0;
    if (!s.empty() && (s[0] == '+' || s[0] == '-')) {
        const bool neg = s[0] == '-';
        p = 1;
        if (!parseFixed(s, p, 6, v) || (neg && v == 0)) return -1;   // "-000000" is not a year
        year = neg ? -v : v;
    } else {
        if (!parseFixed(s, p, 4, v)) return -1;
        year = v;
    }
    int month = 1, day = 1, hh = 0, mm = 0, ss = 0, ms = 0, offsetMin = 0;
    bool hasTime = false, hasOffset = false;
    if (p < s.size() && s[p] == '-') {
        ++p;
        if (!parseFixed(s, p, 2, month)) return -1;
        if (p < s.size() && s[p] == '-') {
            ++p;
            if (!parseFixed(s, p, 2, day)) return -1;
        }
    }
    if (p < s.size() && s[p] == 'T') {
        ++p;
        hasTime = true;
        if (!parseFixed(s, p, 2, hh) || p >= s.size() || s[p] != ':') return -1;
        ++p;
        if (!parseFixed(s, p, 2, mm)) return -1;
        if (p < s.size() && s[p] == ':') {
            ++p;
            if (!parseFixed(s, p, 2, ss)) return -1;
            if (p < s.size() && s[p] == '.') {
                ++p;
                int digits = 0;
                for (; p < s.size() && isDigit(s[p]); ++p, ++digits)
                    if (digits < 3) ms = ms * 10 + (s[p] - '0');
                if (digits == 0) return -1;
                for (int i = digits; i < 3; ++i) ms *= 10;
            }
        }
        if (p < s.size() && s[p] == 'Z') {
            ++p;
            hasOffset = true;
        } else if (p < s.size() && (s[p] == '+' || s[p] == '-')) {
            const int sign = s[p] == '-' ? -1 : 1;
            ++p;
            int oh = 0, om = 0;
            if (!parseFixed(s, p, 2, oh)) return -1;
            if (p < s.size() && s[p] == ':') ++p;          // V8 also takes +HHmm
            if (!parseFixed(s, p, 2, om) || oh > 23 || om > 59) return -1;
            offsetMin = sign * (oh * 60 + om);
            hasOffset = true;
        }
    }
    if (p != s.size()) return -1;
    // V8 checks the day against 31 only and lets MakeDay roll it over: 2026-02-29 is 2026-03-01.
    if (month < 1 || month > 12 || day < 1 || day > 31) return -1;
    if (hh > 24 || mm > 59 || ss > 59 || (hh == 24 && (mm || ss || ms))) return -1;
    int64_t t = 0;
    if (hasTime && !hasOffset) {
        std::tm tm{};
        tm.tm_year = static_cast<int>(year - 1900);
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
        tm.tm_hour = hh;
        tm.tm_min = mm;
        tm.tm_sec = ss;
        tm.tm_isdst = -1;
        const std::time_t local = std::mktime(&tm);   // CRT range 1970..3000; outside it Date.parse still works
        if (local == static_cast<std::time_t>(-1)) return -1;
        t = static_cast<int64_t>(local) * 1000 + ms;
    } else {
        t = (daysFromCivil(year, static_cast<unsigned>(month), 1) + day - 1) * kDayMs +
            ((hh * 60LL + mm) * 60 + ss) * 1000 + ms - offsetMin * 60000LL;
    }
    if (t < -kMaxTimeMs || t > kMaxTimeMs) return -1;
    return t;
}

std::string errorBody(std::string_view text)
{
    std::string out = "{\"error\":";
    appendQuoted(out, text);
    out.push_back('}');
    return out;
}

bool isJsonContentType(std::string_view contentType)
{
    std::string_view t = contentType.substr(0, contentType.find(';'));
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.remove_prefix(1);
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.remove_suffix(1);
    return equalsAsciiNoCase(t, "application/json");
}

JVal parseBody(std::string_view body)
{
    std::string_view t = body;
    if (t.size() >= 3 && uc(t[0]) == 0xEF && uc(t[1]) == 0xBB && uc(t[2]) == 0xBF) t.remove_prefix(3);   // iconv strips it
    if (t.empty()) return JVal::object();         // body-parser: "special-case empty json body"
    size_t i = 0;
    while (i < t.size() && isJsonWs(t[i])) ++i;   // body-parser FIRST_CHAR_REGEXP
    const size_t at = body.size() - t.size() + i;
    if (i == t.size()) throw JsonError("Unexpected end of JSON input (blank body)", at);
    if (t[i] != '{' && t[i] != '[')
        throw JsonError("Strict mode: the body must be a JSON object or array, at position " + std::to_string(at), at);
    return parse(body);
}

// ---- Writer ---------------------------------------------------------------------------------------------------------
// first_ has one slot more than stack_: first_[0] says whether the single top-level value is still to come, the
// others whether the open container has no member yet.

Writer::Writer(Sink& out, size_t flushBytes) : out_(out), flushBytes_(flushBytes ? flushBytes : kCopyBufferBytes)
{
    buf_.reserve(std::min<size_t>(flushBytes_, kCopyBufferBytes) + 256);
    first_.push_back(true);
}

Writer::~Writer() = default;

void Writer::beforeValue()
{
    if (stack_.empty()) {
        if (!first_.front()) throw std::logic_error("json::Writer: a second top-level value");
        first_.front() = false;
        return;
    }
    if (stack_.back() == '{') {
        if (!expectValueAfterKey_) throw std::logic_error("json::Writer: a value in an object needs key() first");
        expectValueAfterKey_ = false;
        return;
    }
    if (!first_.back()) buf_.push_back(',');
    first_.back() = false;
}

void Writer::flushIfFull()
{
    if (buf_.size() < flushBytes_) return;
    out_.write(buf_.data(), buf_.size());
    written_ += static_cast<int64_t>(buf_.size());
    buf_.clear();
}

Writer& Writer::beginObject()
{
    beforeValue();
    buf_.push_back('{');
    stack_.push_back('{');
    first_.push_back(true);
    return *this;
}

Writer& Writer::beginArray()
{
    beforeValue();
    buf_.push_back('[');
    stack_.push_back('[');
    first_.push_back(true);
    return *this;
}

Writer& Writer::end()
{
    if (stack_.empty()) throw std::logic_error("json::Writer: end() without an open object or array");
    if (expectValueAfterKey_) throw std::logic_error("json::Writer: end() right after key()");
    buf_.push_back(stack_.back() == '{' ? '}' : ']');
    stack_.pop_back();
    first_.pop_back();
    flushIfFull();
    return *this;
}

Writer& Writer::key(std::string_view k)
{
    if (stack_.empty() || stack_.back() != '{') throw std::logic_error("json::Writer: key() outside an object");
    if (expectValueAfterKey_) throw std::logic_error("json::Writer: key() twice without a value");
    if (!first_.back()) buf_.push_back(',');
    first_.back() = false;
    appendQuoted(buf_, k);
    buf_.push_back(':');
    expectValueAfterKey_ = true;
    flushIfFull();
    return *this;
}

// Containers go through the writer itself, so a large value is flushed piece by piece instead of built whole.
Writer& Writer::value(const JVal& v)
{
    if (v.isArray()) {
        beginArray();
        for (const JVal& e : v.arr()) value(e);
        return end();
    }
    if (v.isObject()) {
        beginObject();
        for (const JObject::Member& m : v.obj()) {
            key(m.first);
            value(m.second);
        }
        return end();
    }
    beforeValue();
    writeScalar(buf_, v);
    flushIfFull();
    return *this;
}

Writer& Writer::null() { return value(JVal()); }
Writer& Writer::boolean(bool b) { return value(JVal(b)); }

Writer& Writer::integer(int64_t i)
{
    beforeValue();
    appendInt(buf_, i);
    flushIfFull();
    return *this;
}

Writer& Writer::number(double d)
{
    beforeValue();
    appendDouble(buf_, d);
    flushIfFull();
    return *this;
}

Writer& Writer::string(std::string_view s)
{
    beforeValue();
    appendQuoted(buf_, s);
    flushIfFull();
    return *this;
}

Writer& Writer::raw(std::string_view jsonText)
{
    beforeValue();
    buf_.append(jsonText.data(), jsonText.size());
    flushIfFull();
    return *this;
}

void Writer::finish()
{
    if (!stack_.empty() || expectValueAfterKey_) throw std::logic_error("json::Writer: finish() with an open object or array");
    if (first_.front()) throw std::logic_error("json::Writer: finish() before any value");
    if (!buf_.empty()) {
        out_.write(buf_.data(), buf_.size());
        written_ += static_cast<int64_t>(buf_.size());
        buf_.clear();
    }
    out_.flush();
}

} // namespace json
} // namespace anfs
