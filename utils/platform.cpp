#include "platform.h"
#include <QOperatingSystemVersion>

Platform::OS Platform::getPlatform()
{
#ifdef Q_OS_WIN
    return OS::Windows;
#elif defined(Q_OS_MACOS)
    return OS::MacOS;
#elif defined(Q_OS_LINUX)
    return OS::Linux;
#else
    return OS::Unknown;
#endif
}

QString Platform::getPlatformString()
{
    switch (getPlatform()) {
        case OS::Windows:
            return "win32";
        case OS::MacOS:
            return "darwin";
        case OS::Linux:
            return "linux";
        default:
            return "unknown";
    }
}

bool Platform::isWindows()
{
    return getPlatform() == OS::Windows;
}

bool Platform::isMac()
{
    return getPlatform() == OS::MacOS;
}

bool Platform::isLinux()
{
    return getPlatform() == OS::Linux;
}

