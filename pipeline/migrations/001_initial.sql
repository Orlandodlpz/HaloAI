-- B1 schema: users, API keys, requests with revisions, executions, call log, history.
-- Times that drive behavior (expiry, retention) are written by the app clock, not now(),
-- so tests can move time forward without sleeping.

CREATE TABLE users (
    id          uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    name        text NOT NULL UNIQUE,
    created_at  timestamptz NOT NULL DEFAULT now()
);

-- Keys are stored as SHA-256 hashes, never in plain text.
-- scope 'mock' (demo site) can only ever reach mock tools; 'real' (phone) can reach real ones.
CREATE TABLE api_keys (
    id            uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id       uuid NOT NULL REFERENCES users(id),
    client        text NOT NULL,
    scope         text NOT NULL CHECK (scope IN ('real', 'mock')),
    key_hash      text NOT NULL UNIQUE,
    rate_per_min  integer NOT NULL DEFAULT 60 CHECK (rate_per_min > 0),
    created_at    timestamptz NOT NULL DEFAULT now(),
    revoked_at    timestamptz
);

-- One row per user request (request_id comes from the phone). state mirrors the
-- result state the phone sees, plus 'processing' while the model runs.
CREATE TABLE requests (
    id                uuid PRIMARY KEY,
    user_id           uuid NOT NULL REFERENCES users(id),
    scope             text NOT NULL CHECK (scope IN ('real', 'mock')),
    state             text NOT NULL CHECK (state IN (
                          'processing', 'conflict_ambiguous', 'conflict_schedule', 'review',
                          'read_only_result', 'executing', 'success', 'failure',
                          'execution_uncertain', 'cancelled')),
    current_revision  integer NOT NULL CHECK (current_revision >= 1),
    cancel_reason     text CHECK (cancel_reason IN ('user', 'expired', 'replaced')),
    failure_reason    text,
    timezone          text NOT NULL,
    language          text NOT NULL,
    orientation       text NOT NULL CHECK (orientation IN ('landscape', 'portrait')),
    expires_at        timestamptz,
    created_at        timestamptz NOT NULL,
    updated_at        timestamptz NOT NULL
);

-- Hard constraint "one request at a time", enforced by the database itself.
CREATE UNIQUE INDEX requests_one_open_per_user ON requests (user_id)
    WHERE state IN ('processing', 'conflict_ambiguous', 'conflict_schedule', 'review', 'executing');

-- Every revision is created by exactly one input: a voice segment or a candidate selection.
CREATE TABLE segments (
    request_id    uuid NOT NULL REFERENCES requests(id) ON DELETE CASCADE,
    revision      integer NOT NULL,
    kind          text NOT NULL CHECK (kind IN ('voice', 'select')),
    segment_id    uuid,
    text          text,
    candidate_id  text,
    sent_at       timestamptz,
    created_at    timestamptz NOT NULL,
    PRIMARY KEY (request_id, revision)
);

-- The processed result of a revision. Only the latest revision's result is ever stored;
-- results that arrive for an older revision are dropped.
CREATE TABLE revisions (
    request_id           uuid NOT NULL REFERENCES requests(id) ON DELETE CASCADE,
    revision             integer NOT NULL,
    state                text NOT NULL,
    screen_lines         jsonb NOT NULL,
    review_screen_lines  jsonb,
    candidates           jsonb,
    draft                jsonb,
    proposal_hash        text,
    timings              jsonb,
    created_at           timestamptz NOT NULL,
    PRIMARY KEY (request_id, revision)
);

-- Approval exactly once: one execution per request at most, keyed by the execution key.
CREATE TABLE executions (
    execution_key  text PRIMARY KEY,
    request_id     uuid NOT NULL UNIQUE REFERENCES requests(id),
    revision       integer NOT NULL,
    proposal_hash  text NOT NULL,
    status         text NOT NULL CHECK (status IN ('pending', 'applied', 'not_applied', 'unknown')),
    screen_lines   jsonb,
    detail         jsonb,
    created_at     timestamptz NOT NULL,
    updated_at     timestamptz NOT NULL
);

-- Idempotent retries: one row per call_id, kept 24 h.
CREATE TABLE call_log (
    call_id      uuid PRIMARY KEY,
    user_id      uuid NOT NULL REFERENCES users(id),
    path         text NOT NULL,
    body_hash    text NOT NULL,
    status       text NOT NULL CHECK (status IN ('running', 'done', 'abandoned')),
    http_status  integer,
    response     jsonb,
    created_at   timestamptz NOT NULL
);
CREATE INDEX call_log_created_at ON call_log (created_at);

CREATE TABLE history (
    id          bigserial PRIMARY KEY,
    user_id     uuid NOT NULL REFERENCES users(id),
    request_id  uuid,
    event       text NOT NULL,
    data        jsonb,
    at          timestamptz NOT NULL
);
CREATE INDEX history_request ON history (request_id);
