// core/json.h - ordered JSON value with a JSON.stringify-exact writer. FROZEN (ANFS-F0); implemented by ANFS-F1a
// (core/json.cpp).
//
// Why not QJsonObject: it sorts keys, and Node's key order is part of the wire contract (CONVENTIONS.md section 2).
// Objects keep insertion order; assigning an existing key replaces the value in place (JS `obj[k] = v`). JS would list
// array-index keys ("0", "17") first in numeric order; no AN-FS object has such keys, so nothing reorders them.
// Members live in a vector: a reference from find()/set()/operator[] dies when a sibling is inserted, so
// `o["x"] = o["y"]` (y evaluated first, x may reallocate) must copy first: `JVal y = o["y"]; o["x"] = y;`.
//
// Writer rules (anfs-cpp.md 2.6): no whitespace; escapes only `"`, `\` and control characters (\b \f \n \r \t, else
// \u00XX with lowercase hex); lone UTF-16 surrogates encoded in the input (WTF-8) come out as \udXXX; '/' and other
// non-ASCII stay raw; integers print as integers; doubles print as JS Number#toString (shortest round trip, JS
// exponent rules; NaN and +-Infinity print as null like JSON.stringify).
//
// Parser: strict RFC 8259 (no comments, no trailing commas, no NaN), a leading UTF-8 BOM is skipped, nesting is capped
// at 512 levels, integers that fit int64 stay Int, everything else is Double. Invalid input throws JsonError.
// Stack: the parser is iterative (any depth of input costs no stack). Copying, comparing, writing and destroying a JVal
// recurse once per level; at the cap they touch at most ~84 KB (test_anfs_core_json measures this on a 192 KB thread),
// which leaves room below a 256 KB HTTP worker's own frames. Code that walks parsed input recursively should cost no
// more than ~150 bytes of stack per level, or walk with an explicit stack.
#pragma once

#include "core/stream.h"

#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace anfs {

class JsonError : public std::runtime_error {
public:
    JsonError(const std::string& what, size_t pos) : std::runtime_error(what), pos(pos) {}
    size_t pos = 0;   // byte offset of the error in the input
};

class JVal;

// Ordered object: a vector of members, looked up linearly (every AN-FS object is small; large data is arrays).
// Deviation from anfs-cpp.md 2.6 ("plus a small index"): no stored index, because keys stay writable through the
// iterators and an index would go stale. Bulk builders of untrusted input must therefore not call set() per member
// (quadratic): json::parse keeps its own key -> position hash per object while parsing and fills the object with
// append(); a duplicate key replaces the value at the first position, as JSON.parse does ({"a":1,"b":2,"a":3} gives
// {"a":3,"b":2}). A body with 100,000 keys then parses in linear time.
class JObject {
public:
    using Member = std::pair<std::string, JVal>;
    using iterator = std::vector<Member>::iterator;
    using const_iterator = std::vector<Member>::const_iterator;

    JObject() = default;
    JObject(std::initializer_list<Member> init);

    const JVal* find(std::string_view key) const;
    JVal* find(std::string_view key);
    bool has(std::string_view key) const { return find(key) != nullptr; }
    JVal& set(std::string_view key, JVal v);      // replace in place, else append
    JVal& append(std::string key, JVal v);        // no duplicate check: the caller knows the key is new (the parser)
    JVal& operator[](std::string_view key);       // inserts null when absent
    bool erase(std::string_view key);
    void clear();
    size_t size() const;
    bool empty() const;
    void reserve(size_t n);

    iterator begin();
    iterator end();
    const_iterator begin() const;
    const_iterator end() const;
    const std::vector<Member>& members() const { return m_; }

    bool operator==(const JObject& o) const;      // same keys, same order, equal values
    bool operator!=(const JObject& o) const { return !(*this == o); }

private:
    std::vector<Member> m_;
};

class JVal {
public:
    enum class Type : uint8_t { Null, Bool, Int, Double, String, Array, Object };
    using Array = std::vector<JVal>;

    JVal() noexcept = default;
    JVal(std::nullptr_t) noexcept {}
    JVal(bool b) : v_(b) {}
    JVal(int i) : v_(static_cast<int64_t>(i)) {}
    JVal(unsigned i) : v_(static_cast<int64_t>(i)) {}
    JVal(long i) : v_(static_cast<int64_t>(i)) {}
    JVal(unsigned long i) : v_(static_cast<int64_t>(i)) {}
    JVal(long long i) : v_(static_cast<int64_t>(i)) {}
    JVal(unsigned long long i) : v_(static_cast<int64_t>(i)) {}   // values above INT64_MAX wrap: callers never pass them
    JVal(double d) : v_(d) {}
    JVal(const char* s) : v_(std::string(s ? s : "")) {}
    JVal(std::string s) : v_(std::move(s)) {}
    JVal(std::string_view s) : v_(std::string(s)) {}
    JVal(Array a) : v_(std::move(a)) {}
    JVal(JObject o) : v_(std::move(o)) {}

    static JVal array() { return JVal(Array{}); }
    static JVal array(std::initializer_list<JVal> items) { return JVal(Array(items)); }
    static JVal object() { return JVal(JObject{}); }
    static JVal object(std::initializer_list<JObject::Member> members) { return JVal(JObject(members)); }

    Type type() const { return static_cast<Type>(v_.index()); }
    bool isNull() const { return type() == Type::Null; }
    bool isBool() const { return type() == Type::Bool; }
    bool isInt() const { return type() == Type::Int; }
    bool isDouble() const { return type() == Type::Double; }
    bool isNumber() const { return isInt() || isDouble(); }
    bool isString() const { return type() == Type::String; }
    bool isArray() const { return type() == Type::Array; }
    bool isObject() const { return type() == Type::Object; }

    // Lenient scalar reads (C# Json.Str/Int/Bool): the default when the type does not fit. asInt accepts an
    // integral Double; asDouble accepts Int.
    bool asBool(bool def = false) const;
    int64_t asInt(int64_t def = 0) const;
    double asDouble(double def = 0) const;
    const std::string& asString() const;          // "" (static) when not a string
    std::string str(std::string_view def) const;  // the string, else def

    // JS truthiness: null, false, 0, NaN and "" are false; arrays and objects are true.
    bool truthy() const;

    // Arrays. The non-const accessors turn a null value into an empty array first; on any other type they (and the
    // object ones below) throw std::logic_error instead of silently dropping the value.
    const Array& arr() const;                     // empty (static) when not an array
    Array& arr();
    void push(JVal v);
    size_t size() const;                          // array length, object member count, else 0
    const JVal& at(size_t i) const;               // null (static) when out of range or not an array

    // Objects. The non-const accessors turn a null value into an empty object first.
    const JObject& obj() const;                   // empty (static) when not an object
    JObject& obj();
    const JVal* find(std::string_view key) const;
    JVal* find(std::string_view key);
    bool has(std::string_view key) const { return find(key) != nullptr; }
    const JVal& get(std::string_view key) const;  // null (static) when absent
    JVal& set(std::string_view key, JVal v);
    JVal& operator[](std::string_view key);
    bool erase(std::string_view key);

    // Typed member reads with defaults (C# Json.Str/Int/Bool/Obj/Arr).
    std::string getStr(std::string_view key, std::string_view def = {}) const;
    int64_t getInt(std::string_view key, int64_t def = 0) const;
    bool getBool(std::string_view key, bool def = false) const;
    const JVal* getObj(std::string_view key) const;   // nullptr unless an object
    const JVal* getArr(std::string_view key) const;   // nullptr unless an array

    bool operator==(const JVal& o) const;         // deep; Int 1 != Double 1.0; object order matters
    bool operator!=(const JVal& o) const { return !(*this == o); }

private:
    std::variant<std::nullptr_t, bool, int64_t, double, std::string, Array, JObject> v_;
};

namespace json {

// JSON.stringify(v) bytes.
std::string stringify(const JVal& v);
void stringifyTo(std::string& out, const JVal& v);
// JSON.stringify(v, null, indent): used for anfs.json and peer-config.json (agent.js saveConfig).
std::string stringifyPretty(const JVal& v, int indent = 2);

JVal parse(std::string_view text);                                     // throws JsonError
bool tryParse(std::string_view text, JVal& out, std::string* error = nullptr);

// JSON.stringify of one string, quotes included.
std::string quote(std::string_view s);
// JS Number#toString for a double (shortest round trip; 1e21 and up and below 1e-6 in exponent form).
std::string numberToString(double d);

// Date#toISOString for epoch milliseconds: 2026-10-14T13:17:58.770Z (UTC, always milliseconds).
std::string isoMs(int64_t epochMs);
// Parses the toISOString form (and the same without milliseconds or with an offset); -1 when invalid.
int64_t parseIsoMs(std::string_view s);

// The Node-compat error body {"error":"<text>"} (CONVENTIONS.md section 2; no "ok" key).
std::string errorBody(std::string_view text);

// Express json() request bodies (body-parser strict mode, the default). Ctx::readJson (ANFS-F4a) and any route that
// reads its own body use these, so every route answers a bad body alike:
// - isJsonContentType: the media type (before ';', trimmed, any case) is application/json, the type-is match of the
//   default `type`; any other type leaves the body unparsed (Express keeps req.body = {}). Parameters are not
//   validated and the charset is taken as UTF-8 (Express answers 415 for a non-UTF charset; no AN-FS client sends one).
// - parseBody: a zero-length body (after a UTF-8 BOM) gives {}; otherwise the first non-whitespace character must be
//   '{' or '[' (a primitive or a blank body is a strict-mode violation), then parse(). An array stays an array
//   (Express; the C# Api.Body mapped it to {}, but the bridge's PUT /api/playout/sources reads an array body).
// - Every JsonError from parseBody is answered 400 errorBody(kInvalidJsonBody) (D4; Node sent an HTML 400 page).
inline constexpr std::string_view kInvalidJsonBody = "Invalid JSON body.";
bool isJsonContentType(std::string_view contentType);
JVal parseBody(std::string_view body);                                 // throws JsonError

// Streaming writer for large responses (watchdog changes, media pages, the peer's watchdog report). Output is
// byte-identical to stringify() of the equivalent value. Bytes are buffered and pushed to the sink in about 64 KB
// pieces; finish() flushes. Misuse (a value where a key is expected, unbalanced end) throws std::logic_error.
class Writer {
public:
    explicit Writer(Sink& out, size_t flushBytes = kCopyBufferBytes);
    ~Writer();                                   // does not flush: call finish()
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    Writer& beginObject();
    Writer& beginArray();
    Writer& end();                               // closes the innermost object or array
    Writer& key(std::string_view k);
    Writer& value(const JVal& v);
    Writer& null();
    Writer& boolean(bool b);
    Writer& integer(int64_t i);
    Writer& number(double d);
    Writer& string(std::string_view s);
    Writer& raw(std::string_view jsonText);      // pre-serialized JSON for one value
    void finish();
    int64_t bytesWritten() const { return written_; }

private:
    void beforeValue();
    void flushIfFull();

    Sink& out_;
    size_t flushBytes_;
    std::string buf_;
    std::vector<char> stack_;                    // '{' or '['
    std::vector<bool> first_;
    bool expectValueAfterKey_ = false;
    int64_t written_ = 0;
};

} // namespace json
} // namespace anfs
