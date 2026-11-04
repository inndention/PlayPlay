#ifndef PLATFORM_H
#define PLATFORM_H

#include <QString>

class Platform
{
public:
    enum class OS {
        Windows,
        MacOS,
        Linux,
        Unknown
    };
    
    static OS getPlatform();
    static QString getPlatformString();
    static bool isWindows();
    static bool isMac();
    static bool isLinux();
    
private:
    Platform() = default;
};

#endif // PLATFORM_H

