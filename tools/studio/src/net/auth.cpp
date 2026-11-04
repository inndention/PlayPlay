#include "net/auth.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QUrl>
#include <QUrlQuery>
#include <vector>

#include "core/ids.h"
#include "core/log.h"
#include "net/httpserver.h"

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#pragma comment(lib, "Advapi32.lib")
#else
#include <QSaveFile>
#endif

namespace studio::net {

namespace {

QByteArray sha256(QByteArrayView v) { return QCryptographicHash::hash(v, QCryptographicHash::Sha256); }

bool constantTimeEqual(const QByteArray& a, const QByteArray& b) {
    if (a.size() != b.size()) return false;   // both are SHA-256 digests: the length is no secret
    unsigned char diff = 0;
    for (qsizetype i = 0; i < a.size(); ++i) diff |= uchar(a[i]) ^ uchar(b[i]);
    return diff == 0;
}

#ifdef Q_OS_WIN
std::wstring wide(const QString& s) { return QDir::toNativeSeparators(s).toStdWString(); }

QString lastError() {
    const DWORD e = GetLastError();
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0,
                   reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    QString msg = buf ? QString::fromWCharArray(buf).trimmed() : QString();
    if (buf) LocalFree(buf);
    return QStringLiteral("%1 (error %2)").arg(msg).arg(e);
}

// D:P = protected DACL (nothing inherited from the folder); FA = full access for this account and SYSTEM only.
bool ownerOnlyDescriptor(PSECURITY_DESCRIPTOR* sd, QString* error) {
    const QString sid = currentUserSid();
    if (sid.isEmpty()) {
        if (error) *error = QStringLiteral("cannot read this account's SID");
        return false;
    }
    const std::wstring sddl = (QStringLiteral("D:P(A;;FA;;;%1)(A;;FA;;;SY)").arg(sid)).toStdWString();
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, sd, nullptr)) {
        if (error) *error = QStringLiteral("cannot build the token file's security descriptor: %1").arg(lastError());
        return false;
    }
    return true;
}
#endif

} // namespace

// ---------------- private files ----------------

QString currentUserSid() {
#ifdef Q_OS_WIN
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return {};
    DWORD len = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &len);
    QString out;
    if (len > 0) {
        std::vector<BYTE> buf(len);
        if (GetTokenInformation(tok, TokenUser, buf.data(), len, &len)) {
            LPWSTR s = nullptr;
            if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &s)) {
                out = QString::fromWCharArray(s);
                LocalFree(s);
            }
        }
    }
    CloseHandle(tok);
    return out;
#else
    return {};
#endif
}

bool writePrivateFile(const QString& path, const QByteArray& data, bool replace, QString* error, bool* existed) {
    if (existed) *existed = false;
    const QString tmp = path + QStringLiteral(".tmp-") + ids::randomHex(4);
#ifdef Q_OS_WIN
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ownerOnlyDescriptor(&sd, error)) return false;
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), sd, FALSE};
    const std::wstring wtmp = wide(tmp), wpath = wide(path);
    const HANDLE h = CreateFileW(wtmp.c_str(), GENERIC_WRITE, 0, &sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    LocalFree(sd);
    if (h == INVALID_HANDLE_VALUE) {
        if (error) *error = QStringLiteral("cannot create %1: %2").arg(QDir::toNativeSeparators(tmp), lastError());
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(h, data.constData(), DWORD(data.size()), &written, nullptr) && written == DWORD(data.size()) && FlushFileBuffers(h);
    const QString writeError = ok ? QString() : lastError();
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(wtmp.c_str());
        if (error) *error = QStringLiteral("cannot write %1: %2").arg(QDir::toNativeSeparators(tmp), writeError);
        return false;
    }
    // A rename keeps the file's own DACL, so the target is owner-only from its first moment under its real name.
    if (!MoveFileExW(wtmp.c_str(), wpath.c_str(), MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0))) {
        const DWORD e = GetLastError();
        const QString why = lastError();
        DeleteFileW(wtmp.c_str());
        if (e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS) {
            if (existed) *existed = true;
            if (error) *error = QStringLiteral("%1 already exists").arg(QDir::toNativeSeparators(path));
        } else if (error) {
            *error = QStringLiteral("cannot move the new file to %1: %2").arg(QDir::toNativeSeparators(path), why);
        }
        return false;
    }
    return true;
#else
    if (!replace && QFileInfo::exists(path)) {
        if (existed) *existed = true;
        if (error) *error = QStringLiteral("%1 already exists").arg(path);
        return false;
    }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    f.write(data);
    if (!f.commit()) {
        if (error) *error = f.errorString();
        return false;
    }
    return true;
#endif
}

bool restrictToOwner(const QString& path, QString* error) {
#ifdef Q_OS_WIN
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ownerOnlyDescriptor(&sd, error)) return false;
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = nullptr;
    bool ok = GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) && present;
    if (ok) {
        std::wstring w = wide(path);
        const DWORD rc = SetNamedSecurityInfoW(w.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                               nullptr, nullptr, dacl, nullptr);
        if (rc != ERROR_SUCCESS) {
            SetLastError(rc);
            if (error) *error = QStringLiteral("cannot restrict %1: %2").arg(QDir::toNativeSeparators(path), lastError());
            ok = false;
        }
    } else if (error) {
        *error = QStringLiteral("cannot read the owner-only DACL");
    }
    LocalFree(sd);
    return ok;
#else
    if (!QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        if (error) *error = QStringLiteral("cannot restrict %1").arg(path);
        return false;
    }
    return true;
#endif
}

QStringList aclSids(const QString& path, bool* isProtected, QString* error) {
    if (isProtected) *isProtected = false;
#ifdef Q_OS_WIN
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    const std::wstring w = wide(path);
    const DWORD rc = GetNamedSecurityInfoW(w.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &sd);
    if (rc != ERROR_SUCCESS) {
        SetLastError(rc);
        if (error) *error = lastError();
        return {};
    }
    QStringList out;
    SECURITY_DESCRIPTOR_CONTROL ctl = 0;
    DWORD rev = 0;
    if (GetSecurityDescriptorControl(sd, &ctl, &rev) && isProtected) *isProtected = (ctl & SE_DACL_PROTECTED) != 0;
    if (!dacl) {
        out << QStringLiteral("S-1-1-0");  // a NULL DACL grants everyone everything
    } else {
        ACL_SIZE_INFORMATION info{};
        if (GetAclInformation(dacl, &info, sizeof info, AclSizeInformation)) {
            for (DWORD i = 0; i < info.AceCount; ++i) {
                void* ace = nullptr;
                if (!GetAce(dacl, i, &ace)) continue;
                if (static_cast<ACE_HEADER*>(ace)->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
                LPWSTR s = nullptr;
                if (ConvertSidToStringSidW(reinterpret_cast<PSID>(&static_cast<ACCESS_ALLOWED_ACE*>(ace)->SidStart), &s)) {
                    out << QString::fromWCharArray(s);
                    LocalFree(s);
                }
            }
        }
    }
    LocalFree(sd);
    return out;
#else
    Q_UNUSED(path);
    if (error) *error = QStringLiteral("not supported on this platform");
    return {};
#endif
}

// ---------------- Auth ----------------

QString Auth::generate() {
    QByteArray raw(32, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(raw.data()), raw.size() / 4);
    return QString::fromLatin1(raw.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

bool Auth::validToken(QStringView t) {
    if (t.size() < 24 || t.size() > 512) return false;
    for (QChar c : t)
        if (c.unicode() <= 0x20 || c.unicode() >= 0x7f) return false;
    return true;
}

QString Auth::readTokenFile(const QString& path, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot read %1: %2").arg(QDir::toNativeSeparators(path), f.errorString());
        return {};
    }
    QByteArray raw = f.read(4097);
    if (raw.size() > 4096) {
        if (error) *error = QStringLiteral("%1 is too large to be a token file").arg(QDir::toNativeSeparators(path));
        return {};
    }
    if (raw.startsWith("\xEF\xBB\xBF")) raw.remove(0, 3);    // Notepad / PowerShell 5.1 "UTF-8"
    const QString t = QString::fromLatin1(raw.trimmed());
    if (!validToken(t)) {
        if (error) *error = QStringLiteral("%1 does not hold a valid token (24-512 printable characters, no spaces); delete it to "
                                           "make a new one").arg(QDir::toNativeSeparators(path));
        return {};
    }
    return t;
}

void Auth::setToken(const QString& token) {
    m_hash = token.isEmpty() ? QByteArray() : sha256(token.toLatin1());
    if (!token.isEmpty()) log::addSecret(token);
}

bool Auth::load(const QString& dataDir, QString* error) {
    m_hash.clear();
    m_path = QDir::cleanPath(dataDir + QLatin1Char('/') + fileName());
    QString err;
    if (!QFileInfo::exists(m_path)) {
        bool existed = false;
        const QString fresh = generate();
        if (writePrivateFile(m_path, fresh.toLatin1() + '\n', false, &err, &existed)) {
            setToken(fresh);
            log::info(u"auth", QStringLiteral("created the API token file %1").arg(QDir::toNativeSeparators(m_path)));
            return true;
        }
        if (!existed) {                    // else another process created it first: use theirs
            if (error) *error = err;
            return false;
        }
    }
    const QString t = readTokenFile(m_path, &err);
    if (t.isEmpty()) {
        if (error) *error = err;
        return false;
    }
    QString aclErr;
    if (!restrictToOwner(m_path, &aclErr)) log::warn(u"auth", QStringLiteral("token file left as it was: %1").arg(aclErr));
    setToken(t);
    return true;
}

bool Auth::rotate(QString* error) {
    if (m_path.isEmpty()) {
        if (error) *error = QStringLiteral("no token file (load() was not called)");
        return false;
    }
    const QString fresh = generate();
    if (!writePrivateFile(m_path, fresh.toLatin1() + '\n', true, error)) return false;
    setToken(fresh);
    log::info(u"auth", QStringLiteral("API token rotated"));
    return true;
}

bool Auth::matches(QByteArrayView presented) const {
    if (m_hash.isEmpty() || presented.isEmpty() || presented.size() > 4096) return false;
    return constantTimeEqual(sha256(presented), m_hash);
}

QByteArray Auth::presented(const Headers& headers, const QUrlQuery* query) {
    const QByteArray auth = headers.value("authorization").trimmed();
    if (auth.size() > 7 && auth.left(7).compare("bearer ", Qt::CaseInsensitive) == 0) return auth.mid(7).trimmed();
    const QByteArray x = headers.value("x-studio-token").trimmed();
    if (!x.isEmpty()) return x;
    if (query) return query->queryItemValue(QStringLiteral("token"), QUrl::FullyDecoded).toUtf8();
    return {};
}

} // namespace studio::net
