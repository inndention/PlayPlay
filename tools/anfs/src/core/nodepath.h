// core/nodepath.h - Node path.win32 semantics on UTF-8 strings, plus the AN-FS path rules every module shares.
// FROZEN (ANFS-F0); implemented by ANFS-F2a (core/nodepath.cpp).
//
// Pure string functions except resolve() (process cwd for relative input) and conflictName() (asks the file system).
// Vectors (CONVENTIONS.md section 2, mod-fscore.md):
//   resolve("C:\\a\\..\\b\\") = "C:\\b"           basename("C:\\a\\b\\") = "b"      extname(".bashrc") = ""
//   extname("a.tar.gz") = ".gz"                   safeRelDir("C:\\x/../y\\.\\z") = "x\\y\\z"
//   conflictName("C:\\d\\a.txt") with a.txt and a (1).txt present = "C:\\d\\a (2).txt"; ".bashrc" -> ".bashrc (1)"
#pragma once

#include <string>
#include <string_view>

namespace anfs::nodepath {

// path.win32.resolve(p): absolute, '/' turned into '\', '.' and '..' collapsed, no trailing separator except a
// drive root ("C:\"). A relative p resolves against the process cwd ("X:rel" against the drive's "=X:" cwd, as
// Node): callers pass absolute paths. Node 22.13.1 semantics (implementation notes in nodepath.cpp).
std::string resolve(std::string_view p);
std::string resolve(std::string_view base, std::string_view p);
std::string normalize(std::string_view p);        // path.win32.normalize
std::string join(std::string_view a, std::string_view b);   // path.win32.join (normalizes '..')
std::string basename(std::string_view p);
std::string basename(std::string_view p, std::string_view ext);   // strips ext when it matches the end
std::string dirname(std::string_view p);
std::string extname(std::string_view p);          // ".bashrc" has no extension
bool isAbsolute(std::string_view p);              // path.win32.isAbsolute: "X:\", "X:/" or any leading separator

// C# ToLowerInvariant on UTF-8 (full Unicode simple case mapping, like .NET invariant culture).
std::string toLowerInvariant(std::string_view s);
// Comparison key: resolve(p) lower-cased.
std::string key(std::string_view p);
// pKey equals rootKey or starts with rootKey + "\"; a drive root key ("c:\") contains its children (D3).
bool isUnder(std::string_view rootKey, std::string_view pKey);

// agent.js:1328-1356 relDir sanitizing: strip a leading "X:", split on '\' and '/', drop empty, ".", ".." and "X:"
// segments, join with '\'. Hostile input yields "" or a flat name, never an exception.
std::string safeRelDir(std::string_view rel);
// fullPath when free, else "<base> (N)<ext>" for N = 1, 2, ... (Node path.extname rules).
std::string conflictName(std::string_view fullPath);

} // namespace anfs::nodepath
