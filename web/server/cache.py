"""Content-addressed result cache -- and the thing that makes the free lane free.

The product rule is: a result already in this cache costs nothing and needs no
account; producing a NEW one is what the limited free trial meters. So this is
not only a performance cache, it is the authorization boundary. Two
consequences that shape the code:

  * `get` must never be able to run the compiler. It is a pure lookup.
  * The prebaked examples are warmed into the cache at startup, which is what
    lets an anonymous visitor exercise every feature on the playground page
    without ever signing in.

Entries are immutable: a key is a hash of (source, options), so a hit is
always a correct answer for that exact request.
"""

from __future__ import annotations

import json
import os
import tempfile
import threading
from pathlib import Path
from typing import Any

from . import config


class ResultCache:
    def __init__(self, directory: Path | None = None) -> None:
        self.dir = Path(directory or config.CACHE_DIR)
        self.dir.mkdir(parents=True, exist_ok=True)
        self._lock = threading.Lock()
        self.hits = 0
        self.misses = 0

    def _path(self, key: str) -> Path:
        # Two-level fanout so one directory never holds 100k entries.
        return self.dir / key[:2] / f"{key}.json"

    def get(self, key: str) -> dict[str, Any] | None:
        path = self._path(key)
        try:
            payload = json.loads(path.read_text())
        except (OSError, json.JSONDecodeError):
            with self._lock:
                self.misses += 1
            return None
        with self._lock:
            self.hits += 1
        return payload

    def put(self, key: str, value: dict[str, Any]) -> None:
        path = self._path(key)
        path.parent.mkdir(parents=True, exist_ok=True)
        # Atomic replace: a reader never sees a half-written entry.
        fd, tmp = tempfile.mkstemp(dir=path.parent, suffix=".tmp")
        try:
            with os.fdopen(fd, "w") as handle:
                json.dump(value, handle)
            os.replace(tmp, path)
        except BaseException:
            try:
                os.unlink(tmp)
            except OSError:
                pass
            raise

    def has(self, key: str) -> bool:
        return self._path(key).exists()

    def stats(self) -> dict[str, int]:
        with self._lock:
            return {"hits": self.hits, "misses": self.misses}
