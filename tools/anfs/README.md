# anfs.exe (AN-FS C++ port)

The C++ replacement for the Node AN-FS peer (`agent.js`) and bridge (`server.patched.js`), and a
drop-in for the C# `anfs.exe`: same CLI, same `anfs.json` keys, same `status.json`, same SQLite schema. Design notes are kept in the source headers and in this file.

## Layout

| path | what |
|---|---|
| `CMakeLists.txt` | standalone-capable project (also pulled into the PlayPlay tree by `cmake/lane-anfs.cmake`) |
| `src/main.cpp` | CLI (`run`, `status`, `version`, `hash-password`, `config`, `import`, `install`, `selftest`, `peer-legacy`, `shell`) |
| `src/core/*.h` | the frozen foundation API (Wave 0); `*.cpp` added by slices F1-F8 |
| `src/iface/*.h` | the frozen cross-module service interfaces (one header per service) |
| `src/mod/<module>/` | one folder per module, owned by one lane; ends with `ANFS_MODULE(Class, "modes", order);` |
| `third_party/sqlite/` | the vendored SQLite amalgamation (F3) |
| `test/sim/` | `anfs_sim.exe`: NodePeerSim / NodeBridgeSim (F8) |
| `test/harness/` | `run-compat.ps1` differential runner against the C# oracle (F8) |
| `../../tests/anfs/test_anfs_<id>.cpp` | one Qt Test per module or core area; `anfs_testkit.{h,cpp}` shared helpers (F8) |

Interface changes go through the AN-FS coordinator (the `anfs-foundation` lane), never patched in place by a module
lane. `src/core/headers_check.cpp` compiles every frozen header together, so a broken header fails the build at once.

## Build and test

Standalone build (needs Qt 6 and Ninja on the path; adjust `CMAKE_PREFIX_PATH` to your Qt):

```
cmake -S tools/anfs -B build-anfs -G Ninja -DCMAKE_PREFIX_PATH=<qt-root>
cmake --build build-anfs --target anfs test_anfs_secrets
```

Then run the executables from `build-anfs` with the Qt `bin` folder on `PATH`,
`QT_PLUGIN_PATH` pointing at the Qt plugins folder and `QT_FORCE_STDERR_LOGGING=1`
(ctest sets these itself: `ctest --test-dir build-anfs -L anfs`).

- Output: `build-anfs\anfs.exe`. In the PlayPlay tree the targets are `EXCLUDE_FROM_ALL` and land in
  `<build>\anfs\`; `PLAYPLAY_WITH_ANFS` defaults OFF in the main `build` folder and ON in `build-*` trees (see the root README).
- New `.cpp` files are picked up by the `CONFIGURE_DEPENDS` globs; nobody needs to edit `CMakeLists.txt` to add a
  module or a test.

Test helpers and the harness (ANFS-F8):

- `tests/anfs/anfs_testkit.h` (namespace `anfs::testkit`, linked into every `test_anfs_*`): `TestServer` (Router +
  server on an ephemeral loopback port), `ModuleHost` (the in-process runtime: temp data dir, store, private
  registry, ModeManager; `add<M>("peer", 50)`, `write(ModuleHost::cfg("peer"))`, `fetch("/api/x")`), `RawClient`
  (byte-exact requests, `readResponse()` keeps the raw framing), `FakePeer` (canned answers, delays, aborts,
  captured requests), `MemProbe` (peak private bytes delta), `PortHolder`, `freePort()`, `TempDir`, `waitUntil`.
  Timers: `ManualClock` + `ScopedClock` from `core/clock.h`.
- `build-anfs\anfs_sim.exe`: `bridge --port 18081`, `peer --bridge http://127.0.0.1:18080 --port 18897`,
  `record --target http://127.0.0.1:18180 --out DIR`; refuses the live ports and every non-loopback host; the token
  comes from `--token-file` (never printed).
- `tools/anfs/test/harness/run-compat.ps1 [-DryRun | -SmokeOnly]`: guards (`lib-guards.ps1`), scratch configs,
  the C++ server on 18080 started and stopped by recorded PID, `report.md`. The scenario replay against the C#
  oracle is a stub until the W1 slices add `scenarios/*.json` (see `scenarios/README.md`).

Two traps when a Qt Test carries JSON literals (both hit `test_anfs_core_json`):

- MSVC's traditional preprocessor mangles a raw string literal holding `\"` or `\u` inside a macro argument
  (`QCOMPARE(x, R"("a\"b")")` fails with C2017/C3850). Put such literals in a local first.
- moc's tokenizer loses track of raw strings that contain quotes and then reports "No relevant classes found" (link
  errors for `metaObject`). Keep only slot declarations in the `Q_OBJECT` class and define the bodies below it.

## Core rules for module code (ANFS-F1b)

- **Time:** every protocol timer, expiry and id timestamp asks `anfs::clock()` (`nowMs()`, `monoMs()`,
  `clock().waitFor(cv, lock, ms, pred)`), never `std::chrono`/`Sleep` directly, so tests install a `ManualClock` with
  `ScopedClock` and advance time instantly. Socket timeouts and short I/O retries stay real time.
- **Config:** read `store.current()` (an immutable snapshot) per use; write only through `store.update(mutate)`,
  whose mutate may run more than once. Module keys live in `raw` (`rawStr/rawBool/rawInt`); unknown keys survive.
  The `ConfigWatcher` reloads within one 2 s interval after another writer replaces `anfs.json`.
- **Log:** `log.state()` for state changes, `warn`/`error` for problems, never one line per request. Calls never
  block (queued; a stuck stdout only drops console lines). `registerSecret()` every token or password the module
  holds; lines also pass `Log::redact()`. Files: `<data>\logs\anfs-YYYYMMDD.log`, 14 kept.
- **Services:** `svcSet<IFoo>(p)` in `start()`, `svcRemoveIf<IFoo>(p)` in `stop()`, consumers `svcGet<IFoo>()` per
  call and tolerate nullptr.

## Paths, files and content types (ANFS-F2a)

- **Paths:** `nodepath::*` is Node 22's `path.win32` on UTF-8 (`resolve`, `join`, `basename`, `extname`, ...).
  Compare paths only through `nodepath::key()` (resolve + .NET invariant lower case) and `isUnder(rootKey, key)`.
  `safeRelDir()` and `conflictName()` are agent.js's rules (`name (N).ext`).
- **Files:** `fs::*` takes UTF-8 paths; paths of 240+ characters get `\\?\` internally; an embedded NUL is refused.
  `listDir()` keeps the raw file-system order (never sort a listing); entries carry `isReparse`, and walkers must
  not descend into those. `removeFile()`/`removeTree()` delete links, never their targets. `removeTree()` refuses
  a drive or share root in every spelling Win32 resolves to one (`C: `, `\\?\C:`, `//?/UNC/srv/share/`) and any
  device path without a drive or share form (`\\?\GLOBALROOT\...`, `\\?\Volume{...}\`); inside the tree it deletes
  entries by their exact names (`report `, `nul`). `makeDirs()` never loops: a middle name Win32 cannot make as typed
  (`Photos /2024`, `x/.../y`) or a link whose target is gone is ENOENT. Every `err` text is Node's (`ENOENT: no such file or directory, open 'C:\x'`),
  so a 500 body `{"error": err}` reads like Node's `e.message`; `fs::nodeError()` builds the same text.
- **Content types:** `mime::forPath()` / `forExtension()` = the `mime-types` lookup agent.js and the bridge call
  (preview-media adds `.mpg` -> `video/mp4` itself); `mime::contentTypeForFile()` = what Express `res.sendFile` /
  `res.download` send (mime 1.6 table plus `; charset=UTF-8`), the default of `http::sendFile`.

## Streaming ZIP (ANFS-F2b)

`ZipWriter` (`core/zipstream.h`) replaces archiver (`/api/download-dir`, zip-send) and `Compress-Archive` (peer
package). It never seeks the sink, so it writes straight into a chunked response or a pipe:

- Walk a tree as download-dir does: `addDirectory(prefix + name, mtimeMs)` for folders (empty ones survive), skip
  `isReparse` entries, open files with `fs::FileReader` (its `length()` is the file size; 3.75 GiB or more gives the
  entry a zip64 local header) and `addFile(prefix + name, reader, mtimeMs)`. Names use `/`; the writer drops `..`,
  `.` and empty segments, drops every `X:` prefix until a segment is kept (`./C:/x`, `C:/D:/x`, `d:x` -> `x`) and
  replaces invalid UTF-8, so names cannot escape the extraction folder.
- Unreadable files: `addFile` reads up to 64 KB before writing anything. An `IoError` with `bytesWritten()` unchanged
  **and neither `peerGone` nor `timeout` set** means the file failed there and the archive is intact: skip the file
  and go on (the C# port skipped such files). `peerGone` / `timeout` (a cancel, a stalled peer or network source)
  abort the transfer even with `bytesWritten()` unchanged; skipping them would skip every remaining file and finish
  a truncated archive as a success. If `bytesWritten()` moved, the writer is broken (source failed mid-entry, client
  gone, cancel): let it propagate and abort the transfer. Every later call throws, so a damaged archive can never be
  finished.
- Already-compressed media (`.mp4`, `.jpg`, `.zip` ...) can use `Method::Store` to save CPU; Node's archiver deflated
  everything at level 5 (the default here). Deflate needs `ANFS_HAVE_ZLIB` (conda `zlibstatic.lib`); without it
  every entry is stored.
- Memory is flat: two 64 KB buffers, one zlib stream, and the central directory (about 50 bytes + name per entry).
- Readers checked by `test_anfs_core_fs`: Expand-Archive in PowerShell 7 and 5.1, .NET's `ZipFile`, bsdtar
  (`tar.exe`); the heavy slot `zipZip64Over4GiB` (stored 4 GiB + 1 MiB entries, `ANFS_HEAVY_TESTS=1`, about 9 GB free
  next to the test exe or in `ANFS_HEAVY_DIR`, run under the machine-wide build/test lock).

## Database (ANFS-F3)

`core/db` wraps the vendored SQLite 3.54.0 (`third_party/sqlite/`, built as `anfs_sqlite`): one writer connection
behind a lock plus 4 read-only WAL readers. Every connection: WAL, `synchronous=NORMAL`, `foreign_keys=OFF` (Node
parity), `busy_timeout=5000`, `mmap_size=256MB`, extended result codes, double-quoted strings refused (`DQS=0`).

- **Writes:** `db.tx([&](DbConn& c) { ... })` (BEGIN IMMEDIATE ... COMMIT, ROLLBACK when the callback throws). Keep
  each transaction under about 250 ms: ingest in batches of 2,000 rows per tx (CONVENTIONS.md section 1). A prepared
  insert in a loop: `Stmt& s = c.prepare(sql);` then per row `s.bind(..).bind(..); s.run(); s.reset();`.
  `db.write()` holds the writer without a transaction (pragmas, VACUUM); `db.exec/query/scalar` are one-statement
  writer shortcuts and may be called inside `tx()`.
- **Reads:** heavy or frequent reads (stats, media pages, search, feeds, UI polling) go through `db.read()` /
  `db.queryRead()`, which never wait behind an ingest. Inside `tx()`/`write()` on the same thread, `read()` uses the
  writer and so sees the open transaction.
- **Rows** are `JVal` objects in SELECT column order (BLOBs as string bytes); `exec()` returns the changed row count
  (0 when nothing changed). Errors are `DbError` (`what()` is SQLite's text, `code`, `extendedCode`).
- **Statements:** `prepare()` is cached per connection (64); a `Stmt&` from `prepare()` is never evicted before the
  outermost callback on its connection returns, so it stays valid for the whole callback. A transaction a callback
  opens by hand and leaves open is rolled back when that callback returns; use `tx()`. Use `'?'`
  parameters, single-quoted literals, one statement per `exec(sql, params)`; scripts go through `execScript()` (or
  `exec(sql)` without parameters, as C# `Db.Exec` did).
- **Schema:** `ModSchema::apply(db, "<module id>", steps)` at module start; `steps[i]` brings version i to i+1, each in
  its own transaction with the `_mod_schema` bump (byte-identical to C# `FsCoreSchema.cs`, so the C# and C++
  binaries share one `anfs.db`). Copy `CREATE` statements verbatim from the C# store files; never edit an applied
  step, append one.
- **Importer:** `Db::openCopyReadOnly(path)` refuses a database whose `-wal` changed in the last 60 s (a live one).
  It never writes the copy's database or `-wal` bytes. A copy with no `-wal` opens immutable (no `-wal`/`-shm` is
  created, so it can be reopened at once); a copy with a stale `-wal` may get a `-shm` (SQLite's WAL index) next to it.

`test_anfs_core_db` checks all of this, and loads the C# oracle `anfs.exe` into Windows PowerShell 5.1 to show that
both binaries read each other's `_mod_schema` and can use one file at the same time (skipped when the oracle is
missing; `ANFS_CSHARP_ORACLE` names another copy).

## HTTP server (ANFS-F4a)

`core/http_server` + `core/http_ctx`: Winsock2, blocking sockets, one worker thread per connection from an elastic
pool (512 max, 256 KB stacks, 30 s idle expiry; above the cap the accept thread answers 503 `{"error":"Server busy."}`).
Reply bytes follow Node 22 / Express (header order, `Date`, `Connection`, `Keep-Alive: timeout=5`, reason phrases).

- **Handlers** are straight-line blocking code. Throw `HttpError(status, text)` for `{"error":text}`; any other
  exception is logged and answered 500 `{"error":"internal error"}` (never put secrets in exception texts: they reach
  the log). An exception after the headers went out resets the connection. A handler that returns without replying
  gets a 500 (logged).
- **Bodies:** `ctx.readJson(limit)` (Express `json()`), `ctx.body.readAll(limit)` / `copyTo(sink, limit, progress)`
  (413 `{"error":"request entity too large"}`; a declared length past the limit is refused before reading),
  `ctx.body.trailers()` after a chunked body. `IoError{peerGone|timeout}` from a body read means the client left or
  stalled: let it propagate (nothing is logged). An unread body up to 1 MB is drained for keep-alive, a larger one
  closes the connection. `Expect: 100-continue` is answered on the first body read only.
- **Replies:** `ctx.json`, `ctx.error`, `res.send/sendText/sendEmpty/redirect`, or streaming `res.begin(status, len)`
  (`-1` = chunked; HTTP/1.0 then closes) + `write`/`writeFile`/`sink()` + `end()`. Headers go out with the first body
  bytes. HEAD runs the GET handler and sends no body. A header value with CR/LF/NUL is refused (500).
- **TransmitFile** is used by `writeFile` only on server editions of Windows: client editions run at most two at a
  time machine-wide and queue the rest. Elsewhere it copies with 64 KB positioned reads. `ANFS_HTTP_TRANSMITFILE=1|0`
  forces it (tests).
- **Timeouts** (inactivity, per phase): header 30 s, keep-alive idle 5 s, body read and write 120 s; no total limit.
- **Protocol errors** get Node's bare replies (`HTTP/1.1 400 Bad Request` + `Connection: close`, no body): bad request
  line, lower-case method, missing Host on 1.1, repeated Content-Length, Content-Length with Transfer-Encoding, a
  coding other than chunked last, obs-fold, control characters; 431 past 64 KB of headers; 505 for HTTP/2.0 and up;
  417 for an `Expect` other than `100-continue`.
- **stop():** new requests get 503 while in-flight ones finish (`drainOnStopMs`); then listeners close and idle
  connections are woken (CancelIoEx + shutdown) at once. Modules must release parked long polls first.
- `core/http_internal.h` is the private glue between the two files (per-request scope); modules never include it.

## Router and SendFile (ANFS-F4b)

- **Router** (`core/http_router`, C# `PathPattern.Match`): literal segment 3 (percent-decoded, ASCII case-insensitive),
  `{name}` 2, `{*name}` / trailing `*` 1 (the rest, may be empty); an exact method +1 (beats `*` and HEAD-runs-GET);
  best score wins, **a tie goes to the route registered first** (so `ANFS_MODULE` order matters). `kRouteFallback`
  routes only when no regular route matches. No 405: an unmatched method reaches `setFallback` / the default 404
  `{"error":"Not found"}`. Middleware (`use`, `useFirst`) runs after the route is resolved, so a gate sees
  `ctx.route->open()` / `agentExempt()`; a middleware that does not call `next()` must answer. The table is
  copy-on-write: a request keeps the table it started with; `removeOwner(module)` drops routes and middleware.
- **`sendFile(ctx, path, opts)`** = Express `res.sendFile` / `res.download` (`opts.downloadName`) byte for byte
  (checked live against express 4.21 / send 0.19 in `test_anfs_core_http`): weak ETag `W/"<size hex>-<mtime ms hex>"`
  with Node's rounded mtime, `Last-Modified`, `Accept-Ranges`, `Cache-Control: public, max-age=0`, mime 1.x type
  unless the handler set `Content-Type` first. Returns false (nothing sent) for a missing path or a directory: write
  the module's own 404 text. Dot-segment refusal (send's dotfiles) is the caller's job.
  - Range follows range-parser: one range (after merging overlapping/adjacent parts) = 206; several = 200 full;
    nothing satisfiable = 416 (`bytes=5-1` too). If-None-Match / If-Modified-Since are checked first (304 even with
    Range); a stale If-Range gives 200. If-Match / If-Unmodified-Since failures = 412.
  - 412 / 416 are Express's finalhandler page (production form, CSP + nosniff headers).
  - The body goes through `Response::writeFile` from the same handle the size and mtime came from.
- `parseRange`, `weakEtag`, `parseHttpDate` (Date.parse for HTTP date forms, ISO via `json::parseIsoMs`) and
  `contentDisposition` (the content-disposition module: `filename="<latin1 fallback>"`, `filename*` only when needed;
  a lone surrogate throws, as encodeURIComponent does) are public helpers.

## HTTP client (ANFS-F5)

`core/http_client`: outbound HTTP on WinHTTP (an OS component), blocking calls on the caller thread over asynchronous
WinHTTP handles so a request can be cancelled from another thread.

- **`HttpClient`** holds one WinHTTP session: `WINHTTP_ACCESS_TYPE_NO_PROXY`, no User-Agent (Node sends none),
  redirects never followed (a 3xx is an ordinary response with its `location`), cookies and automatic authentication
  off, unlimited connections per server. Keep-alive connections are pooled, so a peer's 5 s register reuses one TCP
  connection. `HttpClient::shared()` is the process-wide instance; a `ClientResponse` keeps the session alive.
- **`send(ClientRequest)`** returns once the response headers arrived; any status is a normal response, transport
  failures throw `ClientError{winError, timeout, cancelled}` (an `IoError`). `fetch(method, url, headers, body, idleMs,
  maxBody)` never throws (`status 0` + `error` text).
- **Request bytes** (pinned by the raw-capture test): request line, WinHTTP's own `Connection: Keep-Alive`, the
  caller's headers in order, the framing header, `Host: host[:port]` last. WinHTTP moves the headers it knows (Accept,
  Accept-Encoding, Authorization, Cookie, If-None-Match, Referer, Range, User-Agent) to the front in canonical case;
  receivers do not depend on order or case. A caller `Host`, `Content-Length` or `Transfer-Encoding` is dropped and
  computed instead; CR/LF/NUL in a header is refused before anything is sent (the value is never echoed). Header
  values are sent as the bytes given (latin1 bytes >= 0x80 included); the path is sent as given with bytes outside
  0x21..0x7E percent-encoded; the query is never put in an error text (it may carry `auth=`).
- **Bodies:** `body` + `bodyLength >= 0` sends `Content-Length` (over 4 GiB works); `bodyLength = -1` writes chunked
  framing itself. No body: POST/PUT/PATCH send `Content-Length: 0`, GET/HEAD/DELETE/OPTIONS send no length (Node).
  Bodies stream 64 KB at a time from the `Source` and into `ClientResponse::read()`; a 2 GiB upload keeps private
  memory flat. A source that throws aborts the request and its exception reaches the caller unchanged; one that ends
  before its length is an error.
- **Responses:** header names lower-cased, values trimmed, duplicates kept in order (`headers().joined(name)` gives
  Node's `, ` form); `contentLength()` is -1 for chunked or close-delimited bodies. A body that ends before its
  Content-Length throws (never a silent EOF). HEAD / 204 / 304 / 1xx have no body. At the end of the body the request
  handle is closed at once, handing the connection back to the pool; `close()` early drops the connection.
- **https:** certificate errors ignored (unknown CA, name mismatch, dates, usage: Node `rejectUnauthorized:false`),
  no client certificate, no revocation check.
- **Timeouts** are inactivity timeouts: resolve 10 s, connect 10 s, then `idleMs` (30 s default) between bytes in
  either direction; a transfer may last hours while bytes keep moving. `idleMs = 0` disables the idle limit. The idle
  clock is the client's own (WinHTTP's receive timeout is not used: it would make WinHTTP retry a silent request on
  fresh connections for seconds); on expiry the request handle is closed and `ClientError{timeout}` thrown.
- **Cancel:** `ClientRequest::cancel` points at a `CancelToken`; `cancel()` from any thread wakes every request
  using the token at once (`ClientError{cancelled}`), also mid-upload while the socket is full. The token is a
  manual-reset event, so it also refuses a request started after the cancel. Only the owner thread closes handles,
  and it waits for WinHTTP's HANDLE_CLOSING before the per-request buffer is freed.

## Network classes, WAN gate and CORS (ANFS-F6)

`core/net`, `core/gate`, `core/cors`: who a client is, whether it needs a password, and which Origins get credentials.

- **Classes** (`net::classify`, C# `Net.Classify` / wan-gate.js `kindOf`): loopback 127/8 and `::1`; LAN 10/8,
  172.16/12, 192.168/16, fc00::/7, fe80::/10; tailnet 100.64/10; everything else WAN. IPv4-mapped IPv6 is
  normalised first, and anything unparsable is WAN (fail closed). `IpAddr::parse` takes `1.2.3.4`, `1.2.3.4:80`,
  `[::1]:80`, `fe80::1%3` and quoted forms; `matches` takes an address or a CIDR.
- **Real client** (`net::realClient`): `X-Forwarded-For` is honoured only when the TCP peer is in
  `wan.trustedProxies`, and then only its LAST hop (the one our own proxy appended). An unparsable last hop is
  `IpAddr::none()` = WAN. The runtime wires `ServerOptions::resolveClient` to `realClient` + `classify`; until it
  does, the server marks every client WAN and the gate gates everything rather than nothing.
- **LanOnly** (`net::isLanSource`): loopback, 10/8, 172.16/12, 192.168/16, 169.254/16, `::1`. The tailnet is NOT
  allowed (Node parity). `isLoopbackHost` (test-mode bridge guard): `localhost`, 127/8, `::1`.
- **Gate** (`Gate::decide`, C# `Gate.Decide`): non-WAN -> allow; not a `wan.gatedPaths` rule (HEAD matched as GET) ->
  allow; `wan.requireTls` and plain -> 403; an agent-exempt route with the `x-agent-auth` token -> allow (the
  `?auth=` form is never honoured here); a live lockout -> 429 + `Retry-After`; no password record -> 401 (fail
  closed; the server still starts); no / malformed Basic -> 401; a cached header -> allow; else PBKDF2 outside the
  lock. A hit is cached 10 min per client, keyed by HMAC-SHA256(per-process key, header + client), never by the
  header. The 20th failure within 10 min locks that client out for 10 min (the right password is refused too; other
  clients and ungated paths are unaffected; a success clears the count). Times come from `anfs::clock()`.
- **Middleware** (`Gate::middleware`, Node wan-gate.js texts): open-flagged routes pass untouched; denials are
  `text/plain; charset=utf-8` with `Cache-Control: no-store`, `Authentication required.\n` + `WWW-Authenticate: Basic
  realm="AN-FS", charset="UTF-8"` (401), `HTTPS is required for remote access.\n` (403), `Too many failed attempts.
  Try again later.\n` + `Retry-After` (429). A refused request with a body closes the connection instead of draining
  the upload. Denials other than a missing credential are logged (class, ip, method, path, reason; never the header).
- **Records** (`makePasswordRecord`, `verifyPassword`, `pbkdf2Sha256` on `BCryptDeriveKeyPBKDF2`): algo
  `pbkdf2-sha256`, 600,000 iterations, 16 random salt bytes, 32-byte key, base64 (`anfs hash-password`). Verify also
  takes the 64-hex hash + hex salt form; iterations above 20,000,000 are refused without deriving; the compare is
  constant-time. Records from the C# `anfs.exe hash-password` and from Node `wan-gate.js makeRecord` verify here.
- **CORS**: `cors::peerMiddleware` (agent.js) reflects any Origin with `Access-Control-Allow-Credentials: true`,
  else `*`; OPTIONS answers `200 OK`. `cors::serverMiddleware` (wan-gate.js `corsMiddleware` + deviation D1)
  reflects with credentials only for `example.invalid` and subdomains, `localhost` / `*.localhost`, hosts that
  are non-WAN IPs, and `config.corsAllow` entries (`https://host[:port]`, `host`, `*.suffix`, `host:port`; a
  default port is never part of an origin); otherwise `*` without credentials; `Vary: Origin` whenever an Origin was
  sent; OPTIONS answers 204 with no body. Both set `Allow-Methods: GET, POST, PUT, DELETE, OPTIONS` and
  `Allow-Headers: Content-Type, Authorization, x-agent-auth, x-upload-id, x-chunk-index`. Install with
  `Router::useFirst` so the preflight answers before the gate.

## Runtime: registry, ModeManager, status, CLI (ANFS-F7)

`anfs.exe run` (src/main.cpp) wires the foundation into the service loop; `core/module.cpp` and `core/status.cpp`
implement the frozen `module.h` / `status.h`.

- **Registry.** `ANFS_MODULE(Class, "modes", order);` at the bottom of a module's `*_module.cpp` registers a factory
  in `ModuleRegistry::global()`; `create(mode)` returns fresh instances of the common modules plus the mode's own,
  sorted by `(order, seq)`. Mode `off` creates nothing. Tests build their own `ModuleRegistry` with only the modules
  under test and hand it to a `ModeManager`.
- **ModeManager.** One generation per `mode|effectivePort|bindAll|bindAddress`. On a key change the old generation
  stops (modules in reverse start order, 2 s budget each, a slow stop is logged; then the server, then the database),
  the new server is bound first (a bind failure costs no module start), then `temp_uploads[_agent]` / `cache` are
  created, `anfs.db` is opened (`ctx.db` is null when that fails; modules still start) and the modules start in
  order. A module whose `start()` throws is recorded as `{"error": text}` under `modules.<name>` and the others
  continue. A bind failure rolls back by rebuilding the last good configuration, keeps `lastError`, leaves
  `appliedRev` at the old rev and sets `failed`; `tick()` (once a second from the run loop) retries every 30 s
  (`setRetryMs`). A same-key edit restarts nothing: modules read `store.current()` per use, and `appliedRev`
  moves at once. Test-mode guards (`test:true`, deviation C7) run before anything else: ports 8080/8899/443 (and
  `tlsPort` 443) are refused, and in peer mode a bridge URL whose host is not loopback is refused unless
  `allowRemoteBridge:true`; a refused config leaves the running generation alone and records `lastError`.
- **Context wiring.** `ServerOptions::resolveClient` = `net::realClient(peer, X-Forwarded-For, wan.trustedProxies)`
  + `net::classify`. In **server** mode the runtime installs `cors::serverMiddleware()` and then
  `Gate::middleware()` with `useFirst` (CORS runs first, owner `"runtime"`): **srv-wan seeds `wan.gatedPaths`,
  `wan.trustedProxies` and `corsAllow` and installs no second gate** (two gates would double-count lockout
  failures); `ctx.gate` is the live instance for `isLockedOut` / `reset` / status. In **peer** mode the runtime
  installs nothing: pfs-setup adds the peer CORS and the agent-token check.
- **status.json** (`StatusWriter`, every 2 s on `anfs::clock()`, atomic, plus `writeNow()` after every apply):
  `mode, version, ver, pid, nodeId, name, port, statusPort, listening, loopbackOnly, startedAt, updatedAt, uptimeSec,
  uptimeS, rev, appliedRev, bridge, modules, lastError, lastErrorTs, configError, generation, gate, db, peers,
  config`. `bridge` comes from `IBridgeClient` (null without one), `peers` from `IAgentRegistry` (absent without
  one), `db` while a generation runs and the file exists, `gate` is `"foundation"`, `config` is `toSafeJval()`.
  `set(key, value)` adds extra top-level keys (null removes). A failed write is logged once (Warn) until the next
  success. `anfs status` reads the file and adds `ageSec` and `running` (pid alive and the file younger than 20 s);
  exit 1 when not running.
- **Status endpoint.** With `statusPort > 0`, `127.0.0.1:<statusPort>/status` (and `/`) answers the same document
  over loopback, read-only, no gate (mgmt-integration.md 1.1); it stays up in mode off and rebinds when the port
  changes (`statusPortError` in the status when it cannot bind).
- **Single instance.** `Globalnfs-<16 upper hex of SHA-1(UTF-8(lower-invariant(dataDir)))>` (C# Program.cs:137
  formula, so the C# and C++ binaries exclude each other on one data folder); the loser prints
  `anfs is already running for <dir>` and exits 3.
- **CLI** (exit codes 0 ok, 1 not running / failure, 2 usage or validation, 3 another instance):
  `run [--data DIR | --config FILE] [--verbose]`, `status [--data DIR]`, `version [--verbose]`,
  `hash-password [--iterations N]` (ANFS_PASSWORD, >= 8 characters, 600,000 iterations by default, never echoed),
  `config get`, `config set k=v ...` (typed keys keep their type, `bridgeFallbacks` / `corsAllow` /
  `wan.trustedProxies` / `wan.gatedPaths` split on commas, `folders.<k>` and dotted module keys go to `raw`,
  other values are JSON when they parse), `config set-wan-record <base64 JSON record>`. `config set` validates
  (`Config::validate`, ports 0 or 1024-65535, the test-mode guards), bumps `rev` and writes atomically under a
  cross-process writer mutex (`Globalnfs-cfg-<hash of the path>`), so parallel writers never lose a bump; `run`
  takes the same mutex for its start-up nodeId fill. `import`, `install`, `uninstall`, `selftest`, `peer-legacy`
  and `shell` parse but answer `not implemented in this build` (exit 2) until their slices land.
- **Run loop.** Data folder created, mutex, `Log(<data>\logs)` with console echo (`--verbose` = Debug), a missing
  `anfs.json` gets defaults (mode off, fresh nodeId, the machine name), `ConfigWatcher` -> `ConfigStore::subscribe`
  -> `ModeManager::apply` + status endpoint rebind + `registerSecret(agentToken)`, `StatusWriter::start(2000)`,
  one `tick()` per second, Ctrl+C / console close -> watcher stop, `shutdown()`, a last status write, exit 0. The
  process stays alive and idle in mode off (StarRoot restarts anything that exits).

## Embedded UI

`anfs.exe` serves its pages from Qt resources compiled with `rcc --no-compress` (served bytes = source bytes):

- every `tools/anfs/*.qrc` (srv-ui's `anfs_ui.qrc`) and every `tools/anfs/ui*/*.qrc` is compiled into `anfs_obj`;
- a `tools/anfs/ui*/` folder that no qrc names gets a generated qrc in the build folder: each file becomes
  `:/<folder>/<relative path>` (for example `ui-peer/setup.html` is `:/ui-peer/setup.html`), the same path a root
  qrc entry `<file>ui-peer/setup.html</file>` under prefix `/` gives. Dot files are skipped.

`test_anfs_secrets` checks every embedded file: stored uncompressed, equal to the file at the same path under
`tools/anfs`, and free of secrets.

## Secrets

The Node agent token, the portal password and peer keys are never written into this repository (code, comments,
tests, fixtures, logs). `test_anfs_secrets` scans `tools/anfs` and `tests/anfs` for token-like literals; with
`--token-file <file>` (or `ANFS_COMPAT_TOKEN_FILE`) it also checks the exact token and its shape. The file must live
outside the repository. A build may embed the token from such a file (`-DANFS_COMPAT_TOKEN_FILE=...`) into
`<build>\anfs_generated\compat_token.h`, which stays in the build folder; otherwise `agentToken` comes from
`anfs.json` (`anfs config set agentToken=...`) and peer/server modes stay idle until it is set.

## Reserved test ports

| what | port |
|---|---|
| server (`test:true`) | 18080 |
| peer | 18899 |
| relay-only peer | 18898 |
| sim peer / sim bridge | 18897 / 18081 |
| C# oracle server / peer | 18180 / 18199 |
| TLS | 18443 |

Never used by tests: 8080, 8899, 443 (the test-mode guard refuses them), 7700/7701 (the PlayPlay Supervisor),
a real Node bridge (`192.168.1.177:8080`, `example.invalid` stand in for it) and a real Node peer on the same PC.
