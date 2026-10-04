-- A reservoir of full reports per group, version and trust level: the
-- envelope and its attachments live in R2 under r2_key, this row says
-- they exist. Purged with the version, and deleted when a newer report
-- replaces the sample.

CREATE TABLE crash_samples (
  id          INTEGER PRIMARY KEY NOT NULL,
  report_id   TEXT    NOT NULL UNIQUE REFERENCES reports(report_id),  -- a retried envelope finds its sample here
  group_id    INTEGER NOT NULL REFERENCES crash_groups(id),
  version     TEXT    NOT NULL REFERENCES versions(version),           -- so the version purge reaches it
  trust       INTEGER NOT NULL,         -- the bucket's key space, as on reports
  r2_key      TEXT    NOT NULL,         -- prefix of the envelope and attachments
  received_at INTEGER NOT NULL          -- unix seconds
);
CREATE INDEX crash_samples_bucket ON crash_samples(group_id, version, trust);

-- A cap for one group in place of the app's, so a spiking group can keep
-- more samples without changing the app. NULL = the app's cap.
ALTER TABLE crash_groups ADD COLUMN sample_cap INTEGER;
