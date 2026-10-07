"""List Nemotron models visible to our Token Factory key.

This is how the model ID in config and docs/CLAUDE.md gets verified.
Run: .venv/bin/python scripts/list_models.py
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from app.config import get_settings  # noqa: E402
from app.llm import get_client  # noqa: E402

models = get_client().models.list()
ids = sorted(m.id for m in models)
print(f"base_url: {get_settings().nebius_base_url}")
print(f"total models visible: {len(ids)}")
for model_id in ids:
    if "nemotron" in model_id.lower():
        print(model_id)
