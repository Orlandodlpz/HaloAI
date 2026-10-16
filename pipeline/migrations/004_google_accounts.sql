-- B4: Google sign in. One connected Google account per user; the refresh token is stored
-- encrypted (key in the server secrets, never in the database or git).
CREATE TABLE google_accounts (
    user_id              uuid PRIMARY KEY REFERENCES users(id),
    email                text NOT NULL,
    refresh_token_enc    text NOT NULL,
    scopes               text NOT NULL,
    connected_at         timestamptz NOT NULL,
    updated_at           timestamptz NOT NULL
);

-- The OAuth callback has no API key (Google calls it), so each sign in carries a one time
-- state: 256 random bits, stored only as a hash, valid 10 minutes, usable once.
CREATE TABLE oauth_states (
    state_hash  text PRIMARY KEY,
    user_id     uuid NOT NULL REFERENCES users(id),
    expires_at  timestamptz NOT NULL,
    used_at     timestamptz
);
