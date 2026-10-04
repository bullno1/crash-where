-- The purge deletes a release's reports and counts by version and channel;
-- neither primary key starts with those columns.
CREATE INDEX reports_release ON reports(version, channel);
CREATE INDEX crash_counts_release ON crash_counts(version, channel);
