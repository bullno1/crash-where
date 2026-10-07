-- Where a build's sources came from, as the upload recorded them: the
-- commit it was built from and the checkout root its paths start with.
-- NULL = not given.
ALTER TABLE builds ADD COLUMN source_commit TEXT;
ALTER TABLE builds ADD COLUMN source_root TEXT;
