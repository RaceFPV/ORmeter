"""Login-autostart toggle for ORmeter.

Manages a per-user HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run value
named "ORmeter" that launches the tray headlessly via pythonw.exe (no console
window).

winreg is Windows stdlib; the import is guarded so this module stays importable
on other platforms for the pure-helper unit tests.

Public API:
  enable(tray_script=None)  -- write/overwrite the Run value
  disable()                 -- remove the Run value; idempotent when absent
  is_enabled()              -- True if the Run value is currently present
"""

import os
import sys
import time

try:
    import winreg as winreg  # type: ignore[import]
except ImportError:
    winreg = None  # type: ignore[assignment]

# Registry key (no leading backslash -- OpenKey takes a path relative to the hive).
_RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
_VALUE_NAME = "ORmeter"


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def looks_like_wsl_or_unc(path: str) -> bool:
    r"""True for a path a logon autostart cannot rely on.

    A UNC path -- notably ``\\wsl.localhost\...`` or ``\\wsl$\...`` -- points
    into a WSL distro's filesystem. WSL is not guaranteed to be running at
    logon, so the Run entry would launch a pythonw.exe that cannot even read
    its own script. Worse, it fails *silently* under pythonw (no console, so
    nothing is printed and nothing appears to happen). The install must live on
    a local Windows path.

    Also catches a ``/mnt/c/...`` path, which is a WSL-side path and only
    meaningful inside the distro.
    """
    p = (path or "").strip()
    if p.startswith("\\\\") or p.startswith("//"):
        return True
    return p.startswith("/mnt/")


def _command(tray_script: str | None = None) -> str:
    """Build the headless launch command for the Run value.

    Uses the BASE interpreter's pythonw.exe -- ``sys.base_exec_prefix`` points
    at the real Python install even from inside a venv -- NOT the venv's
    ``Scripts\\pythonw.exe``. The venv pythonw is a redirector stub that
    re-launches the CONSOLE python.exe as a child process, which pops a black
    console window at logon and kills the tray when that window is closed. The
    base pythonw loads in-process and is genuinely windowless.

    The path is never hard-coded; both paths are quoted for spaces.
    tray_windows.py adds the venv's site-packages to sys.path itself, so the
    venv's dependencies still resolve under the base interpreter.
    """
    pythonw = os.path.join(sys.base_exec_prefix, "pythonw.exe")
    script = os.path.abspath(tray_script if tray_script is not None else __file__)
    if looks_like_wsl_or_unc(script):
        raise ValueError(
            f"refusing to register autostart for a WSL/UNC path: {script}\n"
            "Autostart runs at logon, when WSL may not be running, and fails "
            "silently under pythonw. Install ORmeter on a local Windows path."
        )
    return f'"{pythonw}" "{script}"'


def enable(tray_script: str | None = None) -> None:
    """Write (or overwrite) the HKCU Run value pointing at pythonw.exe.

    No admin elevation required -- HKCU is per-user.
    """
    cmd = _command(tray_script)
    with winreg.OpenKey(
        winreg.HKEY_CURRENT_USER, _RUN_KEY, 0, winreg.KEY_SET_VALUE
    ) as key:
        winreg.SetValueEx(key, _VALUE_NAME, 0, winreg.REG_SZ, cmd)
    log(f"Autostart enabled: {cmd}")


def disable() -> None:
    """Remove the HKCU Run value. Idempotent -- no error if already absent."""
    try:
        with winreg.OpenKey(
            winreg.HKEY_CURRENT_USER, _RUN_KEY, 0, winreg.KEY_SET_VALUE
        ) as key:
            winreg.DeleteValue(key, _VALUE_NAME)
        log("Autostart disabled")
    except FileNotFoundError:
        pass  # already absent


def is_enabled() -> bool:
    """True if the Run value is present right now.

    Queries the live registry on every call, so the state reflects external
    changes (e.g. the user deleting the value by hand).
    """
    try:
        with winreg.OpenKey(
            winreg.HKEY_CURRENT_USER, _RUN_KEY, 0, winreg.KEY_QUERY_VALUE
        ) as key:
            winreg.QueryValueEx(key, _VALUE_NAME)
            return True
    except FileNotFoundError:
        return False
