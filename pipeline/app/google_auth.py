"""Google sign in on the backend (B4, locked decision: sign in happens on the backend).

Flow: the phone asks /v1/google/connect for a link; the user signs in in a browser; Google
calls /v1/google/callback with a one time code; we swap it for tokens and keep only the
refresh token, encrypted. Calendar, Gmail and People calls then ask for a short lived access
token, refreshed when it expires.

The callback is the one endpoint without our API key, because Google calls it. It is protected
by a one time state: 256 random bits, stored only as a hash, tied to the user, valid 10 minutes,
usable once (decided 2026-10-15).
"""

import hashlib
import secrets
import time
from datetime import timedelta
from urllib.parse import urlencode

import httpx
from cryptography.fernet import Fernet

AUTH_URL = "https://accounts.google.com/o/oauth2/v2/auth"
TOKEN_URL = "https://oauth2.googleapis.com/token"
USERINFO_URL = "https://openidconnect.googleapis.com/v1/userinfo"
REVOKE_URL = "https://oauth2.googleapis.com/revoke"
# Exactly the permissions ticked in the Cloud console. Gmail is send only: no mailbox reading.
SCOPES = [
    "openid",
    "https://www.googleapis.com/auth/userinfo.email",
    "https://www.googleapis.com/auth/calendar.events",
    "https://www.googleapis.com/auth/gmail.send",
    "https://www.googleapis.com/auth/contacts.readonly",
    "https://www.googleapis.com/auth/contacts.other.readonly",
]


class GoogleError(Exception):
    """Sign in or token problem; the message is safe to show the user."""


class GoogleNotConnected(GoogleError):
    """This user has no Google account connected."""


def _hash(state: str) -> str:
    return hashlib.sha256(state.encode()).hexdigest()


class GoogleAuth:
    def __init__(self, pool, settings, clock, http: httpx.AsyncClient | None = None):
        self.pool = pool
        self.settings = settings
        self.clock = clock
        self.http = http or httpx.AsyncClient(timeout=15)
        self._access: dict[str, tuple[str, float]] = {}  # user_id -> (access token, expires at, monotonic)
        self.on_change: list = []  # called with the user_id when an account is connected or removed

    # ---------- configuration ----------

    def configured(self) -> bool:
        s = self.settings
        return bool(s.google_client_id and s.google_client_secret and s.token_encryption_key)

    def _fernet(self) -> Fernet:
        return Fernet(self.settings.token_encryption_key.get_secret_value().encode())

    def _client(self) -> tuple[str, str]:
        if not self.configured():
            raise GoogleError("Google sign in is not set up on this server yet")
        return (self.settings.google_client_id.get_secret_value(),
                self.settings.google_client_secret.get_secret_value())

    # ---------- sign in ----------

    async def connect_url(self, user_id: str) -> str:
        client_id, _ = self._client()
        state = secrets.token_urlsafe(32)
        expires = self.clock.now() + timedelta(minutes=self.settings.oauth_state_minutes)
        async with self.pool.connection() as conn:
            await conn.execute("INSERT INTO oauth_states (state_hash, user_id, expires_at) VALUES (%s, %s, %s)",
                               (_hash(state), user_id, expires))
        return AUTH_URL + "?" + urlencode({
            "client_id": client_id, "redirect_uri": self.settings.google_redirect_uri, "response_type": "code",
            "scope": " ".join(SCOPES), "access_type": "offline", "prompt": "consent",
            "include_granted_scopes": "true", "state": state,
        })

    async def callback(self, state: str, code: str) -> str:
        """Finish sign in. Returns the connected Google address."""
        client_id, client_secret = self._client()
        now = self.clock.now()
        async with self.pool.connection() as conn, conn.transaction():
            cur = await conn.execute(  # single use: only the first callback with this state wins
                "UPDATE oauth_states SET used_at = %s WHERE state_hash = %s AND used_at IS NULL AND expires_at > %s"
                " RETURNING user_id", (now, _hash(state), now))
            row = await cur.fetchone()
        if row is None:
            raise GoogleError("This sign in link has expired or was already used. Start again from the app.")
        user_id = str(row["user_id"])
        resp = await self.http.post(TOKEN_URL, data={
            "code": code, "client_id": client_id, "client_secret": client_secret,
            "redirect_uri": self.settings.google_redirect_uri, "grant_type": "authorization_code"})
        if resp.status_code != 200:
            raise GoogleError("Google did not accept the sign in. Start again from the app.")
        tokens = resp.json()
        refresh = tokens.get("refresh_token")
        if not refresh:
            raise GoogleError("Google did not grant offline access. Remove HaloAI from the account's third party "
                              "access and sign in again.")
        granted = set(tokens.get("scope", "").split())
        missing = [s for s in SCOPES if s not in granted and s != "openid"]
        if missing:
            raise GoogleError("Some permissions were not granted. Sign in again and allow all of them.")
        info = await self.http.get(USERINFO_URL, headers={"Authorization": f"Bearer {tokens['access_token']}"})
        email = info.json().get("email", "") if info.status_code == 200 else ""
        async with self.pool.connection() as conn:
            await conn.execute(
                "INSERT INTO google_accounts (user_id, email, refresh_token_enc, scopes, connected_at, updated_at)"
                " VALUES (%s, %s, %s, %s, %s, %s) ON CONFLICT (user_id) DO UPDATE SET email = EXCLUDED.email,"
                " refresh_token_enc = EXCLUDED.refresh_token_enc, scopes = EXCLUDED.scopes,"
                " updated_at = EXCLUDED.updated_at",
                (user_id, email, self._fernet().encrypt(refresh.encode()).decode(), " ".join(sorted(granted)), now, now))
        self._access[user_id] = (tokens["access_token"], time.monotonic() + int(tokens.get("expires_in", 3600)) - 60)
        for listener in self.on_change:
            listener(user_id)
        return email

    async def status(self, user_id: str) -> dict:
        async with self.pool.connection() as conn:
            cur = await conn.execute("SELECT email FROM google_accounts WHERE user_id = %s", (user_id,))
            row = await cur.fetchone()
        return {"connected": row is not None, "email": row["email"] if row else None}

    async def disconnect(self, user_id: str) -> dict:
        async with self.pool.connection() as conn:
            cur = await conn.execute("DELETE FROM google_accounts WHERE user_id = %s RETURNING refresh_token_enc",
                                     (user_id,))
            row = await cur.fetchone()
        self._access.pop(user_id, None)
        for listener in self.on_change:
            listener(user_id)
        if row and self.configured():  # best effort: also revoke at Google
            try:
                token = self._fernet().decrypt(row["refresh_token_enc"].encode()).decode()
                await self.http.post(REVOKE_URL, data={"token": token})
            except Exception:
                pass
        return {"connected": False}

    # ---------- for the Calendar, Gmail and People calls ----------

    def drop_access_token(self, user_id: str) -> None:
        """Google refused the cached token (revoked or expired early): refresh on the next call."""
        self._access.pop(user_id, None)

    async def access_token(self, user_id: str) -> str:
        cached = self._access.get(user_id)
        if cached and cached[1] > time.monotonic():
            return cached[0]
        client_id, client_secret = self._client()
        async with self.pool.connection() as conn:
            cur = await conn.execute("SELECT refresh_token_enc FROM google_accounts WHERE user_id = %s", (user_id,))
            row = await cur.fetchone()
        if row is None:
            raise GoogleNotConnected("No Google account connected")
        refresh = self._fernet().decrypt(row["refresh_token_enc"].encode()).decode()
        resp = await self.http.post(TOKEN_URL, data={"client_id": client_id, "client_secret": client_secret,
                                                     "refresh_token": refresh, "grant_type": "refresh_token"})
        if resp.status_code != 200:
            # Testing mode refresh tokens expire after 7 days (known gotcha): the user must sign in again.
            raise GoogleError("The Google sign in expired. Connect the account again from the app.")
        tokens = resp.json()
        self._access[user_id] = (tokens["access_token"], time.monotonic() + int(tokens.get("expires_in", 3600)) - 60)
        return tokens["access_token"]
