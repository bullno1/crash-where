-- Per-app crash index, one copy inside each app's Durable Object.
-- A version is a tag or commit; it has one build per binary and is made
-- available on any number of channels.

CREATE TABLE versions (
  version    TEXT    PRIMARY KEY NOT NULL,  -- as cw_init receives it, "1.4.2"
  ordinal    INTEGER NOT NULL,              -- numeric sort key derived from it, 1004002
  created_at INTEGER NOT NULL               -- unix seconds
);

CREATE TABLE builds (                       -- one per uploaded symbol table
  build_id    TEXT    PRIMARY KEY NOT NULL, -- hex; matches the cwsym header
  version     TEXT    NOT NULL REFERENCES versions(version),
  uploaded_at INTEGER NOT NULL
);
CREATE INDEX builds_version ON builds(version);

CREATE TABLE releases (                     -- a version made available on a channel
  channel         TEXT    NOT NULL,         -- build stream: "stable", "beta", ...
  version         TEXT    NOT NULL REFERENCES versions(version),
  released_at     INTEGER NOT NULL,
  supported_until INTEGER,                  -- NULL = current on this channel
  PRIMARY KEY (channel, version)
);
