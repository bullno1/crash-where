-- Registered games. The name is the slug the client is configured with;
-- it identifies the app in URL paths, tokens and envelopes and never changes.
CREATE TABLE apps (
  id           INTEGER PRIMARY KEY NOT NULL,  -- internal key; other tables reference this, never the name
  name         TEXT    NOT NULL UNIQUE
               CHECK (length(name) BETWEEN 1 AND 63 AND name NOT GLOB '*[^a-z0-9_-]*'),
  display_name TEXT    NOT NULL,     -- title shown in the dashboard, editable
  created_at   INTEGER NOT NULL,     -- unix seconds
  created_by   TEXT    NOT NULL,     -- email of the creator, or their subject when there is none
  disabled_at  INTEGER               -- NULL = accepting reports; set = kill switch
);
