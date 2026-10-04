-- How many full reports each sample bucket keeps: a bucket is a group, a
-- version and a trust level. Each trust level has its own cap.
ALTER TABLE apps ADD COLUMN sample_cap_trusted   INTEGER NOT NULL DEFAULT 5;  -- authorized reports
ALTER TABLE apps ADD COLUMN sample_cap_untrusted INTEGER NOT NULL DEFAULT 2;  -- unauthorized reports
