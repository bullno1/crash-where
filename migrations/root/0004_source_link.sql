-- Where the sources of a build can be read: an RFC 6570 URI template the
-- crash page expands with the build's commit, its version, the file and
-- the line. NULL = locations are not linked.
ALTER TABLE apps ADD COLUMN source_link_template TEXT;
