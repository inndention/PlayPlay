# Compatibility scenarios (`run-compat.ps1`)

One JSON file per scenario, replayed by `run-compat.ps1` against the C++ build on `127.0.0.1:18080` and, when the
C# reference is given (`-CsExe`), against it on `127.0.0.1:18180`; the two answers are normalised and diffed
(anfs-cpp.md section 7, layer 3). ANFS-F8 ships the frame only: the runner lists the files and reports the replay
as `skipped` until the W1 slices (fscore, pfs-browse, srv-registry) land their first scenarios.

## File shape

```json
{
  "id": "srv-registry-nodes",
  "needs": ["srv-registry"],
  "steps": [
    {
      "name": "register",
      "method": "POST",
      "path": "/api/agents/register",
      "headers": {"content-type": "application/json", "x-agent-auth": "$TOKEN"},
      "body": {"agentId": "$AGENT_ID", "name": "$HOST-sim", "ip": "127.0.0.1", "port": 18897},
      "expect": {"status": 200, "keys": ["ok", "id"], "headerNames": ["Content-Type", "Content-Length", "Date", "Connection"]}
    },
    {
      "name": "nodes",
      "method": "GET",
      "path": "/api/nodes",
      "expect": {"status": 200, "text": null, "bodySha": null, "keys": ["nodes", "ts"]}
    }
  ]
}
```

- `needs`: module ids that must be running; the runner skips the scenario when the status document lacks one.
- `headers` / `body` / `bodyFile`: the request. `bodyFile` is relative to `fixtures/` in the scratch folder.
- `expect.status`: exact. `expect.keys`: the ordered top-level keys of a JSON body (order matters: the Node key
  order is the contract). `expect.headerNames`: the response header names in wire order. `expect.text`: the exact
  body text. `expect.bodySha`: SHA-256 of the body (binary answers). Any absent field is not checked.
- Placeholders: `$TOKEN` (from `-TokenFile`, never written into a scenario or a report), `$AGENT_ID`, `$HOST`,
  `$PORT_CPP`, `$PORT_SIM_PEER`, `$SCRATCH`.
- Normalisation before the C++ vs C# diff: timestamps (`ts`, `updatedAt`, `startedAt`, `Date` header), ids
  (`agentId`, md5 node ids), host names (the `-cpp` suffix, C7) and port numbers are replaced by `<ts>`, `<id>`,
  `<host>`, `<port>`.

## Rules

- Never a token, a password or a real peer key in these files; `test_anfs_secrets` scans this folder.
- Every scenario runs on the reserved test ports (`tools/anfs/README.md`); none names a host other than `127.0.0.1`.
- A scenario that mutates state (shares, uploads) cleans up in its last steps so the replay is repeatable.
