"""Google sign in (B4 step 1) against a fake Google. No real Google calls."""

import time
import uuid
from urllib.parse import parse_qs, urlparse

import httpx
import pytest
from cryptography.fernet import Fernet

from app.google_auth import REVOKE_URL, SCOPES, TOKEN_URL, USERINFO_URL, GoogleAuth, GoogleError
from tests.conftest import auth


class FakeGoogle:
    def __init__(self):
        self.calls: list[tuple[str, dict]] = []
        self.grant_scopes = " ".join(SCOPES)
        self.refresh_ok = True

    def handler(self, request: httpx.Request) -> httpx.Response:
        data = dict(parse_qs(request.content.decode())) if request.content else {}
        data = {k: v[0] for k, v in data.items()}
        self.calls.append((str(request.url), data))
        if str(request.url) == TOKEN_URL and data.get("grant_type") == "authorization_code":
            return httpx.Response(200, json={"access_token": "at1", "refresh_token": "rt-SECRET", "expires_in": 3600,
                                             "scope": self.grant_scopes})
        if str(request.url) == TOKEN_URL and data.get("grant_type") == "refresh_token":
            if not self.refresh_ok:
                return httpx.Response(400, json={"error": "invalid_grant"})
            return httpx.Response(200, json={"access_token": "at2", "expires_in": 3600})
        if str(request.url) == USERINFO_URL:
            return httpx.Response(200, json={"email": "haloai.cov@gmail.com"})
        if str(request.url) == REVOKE_URL:
            return httpx.Response(200)
        return httpx.Response(404)


@pytest.fixture
def fake_google():
    return FakeGoogle()


@pytest.fixture
def gsettings(settings):
    return settings.model_copy(update={"google_client_id": "client-123.apps.googleusercontent.com",
                                       "google_client_secret": "client-secret-xyz",
                                       "token_encryption_key": Fernet.generate_key().decode()})


@pytest.fixture
async def gauth(pool, gsettings, clock, fake_google):
    from pydantic import SecretStr
    s = gsettings.model_copy(update={k: SecretStr(getattr(gsettings, k)) for k in
                                     ("google_client_id", "google_client_secret", "token_encryption_key")})
    return GoogleAuth(pool, s, clock, httpx.AsyncClient(transport=httpx.MockTransport(fake_google.handler)))


@pytest.fixture
async def gclient(services, gauth):
    import httpx as hx

    from app.main import create_app
    services.google = gauth
    app = create_app(services)
    async with hx.AsyncClient(transport=hx.ASGITransport(app=app), base_url="http://test") as c:
        yield c


async def connect(gclient, key):
    r = await gclient.post("/v1/google/connect", headers=auth(key), json={"call_id": str(uuid.uuid4())})
    return r, parse_qs(urlparse(r.json().get("url", "")).query)


async def test_connect_link_asks_for_exactly_our_permissions(gclient, keys, pool):
    r, q = await connect(gclient, keys["phone"])
    assert r.status_code == 200 and r.json()["url"].startswith("https://accounts.google.com/o/oauth2/v2/auth?")
    assert q["client_id"] == ["client-123.apps.googleusercontent.com"]
    assert q["redirect_uri"] == ["https://api.haloai-cov.io/v1/google/callback"]
    assert q["scope"][0].split() == SCOPES and "gmail.readonly" not in q["scope"][0]
    assert q["access_type"] == ["offline"] and q["prompt"] == ["consent"]
    async with pool.connection() as conn:  # the state is stored only as a hash
        cur = await conn.execute("SELECT state_hash FROM oauth_states")
        assert (await cur.fetchone())["state_hash"] != q["state"][0]


async def test_callback_connects_and_stores_the_token_encrypted(gclient, keys, pool, gauth):
    _, q = await connect(gclient, keys["phone"])
    r = await gclient.get("/v1/google/callback", params={"state": q["state"][0], "code": "c0de"})  # no API key
    assert r.status_code == 200 and "connected to haloai.cov@gmail.com" in r.text
    s = await gclient.get("/v1/google/status", headers=auth(keys["phone"]))
    assert s.json() == {"connected": True, "email": "haloai.cov@gmail.com"}
    async with pool.connection() as conn:
        cur = await conn.execute("SELECT refresh_token_enc FROM google_accounts")
        stored = (await cur.fetchone())["refresh_token_enc"]
    assert "rt-SECRET" not in stored and gauth._fernet().decrypt(stored.encode()) == b"rt-SECRET"


async def test_state_is_single_use_and_expires(gclient, keys, clock):
    _, q = await connect(gclient, keys["phone"])
    ok = await gclient.get("/v1/google/callback", params={"state": q["state"][0], "code": "c"})
    again = await gclient.get("/v1/google/callback", params={"state": q["state"][0], "code": "c"})
    assert ok.status_code == 200 and again.status_code == 400 and "expired or was already used" in again.text
    _, q2 = await connect(gclient, keys["phone"])
    clock.advance(11 * 60)
    late = await gclient.get("/v1/google/callback", params={"state": q2["state"][0], "code": "c"})
    assert late.status_code == 400
    unknown = await gclient.get("/v1/google/callback", params={"state": "made-up", "code": "c"})
    denied = await gclient.get("/v1/google/callback", params={"error": "access_denied"})
    assert unknown.status_code == denied.status_code == 400


async def test_missing_permissions_are_refused(gclient, keys, fake_google):
    fake_google.grant_scopes = "openid https://www.googleapis.com/auth/calendar.events"  # user unticked some
    _, q = await connect(gclient, keys["phone"])
    r = await gclient.get("/v1/google/callback", params={"state": q["state"][0], "code": "c"})
    assert r.status_code == 400 and "permissions" in r.text
    assert (await gclient.get("/v1/google/status", headers=auth(keys["phone"]))).json()["connected"] is False


async def test_disconnect_revokes_at_google(gclient, keys, fake_google):
    _, q = await connect(gclient, keys["phone"])
    await gclient.get("/v1/google/callback", params={"state": q["state"][0], "code": "c"})
    r = await gclient.post("/v1/google/disconnect", headers=auth(keys["phone"]), json={"call_id": str(uuid.uuid4())})
    assert r.json() == {"connected": False}
    assert (REVOKE_URL, {"token": "rt-SECRET"}) in fake_google.calls


async def test_demo_site_cannot_connect_google(gclient, keys):
    r, _ = await connect(gclient, keys["demo"])
    assert r.status_code == 422 and "demo site" in r.json()["error"]["message"]


async def test_endpoints_need_the_key_but_the_callback_does_not(gclient):
    assert (await gclient.get("/v1/google/status")).status_code == 401
    assert (await gclient.post("/v1/google/connect", json={"call_id": str(uuid.uuid4())})).status_code == 401
    assert (await gclient.get("/v1/google/callback", params={"state": "x", "code": "y"})).status_code == 400  # not 401


async def test_access_token_is_cached_then_refreshed(gauth, gclient, keys, user, fake_google):
    _, q = await connect(gclient, keys["phone"])
    await gclient.get("/v1/google/callback", params={"state": q["state"][0], "code": "c"})
    assert await gauth.access_token(user) == "at1"  # from the sign in, cached
    gauth._access[user] = ("at1", time.monotonic() - 1)  # expire it
    assert await gauth.access_token(user) == "at2"
    fake_google.refresh_ok = False
    gauth._access.clear()
    with pytest.raises(GoogleError, match="expired"):
        await gauth.access_token(user)


async def test_not_configured_says_so(services, keys, pool, settings, clock):
    import httpx as hx

    from app.main import create_app
    services.google = GoogleAuth(pool, settings, clock)  # no client id or secret
    async with hx.AsyncClient(transport=hx.ASGITransport(app=create_app(services)), base_url="http://test") as c:
        r = await c.post("/v1/google/connect", headers=auth(keys["phone"]), json={"call_id": str(uuid.uuid4())})
    assert r.status_code == 502 and "not set up" in r.json()["error"]["message"]
