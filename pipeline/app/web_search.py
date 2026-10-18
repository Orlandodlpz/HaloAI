"""Tavily web search for the real world (B4 step 4): search then act.

Results are untrusted data: they reach the model only through tools.untrusted(), an address
found in them can never become a recipient unless the user said it (tools._untraced), and
memory never learns from them. Kept small on purpose (5 results, short excerpts, basic depth):
every token and every second counts against the 5 s target.
"""

import httpx

from app.timing import hop
from app.tools import ToolError

SEARCH_URL = "https://api.tavily.com/search"
MAX_RESULTS = 5
SNIPPET_CHARS = 500  # Tavily excerpts ran 450 to 1500 chars in a probe (2026-10-17); keep the part with times
QUERY_CHARS = 200


class TavilySearch:
    def __init__(self, settings, http: httpx.AsyncClient | None = None):
        self.settings = settings
        self.http = http or httpx.AsyncClient(timeout=settings.web_search_timeout_s)

    def configured(self) -> bool:
        return self.settings.tavily_api_key is not None

    async def search(self, query: str) -> list[dict]:
        query = " ".join(str(query).split())[:QUERY_CHARS]
        if not query:
            raise ToolError("query must not be empty")
        with hop("tavily_search") as f:
            try:
                resp = await self.http.post(
                    SEARCH_URL,
                    headers={"Authorization": f"Bearer {self.settings.tavily_api_key.get_secret_value()}"},
                    json={"query": query, "search_depth": "basic", "max_results": MAX_RESULTS,
                          "include_answer": False, "include_raw_content": False},
                    timeout=self.settings.web_search_timeout_s)
            except httpx.HTTPError:
                f["result"] = "no_answer"
                raise ToolError("web search did not answer; answer without it, or say you could not check") from None
            f["status"] = resp.status_code
            if resp.status_code != 200:
                raise ToolError("web search failed; answer without it, or say you could not check")
            results = [{"title": str(r.get("title", ""))[:120],
                        "snippet": " ".join(str(r.get("content", "")).split())[:SNIPPET_CHARS],
                        "url": str(r.get("url", ""))[:200]}
                       for r in (resp.json().get("results") or [])[:MAX_RESULTS]]
            f["results"] = len(results)
        return results
