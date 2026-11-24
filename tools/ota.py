#!/usr/bin/env python3
"""Push firmware to the desk display in resumable chunks.

A slow or interrupted /update stream used to require restarting the whole
image. Local measurements have shown variable HTTP latency and flash-write
stalls; the current measurements, rather than Wi-Fi's nominal rate, determine
push time. Cross-band routing alone does not establish the bottleneck.

/ota prepares the inactive slot, then negotiates one authenticated stream
with bounded receive/write/ACK frames. Older firmware uses raw-body slices.
The device preserves its accepted prefix when a connection is interrupted:

    POST /ota/begin   header X-Ota-Total: <bytes>   allocate the idle slot
    GET  /ota/state                                 where it stopped
    GET  /ota/stream  header X-Ota-Offset: <n>      desk-ota/1 upgrade
    POST /ota/chunk   header X-Ota-Offset: <n>      compatible raw slices
    POST /ota/end                                   verify, swap, reboot

This client drives that protocol, retries a slice a few times, and re-reads
the device's own counter whenever the answer is unclear -- the device, not
this script, decides where the image stopped.

    python3 tools/ota.py 192.0.2.2
    python3 tools/ota.py 192.0.2.2 --chunk 32768 --retries 8
    python3 tools/ota.py 192.0.2.2 --legacy       # one-shot /update

Exit codes: 0 pushed, 1 failed, 2 device has no /ota (firmware older than the
resumable endpoint) and --legacy was not requested.
"""
from __future__ import annotations

import argparse
import base64
import http.client
import json
import struct
import sys
import time
import urllib.parse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BIN = ROOT / '.local/firmware/desk_display.ino.bin'
DEFAULT_TOKEN = ROOT / '.local/token'

MIN_TOTAL = 65536          # the firmware refuses to begin() below this
MAX_TOTAL = 0x200000       # one 2 MB OTA slot


class PushError(RuntimeError):
    pass


class AuthError(PushError):
    """Token rejected; retrying cannot help."""


class NoChunkEndpoint(PushError):
    """The device runs firmware whose OTA is still single-shot /update only."""


class Device:
    """Minimal HTTP client for one device, one connection per request."""

    def __init__(self, host: str, token: str, timeout: float = 15.0):
        parsed = urllib.parse.urlsplit(host if '//' in host else '//' + host)
        self.host = parsed.hostname or host
        self.port = parsed.port or 80
        self.timeout = timeout
        raw = f'admin:{token}'.encode()
        self.auth = 'Basic ' + base64.b64encode(raw).decode()

    def request(self, method: str, path: str, body: bytes | None = None,
                content_type: str | None = None, timeout: float | None = None,
                extra: dict | None = None):
        conn = http.client.HTTPConnection(self.host, self.port,
                                          timeout=timeout or self.timeout)
        headers = {'Authorization': self.auth, 'Connection': 'close'}
        if body is not None:
            headers['Content-Type'] = content_type or 'application/octet-stream'
            headers['Content-Length'] = str(len(body))
        if extra:
            headers.update(extra)
        try:
            started = time.monotonic()
            conn.connect()
            connected = time.monotonic()
            conn.request(method, path, body=body, headers=headers)
            sent = time.monotonic()
            resp = conn.getresponse()
            payload = resp.read()
            self.last_timing = {'connect_ms': round((connected-started)*1000, 1),
                                'send_ms': round((sent-connected)*1000, 1),
                                'response_ms': round((time.monotonic()-sent)*1000, 1)}
            return resp.status, payload
        finally:
            conn.close()

    @staticmethod
    def _json(status: int, payload: bytes) -> dict:
        try:
            parsed = json.loads(payload or b'{}')
        except ValueError:
            parsed = {}
        if not isinstance(parsed, dict):
            parsed = {}
        parsed['_status'] = status
        return parsed

    def _call(self, method: str, path: str, **kwargs) -> dict:
        status, payload = self.request(method, path, **kwargs)
        if status == 401:
            raise AuthError('device rejected the token (401); '
                            '.local/token must match DESK_API_TOKEN')
        if status == 404:
            raise NoChunkEndpoint(path)
        return self._json(status, payload)

    def state(self) -> dict:
        return self._call('GET', '/ota/state')

    def begin(self, total: int) -> dict:
        # Sizes ride in headers because the device parses a raw body before it
        # parses the query string; headers are read first for every method.
        return self._call('POST', '/ota/begin', body=b'',
                          extra={'X-Ota-Total': str(total)})

    def chunk(self, offset: int, blob: bytes, timeout: float) -> dict:
        return self._call('POST', '/ota/chunk', body=blob, timeout=timeout,
                          extra={'X-Ota-Offset': str(offset)})

    def end(self) -> dict:
        return self._call('POST', '/ota/end', body=b'')

    def status(self) -> dict:
        status, payload = self.request('GET', '/api/status', timeout=10)
        return self._json(status, payload)


class OtaStream:
    """Versioned HTTP upgrade; each bounded data frame requires a progress ACK."""
    def __init__(self, dev: Device, offset: int):
        self.conn = http.client.HTTPConnection(dev.host, dev.port, timeout=dev.timeout)
        self.response = None
        try:
            self.conn.request('GET', '/ota/stream', headers={
                'Authorization': dev.auth, 'X-Ota-Offset': str(offset),
                'Connection': 'Upgrade', 'Upgrade': 'desk-ota/1'})
            self.socket = self.conn.sock
            self.response = self.conn.getresponse()
            if self.response.status == 401:
                raise AuthError('device rejected stream credentials')
            if self.response.status != 101 or self.response.getheader('Upgrade') != 'desk-ota/1':
                raise NoChunkEndpoint('stream upgrade unavailable')
            self.offset = offset
        except BaseException:
            self.close()
            raise

    def _ack(self) -> dict:
        line = self.response.fp.readline(512)
        if not line or len(line) >= 512:
            raise PushError('stream progress ACK missing or oversized')
        try:
            reply = json.loads(line)
        except ValueError as exc:
            raise PushError('invalid stream progress ACK') from exc
        if not isinstance(reply, dict) or not reply.get('ok'):
            raise PushError(f'stream refused data: {reply}')
        return reply

    def chunk(self, offset: int, blob: bytes, timeout: float) -> dict:
        if offset != self.offset or not 0 < len(blob) <= 65535:
            raise PushError('stream frame offset or size invalid')
        self.socket.settimeout(timeout)
        self.socket.sendall(struct.pack('!H', len(blob)) + blob)
        reply = self._ack()
        if reply.get('progress') != offset + len(blob):
            raise PushError('stream ACK offset differs; resync required')
        self.offset = int(reply['progress'])
        return reply

    def finish(self) -> None:
        self.socket.sendall(b'\0\0')
        reply = self._ack()
        if reply.get('progress') != self.offset:
            raise PushError('stream final offset differs')
        self.close()

    def close(self) -> None:
        if self.response is not None:
            self.response.close()
        self.conn.close()


def _multipart(image: bytes, boundary: str) -> bytes:
    head = (
        f'--{boundary}\r\n'
        'Content-Disposition: form-data; name="firmware"; '
        'filename="desk_display.ino.bin"\r\n'
        'Content-Type: application/octet-stream\r\n\r\n'
    ).encode()
    return head + image + f'\r\n--{boundary}--\r\n'.encode()


def legacy_push(dev: Device, image: bytes, log) -> dict:
    """One-shot multipart POST to /update, for firmware without /ota."""
    boundary = '----deskdisplayota'
    log('  /ota not present; falling back to one-shot /update '
        '(this is the push that dies on a slow link)')
    dev.request('POST', '/update', body=_multipart(image, boundary),
                content_type=f'multipart/form-data; boundary={boundary}',
                timeout=420)
    return {'legacy': True}


def push(dev: Device, image: bytes, chunk: int = 65536, retries: int = 6,
         log=print, allow_legacy: bool = True, resume: bool = True) -> dict:
    """Stream `image` into the idle OTA slot, resuming from the device counter.

    The device accepted-byte counter includes its pending flash buffer; every
    failure path re-reads it instead of assuming the last acknowledged offset.
    """
    total = len(image)
    if not MIN_TOTAL <= total <= MAX_TOTAL:
        raise PushError(f'image is {total} bytes; the device accepts '
                        f'{MIN_TOTAL}..{MAX_TOTAL}')

    started = time.monotonic()
    for attempt in range(retries + 1):
        try:
            st = dev.state()
            if not resume or not st.get('open') or int(st.get('total') or 0) != total:
                st = dev.begin(total)
                if not st.get('ok'):
                    raise PushError(f'begin failed: {st}')
            break
        except NoChunkEndpoint:
            if not allow_legacy:
                raise
            return legacy_push(dev, image, log)
        except (OSError, http.client.HTTPException) as exc:
            if attempt == retries:
                raise PushError(f'device unreachable before upload: {exc}') from exc
            log(f'  ! setup timed out ({type(exc).__name__}); rechecking device state ({attempt+1}/{retries})')
            time.sleep(1.5)
    sent = int(st.get('progress') or 0)
    if sent:
        log(f'  resuming at {sent / 1024:.1f} KB of {total / 1024:.1f} KB')

    preferred = st.get('preferred_chunk')
    if isinstance(preferred, int) and 4096 <= preferred < chunk:
        chunk = preferred
        log(f'  device requests {chunk // 1024} KB buffered slices')
    target = st.get('target')
    initial_offset = sent
    transfer_started = time.monotonic()
    slices = (total + chunk - 1) // chunk
    done = sent // chunk
    fails = 0
    stream = None
    use_stream = st.get('stream') is True
    if use_stream:
        chunk = min(chunk, 61440)
        log('  one authenticated connection; receive/write/ACK per frame')
    while sent < total:
        if done == sent // chunk:
            done += 1
        blob = image[sent:sent + chunk]
        # 7 KB/s is the observed floor; give a slice 4x its expected time plus
        # headroom so a stalled AP retries the slice rather than the image.
        timeout = max(30.0, 20.0 + len(blob) / 1800.0)
        reply = None
        try:
            if use_stream and stream is None:
                try:
                    stream = OtaStream(dev, sent)
                except NoChunkEndpoint:
                    use_stream = False
                    log('  stream unavailable; falling back to HTTP slices')
            reply = stream.chunk(sent, blob, timeout) if stream else dev.chunk(sent, blob, timeout)
            if reply.get('ok') and int(reply.get('progress') or 0) > sent:
                sent = int(reply['progress'])
                fails = 0
                rate = (sent - initial_offset) / max(time.monotonic() - started, 1e-6) / 1024
                log(f'  [{min(done, slices):>3}/{slices}] {sent / 1024:>8.1f} KB '
                    f'{100.0 * sent / total:>5.1f}%  {rate:>5.1f} KB/s'
                    + (f"  device {reply['chunk_ms']}ms / flash {reply['chunk_flash_ms']}ms" if 'chunk_ms' in reply else ''))
                continue
        except (OSError, http.client.HTTPException, PushError) as exc:
            if stream is not None:
                stream.close(); stream = None
            if isinstance(exc, (AuthError, NoChunkEndpoint)):
                raise
            note = f'{type(exc).__name__}: {exc}'
        else:
            note = reply.get('err') or f'status {reply.get("_status")}'

        fails += 1
        if fails > retries:
            raise PushError(f'slice at {sent} kept failing ({note})')
        log(f'  ! slice at {sent} failed ({note}); resyncing ({fails}/{retries})')
        time.sleep(min(0.4 * fails, 3.0))
        try:
            st = dev.state()
        except (OSError, http.client.HTTPException) as exc:
            log(f'  ! device unreachable ({type(exc).__name__}); retrying')
            continue
        if st.get('open') and int(st.get('total') or 0) == total:
            sent = int(st.get('progress') or 0)
        else:
            st = dev.begin(total)
            if not st.get('ok'):
                raise PushError(f're-begin failed: {st}')
            sent = int(st.get('progress') or 0)

    if stream is not None:
        try:
            stream.finish()
        except (OSError, http.client.HTTPException, PushError):
            stream.close() # all data ACKed; /ota/end will validate the image
    commit_uncertain = False
    try:
        reply = dev.end()
    except (OSError, http.client.HTTPException) as exc:
        log(f'  commit reply lost ({type(exc).__name__}); reboot must be verified')
        commit_uncertain = True
        reply = {}
    if not commit_uncertain and not reply.get('ok'):
        raise PushError(f'end failed: {reply}')
    elapsed = time.monotonic() - started
    log(f'  {"transfer completed; commit pending verification" if commit_uncertain else "image verified"} '
        f'in {elapsed:.1f}s ({(total-initial_offset)/max(elapsed,1e-6)/1024:.1f} KB/s)')
    return {'legacy': False, 'seconds': elapsed, 'target': target,
            'commit_uncertain': commit_uncertain,
            'transfer_seconds': time.monotonic()-transfer_started,
            'flash_seconds': (reply.get('flash_ms',0)+reply.get('begin_ms',0))/1000}


def wait_for_boot(dev: Device, seconds: float, log=print, previous_build=None, previous_uptime=None, expected_partition=None) -> str:
    """Poll /api/status until the device answers again; return its build id."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            st = dev.status()
            build = st.get('build')
            rebooted = previous_uptime is not None and isinstance(st.get('uptime'), (int, float)) and st['uptime'] < previous_uptime - 2
            partition_ok = not expected_partition or st.get('boot_part') == expected_partition
            if build and partition_ok and (previous_build is None or str(build) != previous_build or rebooted):
                return str(build)
        except (OSError, http.client.HTTPException):
            pass
        time.sleep(1.5)
    return ''


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description='Push firmware to the desk display in resumable chunks.')
    ap.add_argument('host', help='device address, e.g. 192.0.2.2')
    ap.add_argument('--bin', default=str(DEFAULT_BIN), help='firmware image')
    ap.add_argument('--token-file', default=str(DEFAULT_TOKEN),
                    help='file holding DESK_API_TOKEN')
    ap.add_argument('--chunk', type=int, default=65536,
                    help='bytes per request (default 65536)')
    ap.add_argument('--retries', type=int, default=6,
                    help='retries per chunk before giving up')
    ap.add_argument('--fresh', action='store_true',
                    help='start a new image instead of resuming a saved prefix')
    ap.add_argument('--legacy', action='store_true',
                    help='skip /ota and POST the whole file to /update')
    ap.add_argument('--wait', type=float, default=60.0,
                    help='seconds to wait for the new build to answer (0 = no wait)')
    args = ap.parse_args(argv)

    def log(line: str) -> None:
        print(line, flush=True)

    image = Path(args.bin).read_bytes()
    token_path = Path(args.token_file)
    if not token_path.is_file():
        print(f'Missing token file: {token_path}', file=sys.stderr)
        return 1
    token = token_path.read_text().strip()
    dev = Device(args.host, token)

    log(f'pushing {len(image) / 1024:.1f} KB to {args.host} '
        f'(requested slice limit {args.chunk // 1024} KB)')
    previous_build = None
    previous_uptime = None
    observed_at = time.monotonic()
    try:
        before = dev.status()
        previous_build = before.get('build')
        previous_uptime = before.get('uptime')
    except (OSError, http.client.HTTPException):
        pass
    try:
        if args.legacy:
            legacy_push(dev, image, log)
            result = {'legacy': True}
        else:
            result = push(dev, image, chunk=args.chunk, retries=args.retries,
                          log=log, resume=not args.fresh)
    except NoChunkEndpoint:
        print('Device has no /ota endpoint: USB-flash this image once, or '
              're-run with --legacy.', file=sys.stderr)
        return 2
    except PushError as exc:
        print(f'push failed: {exc}', file=sys.stderr)
        return 1

    log('device is rebooting into the new image')
    if args.wait > 0:
        expected_uptime = previous_uptime + time.monotonic() - observed_at if isinstance(previous_uptime, (int, float)) else None
        build = wait_for_boot(dev, args.wait, log, previous_build=previous_build, previous_uptime=expected_uptime, expected_partition=result.get('target'))
        if build:
            log(f'back online, build {build}')
        else:
            log(f'new build not confirmed after {args.wait:.0f}s; check the panel')
            return 1
    elif result.get('commit_uncertain'):
        log('commit cannot be confirmed without --wait; deployment not verified')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
