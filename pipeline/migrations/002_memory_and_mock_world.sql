-- B3: memory, plus a mock calendar and outbox per user that approved actions really change.

-- What the assistant remembers. source 'user' = the user said it ("remember that...");
-- 'learned' = taken from an action the user approved. Never learned from content (hard rule).
CREATE TABLE memory_items (
    id          uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id     uuid NOT NULL REFERENCES users(id),
    kind        text NOT NULL CHECK (kind IN ('contact', 'preference', 'fact')),
    label       text NOT NULL,
    value       text NOT NULL,
    source      text NOT NULL CHECK (source IN ('user', 'learned')),
    uses        integer NOT NULL DEFAULT 0,
    created_at  timestamptz NOT NULL,
    updated_at  timestamptz NOT NULL
);
-- One row per (user, kind, label, value), case-insensitive, so learning the same contact again
-- increases uses instead of adding a duplicate.
CREATE UNIQUE INDEX memory_items_unique ON memory_items (user_id, kind, lower(label), lower(value));

-- Recent actions come from approved executions. "Forget" hides one from memory without
-- deleting the execution record, which approval exactly once depends on.
ALTER TABLE executions ADD COLUMN forgotten boolean NOT NULL DEFAULT false;

-- Mock world per user (sample data mode and the demo site). Seeded once, relative to the
-- user's first use, then changed only by approved actions.
CREATE TABLE mock_state (
    user_id    uuid PRIMARY KEY REFERENCES users(id),
    timezone   text NOT NULL,
    seeded_on  date NOT NULL
);

CREATE TABLE mock_events (
    user_id      uuid NOT NULL REFERENCES users(id),
    event_id     text NOT NULL,
    title        text NOT NULL,
    starts_at    timestamptz NOT NULL,
    ends_at      timestamptz NOT NULL,
    notes        text NOT NULL DEFAULT '',
    guests       jsonb NOT NULL DEFAULT '[]',
    video_call   boolean NOT NULL DEFAULT false,
    description  text NOT NULL DEFAULT '',
    PRIMARY KEY (user_id, event_id)
);

CREATE TABLE mock_outbox (
    id             bigserial PRIMARY KEY,
    user_id        uuid NOT NULL REFERENCES users(id),
    execution_key  text NOT NULL UNIQUE,  -- a retried send can never add a second email
    to_addr        text NOT NULL,
    subject        text NOT NULL,
    body           text NOT NULL,
    sent_at        timestamptz NOT NULL
);
