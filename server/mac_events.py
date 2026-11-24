"""Observe the current macOS GUI session without third-party Python packages."""
from __future__ import annotations

import ctypes
import plistlib
import sys
import threading
import time


class SessionReader:
    def __init__(self) -> None:
        self.cg = ctypes.CDLL('/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics')
        self.cf = ctypes.CDLL('/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation')
        self.cg.CGSessionCopyCurrentDictionary.restype = ctypes.c_void_p
        self.cf.CFPropertyListCreateData.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_long, ctypes.c_ulong, ctypes.c_void_p]
        self.cf.CFPropertyListCreateData.restype = ctypes.c_void_p
        self.cf.CFDataGetLength.argtypes = [ctypes.c_void_p]
        self.cf.CFDataGetLength.restype = ctypes.c_long
        self.cf.CFDataGetBytePtr.argtypes = [ctypes.c_void_p]
        self.cf.CFDataGetBytePtr.restype = ctypes.c_void_p
        self.cf.CFRelease.argtypes = [ctypes.c_void_p]

    def locked(self) -> bool:
        session = self.cg.CGSessionCopyCurrentDictionary()
        if not session:
            raise RuntimeError('GUI session unavailable')
        data = None
        try:
            data = self.cf.CFPropertyListCreateData(None, session, 100, 0, None)
            if not data:
                raise RuntimeError('cannot read GUI session')
            value = plistlib.loads(ctypes.string_at(self.cf.CFDataGetBytePtr(data), self.cf.CFDataGetLength(data)))
            # ScreenIsLocked is an undocumented session key; absence means
            # unlocked. OnConsoleKey is the public CGSession property.
            return bool(value.get('CGSSessionScreenIsLocked', False)) or not value.get('kCGSSessionOnConsoleKey', False)
        finally:
            if data:
                self.cf.CFRelease(data)
            self.cf.CFRelease(session)


class PresenceMonitor:
    """Pure event detector, separately testable from platform access."""
    def __init__(self) -> None:
        self.previous: bool | None = None
        self.last_poll: float | None = None
        self.last_sent = 0.0

    def sample(self, locked: bool, now: float) -> str | None:
        woke = self.last_poll is not None and now - self.last_poll > 15
        changed = locked != self.previous
        self.last_poll, self.previous = now, locked
        if changed or woke or now - self.last_sent >= 60:
            self.last_sent = now
            return 'mac_lock' if locked else ('mac_wake' if woke else 'mac_unlock')
        return None


def start_watcher(state):
    if sys.platform != 'darwin':
        return None

    def watch() -> None:
        monitor = PresenceMonitor()
        try:
            reader = SessionReader()
        except (OSError, AttributeError) as exc:
            print(f'Mac events disabled: {exc}', flush=True)
            return
        failed = False
        while True:
            try:
                source = monitor.sample(reader.locked(), time.time())
                if source:
                    state.set_mode('fun' if source == 'mac_lock' else 'work', source)
                failed = False
            except Exception as exc:
                if not failed:
                    print(f'Mac events unavailable (time rules remain active): {exc}', flush=True)
                failed = True
            time.sleep(0.5)

    thread = threading.Thread(target=watch, name='mac-events', daemon=True)
    thread.start()
    return thread
