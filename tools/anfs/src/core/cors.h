// core/cors.h - the two CORS flavours. FROZEN (ANFS-F0); implemented by ANFS-F6 (core/cors.cpp).
//
// Peer (agent.js:431-443, pfs-setup installs it): reflect Origin with Access-Control-Allow-Credentials: true when an
// Origin is present, else Access-Control-Allow-Origin: *; Allow-Methods "GET, POST, PUT, DELETE, OPTIONS";
// Allow-Headers "Content-Type, Authorization, x-agent-auth, x-upload-id, x-chunk-index"; OPTIONS answers 200 "OK".
//
// Server (wan-gate.js corsMiddleware, deviation D1): the Origin is echoed with credentials only when allowed
// (wyat.getstarnews.com, *.wyat.getstarnews.com, localhost, origins whose host is a non-WAN IP, plus
// config.corsAllow entries); otherwise Access-Control-Allow-Origin: * without credentials (plus Vary: Origin when an
// Origin was sent). Same methods and headers as the peer. OPTIONS answers 204 with no body (D1).
#pragma once

#include "core/http_router.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace anfs::cors {

using HeaderList = std::vector<std::pair<std::string, std::string>>;

// Headers for one request; the caller adds them in order.
HeaderList peerHeaders(std::string_view origin);
HeaderList serverHeaders(std::string_view origin, const std::vector<std::string>& extraAllow);
// wan-gate.js corsOriginAllowed plus the extra entries ("https://host[:port]", "host", "*.suffix", "host:port").
bool serverOriginAllowed(std::string_view origin, const std::vector<std::string>& extraAllow);

http::Middleware peerMiddleware();
// extraAllow is read from the request's config snapshot (config.corsAllow).
http::Middleware serverMiddleware();

} // namespace anfs::cors
