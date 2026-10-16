"""Every package app/ imports must be a production dependency, not a dev one.

On 2026-10-15 httpx was only a dev dependency: all tests passed (dev installs it), the server
install did not have it, and the backend failed to start after a deploy."""

import ast
import sys
import tomllib
from pathlib import Path

PIPELINE = Path(__file__).resolve().parent.parent
# import name -> distribution name, where they differ or come in through another package
PROVIDED_BY = {"pydantic": "fastapi", "starlette": "fastapi", "psycopg_pool": "psycopg", "pydantic_settings": "pydantic-settings",
               "sse_starlette": "sse-starlette"}


def test_app_imports_only_production_dependencies():
    deps = {d.split(">")[0].split("[")[0].split("=")[0].strip().lower()
            for d in tomllib.loads((PIPELINE / "pyproject.toml").read_text())["project"]["dependencies"]}
    missing = set()
    for path in (PIPELINE / "app").glob("*.py"):
        for node in ast.walk(ast.parse(path.read_text())):
            names = [a.name for a in node.names] if isinstance(node, ast.Import) else \
                [node.module] if isinstance(node, ast.ImportFrom) and node.module and node.level == 0 else []
            for name in names:
                top = name.split(".")[0]
                if top == "app" or top in sys.stdlib_module_names:
                    continue
                dist = PROVIDED_BY.get(top, top).lower().replace("_", "-")
                if dist not in deps:
                    missing.add(f"{path.name}: {top}")
    assert not missing, f"imported but not a production dependency: {sorted(missing)}"
