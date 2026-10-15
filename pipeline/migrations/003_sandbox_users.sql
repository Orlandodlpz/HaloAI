-- B3: the demo site's user is a sandbox, separate from the phone user, so a judge on the demo
-- site can never see or change the phone user's memory. Demo (mock) keys may only belong to
-- sandbox users and phone (real) keys never; app/auth.py create_key enforces it.
ALTER TABLE users ADD COLUMN sandbox boolean NOT NULL DEFAULT false;
