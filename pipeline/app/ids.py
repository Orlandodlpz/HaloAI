"""Hashes that bind approval to exactly what was shown."""

import base64
import hashlib
import json


def canonical_json(value) -> str:
    """Same data, same string: sorted keys, no extra spaces."""
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False, default=str)


def proposal_hash(draft: dict) -> str:
    """SHA-256 of the draft. Any change to the draft changes the hash."""
    return hashlib.sha256(canonical_json(draft).encode()).hexdigest()


def execution_key(request_id: str, revision: int, proposal_hash_: str) -> str:
    """26 characters: the first 128 bits of SHA-256(request, revision, hash) in base32.

    Deterministic, so a second approval of the same proposal (even with a new call_id
    after an app restart) finds the same execution instead of creating another one.
    Short enough for the device to keep in flash and send in its hello event."""
    digest = hashlib.sha256(f"{request_id}|{revision}|{proposal_hash_}".encode()).digest()
    return base64.b32encode(digest[:16]).decode().rstrip("=").lower()


def key_hash(api_key: str) -> str:
    return hashlib.sha256(api_key.encode()).hexdigest()


def body_hash(path: str, body: dict) -> str:
    """Identity of a call for call_id retries: same path and body (minus call_id)."""
    trimmed = {k: v for k, v in body.items() if k != "call_id"}
    return hashlib.sha256(f"{path}|{canonical_json(trimmed)}".encode()).hexdigest()
