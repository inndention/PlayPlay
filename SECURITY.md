# Security policy

PLAY PLAY is pre-1.0 software written for one small television station. There are no supported releases yet; fixes go
to the `main` branch.

## Reporting a vulnerability

Please report security problems **privately** through GitHub's security advisories ("Security" tab of the repository,
"Report a vulnerability"). Do not open a public issue or pull request for a vulnerability. Include what you found, how to
reproduce it and which commit you tested. This is a small volunteer-run project, so there is no formal response-time
guarantee.

## Things to know before running it

- **The Supervisor's REST (port 7700) and WebSocket (port 7701) servers listen on all network interfaces, and with no
  `rest.token` setting the API is open to every client that can reach them.** The API controls playout, files and
  settings. Set a token (see the Security section of the [README](README.md)) before exposing the machine to any network
  you do not control, and never forward these ports to the internet. The installer's optional firewall task opens
  exactly these two ports.
- Static dashboard files are served without authentication so the login prompt can load; the `/api/` routes and the
  preview streams are what the token protects.
- No credentials are shipped. API keys, stream keys and station passwords you enter are stored in the Supervisor's
  database on that machine and are meant to be write-only in the API responses.
- Findings that are in scope include authentication bypass, path traversal or arbitrary file read/write through the
  API, command injection into the helper programs the Supervisor starts, and secrets leaking into logs or API output.
  Running the software with no token on an open network is a known and documented configuration, not a vulnerability.
