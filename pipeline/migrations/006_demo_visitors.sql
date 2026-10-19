-- B6: every demo site visitor gets their own sandbox user and a key that expires (decided
-- 2026-10-19), so judges never see each other's calendar, emails or memory. Keys and users
-- without an expiry (the phone, the demo site's own key) are unchanged.
ALTER TABLE api_keys ADD COLUMN expires_at timestamptz;
ALTER TABLE users ADD COLUMN expires_at timestamptz;
CREATE INDEX segments_created_at ON segments (created_at);
