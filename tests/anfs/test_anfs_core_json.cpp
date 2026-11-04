// test_anfs_core_json - ordered JSON (ANFS-F1a, core/json): JSON.stringify byte vectors (escapes, lone surrogates,
// invalid UTF-8, key order, integers, shortest doubles, pretty form), the strict parser (JSON.parse grammar, error
// positions, duplicate keys, nesting cap, the stack that values at the cap need on an HTTP-worker-sized thread, linear
// time on 100,000 keys), the Express body rules with the exact
// 400 {"error":"Invalid JSON body."} text, ISO-ms dates, the JVal accessors and the streaming writer.
//
// The literal vectors were produced by Node 22 (JSON.stringify / String(x) / toISOString / Date.parse). The last test
// repeats the comparison live against the local node.exe on ~12,000 doubles, 4,000 random UTF-16 strings (lone
// surrogates included) and 1,500 random documents; it is skipped when node is not installed.
#include "core/json.h"

#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QRandomGenerator>
#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>

#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using namespace anfs;

namespace {

QByteArray qb(const std::string& s) { return QByteArray(s.data(), static_cast<qsizetype>(s.size())); }
std::string unhex(const char* hex) { return QByteArray::fromHex(hex).toStdString(); }
double fromBits(uint64_t b) { double d; std::memcpy(&d, &b, 8); return d; }
uint64_t toBits(double d) { uint64_t b; std::memcpy(&b, &d, 8); return b; }

// QByteArray comparison prints both sides escaped when it fails.
#define JEQ(actual, expected) QCOMPARE(qb(actual), qb(std::string(expected)))

void appendUtf8(std::string& out, uint32_t cp)
{
    if (cp < 0x80) out.push_back(static_cast<char>(cp));
    else if (cp < 0x800) { out.push_back(static_cast<char>(0xC0 | (cp >> 6))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) {
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

// A JS string (UTF-16 code units) as the WTF-8 bytes JVal stores: pairs become one character, lone halves 3 bytes.
std::string wtf8(const std::vector<uint32_t>& u)
{
    std::string out;
    for (size_t i = 0; i < u.size(); ++i) {
        uint32_t c = u[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < u.size() && u[i + 1] >= 0xDC00 && u[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (u[i + 1] - 0xDC00);
            ++i;
        }
        appendUtf8(out, c);
    }
    return out;
}

// How ANFS-F4a's Ctx::readJson answers a body (json.h, Express section): status and response text.
std::pair<int, std::string> answerBody(std::string_view contentType, std::string_view body)
{
    if (body.empty() || !json::isJsonContentType(contentType)) return {200, json::stringify(JVal::object())};
    try {
        return {200, json::stringify(json::parseBody(body))};
    } catch (const JsonError&) {
        return {400, json::errorBody(json::kInvalidJsonBody)};
    }
}

class ChunkSink final : public Sink {
public:
    using Sink::write;
    void write(const char* p, size_t n) override { chunks.push_back(n); data.append(p, n); }
    void flush() override { ++flushes; }
    std::vector<size_t> chunks;
    std::string data;
    int flushes = 0;
};

#if defined(_WIN32)
// Runs fn on a new thread whose stack reserves `reserve` bytes, the way core/http_server.h starts its workers, and
// measures how much of it fn touched: Windows commits stack pages on first use and never gives them back, so the
// committed part below the top is the high-water mark. A stack overflow is caught (SEH) and reported, so the test
// fails instead of dying.
struct StackProbe {
    std::function<void()> fn;
    size_t peak = 0;
    bool overflow = false;
    std::string error;
};

size_t stackHighWater()
{
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    MEMORY_BASIC_INFORMATION mbi{};
    for (ULONG_PTR p = low; p < high; p = reinterpret_cast<ULONG_PTR>(mbi.BaseAddress) + mbi.RegionSize) {
        if (!VirtualQuery(reinterpret_cast<const void*>(p), &mbi, sizeof mbi)) break;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD)) return static_cast<size_t>(high - p);
    }
    return 0;
}

void runProbe(StackProbe* probe)
{
    try {
        probe->fn();
    } catch (const std::exception& e) {
        probe->error = e.what();
    } catch (...) {
        probe->error = "unknown exception";
    }
    probe->peak = stackHighWater();
}

// No C++ objects here: __try cannot share a function with destructors.
DWORD WINAPI probeThread(void* arg)
{
    StackProbe* probe = static_cast<StackProbe*>(arg);
#if defined(_MSC_VER)
    __try {
        runProbe(probe);
    } __except (GetExceptionCode() == EXCEPTION_STACK_OVERFLOW ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        probe->overflow = true;
    }
#else
    runProbe(probe);
#endif
    return 0;
}

StackProbe probeStack(size_t reserve, const std::function<void()>& fn)
{
    StackProbe probe;
    probe.fn = fn;
    HANDLE h = CreateThread(nullptr, reserve, probeThread, &probe, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!h) {
        probe.error = "CreateThread failed";
        return probe;
    }
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    return probe;
}
#endif

// The live oracle script (node <script> <mode> <in> <out>; hex in and out so every byte survives). Defined at the
// end of the file: moc stops seeing the test class when a long raw string comes before it.
const char* oracleJs();

QString findNode()
{
    QString p = QStandardPaths::findExecutable(QStringLiteral("node"));
    if (p.isEmpty() && QFileInfo::exists(QStringLiteral("C:/Program Files/nodejs/node.exe")))
        p = QStringLiteral("C:/Program Files/nodejs/node.exe");
    return p;
}

} // namespace

class TestAnfsCoreJson : public QObject {
    Q_OBJECT

    // Bodies are defined below the class: moc's tokenizer loses track of raw string literals that contain
    // quotes, so the class it parses holds declarations only.
private slots:
    void stringifyScalarsAndIntegers();
    void stringifyEscapes();
    void stringifyLoneSurrogates();
    void stringifyInvalidUtf8();
    void stringifyKeyOrderAndReplacement();
    void numberToStringVectors();
    void stringifyPrettyForm();
    void parseRoundTripAndTypes();
    void parseDuplicateKeys();
    void parseRejects();
    void parseNestingCap();
    void deepValuesFitWorkerStack();
    void parseLargeObjectIsLinear();
    void expressBodyRules();
    void isoDates();
    void jvalAccessors();
    void writerMatchesStringify();
    void writerRejectsMisuse();
    void nodeDifferential();
};

// ---- writer ---------------------------------------------------------------------------------------------------
void TestAnfsCoreJson::stringifyScalarsAndIntegers()
{
    JEQ(json::stringify(JVal()), "null");
    JEQ(json::stringify(JVal(nullptr)), "null");
    JEQ(json::stringify(JVal(true)), "true");
    JEQ(json::stringify(JVal(false)), "false");
    JEQ(json::stringify(JVal(0)), "0");
    JEQ(json::stringify(JVal(-1)), "-1");
    JEQ(json::stringify(JVal(std::numeric_limits<int64_t>::max())), "9223372036854775807");
    JEQ(json::stringify(JVal(std::numeric_limits<int64_t>::min())), "-9223372036854775808");
    JEQ(json::stringify(JVal(size_t(4000000000u))), "4000000000");
    JEQ(json::stringify(JVal(2.0)), "2");
    JEQ(json::stringify(JVal(-0.0)), "0");
    JEQ(json::stringify(JVal(1e21)), "1e+21");
    JEQ(json::stringify(JVal(9007199254740993.0)), "9007199254740992");   // the double nearest 2^53+1
    JEQ(json::stringify(JVal(std::nan(""))), "null");
    JEQ(json::stringify(JVal(std::numeric_limits<double>::infinity())), "null");
    JEQ(json::stringify(JVal(-std::numeric_limits<double>::infinity())), "null");
    JEQ(json::stringify(JVal::array()), "[]");
    JEQ(json::stringify(JVal::object()), "{}");
    JEQ(json::stringify(JVal::object({{"b", 1}, {"a", JVal::array({true, nullptr, 1.5, "x"})}, {"c", JVal::object()},
                                      {"d", JVal::array()}})),
        R"({"b":1,"a":[true,null,1.5,"x"],"c":{},"d":[]})");
    JEQ(json::stringify(JVal::array({JVal::array({JVal::array()}), JVal::object({{"", ""}})})), R"([[[]],{"":""}])");
}

void TestAnfsCoreJson::stringifyEscapes()
{
    std::string controls;
    for (int c = 0; c < 0x20; ++c) controls.push_back(static_cast<char>(c));
    // Raw literals with backslashes live in locals: MSVC's traditional preprocessor mangles them in macro arguments.
    const std::string controlsWant =
        R"("\u0000\u0001\u0002\u0003\u0004\u0005\u0006\u0007\b\t\n\u000b\f\r\u000e\u000f)"
        R"(\u0010\u0011\u0012\u0013\u0014\u0015\u0016\u0017\u0018\u0019\u001a\u001b\u001c\u001d\u001e\u001f")";
    JEQ(json::quote(controls), controlsWant);
    // Node: JSON.stringify('\u0000\u0001\u0007\b\t\n\u000b\f\r\u000e\u001f')
    JEQ(json::quote(std::string("\x00\x01\x07\b\t\n\x0b\f\r\x0e\x1f", 11)),
        unhex("225c75303030305c75303030315c75303030375c625c745c6e5c75303030625c665c725c75303030655c753030316622"));
    const std::string slashWant = R"("\"\\/")";              // '/' stays raw
    JEQ(json::quote("\"\\/"), slashWant);
    // DEL, U+0080, e-acute, U+2028, U+2029, BOM, U+FFFF: all raw (ES2019 JSON.stringify does not escape them)
    JEQ(json::quote(unhex("7fc280c3a9e280a8e280a9efbbbfefbfbf")), unhex("227fc280c3a9e280a8e280a9efbbbfefbfbf22"));
    JEQ(json::quote("plain ascii ~ 123"), R"("plain ascii ~ 123")");
    JEQ(json::quote(""), R"("")");
    const std::string nulWant = R"("a\u0000b")", keyWant = R"({"k\"\n":"v\t"})";
    JEQ(json::stringify(JVal(std::string("a\0b", 3))), nulWant);
    JEQ(json::stringify(JVal::object({{"k\"\n", "v\t"}})), keyWant);
    JEQ(json::stringify(JVal(std::string_view("emoji \xF0\x9F\x98\x80"))), "\"emoji \xF0\x9F\x98\x80\"");
}

void TestAnfsCoreJson::stringifyLoneSurrogates()
{
    // WTF-8 lone halves print as \udxxx, lower-case hex (Node: JSON.stringify('\ud800') and friends)
    JEQ(json::quote(unhex("eda080")), unhex("225c756438303022"));                 // "\ud800"
    JEQ(json::quote(unhex("edbfbf")), unhex("225c756466666622"));                 // "\udfff"
    JEQ(json::quote(unhex("f09f9880")), unhex("22f09f988022"));                   // a real pair stays raw
    JEQ(json::quote(unhex("edb880eda0bd")), unhex("225c75646530305c756438336422")); // low then high: both lone
    JEQ(json::quote(unhex("61eda08062")), unhex("22615c75643830306222"));          // "a\ud800b"
    // '\ud800\ud800\udc00': the second high half pairs with the low one
    JEQ(json::quote(unhex("eda080eda080edb080")), unhex("225c7564383030f090808022"));
    // two lone halves stored next to each other are one character in JS: printed as its UTF-8 form
    JEQ(json::quote(unhex("eda0bdedb880")), unhex("22f09f988022"));
    // a high half at the very end, or followed by a non-surrogate 3-byte character
    JEQ(json::quote(unhex("41edafbf")), "\"A\\udbff\"");
    JEQ(json::quote(unhex("edafbfe282ac")), "\"\\udbff\xE2\x82\xAC\"");
}

void TestAnfsCoreJson::stringifyInvalidUtf8()
{
    // Node: Buffer.from(bytes).toString() then JSON.stringify: one U+FFFD per maximal invalid subpart
    const char* cases[][2] = {
        {"ff", "22efbfbd22"},
        {"c3", "22efbfbd22"},
        {"c328", "22efbfbd2822"},
        {"e28241", "22efbfbd4122"},
        {"f09f98", "22efbfbd22"},
        {"c0af", "22efbfbdefbfbd22"},
        {"e080af", "22efbfbdefbfbdefbfbd22"},
        {"f4908080", "22efbfbdefbfbdefbfbdefbfbd22"},
        {"80", "22efbfbd22"},
        {"ed9fbf", "22ed9fbf22"},          // U+D7FF is a valid character
        {"f48fbfbf", "22f48fbfbf22"},      // U+10FFFF too
        // a cut-off surrogate prefix is not a stored lone half: ED and A0..BF are two subparts, as in Node's decoder
        {"eda0", "22efbfbdefbfbd22"},
        {"eda041", "22efbfbdefbfbd4122"},
        {"edbf", "22efbfbdefbfbd22"},
        {"edbf41", "22efbfbdefbfbd4122"},
        {"61eda0", "2261efbfbdefbfbd22"},
        {"ed9f", "22efbfbd22"},            // ED 9F is one subpart (9F is in ED's range)
    };
    for (const auto& c : cases) JEQ(json::quote(unhex(c[0])), unhex(c[1]));
}

void TestAnfsCoreJson::stringifyKeyOrderAndReplacement()
{
    JVal o = JVal::object();
    o["z"] = 1;
    o["a"] = 2;
    o["m"] = 3;
    JEQ(json::stringify(o), R"({"z":1,"a":2,"m":3})");
    o["a"] = "x";                                              // replaced in place
    JEQ(json::stringify(o), R"({"z":1,"a":"x","m":3})");
    o.set("m", JVal::array({1}));
    JEQ(json::stringify(o), R"({"z":1,"a":"x","m":[1]})");
    QVERIFY(o.erase("z"));
    QVERIFY(!o.erase("z"));
    o["z"] = true;                                             // a deleted key comes back at the end (JS delete)
    JEQ(json::stringify(o), R"({"a":"x","m":[1],"z":true})");
    JEQ(json::stringify(JVal(JObject{{"a", 1}, {"b", 2}, {"a", 3}})), R"({"a":3,"b":2})");   // JS literal rule
    JObject direct;
    direct.append("k", 1);
    direct.append("j", 2);
    JEQ(json::stringify(JVal(direct)), R"({"k":1,"j":2})");
    JVal r;
    r["x"]["y"] = 1;                                           // null turns into an object / array on demand
    r["x"]["z"].push(2);
    r["x"]["z"].push("s");
    JEQ(json::stringify(r), R"({"x":{"y":1,"z":[2,"s"]}})");
}

void TestAnfsCoreJson::numberToStringVectors()
{
    // {bits, String(x)} from Node 22
    const struct { uint64_t bits; const char* js; } v[] = {
        {0x3FB999999999999Aull, "0.1"}, {0x3FD3333333333334ull, "0.30000000000000004"},
        {0x444B1AE4D6E2EF50ull, "1e+21"}, {0x3E7AD7F29ABCAF48ull, "1e-7"}, {0x3C36B082C2148B8Eull, "1.23e-18"},
        {0x0000000000000001ull, "5e-324"}, {0x7FEFFFFFFFFFFFFFull, "1.7976931348623157e+308"},
        {0x4340000000000000ull, "9007199254740992"}, {0x4340000000000001ull, "9007199254740994"},
        {0x3FF8000000000000ull, "1.5"}, {0x4059000000000000ull, "100"}, {0x4415AF1D78B58C40ull, "100000000000000000000"},
        {0x441AC53A7E04BCDAull, "123456789012345680000"}, {0x3EB0C6F7A0B5ED8Dull, "0.000001"},
        {0x3EB4B6231ABFD271ull, "0.0000012345"}, {0x4011666666666666ull, "4.35"}, {0x3FD5555555555555ull, "0.3333333333333333"},
        {0xBE7AD7F29ABCAF48ull, "-1e-7"}, {0x444B1AE4D6E2EF4Full, "999999999999999900000"},
        {0x430C6BF526340000ull, "1000000000000000"}, {0x4341C37937E08000ull, "10000000000000000"},
        {0x4376345785D8A000ull, "100000000000000000"}, {0x3EFA36E2EB1C432Dull, "0.000025"},
        {0xC05EDD2F1A9FBE77ull, "-123.456"}, {0x0000000000000003ull, "1.5e-323"},
        {0x0010000000000000ull, "2.2250738585072014e-308"}, {0x54B249AD2594C37Dull, "1e+100"},
        {0x3EB421F5F40D8376ull, "0.0000012"}, {0x4450AB6B74CB909Eull, "1.23e+21"}, {0x3FE0000000000000ull, "0.5"},
        {0x40934A456D5CFAADull, "1234.5678"}, {0x43E0000000000000ull, "9223372036854776000"},
        {0xC3E0000000000000ull, "-9223372036854776000"}, {0x43F0000000000000ull, "18446744073709552000"},
        {0x400921F9F01B866Eull, "3.14159"}, {0x3EB0000000000000ull, "9.5367431640625e-7"},
        {0x41B1DE784A000000ull, "299792458"}, {0x44DFE185CA57C517ull, "6.02214076e+23"},
        {0x3FF0000000000001ull, "1.0000000000000002"},
    };
    for (const auto& t : v) {
        const double d = fromBits(t.bits);
        JEQ(json::numberToString(d), t.js);
        JEQ(json::stringify(JVal(d)), t.js);
    }
    JEQ(json::numberToString(std::nan("")), "NaN");
    JEQ(json::numberToString(std::numeric_limits<double>::infinity()), "Infinity");
    JEQ(json::numberToString(-std::numeric_limits<double>::infinity()), "-Infinity");
    JEQ(json::numberToString(-0.0), "0");
    JEQ(json::numberToString(0.0), "0");
}

void TestAnfsCoreJson::stringifyPrettyForm()
{
    const JVal v = JVal::object({{"b", 1}, {"a", JVal::array({true, nullptr, 1.5, "x"})}, {"c", JVal::object()},
                                 {"d", JVal::array()}});
    // Node: JSON.stringify(v, null, 2)
    JEQ(json::stringifyPretty(v), "{\n  \"b\": 1,\n  \"a\": [\n    true,\n    null,\n    1.5,\n    \"x\"\n  ],\n"
                                  "  \"c\": {},\n  \"d\": []\n}");
    JEQ(json::stringifyPretty(v, 0), json::stringify(v));
    JEQ(json::stringifyPretty(JVal::array({1}), 20), "[\n          1\n]");          // the space is capped at 10
    const std::string sWant = R"("s\n")";
    JEQ(json::stringifyPretty(JVal("s\n")), sWant);
    JEQ(json::stringifyPretty(JVal::object({{"k", JVal::object({{"n", JVal::array({JVal::object()})}})}}), 1),
        "{\n \"k\": {\n  \"n\": [\n   {}\n  ]\n }\n}");
}

// ---- parser ---------------------------------------------------------------------------------------------------
void TestAnfsCoreJson::parseRoundTripAndTypes()
{
    const std::string canon = "{\"b\":1,\"a\":[true,null,1.5,\"x\",-2,\"\\u0000\\\"\\\\\"],\"c\":{},\"d\":[],"
                              "\"e\":\"\xC3\xA9\xF0\x9F\x98\x80\",\"f\":{\"g\":[[],{}]}}";
    JEQ(json::stringify(json::parse(canon)), canon);
    JEQ(json::stringify(json::parse(" \t\r\n{ \"a\" : [ 1 , 2 ] ,\"b\":{ } } \n")), R"({"a":[1,2],"b":{}})");
    JEQ(json::stringify(json::parse("\xEF\xBB\xBF[1]")), "[1]");                      // leading BOM skipped

    QVERIFY(json::parse("1").isInt());
    QVERIFY(json::parse("1.0").isDouble());
    JEQ(json::stringify(json::parse("1.0")), "1");
    JEQ(json::stringify(json::parse("1e2")), "100");
    JEQ(json::stringify(json::parse("-1.5E-3")), "-0.0015");
    QVERIFY(json::parse("-0").isInt());
    QCOMPARE(json::parse("-0").asInt(), int64_t(0));
    QCOMPARE(json::parse("9223372036854775807").asInt(), std::numeric_limits<int64_t>::max());
    QCOMPARE(json::parse("-9223372036854775808").asInt(), std::numeric_limits<int64_t>::min());
    QVERIFY(json::parse("9223372036854775808").isDouble());
    JEQ(json::stringify(json::parse("9223372036854775808")), "9223372036854776000");   // Node: same bytes
    QCOMPARE(toBits(json::parse("0.1").asDouble()), 0x3FB999999999999Aull);
    QCOMPARE(toBits(json::parse("5e-324").asDouble()), 0x0000000000000001ull);
    const JVal big = json::parse("1e400");
    QVERIFY(big.isDouble() && std::isinf(big.asDouble()) && big.asDouble() > 0);
    JEQ(json::stringify(big), "null");                                                 // Node: null
    QVERIFY(std::isinf(json::parse("-123.4e99999999999").asDouble()));
    const JVal tiny = json::parse("-1e-400");
    QVERIFY(tiny.isDouble() && tiny.asDouble() == 0 && std::signbit(tiny.asDouble()));
    JEQ(json::stringify(tiny), "0");                                                   // Node: 0
    QCOMPARE(json::parse("0.0000e-99999").asDouble(), 0.0);
    QVERIFY(json::parse("true").asBool() && !json::parse("false").asBool(true) && json::parse("null").isNull());

    // escapes: pairs (any hex case) join, a lone half stays one; Node gives "😀😀/\ud800A"
    const JVal s = json::parse(R"("\u00e9\ud83d\ude00\uD83D\uDE00\/\ud800\u0041")");
    JEQ(s.asString(), "\xC3\xA9\xF0\x9F\x98\x80\xF0\x9F\x98\x80/\xED\xA0\x80" "A");
    JEQ(json::stringify(s), "\"\xC3\xA9\xF0\x9F\x98\x80\xF0\x9F\x98\x80/\\ud800A\"");
    const std::string lone1 = R"("\ud800\u0041")", lone2 = R"("\udc00\ud800")";
    const std::string simple = R"("\b\f\n\r\t\"\\")";
    JEQ(json::parse(lone1).asString(), "\xED\xA0\x80" "A");
    JEQ(json::parse(lone2).asString(), "\xED\xB0\x80\xED\xA0\x80");
    JEQ(json::parse(simple).asString(), "\b\f\n\r\t\"\\");
    // raw bytes are UTF-8 decoded with replacement (Node's decoder), WTF-8 surrogate bytes included
    JEQ(json::parse("\"\xFF\"").asString(), "\xEF\xBF\xBD");
    JEQ(json::parse("\"a\xED\xA0\x80z\"").asString(), "a\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBDz");
    JEQ(json::parse("\"\xED\x9F\xBF\x7F\"").asString(), "\xED\x9F\xBF\x7F");
}

void TestAnfsCoreJson::parseDuplicateKeys()
{
    // JSON.parse: the first position, the last value
    JEQ(json::stringify(json::parse(R"({"a":1,"b":2,"a":3})")), R"({"a":3,"b":2})");
    // past the linear-scan size the parser uses its hash index
    std::string doc = "{";
    for (int i = 0; i < 20; ++i) doc += (i ? ",\"k" : "\"k") + std::to_string(i) + "\":" + std::to_string(i);
    doc += R"(,"k15":"dup","k3":"dup","k19":[],"new":1,"k3":"again"})";
    const JVal v = json::parse(doc);
    QCOMPARE(v.size(), size_t(21));
    QVERIFY(v.obj().members()[3].first == "k3" && v.get("k3").asString() == "again");
    QVERIFY(v.obj().members()[15].first == "k15" && v.get("k15").asString() == "dup");
    QVERIFY(v.get("k19").isArray() && v.obj().members()[19].first == "k19");
    QVERIFY(v.obj().members().back().first == "new");
    QCOMPARE(v.get("k7").asInt(), int64_t(7));
}

void TestAnfsCoreJson::parseRejects()
{
    const struct { std::string text; long pos; } bad[] = {
        {"[1,]", 3}, {"{\"a\":1,}", 7}, {"01", 1}, {"1.", 2}, {"-", 1}, {"\"\\x\"", 2}, {"\"\\u12G4\"", 5},
        {"[1 2]", 3}, {"1 2", 2}, {"\"\\\"", 3}, {"", 0}, {" ", 1}, {"{", 1}, {"}", 0}, {"[", 1}, {"]", 0},
        {"{'a':1}", 1}, {"{a:1}", 1}, {".5", 0}, {"+1", 0}, {"1e", 2}, {"1e+", 3}, {"NaN", 0}, {"Infinity", 0},
        {"-Infinity", 1}, {"nul", 3}, {"tru", 3}, {"falsy", 4}, {"\"abc", 4}, {"\"a\nb\"", 2}, {"\"a\tb\"", 2},
        {"/*c*/1", 0}, {"// c\n1", 0}, {"[1]x", 3}, {std::string("\0", 1), 0}, {"{\"a\":1}}", 7},
        {"\xC2\xA0[]", 0}, {"\xEF\xBB\xBF", 3}, {"[1,,2]", 3}, {"{\"a\"}", 4}, {"{\"a\":}", 5}, {"{,}", 1},
        {"[-]", 2}, {"-01", 2}, {"1.e5", 2}, {"0x10", 1}, {"\"\\u00\"", 5}, {"\"\x01\"", 1}, {"\"\\'\"", 2},
        {"[\"a\" \"b\"]", 5}, {"{\"a\":1 \"b\":2}", 7}, {"\"\\uD83D\\u\"", 9},
    };
    for (const auto& b : bad) {
        bool threw = false;
        try {
            json::parse(b.text);
        } catch (const JsonError& e) {
            threw = true;
            QVERIFY2(std::strlen(e.what()) > 0, b.text.c_str());
            QVERIFY2(static_cast<long>(e.pos) == b.pos,
                     qPrintable(QStringLiteral("%1: pos %2, want %3 (%4)").arg(QString::fromStdString(b.text))
                                    .arg(e.pos).arg(b.pos).arg(QString::fromUtf8(e.what()))));
        }
        QVERIFY2(threw, b.text.c_str());
        JVal out = 7;
        std::string err;
        QVERIFY(!json::tryParse(b.text, out, &err));
        QVERIFY(!err.empty());
        QCOMPARE(out.asInt(), int64_t(7));                   // untouched on failure
    }
    JVal ok;
    QVERIFY(json::tryParse("[1]", ok, nullptr) && ok.size() == 1);
    // V8's text for trailing data (Node 22 adds " (line 1 column 4)"); "in JSON" is not doubled
    std::string err;
    QVERIFY(!json::tryParse("[1]x", ok, &err));
    JEQ(err, "Unexpected non-whitespace character after JSON at position 3");
    QVERIFY(!json::tryParse("[1,]", ok, &err));
    JEQ(err, "Unexpected token ] in JSON at position 3");
}

void TestAnfsCoreJson::parseNestingCap()
{
    const std::string deep512 = std::string(512, '[') + std::string(512, ']');
    QCOMPARE(json::parse(deep512).size(), size_t(1));
    const std::string deep513 = std::string(513, '[') + std::string(513, ']');
    QVERIFY_THROWS_EXCEPTION(JsonError, json::parse(deep513));
    std::string objs;
    for (int i = 0; i < 513; ++i) objs += "{\"a\":";
    objs += "1" + std::string(513, '}');
    QVERIFY_THROWS_EXCEPTION(JsonError, json::parse(objs));
    // refused at the 513th '[' (position 512); the parser keeps open containers on the heap, so this costs no stack
    try {
        json::parse(std::string(1000000, '['));
        QFAIL("a 1 MB run of [ was accepted");
    } catch (const JsonError& e) {
        QCOMPARE(e.pos, size_t(512));
    }
    // errors inside and after a deep value keep V8's positions
    const std::string deepThenBad = "[" + std::string(510, '[') + "1" + std::string(510, ']') + ",]";
    try {
        json::parse(deepThenBad);
        QFAIL("a trailing comma after a deep element was accepted");
    } catch (const JsonError& e) {
        QCOMPARE(e.pos, deepThenBad.size() - 1);
    }
}

// The HTTP workers reserve 256 KB of stack (core/http_server.h ServerOptions::threadStackBytes) and a stack overflow
// kills anfs.exe, so the deepest document the parser accepts (and the first one it refuses) must be parsed, copied,
// compared, written and destroyed well inside that, below the server's and the handler's own frames. Each step runs
// on a fresh thread reserving 192 KB and must touch at most 96 KB of it.
void TestAnfsCoreJson::deepValuesFitWorkerStack()
{
#if !defined(_WIN32)
    QSKIP("the stack probe uses Win32 threads");
#else
    constexpr size_t kReserve = 192 * 1024, kBudget = 96 * 1024;
    auto objs = [](int n) {
        std::string s;
        for (int i = 0; i < n; ++i) s += "{\"a\":";
        return s + "1" + std::string(static_cast<size_t>(n), '}');
    };
    auto arrs = [](int n) { return std::string(static_cast<size_t>(n), '[') + std::string(static_cast<size_t>(n), ']'); };
    // {"x":"s","k":[true,{"x":"s","k":[ ... ],"y":[1,2.5,null]},{}],"y":[1,2.5,null]}: n containers deep, with
    // container siblings at every level but the innermost
    auto mixed = [](int n) {
        std::string open, close;
        for (int i = 0; i < n; ++i) {
            const bool last = i == n - 1;
            if (i % 2 == 0) { open += "{\"x\":\"s\",\"k\":"; close.insert(0, last ? ",\"y\":0}" : ",\"y\":[1,2.5,null]}"); }
            else { open += "[true,"; close.insert(0, last ? ",0]" : ",{}]"); }
        }
        return open + "\"leaf\"" + close;
    };
    QStringList report, failures;
    auto step = [&](const std::string& name, const std::function<void()>& fn) {
        const StackProbe p = probeStack(kReserve, fn);
        const QString n = QString::fromStdString(name);
        report << QStringLiteral("%1 %2").arg(n).arg(p.overflow ? QStringLiteral("OVERFLOW") : QString::number(p.peak / 1024));
        if (p.overflow) failures << QStringLiteral("%1: stack overflow in %2 KB").arg(n).arg(kReserve / 1024);
        else if (!p.error.empty()) failures << QStringLiteral("%1: %2").arg(n, QString::fromStdString(p.error));
        else if (p.peak == 0 || p.peak > kBudget) failures << QStringLiteral("%1 touched %2 KB of stack").arg(n).arg(p.peak / 1024);
    };

    const struct { const char* name; std::string text; } accepted[] = {
        {"objects", objs(512)}, {"arrays", arrs(512)}, {"mixed", mixed(512)},
    };
    for (const auto& d : accepted) {
        const JVal v = json::parse(d.text);                     // on the test thread: the inputs of the steps below
        const JVal w = json::parse(d.text);
        QVERIFY2(json::stringify(v) == d.text, d.name);
        const std::string tag = d.name;
        step(tag + " parse+destroy", [&] { JVal x = json::parseBody(d.text); });
        step(tag + " copy+destroy", [&] { JVal x = v; if (x.size() != v.size()) throw std::logic_error("copy"); });
        step(tag + " ==", [&] { if (!(v == w)) throw std::logic_error("not equal"); });
        step(tag + " stringify", [&] { if (json::stringify(v) != d.text) throw std::logic_error("stringify differs"); });
        step(tag + " pretty", [&] { if (json::stringifyPretty(v).size() <= d.text.size()) throw std::logic_error("pretty"); });
        step(tag + " Writer", [&] {
            StringSink sink;
            json::Writer wr(sink, 4096);
            wr.value(v);
            wr.finish();
            if (sink.data != d.text) throw std::logic_error("Writer differs");
        });
    }

    // refused documents, including errors that unwind past a completed deep value
    const struct { const char* name; std::string text; } refused[] = {
        {"objects 513", objs(513)}, {"arrays 513", arrs(513)}, {"mixed 513", mixed(513)},
        {"1 MB of [", std::string(1000000, '[')}, {"1 MB of {\"a\":", [] { std::string s; for (int i = 0; i < 200000; ++i) s += "{\"a\":"; return s; }()},
        {"deep element, bad tail", "[" + arrs(511) + ",]"}, {"deep member, bad tail", "{\"k\":" + objs(511) + ",}"},
        {"deep value, trailing data", mixed(512) + " x"},
    };
    for (const auto& d : refused) {
        step(d.name, [&] {
            try {
                json::parseBody(d.text);
            } catch (const JsonError&) {
                return;
            }
            throw std::logic_error("accepted");
        });
    }
    qInfo("KB of stack touched per step (reserve %d KB, budget %d KB): %s", int(kReserve / 1024), int(kBudget / 1024),
          qPrintable(report.join(QStringLiteral(", "))));
    QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QStringLiteral("; "))));
#endif
}

void TestAnfsCoreJson::parseLargeObjectIsLinear()
{
    std::string big = "{";
    for (int i = 0; i < 100000; ++i) big += (i ? ",\"k" : "\"k") + std::to_string(i) + "\":" + std::to_string(i);
    big += ",\"k5\":\"dup\",\"k99999\":-1}";
    QElapsedTimer t;
    t.start();
    const JVal v = json::parse(big);
    const qint64 ms = t.elapsed();
    QCOMPARE(v.size(), size_t(100000));
    QVERIFY(v.obj().members()[5].first == "k5" && v.get("k5").asString() == "dup");
    QVERIFY(v.obj().members().back().first == "k99999" && v.get("k99999").asInt() == -1);
    QVERIFY2(ms < 5000, qPrintable(QStringLiteral("100,000 keys took %1 ms").arg(ms)));
    qInfo("100,000-key object parsed in %lld ms", static_cast<long long>(ms));
}

// ---- Express bodies -------------------------------------------------------------------------------------------
void TestAnfsCoreJson::expressBodyRules()
{
    QVERIFY(json::kInvalidJsonBody == "Invalid JSON body.");
    JEQ(json::errorBody(json::kInvalidJsonBody), R"({"error":"Invalid JSON body."})");
    JEQ(json::errorBody("bad \"x\" \xC3\xA9\n"), "{\"error\":\"bad \\\"x\\\" \xC3\xA9\\n\"}");   // C# FsCore vector

    QVERIFY(json::isJsonContentType("application/json"));
    QVERIFY(json::isJsonContentType("application/json; charset=utf-8"));
    QVERIFY(json::isJsonContentType("Application/JSON ; charset=UTF-8"));
    QVERIFY(json::isJsonContentType(" application/json\t"));
    QVERIFY(!json::isJsonContentType(""));
    QVERIFY(!json::isJsonContentType("text/plain"));
    QVERIFY(!json::isJsonContentType("text/json"));
    QVERIFY(!json::isJsonContentType("application/vnd.api+json"));
    QVERIFY(!json::isJsonContentType("application/json-patch+json"));
    QVERIFY(!json::isJsonContentType("application/jsonx"));
    QVERIFY(!json::isJsonContentType("multipart/form-data; boundary=x"));

    QVERIFY(json::parseBody("").isObject() && json::parseBody("").size() == 0);
    QVERIFY(json::parseBody("\xEF\xBB\xBF").isObject());       // a BOM-only body decodes to ""
    JEQ(json::stringify(json::parseBody(R"({"a":"v","b":2})")), R"({"a":"v","b":2})");
    JEQ(json::stringify(json::parseBody(" \r\n\t{\"a\":1}")), R"({"a":1})");
    JEQ(json::stringify(json::parseBody("[1,2]")), "[1,2]");  // Express keeps arrays (C# gave {})
    JEQ(json::stringify(json::parseBody("\xEF\xBB\xBF {}")), "{}");
    for (const char* b : {"{bad", "\"str\"", "123", "true", "null", "  ", "\r\n", "\xEF\xBB\xBF  ", "{\"a\":1} x",
                          "\xC2\xA0{}", "[1,]", "{\"a\":1,}"})
        QVERIFY_THROWS_EXCEPTION(JsonError, json::parseBody(b));

    // The C# FsCore_Body vectors, answered the way Ctx::readJson does
#define BODY_EQ(ct, body, status, text)                                                                          \
do {                                                                                                         \
    const auto r_ = answerBody(ct, body);                                                                    \
    QCOMPARE(r_.first, status);                                                                              \
    JEQ(r_.second, text);                                                                                    \
} while (0)
    const char* invalid = R"({"error":"Invalid JSON body."})";
    BODY_EQ("application/json", R"({"a":"v","b":2})", 200, R"({"a":"v","b":2})");
    BODY_EQ("application/json", "", 200, "{}");
    BODY_EQ("text/plain", R"({"a":"v"})", 200, "{}");
    BODY_EQ("", R"({"a":"v"})", 200, "{}");
    BODY_EQ("application/json", "{bad", 400, invalid);
    BODY_EQ("application/json", "\"str\"", 400, invalid);
    BODY_EQ("application/json; charset=utf-8", "[1,2]", 200, "[1,2]");
    BODY_EQ("text/plain", "{bad", 200, "{}");
#undef BODY_EQ
}

// ---- dates ----------------------------------------------------------------------------------------------------
void TestAnfsCoreJson::isoDates()
{
    // {ms, new Date(ms).toISOString()} from Node
    const struct { int64_t ms; const char* iso; } v[] = {
        {0LL, "1970-01-01T00:00:00.000Z"}, {-1LL, "1969-12-31T23:59:59.999Z"},
        {1792000678770LL, "2026-10-14T17:57:58.770Z"}, {253402300799999LL, "9999-12-31T23:59:59.999Z"},
        {253402300800000LL, "+010000-01-01T00:00:00.000Z"}, {-62167219200000LL, "0000-01-01T00:00:00.000Z"},
        {-62167219200001LL, "-000001-12-31T23:59:59.999Z"}, {8640000000000000LL, "+275760-09-13T00:00:00.000Z"},
        {-8640000000000000LL, "-271821-04-20T00:00:00.000Z"}, {951782400000LL, "2000-02-29T00:00:00.000Z"},
        {1709251199999LL, "2024-02-29T23:59:59.999Z"},
    };
    for (const auto& t : v) {
        JEQ(json::isoMs(t.ms), t.iso);
        QCOMPARE(json::parseIsoMs(t.iso), t.ms);
    }
    JEQ(json::isoMs(8640000000000001LL), "");                 // JS: an invalid Date
    JEQ(json::isoMs(-8640000000000001LL), "");

    // {text, Date.parse(text)} from Node
    const struct { const char* text; int64_t ms; } p[] = {
        {"2026-10-14T13:17:58.770Z", 1791983878770LL}, {"2026-10-14T13:17:58Z", 1791983878000LL},
        {"2026-10-14T15:17:58.770+02:00", 1791983878770LL}, {"2026-10-14T08:47:58.7-04:30", 1791983878700LL},
        {"2026-10-14", 1791936000000LL}, {"+010000-01-01T00:00:00.000Z", 253402300800000LL},
        {"-000001-12-31T23:59:59.999Z", -62167219200001LL}, {"2026-10-14T24:00:00Z", 1792022400000LL},
        {"2026-10", 1790812800000LL}, {"2026", 1767225600000LL}, {"2026-10-14T13:17Z", 1791983820000LL},
        {"2026-10-14T13:17:58.123456Z", 1791983878123LL}, {"2026-02-29T00:00:00Z", 1772323200000LL},
        {"2026-10-14T13:17:58+0200", 1791976678000LL}, {"2026-04-31", 1777593600000LL},
    };
    for (const auto& t : p) QVERIFY2(json::parseIsoMs(t.text) == t.ms, t.text);

    for (const char* bad : {"", "garbage", "2026-13-01T00:00:00Z", "2026-02-32T00:00:00Z", "2026-10-14T25:00:00Z",
                            "2026-10-14T24:00:01Z", "2026-10-14T13:17:58.Z", "2026-10-14T13Z", "-000000-01-01T00:00:00Z",
                            "2026-00-10", "2026-10-14T13:60:00Z", "2026-10-14T13:17:58Zx",
                            "+275760-09-13T00:00:00.001Z"})
        QVERIFY2(json::parseIsoMs(bad) == -1, bad);

    // a time without an offset is local time, as in Date.parse
    const qint64 local = QDateTime(QDate(2026, 10, 14), QTime(13, 17, 58)).toMSecsSinceEpoch();
    QCOMPARE(json::parseIsoMs("2026-10-14T13:17:58.250"), int64_t(local + 250));

    QRandomGenerator rng(7);
    for (int i = 0; i < 5000; ++i) {
        const int64_t ms = static_cast<int64_t>(rng.generate64() % 17280000000000001ull) - 8640000000000000LL;
        QCOMPARE(json::parseIsoMs(json::isoMs(ms)), ms);
    }
}

// ---- accessors ------------------------------------------------------------------------------------------------
void TestAnfsCoreJson::jvalAccessors()
{
    QCOMPARE(JVal(3.0).asInt(-1), int64_t(3));
    QCOMPARE(JVal(3.5).asInt(-1), int64_t(-1));
    QCOMPARE(JVal(9.3e18).asInt(-1), int64_t(-1));
    QCOMPARE(JVal(std::nan("")).asInt(-1), int64_t(-1));
    QCOMPARE(JVal("3").asInt(-1), int64_t(-1));
    QCOMPARE(JVal(true).asInt(7), int64_t(7));
    QCOMPARE(JVal(2).asDouble(), 2.0);
    QCOMPARE(JVal("x").asDouble(1.5), 1.5);
    QVERIFY(JVal(true).asBool() && !JVal(1).asBool(false) && JVal(1).asBool(true));
    QVERIFY(JVal(5).asString().empty());
    QVERIFY(JVal("x").asString() == "x");
    QVERIFY(JVal(5).str("d") == "d" && JVal("s").str("d") == "s");

    const JVal falsy[] = {JVal(), JVal(false), JVal(0), JVal(0.0), JVal(-0.0), JVal(std::nan("")), JVal("")};
    for (const JVal& f : falsy) QVERIFY2(!f.truthy(), json::stringify(f).c_str());
    const JVal truthy[] = {JVal(true), JVal(1), JVal(-1), JVal(0.5), JVal("0"), JVal(" "), JVal::array(), JVal::object()};
    for (const JVal& t : truthy) QVERIFY2(t.truthy(), json::stringify(t).c_str());

    JVal n;
    n.push(1);
    n.push("a");
    QVERIFY(n.isArray() && n.size() == 2 && n.at(1).asString() == "a" && n.at(5).isNull());
    n.arr().push_back(JVal::object());
    JEQ(json::stringify(n), R"([1,"a",{}])");

    JVal s("x");
    QVERIFY_THROWS_EXCEPTION(std::logic_error, s.push(1));
    QVERIFY_THROWS_EXCEPTION(std::logic_error, s["k"]);
    QVERIFY_THROWS_EXCEPTION(std::logic_error, s.set("k", 1));
    QVERIFY_THROWS_EXCEPTION(std::logic_error, JVal::array()["k"]);
    QVERIFY_THROWS_EXCEPTION(std::logic_error, JVal::object().push(1));
    const JVal& cs = s;
    QVERIFY(cs.arr().empty() && cs.obj().empty() && cs.get("k").isNull() && !cs.find("k") && !s.erase("k"));
    QVERIFY(cs.size() == 0 && cs.at(0).isNull());
    QVERIFY(json::stringify(s) == "\"x\"");                    // unchanged by the failed calls

    const JVal o = json::parse(R"({"s":"x","i":5,"d":1.5,"b":true,"n":null,"a":[1],"big":1e21,"o":{"k":1},)"
                               R"("neg":-7.9,"pad":" -42 ","plus":"+8","junk":"4x","pm":"+-1","huge":1e300,)"
                               R"("st":"TRUE","sf":"False","s1":"1"})");
    QVERIFY(o.getStr("s") == "x" && o.getStr("i") == "5" && o.getStr("d") == "1.5" && o.getStr("b") == "true");
    QVERIFY(o.getStr("n", "def") == "def" && o.getStr("a", "def") == "def" && o.getStr("missing", "def") == "def");
    QVERIFY(o.getStr("big") == "1e+21");
    QCOMPARE(o.getInt("i"), int64_t(5));
    QCOMPARE(o.getInt("neg"), int64_t(-7));
    QCOMPARE(o.getInt("pad"), int64_t(-42));
    QCOMPARE(o.getInt("plus"), int64_t(8));
    QCOMPARE(o.getInt("junk", 99), int64_t(99));
    QCOMPARE(o.getInt("pm", 99), int64_t(99));
    QCOMPARE(o.getInt("huge", 99), int64_t(99));
    QCOMPARE(o.getInt("b", 99), int64_t(99));
    QCOMPARE(o.getInt("missing", 99), int64_t(99));
    QVERIFY(o.getBool("b") && o.getBool("st") && !o.getBool("sf", true));
    QVERIFY(o.getBool("s1", true) && !o.getBool("s1", false) && !o.getBool("i", false));
    QVERIFY(o.getObj("o") && o.getObj("o")->getInt("k") == 1 && !o.getObj("a") && !o.getObj("missing"));
    QVERIFY(o.getArr("a") && o.getArr("a")->size() == 1 && !o.getArr("o"));
    QVERIFY(o.has("n") && !o.has("nope") && o.find("i")->isInt());

    QVERIFY(JVal(1) != JVal(1.0));                              // Int and Double differ
    QVERIFY(JVal(1) == JVal(int64_t(1)));
    QVERIFY(json::parse(R"({"a":1,"b":2})") != json::parse(R"({"b":2,"a":1})"));   // order matters
    QVERIFY(JVal::object({{"a", JVal::array({1, "x"})}}) == json::parse(R"({"a":[1,"x"]})"));
    JVal copy = o;
    QVERIFY(copy == o);
    copy["o"]["k"] = 2;
    QVERIFY(copy != o && o.getObj("o")->getInt("k") == 1);    // deep copy
}

// ---- streaming writer -----------------------------------------------------------------------------------------
void TestAnfsCoreJson::writerMatchesStringify()
{
    JVal big = JVal::array();
    for (int i = 0; i < 2000; ++i)
        big.push(JVal::object({{"id", i}, {"name", "file \"" + std::to_string(i) + "\"\n\xC3\xA9"},
                               {"size", 1.5 * i}, {"tags", JVal::array({"a", nullptr, false})}, {"o", JVal::object()}}));
    const std::string expect = json::stringify(big);
    ChunkSink sink;
    json::Writer w(sink, 1024);
    w.value(big);
    w.finish();
    JEQ(sink.data, expect);
    QCOMPARE(w.bytesWritten(), static_cast<int64_t>(expect.size()));
    QCOMPARE(sink.flushes, 1);
    QVERIFY(sink.chunks.size() > 50);
    for (size_t i = 0; i + 1 < sink.chunks.size(); ++i)   // pieces of about flushBytes, never tiny
        QVERIFY2(sink.chunks[i] >= 1024 && sink.chunks[i] < 1024 + 256, qPrintable(QString::number(sink.chunks[i])));

    ChunkSink s2;
    json::Writer w2(s2);
    w2.beginObject().key("a").integer(1).key("b").beginArray().number(1.5).string("x\n").null().boolean(false)
        .raw(R"({"r":1})").number(std::nan("")).integer(std::numeric_limits<int64_t>::min()).end()
        .key("c").value(JVal::object({{"d", JVal::array()}})).key("k\"").string("\xED\xA0\x80").end();
    QVERIFY(s2.data.empty());                                  // buffered until finish() or a full buffer
    w2.finish();
    const std::string s2Want =
        R"({"a":1,"b":[1.5,"x\n",null,false,{"r":1},null,-9223372036854775808],"c":{"d":[]},"k\"":"\ud800"})";
    JEQ(s2.data, s2Want);

    ChunkSink s3;
    { json::Writer w3(s3); w3.beginArray().integer(1).end(); }   // destroyed without finish(): nothing written
    QVERIFY(s3.data.empty());
    ChunkSink s4;
    json::Writer w4(s4);
    w4.string("top");
    w4.finish();
    JEQ(s4.data, "\"top\"");
}

void TestAnfsCoreJson::writerRejectsMisuse()
{
    ChunkSink s;
    { json::Writer w(s); w.beginObject(); QVERIFY_THROWS_EXCEPTION(std::logic_error, w.integer(1)); }
    { json::Writer w(s); w.beginArray(); QVERIFY_THROWS_EXCEPTION(std::logic_error, w.key("x")); }
    { json::Writer w(s); QVERIFY_THROWS_EXCEPTION(std::logic_error, w.key("x")); }
    { json::Writer w(s); QVERIFY_THROWS_EXCEPTION(std::logic_error, w.end()); }
    { json::Writer w(s); w.integer(1); QVERIFY_THROWS_EXCEPTION(std::logic_error, w.integer(2)); }
    { json::Writer w(s); w.beginObject(); QVERIFY_THROWS_EXCEPTION(std::logic_error, w.finish()); }
    { json::Writer w(s); w.beginObject().key("a"); QVERIFY_THROWS_EXCEPTION(std::logic_error, w.end()); }
    { json::Writer w(s); w.beginObject().key("a"); QVERIFY_THROWS_EXCEPTION(std::logic_error, w.key("b")); }
    { json::Writer w(s); w.beginObject().key("a"); QVERIFY_THROWS_EXCEPTION(std::logic_error, w.finish()); }
    { json::Writer w(s); QVERIFY_THROWS_EXCEPTION(std::logic_error, w.finish()); }
    { json::Writer w(s); w.beginArray().end(); QVERIFY_THROWS_EXCEPTION(std::logic_error, w.beginArray()); }
    QVERIFY(s.data.empty());
}

// ---- live oracle ----------------------------------------------------------------------------------------------
void TestAnfsCoreJson::nodeDifferential()
{
    const QString node = findNode();
    if (node.isEmpty()) QSKIP("node.exe not found: the literal Node vectors above still ran");
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString script = tmp.filePath(QStringLiteral("oracle.js"));
    {
        QFile f(script);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(oracleJs());
    }
    auto run = [&](const QString& mode, const QString& in) -> QList<QByteArray> {
        const QString out = tmp.filePath(mode + QStringLiteral(".out"));
        QProcess p;
        p.start(node, {script, mode, in, out});
        if (!p.waitForFinished(60000)) { p.kill(); p.waitForFinished(5000); return {}; }
        if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
            qWarning("node %s failed: %s", qPrintable(mode), p.readAllStandardError().constData());
            return {};
        }
        QFile f(out);
        if (!f.open(QIODevice::ReadOnly)) return {};
        QList<QByteArray> lines = f.readAll().split('\n');
        while (!lines.isEmpty() && lines.back().isEmpty()) lines.pop_back();
        return lines;
    };

    // 1. doubles: random bit patterns, decimal-looking values, subnormals and the format boundaries
    std::vector<uint64_t> bits;
    QRandomGenerator rng(20261026);
    for (int i = 0; i < 6000; ++i) bits.push_back(rng.generate64());
    for (int i = 0; i < 3000; ++i) {
        const double m = static_cast<double>(rng.bounded(1, 1000000000));
        const double p10 = std::pow(10.0, rng.bounded(0, 26));
        bits.push_back(toBits(rng.bounded(2) ? m / p10 : m * p10));
    }
    for (int i = 0; i < 1000; ++i) bits.push_back(rng.generate64() & 0x800FFFFFFFFFFFFFull);
    for (int k = -12; k <= 25; ++k) {
        const double p = std::pow(10.0, k);
        for (double d : {p, std::nextafter(p, 0.0), std::nextafter(p, 1e300), -p, p * 1.5, p * 9.999999999999999})
            bits.push_back(toBits(d));
    }
    for (int i = -40; i <= 40; ++i) {
        bits.push_back(toBits(9007199254740992.0 + 2.0 * i));
        bits.push_back(toBits(std::ldexp(1.0, 63) + 2048.0 * i));
    }
    for (double d : {std::nan(""), std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                     -0.0, 0.0, std::numeric_limits<double>::max(), std::numeric_limits<double>::min(),
                     std::numeric_limits<double>::denorm_min(), 1e21, 1e-7})
        bits.push_back(toBits(d));
    {
        QFile f(tmp.filePath(QStringLiteral("numbers.in")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        for (uint64_t b : bits) f.write(QByteArray::number(static_cast<qulonglong>(b), 16).rightJustified(16, '0') + '\n');
    }
    const QList<QByteArray> nums = run(QStringLiteral("numbers"), tmp.filePath(QStringLiteral("numbers.in")));
    QCOMPARE(nums.size(), static_cast<qsizetype>(bits.size()));
    int bad = 0;
    for (size_t i = 0; i < bits.size(); ++i) {
        const QList<QByteArray> cols = nums[static_cast<qsizetype>(i)].split('\t');
        QCOMPARE(cols.size(), qsizetype(2));
        const double d = fromBits(bits[i]);
        const QByteArray want = QByteArray::fromHex(cols[0]), wantJson = QByteArray::fromHex(cols[1]);
        if (qb(json::numberToString(d)) != want || qb(json::stringify(JVal(d))) != wantJson) {
            if (++bad <= 10)
                qWarning("double %016llx: got %s / %s, node %s / %s", static_cast<unsigned long long>(bits[i]),
                         json::numberToString(d).c_str(), json::stringify(JVal(d)).c_str(), want.constData(),
                         wantJson.constData());
        }
    }
    QVERIFY2(bad == 0, qPrintable(QStringLiteral("%1 of %2 doubles differ from Node").arg(bad).arg(bits.size())));

    // 2. strings of random UTF-16 code units (controls, specials, BMP, lone halves, pairs)
    const QList<QByteArray> strs = run(QStringLiteral("strings"), QString());
    QCOMPARE(strs.size(), qsizetype(4000));
    bad = 0;
    for (const QByteArray& line : strs) {
        const QList<QByteArray> cols = line.split('\t');
        QCOMPARE(cols.size(), qsizetype(2));
        std::vector<uint32_t> units;
        if (!cols[0].isEmpty())
            for (const QByteArray& u : cols[0].split(',')) units.push_back(u.toUInt(nullptr, 16));
        const std::string s = wtf8(units);
        const std::string want = QByteArray::fromHex(cols[1]).toStdString();
        if (json::quote(s) != want || json::parse(want).asString() != s) {
            if (++bad <= 10) qWarning("string %s: got %s, node %s", cols[0].constData(), json::quote(s).c_str(), want.c_str());
        }
    }
    QVERIFY2(bad == 0, qPrintable(QStringLiteral("%1 of 4000 strings differ from Node").arg(bad)));

    // 3. random documents in compact and indented forms: parse, then stringify and stream back to Node's bytes
    const QList<QByteArray> docs = run(QStringLiteral("docs"), QString());
    QCOMPARE(docs.size(), qsizetype(1500));
    bad = 0;
    for (const QByteArray& line : docs) {
        const QList<QByteArray> cols = line.split('\t');
        QCOMPARE(cols.size(), qsizetype(2));
        const std::string doc = QByteArray::fromHex(cols[0]).toStdString();
        const std::string want = QByteArray::fromHex(cols[1]).toStdString();
        std::string got, streamed;
        try {
            const JVal v = json::parse(doc);
            got = json::stringify(v);
            StringSink sink;
            json::Writer w(sink, 512);
            w.value(v);
            w.finish();
            streamed = sink.data;
        } catch (const std::exception& e) {
            got = std::string("exception: ") + e.what();
        }
        if (got != want || streamed != want) {
            if (++bad <= 5) qWarning("doc %s\n got %s\n node %s", doc.c_str(), got.c_str(), want.c_str());
        }
    }
    QVERIFY2(bad == 0, qPrintable(QStringLiteral("%1 of 1500 documents differ from Node").arg(bad)));
    qInfo("node oracle: %d doubles, 4000 strings, 1500 documents identical", static_cast<int>(bits.size()));
}

QTEST_GUILESS_MAIN(TestAnfsCoreJson)
#include "test_anfs_core_json.moc"

namespace {
const char* oracleJs()
{
    return R"JS('use strict';
const fs = require('fs');
const [mode, inFile, outFile] = process.argv.slice(2);
let seed = 0x5eed1234;
function rnd() { seed = (seed + 0x6D2B79F5) | 0; let t = Math.imul(seed ^ (seed >>> 15), 1 | seed); t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; }
function ri(n) { return Math.floor(rnd() * n); }
const SPECIAL = [0x22, 0x5c, 0x2f, 0x7f, 0x2028, 0x2029, 0xfeff, 0xfffd, 0xffff, 0xe000, 0x80, 0x7ff, 0x800];
function unit(u) {
  const r = rnd();
  if (r < 0.30) u.push(0x20 + ri(95));
  else if (r < 0.40) u.push(ri(32));
  else if (r < 0.48) u.push(SPECIAL[ri(SPECIAL.length)]);
  else if (r < 0.56) u.push(0x80 + ri(0x780));
  else if (r < 0.64) u.push(0x800 + ri(0xd000));
  else if (r < 0.74) u.push(0xd800 + ri(0x400));
  else if (r < 0.84) u.push(0xdc00 + ri(0x400));
  else { const cp = ri(0x100000); u.push(0xd800 + (cp >> 10), 0xdc00 + (cp & 0x3ff)); }
}
function randStr(maxLen) { const u = []; const n = ri(maxLen + 1); for (let i = 0; i < n; i++) unit(u); return String.fromCharCode(...u); }
function num() {
  const r = rnd();
  if (r < 0.4) return ri(2000001) - 1000000;
  if (r < 0.5) return (ri(2) ? 1 : -1) * Math.floor(rnd() * 9007199254740991);
  if (r < 0.8) return (ri(2000001) - 1000000) / Math.pow(10, ri(12));
  const b = Buffer.alloc(8); for (let i = 0; i < 8; i++) b[i] = ri(256);
  const d = b.readDoubleBE(0); return Number.isFinite(d) ? d : 0.5;
}
function val(depth) {
  const r = rnd();
  if (depth > 4 || r < 0.5) { const s = rnd(); if (s < 0.1) return null; if (s < 0.2) return rnd() < 0.5; if (s < 0.55) return num(); return randStr(8); }
  if (r < 0.75) { const a = []; const n = ri(5); for (let i = 0; i < n; i++) a.push(val(depth + 1)); return a; }
  const o = {}; const n = ri(5); for (let i = 0; i < n; i++) o['k' + randStr(4)] = val(depth + 1); return o;
}
const hex = (s) => Buffer.from(s, 'utf8').toString('hex');
const out = [];
if (mode === 'numbers') {
  for (const line of fs.readFileSync(inFile, 'latin1').split('\n')) {
    if (!line.trim()) continue;
    const x = Buffer.from(line.trim(), 'hex').readDoubleBE(0);
    out.push(hex(String(x)) + '\t' + hex(JSON.stringify(x)));
  }
} else if (mode === 'strings') {
  for (let i = 0; i < 4000; i++) {
    const s = randStr(10); const u = [];
    for (let j = 0; j < s.length; j++) u.push(s.charCodeAt(j).toString(16));
    out.push(u.join(',') + '\t' + hex(JSON.stringify(s)));
  }
} else if (mode === 'docs') {
  const SPACES = ['', 1, 2, '\t', '\r\n', ' \n\t'];
  for (let i = 0; i < 1500; i++) { const v = val(0); out.push(hex(JSON.stringify(v, null, SPACES[ri(SPACES.length)])) + '\t' + hex(JSON.stringify(v))); }
}
fs.writeFileSync(outFile, out.join('\n') + '\n');
)JS";
}
} // namespace
