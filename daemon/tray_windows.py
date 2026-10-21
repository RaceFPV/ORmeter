"""Windows system-tray entry and state bridge for ORmeter.

Provides:
  TrayState    -- thread-safe scalar bridge (daemon loop writes, tray reads)
  header_text  -- pure helper producing the status-header string
  main()       -- tray entry: builds per-state icons, runs the daemon loop in a
                  supervised background thread, runs pystray.Icon on the main thread

The daemon loop (meter_daemon.main) is unaware of the tray; this module injects
only additive state-setter calls at its existing branch points.

Usage::

    python tray_windows.py
"""

import os
import sys
import threading
import time

# Resolve the daemon directory from __file__ so the sibling-module imports and
# the venv site-packages lookup work no matter what the current directory is --
# critical for logon autostart, where the Run entry starts with cwd = System32.
_DAEMON_DIR = os.path.dirname(os.path.abspath(__file__))
if _DAEMON_DIR not in sys.path:
    sys.path.insert(0, _DAEMON_DIR)

# Autostart launches us with the BASE interpreter's pythonw.exe, not the venv's
# (see autostart_windows._command). The base interpreter does not see the venv's
# site-packages, so add them here to resolve pystray/bleak/PIL. isdir guards the
# no-venv and already-inside-venv cases.
_VENV_SITE = os.path.join(_DAEMON_DIR, ".venv", "Lib", "site-packages")
if os.path.isdir(_VENV_SITE):
    import site
    site.addsitedir(_VENV_SITE)


class TrayState:
    """Shared state bridging the daemon asyncio loop to the tray.

    The loop writes via the set_* methods; the tray only reads the scalars. No
    lock is needed: these are atomic attribute assignments of simple Python
    scalars.
    """

    def __init__(self) -> None:
        self.state: str = "scanning"          # "connected" | "scanning" | "error"
        self.reason: str = ""                 # error reason string
        self.last_sync: float | None = None   # time.time() of the last good write

        # Populated by the daemon's main() at startup:
        self.loop = None         # running asyncio loop (for call_soon_threadsafe)
        self.stop_event = None   # asyncio.Event (the clean-shutdown hook)

    def set_connected(self, ts: float) -> None:
        self.state = "connected"
        self.reason = ""
        self.last_sync = ts

    def set_scanning(self) -> None:
        # BLE churn stays "Scanning": it is normal, not an error the user must act on.
        self.state = "scanning"
        self.reason = ""

    def set_error(self, why: str) -> None:
        self.state = "error"
        self.reason = why


def header_text(ts: TrayState) -> str:
    """The menu status-header string for the current state.

    Shapes::

        "Connected - last update HH:MM"
        "Connected - last update never"
        "Scanning..."
        "Error: {reason}"
    """
    if ts.state == "connected":
        when = time.strftime("%H:%M", time.localtime(ts.last_sync)) if ts.last_sync else "never"
        return f"Connected - last update {when}"
    if ts.state == "scanning":
        return "Scanning..."
    return f"Error: {ts.reason}"


# ---------------------------------------------------------------------------
# single-instance guard (named kernel mutex -- no stale-lock problem)
# ---------------------------------------------------------------------------

# "Local\\" scopes the mutex to the interactive logon: exactly the granularity
# we want, one tray per signed-in user. Both the headless autostart and any
# console instance live in the same session, so this catches the duplicate
# launch that has two processes fighting over the one BLE link.
_SINGLETON_MUTEX_NAME = "Local\\ORmeter-tray-singleton"
_ERROR_ALREADY_EXISTS = 183


def _acquire_single_instance():
    """Acquire the process-wide single-instance lock.

    Returns a handle to keep alive for the process lifetime if this is the only
    tray, or None if another ORmeter tray already holds the lock (the caller
    must then exit immediately, before touching BLE).

    A named kernel mutex releases automatically when its owner dies, so there
    is no stale-lock cleanup (unlike a pidfile). The handle is never closed --
    it lives until process exit, which is the lock lifetime we want.

    Off-Windows this is a no-op that always succeeds, so the pure helpers stay
    testable on a dev box.
    """
    if sys.platform != "win32":
        return object()

    import ctypes
    from ctypes import wintypes

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateMutexW.restype = wintypes.HANDLE
    kernel32.CreateMutexW.argtypes = [wintypes.LPVOID, wintypes.BOOL, wintypes.LPCWSTR]

    handle = kernel32.CreateMutexW(None, True, _SINGLETON_MUTEX_NAME)
    if not handle:
        # Could not create the mutex at all -- fail OPEN so a kernel quirk never
        # stops the tray from starting. Single-instance is hardening, not a
        # correctness requirement.
        return object()
    if ctypes.get_last_error() == _ERROR_ALREADY_EXISTS:
        return None
    return handle


def main() -> None:
    """Tray entry point: guard, build icons, start the daemon thread, run pystray.

    ``import pystray`` is deliberately inside this function so the module stays
    importable for unit tests of the pure helpers without a tray backend.
    """
    # Single-instance guard FIRST -- before icons, the daemon thread, or any BLE.
    _instance_lock = _acquire_single_instance()
    if _instance_lock is None:
        return

    import asyncio as _asyncio
    import pystray
    from pystray import Menu, MenuItem

    import autostart_windows as autostart
    from icon_assets import build_state_icons
    from meter_daemon import main as daemon_main, log as daemon_log

    images = build_state_icons()

    ts = TrayState()
    icon = pystray.Icon("ORmeter", images["scanning"], "ORmeter")

    # Set by the Quit handler so the supervisor below knows a clean stop was
    # requested and must NOT resurrect the loop.
    _quit_requested = threading.Event()

    def _run_daemon() -> None:
        """Run the asyncio loop, supervised.

        An unhandled crash used to end this thread for good: polling stopped
        silently and the tray froze on its last state until the user relaunched
        it. Now the traceback is logged, the tray flips to an actionable error,
        and the loop restarts with capped backoff. A CLEAN return means Quit was
        requested, so we stay down and do not restart.
        """
        backoff = 2
        while not _quit_requested.is_set():
            try:
                _asyncio.run(daemon_main(tray_state=ts))
                return  # clean exit == Quit requested
            except Exception as e:  # last-resort thread guard
                import traceback
                daemon_log(f"Daemon thread crashed: {e!r}")
                daemon_log(traceback.format_exc())
                ts.set_error(f"daemon crashed: {type(e).__name__}")
            if _quit_requested.is_set():
                return
            daemon_log(f"Restarting daemon loop in {backoff}s after crash")
            if _quit_requested.wait(timeout=backoff):
                return
            backoff = min(backoff * 2, 30)

    daemon_thread = threading.Thread(target=_run_daemon, daemon=True)
    daemon_thread.start()

    def _on_quit(icon_ref, _item) -> None:
        # Never set stop_event directly from the tray thread: asyncio.Event is
        # not thread-safe.
        #
        # After signalling, wait for the daemon thread to finish its graceful
        # shutdown (the loop's finally: client.disconnect()) BEFORE stopping the
        # icon. Without this join the daemon thread is killed mid-flight, the
        # device never gets a clean GATT disconnect, and it sits frozen on stale
        # data instead of returning to its waiting screen. The timeout caps the
        # block so Quit can never hang if a WinRT disconnect wedges.
        _quit_requested.set()
        if ts.loop is not None and ts.stop_event is not None:
            ts.loop.call_soon_threadsafe(ts.stop_event.set)
            daemon_thread.join(timeout=6.0)
        icon_ref.stop()

    def _on_toggle(_icon_ref, _item) -> None:
        if autostart.is_enabled():
            autostart.disable()
        else:
            # Pass THIS file explicitly -- without it, enable() defaults the Run
            # value to autostart_windows.py, which has no entry point and would
            # start nothing, silently breaking menu-enabled autostart.
            autostart.enable(tray_script=os.path.abspath(__file__))
        icon.update_menu()

    icon.menu = Menu(
        MenuItem(lambda _item: header_text(ts), None, enabled=False),
        # checked= is a callable for a live query, not a snapshot.
        MenuItem("Start at login", _on_toggle, checked=lambda _item: autostart.is_enabled()),
        MenuItem("Quit", _on_quit),
    )

    prev_state: dict = {"state": None, "last_sync": None}

    def _refresh(_icon: pystray.Icon) -> None:
        _icon.visible = True
        while _icon._running:  # type: ignore[attr-defined]
            current = ts.state
            last_sync = ts.last_sync
            state_changed = current != prev_state["state"]
            # Refresh on last_sync advance too, not only on state change: a
            # healthy daemon reading a flat usage value never changes state, so
            # a transition-only refresh would freeze the "last update" tooltip
            # and read as a dead daemon.
            if state_changed or last_sync != prev_state["last_sync"]:
                if state_changed:
                    _icon.icon = images[current]
                _icon.title = header_text(ts)
                # Toast only on the transition INTO error, not on every tick.
                if current == "error" and prev_state["state"] != "error":
                    _icon.notify(ts.reason or "ORmeter error", "ORmeter")
                prev_state["state"] = current
                prev_state["last_sync"] = last_sync
                _icon.update_menu()
            time.sleep(1.0)

    # Blocks the main thread until icon.stop() is called from _on_quit.
    icon.run(setup=_refresh)


if __name__ == "__main__":
    main()
