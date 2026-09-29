-- LövGlöd driftstatistik. Kör om utan risk: allt är IF NOT EXISTS.
--   npx wrangler d1 execute lovglod-telemetri --remote --file=schema.sql

-- En rad per lampa, uppdateras vid varje rapport.
CREATE TABLE IF NOT EXISTS lamps (
    id          TEXT PRIMARY KEY,          -- slumpat i lampan, 16 hex
    name        TEXT,                      -- smeknamnet från admin-sidan
    fw          TEXT,
    first_seen  INTEGER NOT NULL,          -- unix-sekunder, serverns klocka
    last_seen   INTEGER NOT NULL,
    last_report INTEGER                    -- reports.id
);

-- Varje hälsorapport. De vanligaste fälten har egna kolumner så att de går att
-- fråga på; hela rapporten ligger kvar i body för det som tillkommer senare.
CREATE TABLE IF NOT EXISTS reports (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    lamp_id     TEXT    NOT NULL,
    received_at INTEGER NOT NULL,
    fw          TEXT,
    beta        INTEGER,
    up          INTEGER,                   -- sekunder sedan start
    boots       INTEGER,
    bad_boots   INTEGER,
    reset       TEXT,
    heap        INTEGER,
    min_heap    INTEGER,
    rssi        INTEGER,
    shl_err     INTEGER,
    sse_re      INTEGER,
    body        TEXT    NOT NULL
);
CREATE INDEX IF NOT EXISTS reports_lamp ON reports (lamp_id, received_at);

-- Händelser som lampan köat sedan förra rapporten.
CREATE TABLE IF NOT EXISTS events (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    lamp_id     TEXT    NOT NULL,
    report_id   INTEGER NOT NULL,
    at          INTEGER NOT NULL,          -- när det hände, se index.js
    type        TEXT    NOT NULL,
    detail      TEXT
);
CREATE INDEX IF NOT EXISTS events_lamp ON events (lamp_id, at);
