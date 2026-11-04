# SQLite 3.54.0 amalgamation (vendored)

| file | SHA-256 |
|---|---|
| `sqlite3.c` | `2da161e310ef0aba3465f22a4d12b8a43723236f0942fa76e8aac523c3317243` |
| `sqlite3.h` | `cd20a2877bc82b94e36c1093b727dd87cfa3c1d581aac6e240925535cbd90166` |

- **Version:** 3.54.0, `SQLITE_SOURCE_ID` "2026-10-09 15:46:58 be8d059e9a49089ab2dce5ed26dd87aaf598fdc5fbe0b107c0dd758464bcd5e3".
- **Source:** copied unchanged from the original C# port's `native/src/` folder (the same files its
  `native\build-native.cmd` compiles into the `sqlite3.dll` that the C# `anfs.exe` embeds). Nothing was downloaded.
  `shell.c` and `sqlite3ext.h` are not needed and not copied.
- **License:** public domain; the authors disclaim copyright and give a blessing in place of a license (see the
  notices in `sqlite3.c` and https://sqlite.org/copyright.html). No attribution is required.
- **Do not edit these files.** To move to a newer release, replace both files from an official amalgamation, update the
  table above and `Db::sqliteVersion()` expectations in `tests/anfs/test_anfs_core_db.cpp`, and keep the file format
  compatible with the C# binary (any 3.x release is).

## How it is built

`tools/anfs/CMakeLists.txt` turns `sqlite3.c` into the static library `anfs_sqlite` (C, `/W0`), linked by `anfs_obj`.
Compile options:

| option | why |
|---|---|
| `SQLITE_THREADSAFE=1` | C# build; `core/db` opens every connection with `SQLITE_OPEN_NOMUTEX` and serializes use itself |
| `SQLITE_ENABLE_FTS5` | C# build; search tables (`index`, `knowledge`, `media-lib`) |
| `SQLITE_ENABLE_JSON1` | C# build (JSON is built in since 3.38; kept for parity) |
| `SQLITE_DQS=0` | C# build: a double-quoted string literal is an error, SQL uses single quotes only |
| `SQLITE_DEFAULT_WAL_SYNCHRONOUS=1` | extra: NORMAL in WAL mode (core/db sets it anyway) |
| `SQLITE_OMIT_DEPRECATED` | extra: drops APIs nobody here calls |

None of them changes the file format, so the C# and the C++ binaries open the same `anfs.db`
(`test_anfs_core_db` checks this against the C# oracle). `core/db` turns memory statistics off at start-up
(`SQLITE_CONFIG_MEMSTATUS`), so the reader pool does not contend on SQLite's global allocator mutex.

`-DANFS_SQLITE=conda` builds against the conda `sqlite3.dll` (3.50.4, FTS5 included) instead; it is a fallback for
experiments, not for releases.
