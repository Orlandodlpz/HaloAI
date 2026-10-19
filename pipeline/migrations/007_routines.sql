-- Routines (decided 2026-10-19): a saved command the user can reuse by name, the Personal AI
-- track's "reusable skills". Stored as memory, so the user can view, edit and delete them like
-- everything else the assistant remembers. They never run on their own: naming one starts a
-- normal request that ends in REVIEW and needs a press.
ALTER TABLE memory_items DROP CONSTRAINT memory_items_kind_check;
ALTER TABLE memory_items ADD CONSTRAINT memory_items_kind_check
    CHECK (kind IN ('contact', 'preference', 'fact', 'routine'));
