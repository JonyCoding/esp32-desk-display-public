#!/usr/bin/env python3
"""Mac data provider and expiring command feed; ESP32 owns local operation."""
from __future__ import annotations

import argparse
import hmac
import json
import os
import secrets
import socket
import threading
import time
import uuid
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import urlparse

from quota import read_quota
from stock import STOCK_MONITOR

ROOT = Path(__file__).resolve().parent.parent

PORT = int(os.environ.get('DESKDISPLAY_PORT', '8765'))
MANUAL_TTL = 1800
PRESENCE_TTL = 12 * 3600
# Absolute page numbering shared with the firmware (Page enum order).
# 'clock' stays as a deprecated alias for 'loft' so old scripts keep working.
PAGES = ('loft', 'quota', 'status', 'stock')
PAGE_ALIASES = {'clock': 'loft'}
MODES = ('work', 'fun')
SOURCES = {'manual', 'mac_wake', 'mac_unlock', 'mac_lock', 'mac_sleep', 'auto'}


def valid_ttl(value: Any, default: int) -> int:
    value = default if value is None else value
    if type(value) is not int or not 1 <= value <= 86400:
        raise ValueError('ttl must be an integer from 1 to 86400')
    return value


class State:
    def __init__(self) -> None:
        self.lock = threading.RLock()
        self.session = uuid.uuid4().hex
        self.revision = 0
        self.commands: dict[str, dict[str, Any]] = {}
        self.mac_locked: bool | None = None
        self.mode = self.auto_mode()
        self.page = 1 if self.mode == 'work' else 0  # mode home: quota / loft
        self.source = 'auto'
        self.since = time.time()
        self.message: dict[str, Any] | None = None
        self.brightness: int | None = None
        self.brightness_revision = 0
        self.brightness_issued = 0.0
        self.resume_revision = 0
        self.resume_issued = 0.0
        self.quota_cache: dict[str, Any] = {'available': False}
        self.quota_error: str | None = None
        self.started = time.monotonic()

    def auto_mode(self) -> str:
        now = datetime.now()
        return 'work' if now.weekday() < 5 and 9 <= now.hour < 18 else 'fun'

    def _resolve(self) -> None:
        now = time.monotonic()
        if self.mac_locked:
            source = self.commands['presence']['source']
            if self.source != source:
                self.since = time.time()
            self.mode, self.page, self.source = 'fun', 0, source
            return
        command = next((self.commands[k] for k in ('manual', 'presence', 'auto')
                        if k in self.commands and self.commands[k]['until'] > now), None)
        mode = command['mode'] if command else self.auto_mode()
        source = command['source'] if command else 'auto'
        if mode != self.mode or source != self.source:
            home = 1 if mode == 'work' else 0
            self.mode, self.source, self.page, self.since = mode, source, home, time.time()
        if command:
            self.page = command['page']

    def effective_mode(self) -> str:
        with self.lock:
            self._resolve()
            return self.mode

    def _issue(self, mode: str, source: str, ttl: int, page: int | None = None) -> None:
        if source == 'manual' and self.mac_locked:
            raise ValueError('Mac session is locked')
        if page is None:
            page = 1 if mode == 'work' else 0  # mode home: quota / loft
        self.revision += 1
        now = time.monotonic()
        key = 'manual' if source == 'manual' else ('auto' if source == 'auto' else 'presence')
        self.commands[key] = {'id': f'{self.session}:{self.revision}', 'mode': mode, 'page': page,
                              'source': source, 'issued': now, 'until': now + ttl}
        self._resolve()

    def set_mode(self, mode: str, source: str = 'manual', ttl: int | None = None) -> None:
        if not isinstance(mode, str) or mode not in MODES:
            raise ValueError('mode must be work or fun')
        if not isinstance(source, str) or source not in SOURCES:
            raise ValueError('invalid source')
        ttl = valid_ttl(ttl, MANUAL_TTL if source in ('manual', 'auto') else PRESENCE_TTL)
        if source in ('mac_wake', 'mac_unlock') and mode != 'work':
            raise ValueError('wake/unlock mode must be work')
        if source in ('mac_lock', 'mac_sleep') and mode != 'fun':
            raise ValueError('lock/sleep mode must be fun')
        with self.lock:
            if source.startswith('mac_'):
                locked = source in ('mac_lock', 'mac_sleep')
                if locked != self.mac_locked:
                    if locked or self.mac_locked:
                        self.commands.pop('manual', None)
                    self.mac_locked = locked
                elif locked:
                    previous = self.commands.get('presence')
                    if previous and previous['source'] == source:
                        previous['until'] = time.monotonic() + ttl
                        return
            self._issue(mode, source, ttl)

    def handle_button(self, action: str) -> None:
        if not isinstance(action, str) or action not in ('short', 'long', 'double'):
            raise ValueError('action must be short, long or double')
        with self.lock:
            self._resolve()
            mode = self.mode
            page = (self.page + 1) % len(PAGES) if action == 'short' else 0
            if action == 'long':
                mode = 'fun' if mode == 'work' else 'work'
            self._issue(mode, 'manual', MANUAL_TTL, page)

    def set_message(self, text: str, ttl: int = 60) -> None:
        if not isinstance(text, str) or not text.strip() or len(text) > 160:
            raise ValueError('text must contain 1 to 160 characters')
        ttl = valid_ttl(ttl, 60)
        with self.lock:
            self.message = {'text': text, 'until': time.monotonic() + ttl}

    def set_page(self, page: str) -> None:
        if not isinstance(page, str) or page not in ('quota', 'clock', 'loft', 'status', 'stock'):
            raise ValueError('page must be quota, clock, loft, status or stock')
        with self.lock:
            self._resolve()
            page = PAGE_ALIASES.get(page, page)
            mode = 'work' if page == 'quota' else ('fun' if page == 'loft' else self.mode)
            self._issue(mode, 'manual', MANUAL_TTL, PAGES.index(page))

    def set_brightness(self, brightness: int) -> None:
        if type(brightness) is not int or not 8 <= brightness <= 160:
            raise ValueError('brightness must be an integer from 8 to 160')
        with self.lock:
            self.brightness = brightness
            self.brightness_revision = max(self.brightness_revision + 1, time.time_ns() // 1_000_000)
            self.brightness_issued = time.monotonic()

    def resume_carousel(self) -> None:
        with self.lock:
            if self.mac_locked:
                raise RequestError(409, 'Mac session is locked')
            self.commands.pop('manual', None)
            self.resume_revision = max(self.resume_revision + 1, time.time_ns() // 1_000_000)
            self.resume_issued = time.monotonic()

    def refresh_quota(self) -> None:
        try:
            data = read_quota()
        except Exception as exc:
            data = {'available': False, 'error': str(exc)}
        with self.lock:
            self.quota_error = None if data.get('available') else data.get('error', 'quota unavailable')
            if data.get('available') or not self.quota_cache.get('available'):
                self.quota_cache = data

    def snapshot(self) -> dict[str, Any]:
        with self.lock:
            self._resolve()
            now, wall = time.monotonic(), time.time()
            controls = {}
            for key, command in self.commands.items():
                if command['until'] > now:
                    controls[key] = {k: command[k] for k in ('id', 'mode', 'page', 'source')}
                    controls[key].update(remaining_ms=max(0, int((command['until'] - now) * 1000)),
                                         age_ms=max(0, int((now - command['issued']) * 1000)))
            quota = {'available': False} if self.mac_locked else dict(self.quota_cache)
            if quota.get('available'):
                quota['data_age_sec'] = max(0, int(wall - quota['snapshot_at_epoch']))
                quota['stale'] = quota['data_age_sec'] > 300 or self.quota_error is not None
            if self.quota_error and not self.mac_locked:
                quota['refresh_error'] = self.quota_error
            message = self.message if self.message and self.message['until'] > now else None
            return {'protocol': 2, 'mode': self.mode, 'page': self.page, 'source': self.source,
                    'since': self.since, 'epoch': int(wall), 'control': controls, 'mac_locked': self.mac_locked,
                    'resume_revision': self.resume_revision,
                    'resume_age_ms': min(0xFFFFFFFF,int((now-self.resume_issued)*1000)) if self.resume_revision else 0,
                    'manual_remaining_sec': controls.get('manual', {}).get('remaining_ms', 0) // 1000,
                    'data': {'clock': datetime.now().strftime('%H:%M'), 'date': datetime.now().strftime('%m-%d %a'),
                             'quota': quota, 'stock': STOCK_MONITOR.get_stock(),
                             'brightness': self.brightness,
                             'brightness_revision': self.brightness_revision,
                             'brightness_age_ms': min(0xFFFFFFFF, int((now - self.brightness_issued) * 1000)) if self.brightness is not None else 0,
                             'message': message['text'] if message else None,
                             'message_remaining_ms': max(0, int((message['until'] - now) * 1000)) if message else 0}}


STATE = State()


class RequestError(ValueError):
    def __init__(self, code: int, message: str):
        self.code = code
        super().__init__(message)


class Handler(BaseHTTPRequestHandler):
    server_version = 'DeskDisplay/0.2'

    def setup(self) -> None:
        super().setup()
        self.connection.settimeout(5)

    def log_message(self, fmt: str, *args: Any) -> None:
        print(f'[{self.log_date_time_string()}] {fmt % args}', flush=True)

    def _send(self, code: int, payload: dict[str, Any]) -> None:
        self.close_connection = True
        body = json.dumps(payload, ensure_ascii=False, allow_nan=False).encode('utf-8')
        self.send_response(code)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Connection', 'close')
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError, socket.timeout):
            pass

    def _authorized(self) -> bool:
        expected = f'Bearer {self.server.token}'
        if not hmac.compare_digest(self.headers.get('Authorization', '').encode(), expected.encode()):
            self._send(401, {'error': 'Bearer token required'})
            return False
        return True

    def _body(self) -> dict[str, Any]:
        if self.headers.get('Transfer-Encoding'):
            raise RequestError(400, 'Transfer-Encoding is unsupported')
        if self.headers.get_content_type() != 'application/json':
            raise RequestError(415, 'Content-Type must be application/json')
        try:
            length = int(self.headers.get('Content-Length', '0'))
        except ValueError:
            raise RequestError(400, 'invalid Content-Length')
        if length > 4096:
            raise RequestError(413, 'request body too large')
        if length <= 0:
            raise RequestError(400, 'JSON object required')
        try:
            body = self.rfile.read(length)
            if len(body) != length:
                raise RequestError(400, 'incomplete body')
            result = json.loads(body, parse_constant=lambda _: (_ for _ in ()).throw(ValueError('invalid JSON number')))
        except (ValueError, UnicodeDecodeError, socket.timeout) as exc:
            raise RequestError(400, 'invalid JSON body') from exc
        if not isinstance(result, dict):
            raise RequestError(400, 'JSON object required')
        return result

    def do_GET(self) -> None:
        path = urlparse(self.path).path
        if path == '/health':
            self._send(200, {'ok': True, 'uptime_sec': int(time.monotonic() - STATE.started), 'protocol': 2})
        elif path == '/state':
            if self._authorized():
                if self.client_address and self.client_address[0] not in ('127.0.0.1', '::1', 'localhost'):
                    try:
                        (ROOT / '.local/esp32-host').write_text(self.client_address[0])
                    except Exception:
                        pass
                self._send(200, STATE.snapshot())
        else:
            self._send(404, {'error': 'not found'})

    def do_POST(self) -> None:
        if not self._authorized():
            return
        try:
            path = urlparse(self.path).path
            if path not in ('/mode', '/button', '/message', '/page', '/display', '/resume'):
                raise RequestError(404, 'not found')
            body = self._body()
            if path == '/mode':
                STATE.set_mode(body.get('mode'), body.get('source', 'manual'), body.get('ttl'))
            elif path == '/button':
                STATE.handle_button(body.get('action'))
            elif path == '/page':
                STATE.set_page(body.get('page'))
            elif path == '/display':
                STATE.set_brightness(body.get('brightness'))
            elif path == '/resume':
                STATE.resume_carousel()
            else:
                STATE.set_message(body.get('text'), body.get('ttl', 60))
            self._send(200, STATE.snapshot())
        except RequestError as exc:
            self._send(exc.code, {'error': str(exc)})
        except (ValueError, TypeError) as exc:
            self._send(400, {'error': str(exc)})


def quota_loop(interval: int) -> None:
    while True:
        STATE.refresh_quota()
        time.sleep(interval)


def load_token(path: Path) -> str:
    token = os.environ.get('DESKDISPLAY_TOKEN', '').strip()
    if not token:
        path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        try:
            fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, 'w') as fh:
                fh.write(secrets.token_hex(24) + '\n')
        except FileExistsError:
            pass
        token = path.read_text().strip()
    if len(token) < 16 or not token.isascii() or any(c.isspace() for c in token):
        raise ValueError('token must contain at least 16 non-whitespace ASCII characters')
    return token


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', default='0.0.0.0')
    parser.add_argument('--port', type=int, default=PORT)
    parser.add_argument('--quota-interval', type=int, default=30)
    parser.add_argument('--token-file', type=Path, default=Path(__file__).resolve().parents[1] / '.local/token')
    parser.add_argument('--no-mac-events', action='store_true')
    args = parser.parse_args()
    if args.quota_interval < 1:
        parser.error('--quota-interval must be positive')
    token = load_token(args.token_file)
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.token = token
    threading.Thread(target=quota_loop, args=(args.quota_interval,), daemon=True).start()
    STOCK_MONITOR.start()
    if not args.no_mac_events:
        from mac_events import start_watcher
        start_watcher(STATE)
    print(f'Desk display on http://{args.host}:{args.port}; token file: {args.token_file}', flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == '__main__':
    main()
