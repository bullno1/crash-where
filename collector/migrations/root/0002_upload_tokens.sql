-- Bearer tokens that let CI register releases and upload symbol tables for
-- one app. Only a hash of the token is kept; the token itself is shown once.
CREATE TABLE upload_tokens (
  id           INTEGER PRIMARY KEY NOT NULL,
  app_id       INTEGER NOT NULL REFERENCES apps(id),
  hash         TEXT    NOT NULL UNIQUE,  -- hex SHA-256 of the token
  label        TEXT    NOT NULL,         -- what the token is for, as typed when it was created
  created_at   INTEGER NOT NULL,         -- unix seconds
  created_by   TEXT    NOT NULL,         -- email of the creator, or their subject when there is none
  revoked_at   INTEGER,                  -- NULL = usable
  last_used_at INTEGER                   -- NULL = never
);
CREATE INDEX upload_tokens_app ON upload_tokens(app_id);
