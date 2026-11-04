// core/version.cpp - version strings (ANFS-F0). Also the one source file the OBJECT library anfs_obj always has, so
// the skeleton builds before the foundation slices add their .cpp files.
#include "core/version.h"

#include <string>

#define ANFS_STR2(x) #x
#define ANFS_STR(x) ANFS_STR2(x)

namespace anfs {

const char* versionLine()
{
    static const std::string line = std::string("anfs ") + kVersion;
    return line.c_str();
}

const char* buildInfo()
{
#if defined(_MSC_VER)
    return "cpp, MSVC " ANFS_STR(_MSC_VER) ", " __DATE__ " " __TIME__;
#else
    return "cpp, " __DATE__ " " __TIME__;
#endif
}

} // namespace anfs
