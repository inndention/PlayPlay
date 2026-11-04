// test_anfs_secrets - the AN-FS secrets guard (ANFS-F0, anfs-cpp.md section 10 risk 5).
//
// The Node agent token must never be committed: the C# port kept the literal in CompatConst.cs, which is acceptable
// there (no git) but not in this repository. This test scans every text file under tools/anfs and tests/anfs:
//   1. with a token file (--token-file <file> or env ANFS_COMPAT_TOKEN_FILE; the file lives OUTSIDE the repo):
//      the exact token (any case, also its hex and base64 forms) and any literal with the token's shape (the same
//      run lengths of letters/digits and the same separators, derived from the file at run time);
//   2. always: generic token-like literals (upper-case letter/digit groups joined by at least two '-', 16+ chars,
//      with letters and digits, not a GUID), private-key blocks and well-known API-key prefixes.
// The token, its shape and every finding are reported masked ("AB...(24)"), so a failing run never prints a secret.
// Without a token file the exact-token check is skipped (QSKIP) and the generic checks still run.
// The UI files embedded in anfs.exe (Qt resources) get the same scan, and must be stored uncompressed and equal their
// source files (skipped while nothing is embedded).
//
// Usage: test_anfs_secrets [--root <repo>] [--token-file <file>] [QtTest options]
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QRandomGenerator>
#include <QtCore/QRegularExpression>
#include <QtCore/QResource>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>

#include <cstring>
#include <vector>

#ifndef ANFS_REPO_ROOT
#define ANFS_REPO_ROOT ""
#endif

namespace {

QString g_root;
QString g_tokenFile;

struct Finding {
    QString file;
    int line = 0;
    QString rule;
    QString masked;
};

QString mask(const QString& s)
{
    return s.left(2) + QStringLiteral("...(") + QString::number(s.size()) + QLatin1Char(')');
}

// Reads the token file: the first non-empty trimmed line. Empty when unreadable.
QString readTokenFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QList<QByteArray> lines = f.readAll().split('\n');
    for (const QByteArray& l : lines) {
        QString t = QString::fromUtf8(l).trimmed();
        if (!t.isEmpty()) return t;
    }
    return {};
}

class SecretScanner {
public:
    explicit SecretScanner(const QString& token = {}) : token_(token)
    {
        // Generic shape: alphanumeric groups joined by '-', at least three groups, not inside a longer identifier.
        generic_.setPattern(QStringLiteral("(?<![A-Za-z0-9_\\-])[A-Za-z0-9]{2,}(?:-[A-Za-z0-9]{2,}){2,}(?![A-Za-z0-9_\\-])"));
        guid_.setPattern(QStringLiteral("^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}$"));
        // Assembled from pieces so this file never contains a complete marker itself.
        extra_.push_back({QStringLiteral("private-key block"),
                          QRegularExpression(QStringLiteral("-----BEGIN ([A-Z]+ )*PRIV") + QStringLiteral("ATE KEY-----"))});
        extra_.push_back({QStringLiteral("OpenRouter key"),
                          QRegularExpression(QStringLiteral("sk-or-") + QStringLiteral("v1-[0-9a-f]{32,}"))});
        extra_.push_back({QStringLiteral("Anthropic key"),
                          QRegularExpression(QStringLiteral("sk-an") + QStringLiteral("t-[A-Za-z0-9_\\-]{20,}"))});
        extra_.push_back({QStringLiteral("AWS key id"),
                          QRegularExpression(QStringLiteral("AK") + QStringLiteral("IA[0-9A-Z]{16}"))});
        if (token_.size() >= 8) {
            variants_ << token_
                      << QString::fromLatin1(token_.toUtf8().toHex())
                      << QString::fromLatin1(token_.toUtf8().toBase64(QByteArray::OmitTrailingEquals));
            shape_.setPattern(QStringLiteral("(?<![A-Za-z0-9_\\-])") + shapePattern(token_)
                              + QStringLiteral("(?![A-Za-z0-9_\\-])"));
        }
    }

    bool hasToken() const { return !variants_.isEmpty(); }

    // "[A-Za-z0-9]{2}-[A-Za-z0-9]{7}" style pattern: same run lengths, same separators.
    static QString shapePattern(const QString& token)
    {
        QString out;
        int run = 0;
        auto flush = [&] {
            if (run) out += QStringLiteral("[A-Za-z0-9]{") + QString::number(run) + QLatin1Char('}');
            run = 0;
        };
        for (QChar c : token) {
            if (c.isLetterOrNumber() && c.unicode() < 128) { ++run; continue; }
            flush();
            out += QRegularExpression::escape(QString(c));
        }
        flush();
        return out;
    }

    bool genericCandidate(const QString& s) const
    {
        if (s.size() < 16 || guid_.match(s).hasMatch()) return false;
        bool digit = false, letter = false;
        for (QChar c : s) {
            if (c.isDigit()) digit = true;
            else if (c.isLetter()) {
                letter = true;
                if (c.isLower()) return false;    // the token shape is upper-case letters and digits
            }
        }
        if (!digit || !letter) return false;
        // Algorithm and protocol names ("PBKDF2-HMAC-SHA256", "TLS-ECDHE-RSA-AES128") are not secrets: a literal
        // whose groups are all well-known words or plain numbers passes. A random token never consists of them.
        static const QStringList words = {
            QStringLiteral("PBKDF2"), QStringLiteral("HMAC"), QStringLiteral("SHA1"), QStringLiteral("SHA224"),
            QStringLiteral("SHA256"), QStringLiteral("SHA384"), QStringLiteral("SHA512"), QStringLiteral("MD5"),
            QStringLiteral("AES"), QStringLiteral("AES128"), QStringLiteral("AES192"), QStringLiteral("AES256"),
            QStringLiteral("GCM"), QStringLiteral("CBC"), QStringLiteral("CTR"), QStringLiteral("RSA"),
            QStringLiteral("ECDHE"), QStringLiteral("ECDSA"), QStringLiteral("DHE"), QStringLiteral("TLS"),
            QStringLiteral("SSL"), QStringLiteral("UTF8"), QStringLiteral("UTF16"), QStringLiteral("UTF"),
            QStringLiteral("ISO"), QStringLiteral("IEC"), QStringLiteral("RFC"), QStringLiteral("HTTP"),
            QStringLiteral("HTTPS"), QStringLiteral("TCP4"), QStringLiteral("TCP6"), QStringLiteral("UDP"),
            QStringLiteral("IPV4"), QStringLiteral("IPV6"), QStringLiteral("X509"), QStringLiteral("PEM"),
            QStringLiteral("DER"), QStringLiteral("PKCS1"), QStringLiteral("PKCS7"), QStringLiteral("PKCS8"),
            QStringLiteral("PKCS12"), QStringLiteral("ZIP64"), QStringLiteral("CRC32"), QStringLiteral("FTS5"),
            QStringLiteral("WAL"), QStringLiteral("JSON"), QStringLiteral("JSON1"), QStringLiteral("H264"),
            QStringLiteral("H265"), QStringLiteral("MP4"), QStringLiteral("MPEG2"), QStringLiteral("NDI"),
        };
        bool allKnown = true;
        for (const QString& g : s.split(QLatin1Char('-'))) {
            bool numeric = true;
            for (QChar c : g) numeric = numeric && c.isDigit();
            if (!numeric && !words.contains(g)) { allKnown = false; break; }
        }
        return !allKnown;
    }

    QList<Finding> scanText(const QString& name, const QByteArray& data) const
    {
        QList<Finding> out;
        const QString text = QString::fromUtf8(data);
        const QStringList lines = text.split(QLatin1Char('\n'));
        for (int i = 0; i < lines.size(); ++i) {
            const QString& line = lines.at(i);
            for (const QString& v : variants_)
                if (line.contains(v, Qt::CaseInsensitive)) out.push_back({name, i + 1, QStringLiteral("agent token"), mask(v)});
            if (hasToken()) {
                auto it = shape_.globalMatch(line);
                while (it.hasNext()) out.push_back({name, i + 1, QStringLiteral("token shape"), mask(it.next().captured(0))});
            }
            auto git = generic_.globalMatch(line);
            while (git.hasNext()) {
                const QString m = git.next().captured(0);
                if (genericCandidate(m)) out.push_back({name, i + 1, QStringLiteral("token-like literal"), mask(m)});
            }
            for (const auto& e : extra_) {
                auto eit = e.second.globalMatch(line);
                while (eit.hasNext()) out.push_back({name, i + 1, e.first, mask(eit.next().captured(0))});
            }
        }
        return out;
    }

    // Every regular file below dir (no .git), skipping binaries (a NUL byte in the first 8 KB) and files over 64 MB.
    QList<Finding> scanTree(const QString& dir, int* filesScanned = nullptr) const
    {
        QList<Finding> out;
        int n = 0;
        QDirIterator it(dir, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            if (path.contains(QStringLiteral("/.git/"))) continue;
            QFile f(path);
            if (f.size() > (64LL << 20) || !f.open(QIODevice::ReadOnly)) continue;
            const QByteArray data = f.readAll();
            if (data.left(8192).contains('\0')) continue;
            ++n;
            out += scanText(QDir(g_root).relativeFilePath(path), data);
        }
        if (filesScanned) *filesScanned = n;
        return out;
    }

private:
    QString token_;
    QStringList variants_;
    QRegularExpression generic_, guid_, shape_;
    std::vector<std::pair<QString, QRegularExpression>> extra_;
};

QString report(const QList<Finding>& f)
{
    QStringList lines;
    for (const Finding& x : f) lines << QStringLiteral("%1:%2: %3 %4").arg(x.file).arg(x.line).arg(x.rule, x.masked);
    return lines.join(QLatin1Char('\n'));
}

// A random upper-case token with the given group lengths (letters and digits both present), built at run time so
// this source never holds a token-shaped literal.
QString syntheticToken(const std::vector<int>& groups)
{
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    QString t;
    for (size_t g = 0; g < groups.size(); ++g) {
        if (g) t += QLatin1Char('-');
        for (int i = 0; i < groups[g]; ++i)
            t += QLatin1Char(alphabet[QRandomGenerator::global()->bounded(int(sizeof(alphabet) - 1))]);
    }
    t[0] = QLatin1Char('Q');                       // a letter
    t[1] = QLatin1Char('7');                       // and a digit, whatever the random draw gave
    return t;
}

bool writeFile(const QString& path, const QByteArray& data)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

} // namespace

class TestAnfsSecrets : public QObject {
    Q_OBJECT

private slots:
    void scannerFindsSyntheticToken()
    {
        const QString fake = syntheticToken({4, 4, 4, 6});
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QVERIFY(writeFile(tmp.filePath(QStringLiteral("a.cpp")), "static const char* k = \"" + fake.toUtf8() + "\";\n"));

        SecretScanner generic;
        QList<Finding> f = generic.scanTree(tmp.path());
        QCOMPARE(f.size(), 1);
        QCOMPARE(f[0].rule, QStringLiteral("token-like literal"));
        QCOMPARE(f[0].line, 1);
        QVERIFY2(!f[0].masked.contains(fake), "findings are masked");

        SecretScanner withToken(fake);
        f = withToken.scanTree(tmp.path());
        QStringList rules;
        for (const Finding& x : f) rules << x.rule;
        QVERIFY2(rules.contains(QStringLiteral("agent token")), qPrintable(report(f)));
        QVERIFY2(rules.contains(QStringLiteral("token shape")), qPrintable(report(f)));
    }

    void scannerFindsEncodedAndLowerCaseToken()
    {
        const QString fake = syntheticToken({3, 5, 5, 5});
        SecretScanner s(fake);
        QVERIFY(!s.scanText(QStringLiteral("x"), "v = '" + fake.toLower().toUtf8() + "'").isEmpty());
        QVERIFY(!s.scanText(QStringLiteral("x"), "hex " + fake.toUtf8().toHex()).isEmpty());
        QVERIFY(!s.scanText(QStringLiteral("x"), "b64 " + fake.toUtf8().toBase64(QByteArray::OmitTrailingEquals)).isEmpty());
    }

    void shapeComesFromTheTokenFile()
    {
        const QString fake = syntheticToken({2, 6, 3, 9});
        QTemporaryDir tmp;
        QVERIFY(writeFile(tmp.filePath(QStringLiteral("token.txt")), "  " + fake.toUtf8() + "\r\n"));
        const QString read = readTokenFile(tmp.filePath(QStringLiteral("token.txt")));
        QCOMPARE(read, fake);
        // Another literal with the same shape but different characters is caught by the derived shape rule.
        QString other = fake;
        for (int i = 0; i < other.size(); ++i)
            if (other[i] != QLatin1Char('-')) other[i] = other[i].isDigit() ? QLatin1Char('9') : QLatin1Char('Z');
        SecretScanner s(read);
        QList<Finding> f = s.scanText(QStringLiteral("y"), other.toUtf8());
        bool shape = false;
        for (const Finding& x : f) shape = shape || x.rule == QStringLiteral("token shape");
        QVERIFY2(shape, qPrintable(report(f)));
        QCOMPARE(SecretScanner::shapePattern(QStringLiteral("AB-1234")), QStringLiteral("[A-Za-z0-9]{2}\\-[A-Za-z0-9]{4}"));
    }

    void scannerIgnoresBenignText()
    {
        SecretScanner s;
        const QByteArray benign =
            "CLSID {4D36E967-E325-11CE-BFC1-08002BE10318}\n"     // GUID
            "X-Content-Type-Options: nosniff\n"                  // header names (mixed case)
            "CONTENT-LENGTH-HEADER-VALUE\n"                      // no digit
            "v1-2-3 2026-10-26T13:17:58.770Z\n"                  // version and date
            "anfs-0123456789-abcdef-test\n"                      // lower case
            "PBKDF2-HMAC-SHA256 TLS-ECDHE-RSA-AES128-GCM-SHA256\n" // algorithm names
            "build-anfs\\anfs.exe build-seg 18080-18899-18898\n"; // short or digits only
        const QList<Finding> f = s.scanText(QStringLiteral("benign"), benign);
        QVERIFY2(f.isEmpty(), qPrintable(report(f)));
    }

    void scannerSkipsBinaryFiles()
    {
        const QString fake = syntheticToken({4, 4, 4, 6});
        QTemporaryDir tmp;
        QByteArray bin("\x7f" "ELF");
        bin.append('\0');
        bin.append(fake.toUtf8());
        QVERIFY(writeFile(tmp.filePath(QStringLiteral("blob.bin")), bin));
        QVERIFY(SecretScanner(fake).scanTree(tmp.path()).isEmpty());
    }

    void repoHasNoTokenLikeLiterals()
    {
        QVERIFY2(QFileInfo(g_root + QStringLiteral("/tools/anfs")).isDir(), qPrintable(QStringLiteral("no tools/anfs under ") + g_root));
        SecretScanner s(g_tokenFile.isEmpty() ? QString() : readTokenFile(g_tokenFile));
        int files = 0, testFiles = 0;
        QList<Finding> f = s.scanTree(g_root + QStringLiteral("/tools/anfs"), &files);
        if (QFileInfo(g_root + QStringLiteral("/tests/anfs")).isDir())
            f += s.scanTree(g_root + QStringLiteral("/tests/anfs"), &testFiles);
        qInfo("scanned %d files under tools/anfs and %d under tests/anfs (token file: %s)", files, testFiles,
              g_tokenFile.isEmpty() ? "none" : "given");
        QVERIFY(files > 0);
        QVERIFY2(f.isEmpty(), qPrintable(QStringLiteral("secret-looking literals in the repository:\n") + report(f)));
    }

    void generatedTokenHeaderNeverInSourceTree()
    {
        for (const char* sub : {"/tools/anfs", "/tests/anfs"}) {
            QDirIterator it(g_root + QLatin1String(sub), {QStringLiteral("compat_token.h")}, QDir::Files,
                            QDirIterator::Subdirectories);
            QVERIFY2(!it.hasNext(), "compat_token.h belongs in the build folder only");
        }
    }

    // The UI embedded through tools/anfs/CMakeLists.txt ships inside anfs.exe (anfs_obj is linked into this test as
    // well): every resource must be stored uncompressed (rcc --no-compress, so served bytes are the source bytes,
    // anfs-cpp.md 2.2), equal its source file when one sits at the same path under tools/anfs (a generated qrc maps
    // :/ui-peer/x to tools/anfs/ui-peer/x; a hand-written qrc may use aliases), and hold no secret.
    void embeddedResourcesAreUncompressedSourceCopies()
    {
        const QString base = g_root + QStringLiteral("/tools/anfs/");
        SecretScanner s(g_tokenFile.isEmpty() ? QString() : readTokenFile(g_tokenFile));
        int resources = 0, compared = 0;
        QStringList problems;
        QList<Finding> secrets;
        QDirIterator it(QStringLiteral(":/"), QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            if (path.startsWith(QStringLiteral(":/qt-project.org/"))) continue;   // Qt's own (mime database ...)
            ++resources;
            const QString rel = path.mid(2);
            QResource r(path);
            if (r.compressionAlgorithm() != QResource::NoCompression)
                problems << rel + QStringLiteral(": stored compressed (rcc --no-compress missing)");
            QFile rf(path);
            QVERIFY2(rf.open(QIODevice::ReadOnly), qPrintable(path));
            const QByteArray bytes = rf.readAll();
            secrets += s.scanText(QStringLiteral("resource ") + path, bytes);
            QFile src(base + rel);
            if (!src.open(QIODevice::ReadOnly)) continue;
            ++compared;
            if (src.readAll() != bytes) problems << rel + QStringLiteral(": differs from tools/anfs/") + rel;
        }
        qInfo("%d embedded resources, %d compared byte for byte with their source", resources, compared);
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QLatin1Char('\n'))));
        QVERIFY2(secrets.isEmpty(), qPrintable(QStringLiteral("secret-looking literals in embedded resources:\n") + report(secrets)));
        if (resources == 0) QSKIP("no embedded UI yet (ui/ arrives with srv-ui, ui-peer/ with pfs-setup)");
    }

    void repoHasNoExactToken()
    {
        if (g_tokenFile.isEmpty()) QSKIP("no --token-file / ANFS_COMPAT_TOKEN_FILE: exact-token scan skipped");
        const QString abs = QFileInfo(g_tokenFile).absoluteFilePath();
        const QString root = QFileInfo(g_root).absoluteFilePath() + QLatin1Char('/');
        QVERIFY2(!abs.startsWith(root, Qt::CaseInsensitive), "the token file must live outside the repository");
        const QString token = readTokenFile(g_tokenFile);
        QVERIFY2(token.size() >= 8, "the token file is empty or unreadable");
        SecretScanner s(token);
        QVERIFY(s.hasToken());
        QList<Finding> f = s.scanTree(g_root + QStringLiteral("/tools/anfs"));
        if (QFileInfo(g_root + QStringLiteral("/tests/anfs")).isDir()) f += s.scanTree(g_root + QStringLiteral("/tests/anfs"));
        QVERIFY2(f.isEmpty(), qPrintable(QStringLiteral("the agent token (or its shape) is in the repository:\n") + report(f)));
    }
};

int main(int argc, char** argv)
{
    // Our options are removed before QtTest parses the rest.
    std::vector<char*> rest;
    for (int i = 0; i < argc; ++i) {
        if (i > 0 && i + 1 < argc && std::strcmp(argv[i], "--root") == 0) { g_root = QString::fromLocal8Bit(argv[++i]); continue; }
        if (i > 0 && i + 1 < argc && std::strcmp(argv[i], "--token-file") == 0) { g_tokenFile = QString::fromLocal8Bit(argv[++i]); continue; }
        rest.push_back(argv[i]);
    }
    if (g_root.isEmpty()) g_root = qEnvironmentVariable("ANFS_REPO_ROOT", QStringLiteral(ANFS_REPO_ROOT));
    if (g_tokenFile.isEmpty()) g_tokenFile = qEnvironmentVariable("ANFS_COMPAT_TOKEN_FILE");
    g_root = QDir::fromNativeSeparators(QDir::cleanPath(g_root));
    int n = static_cast<int>(rest.size());
    rest.push_back(nullptr);
    QCoreApplication app(n, rest.data());
    TestAnfsSecrets t;
    return QTest::qExec(&t, n, rest.data());
}

#include "test_anfs_secrets.moc"
