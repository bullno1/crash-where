-- Crash index for accepted envelopes. A group is one thing to fix, stable
-- across builds and platforms; reports are counted per group and day.

CREATE TABLE crash_groups (
  id          INTEGER PRIMARY KEY NOT NULL,
  fingerprint TEXT    NOT NULL UNIQUE,  -- hex16 hash of fault, selected frames and message
  fault       TEXT    NOT NULL,         -- normalized class: memory, abort, hang, ...
  frames      TEXT    NOT NULL,         -- JSON array of the top raw frames, before the skip list
  message     TEXT,                     -- normalized message; NULL when the fault does not hash it
  first_seen  INTEGER NOT NULL,         -- unix seconds
  last_seen   INTEGER NOT NULL
);

CREATE TABLE crash_counts (             -- the write on every report
  group_id INTEGER NOT NULL REFERENCES crash_groups(id),
  version  TEXT    NOT NULL REFERENCES versions(version),
  channel  TEXT    NOT NULL,            -- as the envelope reported it
  trust    INTEGER NOT NULL,            -- 0 = unauthorized; 1 = authorized, once the auth route exists
  day      INTEGER NOT NULL,            -- UTC days since the epoch
  count    INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (group_id, version, channel, trust, day)
) WITHOUT ROWID;

CREATE TABLE reports (                  -- every accepted envelope, so a retry counts once
  report_id   TEXT    PRIMARY KEY NOT NULL,  -- client-minted UUID
  received_at INTEGER NOT NULL               -- unix seconds
);
