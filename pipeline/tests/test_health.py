import pytest
from fastapi.testclient import TestClient

from app.config import get_settings

TEST_KEY = "test-backend-key"


@pytest.fixture
def client(monkeypatch):
    monkeypatch.setenv("BACKEND_API_KEY", TEST_KEY)
    monkeypatch.setenv("NEBIUS_API_KEY", "unused-in-these-tests")
    get_settings.cache_clear()
    from app.main import app

    yield TestClient(app)
    get_settings.cache_clear()


def test_health_ok_with_key(client):
    r = client.get("/health", headers={"Authorization": f"Bearer {TEST_KEY}"})
    assert r.status_code == 200
    assert r.json() == {"status": "ok"}


def test_health_rejects_missing_key(client):
    assert client.get("/health").status_code == 401


def test_health_rejects_wrong_key(client):
    r = client.get("/health", headers={"Authorization": "Bearer wrong"})
    assert r.status_code == 401
