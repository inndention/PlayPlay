// core/version.h - the anfs.exe version. Same version line as the C# anfs.exe ("anfs 0.1.0") so StarRoot/starcmd
// parse both; `anfs version --verbose` adds the implementation and build.
#pragma once

namespace anfs {

constexpr const char* kVersion = "0.1.0";
constexpr const char* kImplementation = "cpp";    // the C# port reports no implementation field

const char* versionLine();                        // "anfs 0.1.0"
const char* buildInfo();                          // "cpp, MSVC <ver>, <date> <time>"

} // namespace anfs
