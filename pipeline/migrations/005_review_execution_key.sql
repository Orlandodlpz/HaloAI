-- The execution key is known as soon as a proposal exists, so REVIEW carries it and the device
-- saves it to flash before it approves (asked by Victor, 2026-10-16). Stored per revision so a
-- status query for a key that was never approved still finds its request, instead of a 404.
-- Older revisions keep NULL: they expired long ago and can no longer be approved.
ALTER TABLE revisions ADD COLUMN execution_key text;
CREATE UNIQUE INDEX revisions_execution_key ON revisions (execution_key) WHERE execution_key IS NOT NULL;
