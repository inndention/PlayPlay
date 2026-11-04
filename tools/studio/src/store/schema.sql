-- PlayPlayStudio store: core schema, migration core/1 (docs/plans/starmedia-studio.md §4.8).
--
-- store/migrations.cpp applies this file once per database, inside one transaction, and records a hash of its
-- statements (comments and spacing do not count) in schema_migrations. A database that already holds core/1 refuses
-- to open when the statements here change, so never edit a released statement: add a step to coreModule() in
-- migrations.cpp instead (ALTER TABLE ... ADD COLUMN, a new table, a new index).
--
-- Conventions:
--   STRICT tables: a column takes only its declared type, so a fractional USD double can never land in a money column.
--   *_micro   money, INTEGER micro-USD (core/money.h); never REAL.
--   *_ms      INTEGER milliseconds since the Unix epoch (UTC); 0 = not set.
--   *_json    TEXT holding JSON.
--   flags     INTEGER 0 / 1.
--   optional text is '' (NOT NULL DEFAULT ''); the repos write '' for an empty value, never NULL.
--   No FOREIGN KEY clauses: batched writes may land a child row in the same batch as, or before, its parent; the repos
--   keep references consistent and the film / scheduler modules validate ids before writing.

-- One film, spot or article. spent_micro is the booked total (ST-11 keeps it in step with the spend rows).
CREATE TABLE projects (
    id            TEXT PRIMARY KEY NOT NULL,
    kind          TEXT NOT NULL DEFAULT 'film',          -- film | spot | article
    title         TEXT NOT NULL DEFAULT '',
    brief_json    TEXT NOT NULL DEFAULT '{}',
    budget_micro  INTEGER NOT NULL DEFAULT 0 CHECK (budget_micro >= 0),
    spent_micro   INTEGER NOT NULL DEFAULT 0 CHECK (spent_micro >= 0),
    tier          TEXT NOT NULL DEFAULT '',              -- animatic | draft | final
    status        TEXT NOT NULL DEFAULT 'draft',
    created_ms    INTEGER NOT NULL DEFAULT 0,
    updated_ms    INTEGER NOT NULL DEFAULT 0
) STRICT;

-- The editable film tree: sequence > scene > shot / beat / line / cue. input_hash drives stale propagation (ST-21).
CREATE TABLE film_nodes (
    id              TEXT PRIMARY KEY NOT NULL,
    project_id      TEXT NOT NULL,
    parent_id       TEXT NOT NULL DEFAULT '',             -- '' = top level
    kind            TEXT NOT NULL,                        -- sequence | scene | shot | beat | line | cue
    order_key       TEXT NOT NULL DEFAULT '',
    data_json       TEXT NOT NULL DEFAULT '{}',
    input_hash      TEXT NOT NULL DEFAULT '',
    locked          INTEGER NOT NULL DEFAULT 0,
    stale           INTEGER NOT NULL DEFAULT 0,
    chosen_take_id  TEXT NOT NULL DEFAULT '',
    updated_ms      INTEGER NOT NULL DEFAULT 0
) STRICT;
CREATE INDEX film_nodes_tree ON film_nodes (project_id, parent_id, order_key);

-- Characters, locations, props, styles and voices of one project, with their reference artifacts.
CREATE TABLE bible (
    id                  TEXT PRIMARY KEY NOT NULL,
    project_id          TEXT NOT NULL,
    kind                TEXT NOT NULL,                    -- character | location | prop | style | voice
    name                TEXT NOT NULL DEFAULT '',
    data_json           TEXT NOT NULL DEFAULT '{}',
    ref_artifacts_json  TEXT NOT NULL DEFAULT '[]',
    updated_ms          INTEGER NOT NULL DEFAULT 0
) STRICT;
CREATE INDEX bible_project ON bible (project_id, kind);

-- One execution of a priced plan. ceiling_micro = budget x 1.15 for optional nodes (executor.ts).
CREATE TABLE runs (
    id             TEXT PRIMARY KEY NOT NULL,
    project_id     TEXT NOT NULL DEFAULT '',
    phase          TEXT NOT NULL DEFAULT '',
    tier           TEXT NOT NULL DEFAULT '',
    plan_json      TEXT NOT NULL DEFAULT '{}',
    plan_hash      TEXT NOT NULL DEFAULT '',
    approval_id    TEXT NOT NULL DEFAULT '',
    budget_micro   INTEGER NOT NULL DEFAULT 0 CHECK (budget_micro >= 0),
    ceiling_micro  INTEGER NOT NULL DEFAULT 0 CHECK (ceiling_micro >= 0),
    spent_micro    INTEGER NOT NULL DEFAULT 0 CHECK (spent_micro >= 0),
    state          TEXT NOT NULL DEFAULT 'pending',
    started_ms     INTEGER NOT NULL DEFAULT 0,
    finished_ms    INTEGER NOT NULL DEFAULT 0
) STRICT;
CREATE INDEX runs_project ON runs (project_id, started_ms);

-- DAG nodes (§4.3). state: pending | ready | admitted | running | polling | done | failed | skipped | cancelled | unknown.
-- provider_job_id is the provider-accepted marker: written durably before the first poll, so a restart resumes polling
-- instead of paying twice. idem_key = sha256(node, input_hash, attempt) (core/ids.h idemKey).
CREATE TABLE jobs (
    id               TEXT PRIMARY KEY NOT NULL,
    run_id           TEXT NOT NULL DEFAULT '',
    film_node_id     TEXT NOT NULL DEFAULT '',
    kind             TEXT NOT NULL DEFAULT '',
    role             TEXT NOT NULL DEFAULT '',
    lane             TEXT NOT NULL DEFAULT '',
    priority         INTEGER NOT NULL DEFAULT 0,
    order_key        TEXT NOT NULL DEFAULT '',
    deps_json        TEXT NOT NULL DEFAULT '[]',
    indegree         INTEGER NOT NULL DEFAULT 0,
    state            TEXT NOT NULL DEFAULT 'pending',
    model            TEXT NOT NULL DEFAULT '',
    fallbacks_json   TEXT NOT NULL DEFAULT '[]',
    estimate_micro   INTEGER NOT NULL DEFAULT 0 CHECK (estimate_micro >= 0),
    actual_micro     INTEGER NOT NULL DEFAULT 0 CHECK (actual_micro >= 0),
    cost_source      TEXT NOT NULL DEFAULT '',            -- actual | estimated
    attempts         INTEGER NOT NULL DEFAULT 0,
    provider_job_id  TEXT NOT NULL DEFAULT '',
    idem_key         TEXT NOT NULL DEFAULT '',
    input_hash       TEXT NOT NULL DEFAULT '',
    lease_until_ms   INTEGER NOT NULL DEFAULT 0,
    outputs_json     TEXT NOT NULL DEFAULT '{}',
    error            TEXT NOT NULL DEFAULT '',
    created_ms       INTEGER NOT NULL DEFAULT 0,
    started_ms       INTEGER NOT NULL DEFAULT 0,
    finished_ms      INTEGER NOT NULL DEFAULT 0
) STRICT;
CREATE INDEX jobs_run_state ON jobs (run_id, state);
CREATE INDEX jobs_state_lease ON jobs (state, lease_until_ms);
CREATE INDEX jobs_film_node ON jobs (film_node_id);
CREATE INDEX jobs_idem ON jobs (idem_key);

-- from_id must finish before to_id may start.
CREATE TABLE job_edges (
    from_id  TEXT NOT NULL,
    to_id    TEXT NOT NULL,
    PRIMARY KEY (from_id, to_id)
) STRICT, WITHOUT ROWID;
CREATE INDEX job_edges_to ON job_edges (to_id, from_id);

-- Content-addressed outputs and uploads: <data>/artifacts/ab/cd/<sha256> (ST-13).
CREATE TABLE artifacts (
    sha256       TEXT PRIMARY KEY NOT NULL,
    path         TEXT NOT NULL DEFAULT '',
    mime         TEXT NOT NULL DEFAULT '',
    bytes        INTEGER NOT NULL DEFAULT 0,
    width        INTEGER NOT NULL DEFAULT 0,
    height       INTEGER NOT NULL DEFAULT 0,
    duration_ms  INTEGER NOT NULL DEFAULT 0,
    origin       TEXT NOT NULL DEFAULT '',                -- ai | upload | render
    model        TEXT NOT NULL DEFAULT '',
    job_id       TEXT NOT NULL DEFAULT '',
    created_ms   INTEGER NOT NULL DEFAULT 0
) STRICT;
CREATE INDEX artifacts_job ON artifacts (job_id);

-- Candidate generations for a film node; film_nodes.chosen_take_id picks one.
CREATE TABLE takes (
    id            TEXT PRIMARY KEY NOT NULL,
    film_node_id  TEXT NOT NULL,
    artifact_sha  TEXT NOT NULL DEFAULT '',
    kind          TEXT NOT NULL DEFAULT '',               -- keyframe | clip | voice | music | sfx
    model         TEXT NOT NULL DEFAULT '',
    group_tag     TEXT NOT NULL DEFAULT '',
    note          TEXT NOT NULL DEFAULT '',
    created_ms    INTEGER NOT NULL DEFAULT 0
) STRICT;
CREATE INDEX takes_node ON takes (film_node_id, created_ms);
CREATE INDEX takes_artifact ON takes (artifact_sha);

-- The money ledger: append-only, every row committed durably (synchronous=FULL) before the request it pays for is sent.
-- kind reserve | charge | release; micro is always >= 0 (the kind gives the direction); source actual | estimated.
CREATE TABLE spend (
    id             TEXT PRIMARY KEY NOT NULL,
    ts_ms          INTEGER NOT NULL,
    project_id     TEXT NOT NULL DEFAULT '',
    run_id         TEXT NOT NULL DEFAULT '',
    job_id         TEXT NOT NULL DEFAULT '',
    agent_id       TEXT NOT NULL DEFAULT '',
    model          TEXT NOT NULL DEFAULT '',
    kind           TEXT NOT NULL,
    micro          INTEGER NOT NULL CHECK (micro >= 0),
    source         TEXT NOT NULL DEFAULT 'estimated',
    generation_id  TEXT NOT NULL DEFAULT ''
) STRICT;
CREATE INDEX spend_ts ON spend (ts_ms);
CREATE INDEX spend_project ON spend (project_id, ts_ms);
CREATE INDEX spend_run ON spend (run_id);
CREATE INDEX spend_job ON spend (job_id);

-- Single-use approval tokens bound to a scope hash and an amount (ST-11).
CREATE TABLE approvals (
    token        TEXT PRIMARY KEY NOT NULL,
    kind         TEXT NOT NULL,                           -- spend | publish | tier
    project_id   TEXT NOT NULL DEFAULT '',
    scope_hash   TEXT NOT NULL,
    micro        INTEGER NOT NULL DEFAULT 0 CHECK (micro >= 0),
    expires_ms   INTEGER NOT NULL,
    state        TEXT NOT NULL DEFAULT 'pending',         -- pending | approved | declined | used | expired
    decided_by   TEXT NOT NULL DEFAULT '',
    decided_ms   INTEGER NOT NULL DEFAULT 0,
    detail_json  TEXT NOT NULL DEFAULT '{}',
    created_ms   INTEGER NOT NULL DEFAULT 0
) STRICT;
CREATE INDEX approvals_state ON approvals (state, expires_ms);

-- Tool-loop agents (state machines, no thread each) and their transcripts.
CREATE TABLE agents (
    id            TEXT PRIMARY KEY NOT NULL,
    project_id    TEXT NOT NULL DEFAULT '',
    parent_id     TEXT NOT NULL DEFAULT '',
    role          TEXT NOT NULL DEFAULT '',
    state         TEXT NOT NULL DEFAULT 'queued',
    budget_micro  INTEGER NOT NULL DEFAULT 0 CHECK (budget_micro >= 0),
    used_micro    INTEGER NOT NULL DEFAULT 0 CHECK (used_micro >= 0),
    steps         INTEGER NOT NULL DEFAULT 0,
    max_steps     INTEGER NOT NULL DEFAULT 0,
    tools_json    TEXT NOT NULL DEFAULT '[]',
    created_ms    INTEGER NOT NULL DEFAULT 0
) STRICT;
CREATE INDEX agents_project ON agents (project_id, created_ms);
CREATE INDEX agents_parent ON agents (parent_id);

CREATE TABLE agent_messages (
    agent_id      TEXT NOT NULL,
    seq           INTEGER NOT NULL,
    role          TEXT NOT NULL,
    content_json  TEXT NOT NULL DEFAULT '{}',
    PRIMARY KEY (agent_id, seq)
) STRICT, WITHOUT ROWID;

-- The provider catalog cache and per-model health (quota breaker, outages).
CREATE TABLE models (
    id          TEXT PRIMARY KEY NOT NULL,
    data_json   TEXT NOT NULL DEFAULT '{}',
    fetched_ms  INTEGER NOT NULL DEFAULT 0
) STRICT;

CREATE TABLE model_health (
    model     TEXT PRIMARY KEY NOT NULL,
    kind      TEXT NOT NULL DEFAULT '',
    until_ms  INTEGER NOT NULL DEFAULT 0,
    message   TEXT NOT NULL DEFAULT ''
) STRICT;

-- Voice profiles: a clone is only ever sent with a consent record (re-validated at send time) and every action is
-- audited, append-only.
CREATE TABLE voices (
    id            TEXT PRIMARY KEY NOT NULL,
    name          TEXT NOT NULL DEFAULT '',
    sample_sha    TEXT NOT NULL DEFAULT '',
    consent_json  TEXT NOT NULL DEFAULT '{}',
    created_ms    INTEGER NOT NULL DEFAULT 0
) STRICT;

CREATE TABLE voice_audit (
    ts_ms     INTEGER NOT NULL,
    voice_id  TEXT NOT NULL,
    action    TEXT NOT NULL,
    actor     TEXT NOT NULL DEFAULT ''
) STRICT;
CREATE INDEX voice_audit_voice ON voice_audit (voice_id, ts_ms);

-- Jobs API v1 keys (only the hash is stored) and their idempotency records.
CREATE TABLE api_keys (
    id                   TEXT PRIMARY KEY NOT NULL,
    name                 TEXT NOT NULL DEFAULT '',
    hash                 TEXT NOT NULL,
    scopes               TEXT NOT NULL DEFAULT '',
    daily_usd_cap_micro  INTEGER NOT NULL DEFAULT 1000000 CHECK (daily_usd_cap_micro >= 0),
    daily_job_cap        INTEGER NOT NULL DEFAULT 0,
    created_ms           INTEGER NOT NULL DEFAULT 0
) STRICT;
CREATE UNIQUE INDEX api_keys_hash ON api_keys (hash);

CREATE TABLE idem (
    key        TEXT PRIMARY KEY NOT NULL,
    key_id     TEXT NOT NULL DEFAULT '',
    body_hash  TEXT NOT NULL DEFAULT '',
    job_id     TEXT NOT NULL DEFAULT '',
    ts_ms      INTEGER NOT NULL DEFAULT 0
) STRICT;
CREATE INDEX idem_key_ts ON idem (key_id, ts_ms);

-- WS replay log: clients reconnect with ?since=<seq>. AUTOINCREMENT so a seq is never reused after a prune.
CREATE TABLE events (
    seq           INTEGER PRIMARY KEY AUTOINCREMENT,
    ts_ms         INTEGER NOT NULL,
    type          TEXT NOT NULL,
    project_id    TEXT NOT NULL DEFAULT '',
    payload_json  TEXT NOT NULL DEFAULT '{}'
) STRICT;
CREATE INDEX events_project ON events (project_id, seq);
CREATE INDEX events_ts ON events (ts_ms);

-- Redacted memory notes (labelled untrusted when an agent reads them).
CREATE TABLE memory_notes (
    id          TEXT PRIMARY KEY NOT NULL,
    scope       TEXT NOT NULL DEFAULT 'project',          -- user | brand | project
    project_id  TEXT NOT NULL DEFAULT '',
    title       TEXT NOT NULL DEFAULT '',
    body        TEXT NOT NULL DEFAULT '',
    source      TEXT NOT NULL DEFAULT '',
    created_ms  INTEGER NOT NULL DEFAULT 0
) STRICT;
CREATE INDEX memory_notes_scope ON memory_notes (scope, project_id, created_ms);

-- Who did what (approvals, budget changes, deliveries), append-only.
CREATE TABLE audit (
    ts_ms        INTEGER NOT NULL,
    actor        TEXT NOT NULL DEFAULT '',
    action       TEXT NOT NULL,
    target       TEXT NOT NULL DEFAULT '',
    detail_json  TEXT NOT NULL DEFAULT '{}'
) STRICT;
CREATE INDEX audit_ts ON audit (ts_ms);
