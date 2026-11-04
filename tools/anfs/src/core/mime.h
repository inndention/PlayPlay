// core/mime.h - content types (own table replacing mime-types / Express send). FROZEN (ANFS-F0); implemented by
// ANFS-F2a (core/mime.cpp).
#pragma once

#include <string>
#include <string_view>

namespace anfs::mime {

constexpr const char* kDefault = "application/octet-stream";

// The mime-types db type for an extension (".mp4" or "mp4", case-insensitive); "" when unknown.
std::string forExtension(std::string_view ext);
// forExtension(nodepath::extname(path)), else fallback.
std::string forPath(std::string_view path, std::string_view fallback = kDefault);
// What Express res.sendFile/send puts in Content-Type: the type plus "; charset=UTF-8" for text/*,
// application/javascript and application/json (mime 1.x charsets.lookup).
std::string contentTypeForFile(std::string_view path, std::string_view fallback = kDefault);

} // namespace anfs::mime
