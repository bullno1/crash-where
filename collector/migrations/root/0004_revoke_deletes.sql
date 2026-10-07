-- A revoked token is deleted rather than kept: nothing references the row
-- and its hash matches nothing anyone should hold. Regenerating a token
-- replaces the hash on its row instead of adding one.
DELETE FROM upload_tokens WHERE revoked_at IS NOT NULL;
ALTER TABLE upload_tokens DROP COLUMN revoked_at;
