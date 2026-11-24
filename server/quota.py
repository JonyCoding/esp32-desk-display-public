#!/usr/bin/env python3
"""Incremental reader of timestamped Codex token_count quota events."""
from __future__ import annotations

import json
import math
import os
import threading
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


def _codex_sessions_dir() -> Path:
    return Path(os.environ.get('CODEX_HOME', str(Path.home() / '.codex'))).expanduser() / 'sessions'


def _timestamp(value: Any) -> float | None:
    try:
        result = datetime.fromisoformat(value.replace('Z', '+00:00'))
        if result.tzinfo is None:
            return None
        return result.timestamp()
    except (ValueError, TypeError, AttributeError, OverflowError):
        return None


def _window(value: Any) -> dict[str, Any]:
    value = value if isinstance(value, dict) else {}
    used = value.get('used_percent')
    if isinstance(used, bool) or not isinstance(used, (int, float)) or not math.isfinite(used):
        used = None
    else:
        used = max(0.0, min(100.0, float(used)))
    reset = value.get('resets_at')
    try:
        reset = int(reset) if reset is not None else None
        formatted = datetime.fromtimestamp(reset, timezone.utc).astimezone().strftime('%Y-%m-%d %H:%M') if reset else None
    except (ValueError, TypeError, OSError, OverflowError):
        reset, formatted = None, None
    return {'used_percent': used, 'remaining_percent': round(100 - used, 1) if used is not None else None,
            'resets_at': formatted, 'resets_at_epoch': reset, 'window_minutes': value.get('window_minutes')}


class QuotaReader:
    """Cache offsets; keep partial final lines for the next scan, and notice rotation."""
    MAX_LINE = 4 * 1024 * 1024

    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.root: Path | None = None
        self.files: dict[Path, tuple[int, int, int, int]] = {}
        self.newest: tuple[float, dict[str, Any]] | None = None

    def read(self) -> dict[str, Any]:
        with self.lock:
            root = _codex_sessions_dir()
            if root != self.root:
                self.root, self.files, self.newest = root, {}, None
            if not root.exists():
                return {'available': False, 'source': 'unavailable', 'error': 'sessions directory missing'}
            paths = set(root.rglob('rollout-*.jsonl'))
            for missing in self.files.keys() - paths:
                del self.files[missing]
            for path in paths:
                try:
                    st = path.stat()
                    previous = self.files.get(path)
                    offset = previous[2] if previous and previous[:2] == (st.st_dev, st.st_ino) and st.st_size >= previous[2] else 0
                    if previous and st.st_size == offset and previous[3] == st.st_mtime_ns:
                        continue
                    if previous and st.st_size == offset and previous[3] != st.st_mtime_ns:
                        offset = 0  # same-size rewrite
                    with path.open('rb') as fh:
                        fh.seek(offset)
                        while True:
                            start = fh.tell()
                            line = fh.readline(self.MAX_LINE + 1)
                            if not line:
                                break
                            if len(line) > self.MAX_LINE:
                                # Skip an oversized complete record without allocating it all.
                                while line and not line.endswith(b'\n'):
                                    line = fh.readline(self.MAX_LINE + 1)
                                if not line:
                                    fh.seek(start)
                                    break
                                continue
                            if not line.endswith(b'\n'):
                                fh.seek(start)
                                break
                            if b'"rate_limits"' not in line:
                                continue
                            try:
                                record = json.loads(line)
                                if not isinstance(record, dict) or record.get('type') != 'event_msg':
                                    continue
                                payload = record.get('payload')
                                if not isinstance(payload, dict) or payload.get('type') != 'token_count':
                                    continue
                                snap = payload.get('rate_limits')
                                stamp = _timestamp(record.get('timestamp'))
                                if not isinstance(snap, dict) or snap.get('limit_id') not in (None, 'codex'):
                                    continue
                                if stamp is None or stamp > time.time() + 300 or not isinstance(snap.get('primary'), dict):
                                    continue
                                if _window(snap['primary'])['remaining_percent'] is None:
                                    continue
                                if self.newest is None or stamp >= self.newest[0]:
                                    self.newest = (stamp, snap)
                            except (ValueError, TypeError, UnicodeDecodeError):
                                continue
                        self.files[path] = (st.st_dev, st.st_ino, fh.tell(), st.st_mtime_ns)
                except OSError:
                    continue
            if self.newest is None:
                return {'available': False, 'source': 'unavailable', 'error': 'no timestamped quota event found'}
            stamp, snap = self.newest
            credits = snap.get('credits')
            credits = credits if isinstance(credits, dict) else {}
            return {'available': True, 'source': 'local_logs', 'five_hour': _window(snap.get('primary')),
                    'weekly': _window(snap.get('secondary')), 'plan_type': snap.get('plan_type'),
                    'credits': {'has_credits': bool(credits.get('has_credits')), 'balance': credits.get('balance')},
                    'snapshot_at_epoch': stamp, 'data_age_sec': max(0, int(time.time() - stamp))}


_READER = QuotaReader()


def read_quota() -> dict[str, Any]:
    return _READER.read()


if __name__ == '__main__':
    print(json.dumps(read_quota(), ensure_ascii=False, indent=2))
