-- Which web pages may send reports: '*' for any origin, otherwise one
-- origin pattern per line, where '*' matches any run of characters.
-- NULL = no cross-origin requests are allowed.
ALTER TABLE apps ADD COLUMN cors_origins TEXT;
