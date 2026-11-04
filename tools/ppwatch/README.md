# PlayPlayWatchdog.exe (ppwatch)

External, **report-only** watchdog for the playout stack (docs/plans/perf-supervisor-app.md §5.3, work item W19, lane
stability slice STB-6).

On 10-24 and 10-25 the Supervisor froze for hours. Nothing outside it noticed, and it left no evidence. STB-4 gave every
process its own `/healthz`, answered from a thread of its own while the main thread is frozen (Supervisor 7709, engines
on their `--port`). ppwatch polls those endpoints from outside. When one process hangs, ppwatch writes an incident bundle
and a minidump of **that process**. It also catches what the in-process watchdog cannot see: a process that is frozen as a
whole (suspended, or a loader-lock deadlock) and so cannot dump itself.

Not built by default (`EXCLUDE_FROM_ALL`): `cmake --build <dir> --target PlayPlayWatchdog`. It needs `Qt6Core.dll` and
`Qt6Network.dll` next to it or on `PATH`, as the other programs do. It is not packaged or installed yet.

## What it does

The cycle repeats every `--interval-ms` (default 2000):

1. It reads the TCP table once and finds the owner of each target's listening port. When nothing listens, the target is
   reported as `down` and no connection is made. On Windows a connect to a closed loopback port takes about 2 s.
2. It sends `GET /healthz` to every target at once (non-blocking sockets, one `select`). A frozen process therefore costs
   one timeout (`--timeout-ms`, default 1500) per cycle, not one per target. Probes that time out are reset, so a hung
   server's accept queue does not fill with them.
3. It runs a state machine per target:

   | observation | state |
   |---|---|
   | answered | the body's `state`: `ok`, `stall`, `hang` or `exiting`. 2xx without a `state` field = `ok`, other status = `error` |
   | no answer, listener still there, `--unresponsive-polls` (2) times in a row | `unresponsive` |
   | nothing listens | `down` |
   | the pinned pid has exited | `gone` |

   `hang` and `unresponsive` form a **hang episode**. Its first poll opens **one incident**. The episode ends on any
   other state, or when a different process answers on the port.
4. Every 5th cycle it discovers engines (`--discover-engines`). Every 15th cycle it reports orphans (`--report-orphans`).

### Who it acts on (identity)

ppwatch dumps, and with the restart opt-in terminates, only the **owner of the configured port's listener**, and only
when the configuration confirms that process:

* `pid=` pinned: the listener must be that pid, with the same create time (pid reuse is caught); or
* `exe=`: the listener's image must have that name (role `supervisor` defaults to `PlayPlaySupervisor.exe` and role
  `engine` to `PlayPlayChannelEngine.exe`); or
* the `/healthz` body names the listener's own pid (now, or in an earlier answer from the same process).

Two more rules apply:

* If the body names a different pid than the port owner, nothing is done.
* System and session processes are never touched (`csrss`, `lsass`, `svchost`, `explorer`, `conhost` ...), and neither
  is ppwatch itself.

The dump and the kill run through a process handle whose create time and image are checked again just before the
action. A refusal is logged with its reason (`"event":"refused"`). Hosts other than loopback (`127.0.0.1`, `localhost`,
`::1`) are refused when the configuration is read.

### Incident bundle

Each incident is written to `<out>/incidents/<yyyyMMddTHHmmssZ>-<target>-<pid>/`:

| file | what |
|---|---|
| `incident.json` | summary sentence, trigger, identity checks, last `/healthz` body, last 30 polls, process facts (parent, threads, handles, private MB, CPU), thread-state counts, the first thread's state, file list |
| `threads.txt` | every thread: state and **wait reason** (`UserRequest`, `DelayExecution`, `Suspended`, `WrLpcReply`, `WrAlertByThreadId` ...), CPU, context switches, Win32 start address as `module+0x...`, thread name. Read with `NtQuerySystemInformation`, nothing attached. Taken before the dump |
| `<target>-<pid>-<utc>-hang.dmp` | `MiniDumpWriteDump` of that process: thread info, unloaded modules, indirectly referenced memory, handle data. DbgHelp pauses the target's threads while it copies them, about 0.3 s for the test Supervisor |
| `netstat.txt` | TCP rows on `--ports` (station: 7700, 7701, 7709), every target's health port, every socket of the target pid, and a per-port count by state |
| `processes.txt` | the `PlayPlay*` processes, the configured images, and the target's parent and children. Shows their parents, create times and command lines (credential-like options masked), plus the orphan check |
| `logs/<file>.tail.txt` | the last `--tail-lines` (500) lines of the newest file matching each target log glob and each `--log` glob. Files are opened with full sharing |

Limits: at most `--max-incidents-per-hour` (6) incidents and `--max-dumps-per-hour` (2) dumps per target. Both are
counted from the folders on disk, so they survive a ppwatch restart. Only the newest `--keep-incidents` (50) incident
folders are kept, and only folders named like ppwatch's own are pruned.

### Restart (opt-in, off by default)

`--restart-hung-supervisor-after <sec>` applies only to a `role=supervisor` target whose `exe=` matches. Once per hang
episode, after the episode has lasted `<sec>`:

1. It waits until that episode's incident and dump are on disk.
2. It probes once more. If the target recovered, or another process owns the port, nothing is done.
3. It terminates the process with exit code `0xDEAD` (the same code as the in-process abort policy).
4. With `--restart-command "<command line>"`, it starts that command with no console, NUL stdio and no inherited
   handles.

There are at most 3 restarts per target per hour.

Leave `--restart-command` out while the operator app runs: the app's SupervisorClient relaunches a missing Supervisor on
its own. Until the orphan-engine adoption (W5, perf-supervisor lane) is in, a restarted Supervisor starts **new** engines
while the old ones are still playing (NDI name and port clashes). So keep restart off on air.

### Engines and orphans

* With `--discover-engines`, every `PlayPlayChannelEngine.exe` that is a child of a verified supervisor target becomes a
  target `engine-<id>`. Its URL comes from the `--port` in its command line, and its logs come from its `--log-dir`. It
  is watched and gets incidents, but it is **dumped only with `--dump-engines`**: a dump pauses all of an engine's
  threads, its clock thread included.
* With `--report-orphans`, ppwatch reports engines whose parent has exited, whose parent pid now belongs to a newer
  process, or whose parent is not `PlayPlaySupervisor.exe`. Each one is logged once as `"event":"orphan"`. It never
  touches them. `--standalone` engines are not reported.

## Usage

```text
PlayPlayWatchdog --station                         # Supervisor :7709 + its engines, live log folders,
                                                   # incidents in %APPDATA%\PlayPlay\PLAY PLAY\supervisor\logs\incidents
PlayPlayWatchdog --target "name=supervisor,url=http://127.0.0.1:7799/healthz,role=supervisor,logs=C:\x\logs\supervisor-*.log;C:\x\logs\watchdog-supervisor-*.jsonl" --out D:\ppwatch
PlayPlayWatchdog --once --target "url=http://127.0.0.1:9001/healthz"     # one look, JSON on stdout, exit 0 = all ok/stall
PlayPlayWatchdog --station --print-config > ppwatch.json                 # effective configuration (polls nothing)
PlayPlayWatchdog --config ppwatch.json                                   # the same shape back
```

Target spec keys: `name`, `url`, `pid`, `exe`, `role` (`supervisor` | `engine` | `app` | `other`), `logs` (globs
separated by `;`), `dump` (`0`/`1`). More options: `--interval-ms`, `--timeout-ms`, `--unresponsive-polls`, `--log`
(repeatable), `--ports`, `--engine-exe`, `--supervisor-exe`, `--tail-lines`, `--run-for`, `--data-dir` /
`--app-log-dir` / `--station-health-port` (station preset). `--help` lists them all.

**Output.** While watching, ppwatch never writes to the console (a paused console must not stop it, the failure this
lane removes). Its events go to `<out>/ppwatch-YYYY-MM-DD.jsonl`, pruned after 30 days: `start`, `state`, `hang`,
`incident`, `refused`, `skipped`, `restart-due`, `restart`, `discover`, `forget`, `orphan`, `prune`, `stop`. Only
`--once`, `--print-config`, `--help` and start-up errors print.

**Exit codes.** 0 = stopped normally (Ctrl+C, Ctrl+Break, console close, `--run-for`); `--once` returns 1 when a target
is not ok/stall. 2 = bad arguments. 3 = another ppwatch already writes to the same `--out` (named mutex).

**Running it.** It is a console program. To run it unattended without a window, start it hidden: a Task Scheduler task
set to "run whether the user is logged on or not", or WMI (`Win32_Process.Create` with `ShowWindow=0`). How
and when to install it on the playout machine is the operator's decision. Nothing here installs it.

## Verified (2026-10-26)

* `tests/test_ppwatch.cpp`: 11 functions (13 with init/cleanup), about 31 s. Covers the probe, tracker, snapshots, text
  helpers, incident folders and a direct bundle, plus three runs of the exe against `tests/helpers/hang_helper`:
  * a blocked main thread gives a `hang` incident;
  * a process suspended as a whole gives an `unresponsive` incident;
  * a hung process reachable only through a target pinned to another pid is refused;
  * the dump file and the pid recorded inside each dump belong only to the configured processes;
  * nobody is killed by default;
  * with the opt-in restart, the incident comes before the kill, the exit code is 0xDEAD, an unconfigured process that
    hangs at the same time is left alone, and the restart command runs.
* The integration run against the lane test Supervisor (`/healthz` on 7799) is described in
  `docs/plans/impl-log/stability.md` (STB-6).
