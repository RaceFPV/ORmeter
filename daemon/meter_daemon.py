#!/usr/bin/env python3
"""ORmeter host daemon (Windows).

Polls the OpenRouter usage API and writes a compact JSON payload to the ORmeter
desk display over Bluetooth LE.

Windows-only, and unavoidably so: the BLE link goes through WinRT (via bleak),
and WSL2 ships no Bluetooth stack at all -- ``/sys/class/bluetooth`` does not
exist. The repository may live anywhere; this process must run on native
Windows.

Run::

    python meter_daemon.py                 # normal daemon loop
    python meter_daemon.py --print-payload # fetch once, print the payload, exit
                                           # (no BLE; safe with the daemon running)
"""

import argparse
import asyncio
import calendar
import datetime
import json
import logging
import logging.handlers
import os
import re
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

import httpx
from bleak import BleakClient, BleakScanner
from bleak.backends.device import BLEDevice
from bleak.exc import BleakError

# --- GATT contract --------------------------------------------------------
# These UUIDs are baked into the DEVICE firmware, not into this project, so
# they are fixed until the firmware is rebuilt. Do not "tidy" them.
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"

# The device's ADVERTISED / BONDED name as the firmware reports it today.
# Windows stores this as the PnP FriendlyName, which is what we filter on.
# It is a firmware string: changing it here without also rebuilding the
# firmware means the bonded device will never be found. Overridable for
# testing, and to be flipped when the firmware is rebranded.
# Must match the firmware's advertised name EXACTLY. The two are coupled: this
# string is what we filter the Windows PnP table on, and the firmware's has to
# be short enough to fit the 31-byte advertising packet next to the service UUID
# (8 usable characters) or the device advertises with no name at all and is
# invisible to the OS Bluetooth UI. See the firmware's DEVICE_NAME for the math.
DEVICE_NAME = os.environ.get("ORMETER_DEVICE_NAME", "ORmeter")

# --- Timing ---------------------------------------------------------------
POLL_INTERVAL = 60         # seconds between pushes to the device
TICK = 5                   # loop wake granularity; bounds how fast Quit lands

# --- Link resilience ------------------------------------------------------
# Values carried across from the reference implementation, where they were
# tuned against real WinRT failure modes on this exact hardware. Do not
# "simplify" them without a device to test on: rediscovering them costs a
# reflash and a re-pair each round.
CONNECT_RETRIES = 3        # attempts before giving up on one device handle
CONNECT_RETRY_DELAY = 2.0  # seconds between failed connect attempts
ZOMBIE_BREAK_LIMIT = 1     # consecutive write failures before abandoning a
                           # half-open link. N=1 abandons at ~60s, leaving
                           # headroom to reconnect and poll again inside the
                           # 120s freshness budget; N=2 would overshoot it
                           # before reconnect even begins.
RECONNECT_BACKOFF_CAP = 8  # seconds; keeps stacked retries inside that budget

# --- Paths ----------------------------------------------------------------
APP_DIR = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local")) / "ORmeter"
CONFIG_FILE = APP_DIR / "config"

OPENROUTER_API_BASE = "https://openrouter.ai/api/v1"


# ---------------------------------------------------------------------------
# logging
# ---------------------------------------------------------------------------

def _build_file_logger() -> logging.Logger | None:
    """Rotating file logger, or None.

    Under autostart we run on pythonw.exe, which has no console: stdout is
    discarded and is in fact None, so print() is unsafe. A rotating file is
    then the only trail when the daemon stalls in the field. Gated to win32 so
    unit tests on other platforms never write stray log files.
    """
    if sys.platform != "win32":
        return None
    logger = logging.getLogger("ormeter.daemon")
    if logger.handlers:
        return logger  # idempotent across re-import
    # 512 KB x3, same rotation budget the reference used -- a long field stall
    # must not be able to grow the log without bound.
    path = APP_DIR / "meter.log"
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        handler = logging.handlers.RotatingFileHandler(
            path, maxBytes=512 * 1024, backupCount=3, encoding="utf-8"
        )
    except OSError:
        return None  # best-effort: logging must never stop the daemon
    handler.setFormatter(logging.Formatter("%(asctime)s %(message)s", "%Y-%m-%d %H:%M:%S"))
    logger.addHandler(handler)
    logger.setLevel(logging.INFO)
    logger.propagate = False
    return logger


_FILE_LOGGER = _build_file_logger()


def log(msg: str) -> None:
    line = f"[{time.strftime('%H:%M:%S')}] {msg}"
    # sys.stdout is None under pythonw and print() would raise; guard it so a
    # missing console can never kill the thread (the silent-freeze mode).
    try:
        print(line, flush=True)
    except (OSError, ValueError, AttributeError, RuntimeError):
        pass
    if _FILE_LOGGER is not None:
        _FILE_LOGGER.info(msg)


# ---------------------------------------------------------------------------
# errors
# ---------------------------------------------------------------------------

class AuthError(Exception):
    """A 401/403 from OpenRouter: the key really is rejected and only the user
    can fix it. Deliberately distinct from a None return, which means a
    TRANSIENT failure (DNS, timeout, rate-limit, 5xx) that must never be
    reported as a credential problem."""


class SourceConfigError(Exception):
    """The provider setup is wrong in a way only the user can fix -- no key
    found, or the key has no spend limit configured. Distinct from None
    (transient, retry quietly) and from AuthError (provider rejected us)."""


# ---------------------------------------------------------------------------
# config
# ---------------------------------------------------------------------------

def read_config() -> dict:
    """Parse the config file into a dict of lowercased key -> raw string value.

    Format is ``key = value`` with ``#`` comments and blank lines ignored.
    Re-read on every poll, so edits take effect within ~60s with no restart --
    that property is load-bearing and worth preserving on any refactor.
    """
    out: dict[str, str] = {}
    try:
        text = CONFIG_FILE.read_text(encoding="utf-8")
    except OSError:
        return out
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if "=" not in line:
            continue
        key, val = line.split("=", 1)
        out[key.strip().lower()] = val.strip()
    return out


def read_wallet_reference() -> float | None:
    """The optional ``wallet_reference`` (USD).

    What the bottom bar is measured against. Unset means OpenRouter's total
    purchased credits -- "how much of what I bought is left" -- which is the
    wrong denominator: it grows with every top-up, so the gauge sags toward 0%
    permanently and eventually reads "top up now" right after a top-up. Set it
    to a number you actually think in (typically your usual top-up) for a
    denominator that holds still.
    """
    raw = read_config().get("wallet_reference")
    if raw is None:
        return None
    try:
        ref = float(raw)
    except ValueError:
        return None
    return ref if ref > 0 else None


# ---------------------------------------------------------------------------
# OpenRouter
# ---------------------------------------------------------------------------

def _is_num(value) -> bool:
    """True for a real int/float (bools excluded)."""
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _parse_reset_epoch(iso) -> float | None:
    """Absolute epoch for an OpenRouter ISO reset timestamp, or None.

    OpenRouter returns e.g. ``"2026-11-01T00:00:00.000Z"``. Guarded: garbage
    must never crash the poll thread, it degrades to the neutral default.
    """
    if not iso or not isinstance(iso, str):
        return None
    try:
        return datetime.datetime.fromisoformat(iso.replace("Z", "+00:00")).timestamp()
    except (ValueError, OSError, OverflowError):
        return None


def _dotenv_candidates() -> list[Path]:
    """Ordered .env paths to probe (first hit wins).

    Deliberately self-contained: the daemon reads a .env next to its own config
    rather than reaching into another tool's files, so a copy of ORmeter on any
    machine behaves the same way.
    """
    out = []
    if explicit := os.environ.get("ORMETER_ENV_PATH"):
        out.append(Path(explicit))
    out.append(CONFIG_FILE.parent / ".env")
    return out


def read_openrouter_key() -> str | None:
    """The OpenRouter API key: the environment first, then a .env beside the config."""
    if env_key := os.environ.get("OPENROUTER_API_KEY"):
        if env_key.strip():
            return env_key.strip()
    for path in _dotenv_candidates():
        try:
            for line in path.read_text(encoding="utf-8").splitlines():
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                key, val = line.split("=", 1)
                if key.strip() == "OPENROUTER_API_KEY":
                    val = val.strip().strip('"').strip("'")
                    if val:
                        return val
        except OSError:
            continue
    return None


def _fmt_wallet(balance: float) -> str:
    """Short dollar figure for the firmware's caption slot.

    The firmware's layout budgets the caption for a constant's width and then
    renders whatever we send at that size -- and digits and '$' are wider
    glyphs than the lowercase text that constant assumes, so a same-length
    string still clips. Whole dollars keeps the tail at 2-5 glyphs.
    """
    b = max(0.0, balance)
    if b >= 10_000:
        return f"${b / 1000:.0f}k"
    if b >= 1_000:
        return f"${b / 1000:.1f}k"
    return f"${b:.0f}"


def _period_from_reset(now: float, reset_epoch: float) -> dict:
    """Fraction of the billing period elapsed (``tp``, 0-100) and its length in
    days (``pd``), derived from an exact reset instant.

    Monthly window is assumed: the API exposes the reset instant but not the
    period length, and spend-limit periods are monthly-only today.
    """
    try:
        dt_end = datetime.datetime.fromtimestamp(reset_epoch)
        prev_month = dt_end.month - 1 or 12
        prev_year = dt_end.year if dt_end.month > 1 else dt_end.year - 1
        prev_day = min(dt_end.day, calendar.monthrange(prev_year, prev_month)[1])
        dt_start = dt_end.replace(year=prev_year, month=prev_month, day=prev_day)
        period_start = dt_start.timestamp()
    except (OSError, OverflowError, ValueError):
        # Windows datetime conversion raises OSError/OverflowError/ValueError
        # for out-of-range values (a far-future reset overflows fromtimestamp).
        # Garbage must never crash the poll thread.
        return {"tp": 0, "pd": 30}
    period_len = reset_epoch - period_start
    if period_len <= 0:
        return {"tp": 0, "pd": 30}
    pct_val = (now - period_start) / period_len * 100
    return {
        "tp": max(0, min(100, int(round(pct_val)))),
        "pd": int(round(period_len / 86400)),
    }


async def poll_openrouter() -> dict | None:
    """Fetch usage from OpenRouter and build the device payload.

    Two documented, authenticated GETs:
        /credits -> total_credits, total_usage      (prepaid balance)
        /key     -> limit, limit_remaining, usage,  (key spend quota)
                    effective_budget.resets_at      (the real reset instant)

    Raises AuthError on 401/403 so the caller can signal no-data. Returns None
    on a transient failure (network/DNS/5xx) so the caller stays quiet and
    retries. Raises SourceConfigError when the setup itself is wrong.
    """
    key = read_openrouter_key()
    if not key:
        raise SourceConfigError(
            "no OpenRouter API key. Set OPENROUTER_API_KEY in the environment, "
            f"or put 'OPENROUTER_API_KEY=sk-or-...' in {CONFIG_FILE.parent / '.env'}"
        )

    headers = {"Authorization": f"Bearer {key}", "Accept": "application/json"}
    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            credits_resp = await http.get(f"{OPENROUTER_API_BASE}/credits", headers=headers)
            key_resp = await http.get(f"{OPENROUTER_API_BASE}/key", headers=headers)
    except httpx.HTTPError as e:
        log(f"OpenRouter API call failed: {e}")
        return None

    for name, resp in (("credits", credits_resp), ("key", key_resp)):
        if resp.status_code in (401, 403):
            log(f"OpenRouter {name} HTTP {resp.status_code}: {resp.text[:200]}")
            raise AuthError(resp.status_code)
        if resp.status_code >= 400:
            log(f"OpenRouter {name} HTTP {resp.status_code}: {resp.text[:200]}")
            return None

    credits = (credits_resp.json() or {}).get("data") or {}
    key_data = (key_resp.json() or {}).get("data") or {}

    limit = key_data.get("limit")
    remaining = key_data.get("limit_remaining")
    if not (_is_num(limit) and float(limit) > 0 and _is_num(remaining)):
        # No per-key spend cap -> there is no meaningful percentage to show.
        raise SourceConfigError(
            "the OpenRouter key has no spend limit set (set one in the dashboard)"
        )
    limit_v, remaining_v = float(limit), float(remaining)
    used_pct = max(0.0, min(100.0, (limit_v - remaining_v) / limit_v * 100.0))

    # Prefer the effective budget envelope; fall back to the budgets list.
    reset_iso = None
    budget = key_data.get("effective_budget")
    if isinstance(budget, dict):
        reset_iso = budget.get("resets_at")
    if not reset_iso:
        budgets = key_data.get("budgets")
        if isinstance(budgets, list) and budgets and isinstance(budgets[0], dict):
            reset_iso = budgets[0].get("resets_at")

    now = time.time()
    reset_epoch = _parse_reset_epoch(reset_iso)
    if reset_epoch and reset_epoch > now:
        bp = _period_from_reset(now, reset_epoch)
        sr = max(0, int(round((reset_epoch - now) / 60.0)))
    else:
        bp = {"tp": 0, "pd": 30}
        sr = 0

    total_credits = float(credits.get("total_credits") or 0.0)
    total_usage = float(credits.get("total_usage") or 0.0)
    balance = total_credits - total_usage

    # The bottom row is the wallet. `tp` drives BOTH its "%d%%" number and its
    # bar fill, so it becomes the remaining share of the reference; `rd` carries
    # the balance itself for the caption. NOTE: the row's pill still reads
    # "Period" and its caption still opens with "Resets" -- both are compiled
    # into the firmware and not reachable from here.
    reference = read_wallet_reference()
    if not (reference and reference > 0):
        reference = total_credits
    wallet_pct = max(0, min(100, int(round(balance / reference * 100)))) if reference > 0 else 0
    bp["tp"] = wallet_pct
    bp["rd"] = _fmt_wallet(balance)

    log(
        f"OpenRouter: quota {used_pct:.2f}% of ${limit_v:.2f} "
        f"(${remaining_v:.2f} left) - wallet ${max(0.0, balance):.2f} "
        f"({wallet_pct}% of ${reference:.2f} reference) - quota resets {reset_iso or 'unknown'}"
    )

    payload = {
        "s": int(round(used_pct)),
        "sr": sr,
        "w": 0,
        "wr": 0,
        "st": "limited" if used_pct >= 100 else "allowed",
        "acct": "ent",
        # The raw figures behind "s". A bare percentage never says what it is a
        # percentage OF, which makes the device's bottom row unreadable -- the
        # firmware renders these as "$5.25 of $50".
        "limit": round(limit_v, 2),
        "used": round(limit_v - remaining_v, 2),
        **bp,
        "ok": True,
    }
    return payload


# ---------------------------------------------------------------------------
# BLE
# ---------------------------------------------------------------------------

def _mac_from_pnp_instance_id(instance_id: str) -> str | None:
    """Recover a canonical BLE MAC ("AA:BB:CC:DD:EE:FF") from a PnP instance id.

    Windows encodes a paired BLE device's address in its PnP instance id as a
    12-hex run after a ``DEV_`` token, e.g.::

        BTHLE\\DEV_AABBCCDDEEFF\\7&B8081D1&0&AABBCCDDEEFF  ->  AA:BB:CC:DD:EE:FF

    Returns None when no ``DEV_<12 hex>`` token is present. Pure -- the
    subprocess that produces the instance id lives in discover_bonded_address().
    """
    m = re.search(r"DEV_([0-9A-Fa-f]{12})(?![0-9A-Fa-f])", instance_id)
    if not m:
        return None
    h = m.group(1).upper()
    return ":".join(h[i:i + 2] for i in range(0, 12, 2))


def discover_bonded_address() -> str | None:
    """Return the BLE address of the bonded meter, or None.

    This is the least obvious thing in the whole host side, so it is worth
    stating plainly: **a paired device that Windows is already connected to
    stops advertising**, so a BleakScanner scan cannot see it -- and that is
    the steady state once paired. WinRT can still connect to it directly by
    address, so we recover the address from the OS instead:

    1. ``ORMETER_BLE_ADDRESS`` env
       override -- skips discovery entirely, for pinning and for testing.
    2. The Windows PnP table, filtered to the device's FriendlyName.

    Never scans for a nearby device by name, so it cannot grab a stranger's
    unit. Non-Windows or any failure returns None.
    """
    override = os.environ.get("ORMETER_BLE_ADDRESS")
    if override:
        return override.strip().upper()
    if sys.platform != "win32":
        return None
    command = (
        "Get-PnpDevice -Class Bluetooth -ErrorAction SilentlyContinue | "
        f"Where-Object {{ $_.FriendlyName -eq '{DEVICE_NAME}' }} | "
        "Select-Object -ExpandProperty InstanceId"
    )
    try:
        result = subprocess.run(
            ["powershell", "-NoProfile", "-NonInteractive", "-Command", command],
            capture_output=True,
            text=True,
            timeout=10,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
    except (OSError, subprocess.SubprocessError) as e:
        log(f"Bonded-address lookup failed: {e}")
        return None
    for line in result.stdout.splitlines():
        if mac := _mac_from_pnp_instance_id(line):
            return mac
    return None


async def scan_for_meter() -> BLEDevice | None:
    """Find the meter by scanning for what it advertises, or None.

    This is the discovery path that needs NO pairing and NO hardcoded address:
    the device identifies itself by its service UUID, with its advertised name as
    a fallback. Matching the 128-bit service UUID first is a far stronger
    identity check than the name alone, so a scan cannot silently grab a
    stranger's unit.

    Returns a full BLEDevice (not a bare address string), because that is what
    WinRT needs in order to connect without performing its own advertisement
    lookup -- see the note in acquire_target().
    """
    try:
        found = await BleakScanner.discover(timeout=6.0, return_adv=True)
    except Exception as e:  # noqa: BLE001 -- a scan failure must never kill the daemon
        log(f"Scan failed: {e}")
        return None

    by_uuid: list[BLEDevice] = []
    by_name: list[BLEDevice] = []
    for _address, (device, adv) in found.items():
        uuids = [str(u).lower() for u in (adv.service_uuids or [])]
        if SERVICE_UUID.lower() in uuids:
            by_uuid.append(device)
        elif (device.name or "") == DEVICE_NAME:
            by_name.append(device)

    if by_uuid:
        log(f"Found {DEVICE_NAME} by advertised service UUID at {by_uuid[0].address}")
        return by_uuid[0]
    if by_name:
        log(f"Found {DEVICE_NAME} by advertised name at {by_name[0].address}")
        return by_name[0]
    return None


async def acquire_target() -> BLEDevice | None:
    """A connectable handle for the meter, or None.

    Three ways to find it, tried in order, so nothing needs to be hardcoded:

    1. An explicit address override (pinning, testing).
    2. The Windows PnP table -- the device is paired to this machine.
    3. A scan for what the device advertises -- needs no pairing at all, which
       is what makes the meter work on a machine it was never paired with.
    """
    address = discover_bonded_address()
    if address:
        # CRITICAL: hand BleakClient a BLEDevice, not the bare address string.
        # WinRT's connect() resolves a bare string via an advertisement scan
        # (find_device_by_address) -- which always fails for a bonded device that
        # has stopped advertising, the very case this branch handles. A BLEDevice
        # sets _device_info directly, so WinRT skips that scan.
        log(f"Using paired/configured address {address}")
        return BLEDevice(address, DEVICE_NAME, None)

    log("No paired or pinned address; scanning for the meter...")
    device = await scan_for_meter()
    if device is None:
        log("Meter not found by pin, pairing, or scan")
    return device


class Session:
    """One connected BLE session: payload writes plus the device's poll request."""

    def __init__(self, client: BleakClient) -> None:
        self.client = client
        self.refresh_requested = asyncio.Event()

    def _on_refresh(self, _char, _data: bytearray) -> None:
        log("Refresh requested by device")
        self.refresh_requested.set()

    async def setup_refresh_subscription(self) -> None:
        """Subscribe to the device's poll request.

        Optional: the 60s loop works without it. WinRT's start_notify() CCCD
        write can raise a raw OSError/WinError (not wrapped as BleakError) when
        the peer's GATT server is transiently unavailable -- e.g. a
        just-power-cycled ESP32. Degrade gracefully rather than crash the
        daemon, so a power-cycle reconnect stays in one process.
        """
        try:
            await self.client.start_notify(REQ_CHAR_UUID, self._on_refresh)
        except (BleakError, ValueError, OSError) as e:
            log(f"Refresh subscription unavailable: {e}")

    async def write_payload(self, payload: dict) -> bool:
        data = json.dumps(payload, separators=(",", ":")).encode()
        log(f"Sending: {data.decode()}")
        try:
            await self.client.write_gatt_char(RX_CHAR_UUID, data, response=False)
            return True
        except (BleakError, OSError) as e:
            # WinRT raises a raw OSError/WinError (NOT BleakError) when the peer
            # GATT server goes transiently unavailable mid-write. Returning
            # False trips the zombie-link break -> clean reconnect, instead of
            # an uncaught exception killing the daemon thread.
            log(f"Write failed: {e}")
            return False


async def _wait_first(*events: asyncio.Event, timeout: float) -> None:
    """Return when any `event` is set, or after `timeout` seconds.

    Lets the tick sleep wake immediately on the stop signal (a responsive Quit)
    without losing the refresh-request wakeup. Cancels and drains the loser
    tasks so they do not warn.
    """
    tasks = [asyncio.ensure_future(e.wait()) for e in events]
    try:
        await asyncio.wait(tasks, timeout=timeout, return_when=asyncio.FIRST_COMPLETED)
    finally:
        for t in tasks:
            t.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)


async def connect_and_run(device, stop_event: asyncio.Event, tray_state=None) -> bool:
    """Connect to the device and poll until disconnected or stopped.

    Returns True if at least one successful write occurred.
    """
    log(f"Connecting to {getattr(device, 'address', device)}...")

    # Retry wrapper. Each attempt builds a FRESH BleakClient: WinRT surfaces
    # post-wake failure modes ("Could not get GATT services: Unreachable", a
    # stale is_connected) that recycling the old handle does not clear.
    client = None
    for attempt in range(CONNECT_RETRIES):
        # Pass a BLEDevice (not an address string); address_type="random"
        # because the firmware's NimBLE uses a static-random address;
        # use_cached_services=False because WinRT's GATT cache may be stale
        # after a firmware reflash.
        client = BleakClient(device, address_type="random", use_cached_services=False)
        try:
            await client.connect()
        except (BleakError, OSError, asyncio.TimeoutError, AssertionError) as e:
            # WinRT service discovery inside connect() can surface a raw
            # OSError (WinError) or a bare AssertionError from bleak's
            # FutureLike when the peer drops the link mid-discovery -- neither
            # is wrapped as BleakError. Treat both as a normal failed attempt.
            log(f"Connection attempt {attempt + 1}/{CONNECT_RETRIES} failed: {type(e).__name__}: {e}")
            try:
                await client.disconnect()
            except BleakError:
                pass
            if attempt < CONNECT_RETRIES - 1:
                await asyncio.sleep(CONNECT_RETRY_DELAY)
            continue

        if not client.is_connected:
            log(f"Connection attempt {attempt + 1}/{CONNECT_RETRIES} failed (not connected)")
            try:
                await client.disconnect()
            except BleakError:
                pass
            if attempt < CONNECT_RETRIES - 1:
                await asyncio.sleep(CONNECT_RETRY_DELAY)
            continue

        break
    else:
        log(f"Connection failed after {CONNECT_RETRIES} attempts")
        return False

    log("Connected")
    session = Session(client)
    await session.setup_refresh_subscription()

    last_poll = 0.0  # poll immediately on first connect
    used_successfully = False
    consecutive_failures = 0

    def note_write_failure() -> bool:
        """Count a failed device write toward the zombie-link breaker.

        Returns True when too many writes have failed in a row and the caller
        should abandon the (likely zombie) link so the outer loop reconnects.
        Applies to every device write, data payloads and no-data beats alike,
        so a dead link still trips the breaker.
        """
        nonlocal consecutive_failures
        consecutive_failures += 1
        if consecutive_failures >= ZOMBIE_BREAK_LIMIT:
            log(
                f"Zombie link detected ({consecutive_failures} consecutive write "
                "failures); abandoning connection"
            )
            return True
        return False

    try:
        while client.is_connected and not stop_event.is_set():
            elapsed = time.time() - last_poll
            if session.refresh_requested.is_set() or elapsed >= POLL_INTERVAL:
                session.refresh_requested.clear()
                payload = None
                rejected = False
                try:
                    payload = await poll_openrouter()
                except AuthError:
                    rejected = True
                    log("OpenRouter key rejected (401/403); signalling no-data")
                    if tray_state:
                        tray_state.set_error("openrouter key rejected")
                except SourceConfigError as e:
                    rejected = True
                    log(f"OpenRouter source misconfigured: {e}")
                    if tray_state:
                        tray_state.set_error(str(e))

                if payload is not None:
                    if await session.write_payload(payload):
                        last_poll = time.time()
                        used_successfully = True
                        consecutive_failures = 0
                        if tray_state:
                            tray_state.set_connected(time.time())
                    elif note_write_failure():
                        break
                elif rejected:
                    # A dead key shows "no data" now rather than stale numbers.
                    log("No data; signalling idle to device")
                    if await session.write_payload({"ok": False}):
                        last_poll = time.time()
                        consecutive_failures = 0  # the link itself is healthy
                    elif note_write_failure():
                        break
                # else: transient failure, already logged. Stay quiet -- the
                # device keeps its last reading and the next tick retries.
                # Never report a DNS blip as a credential problem.

            # Wake on a refresh request OR a stop, whichever lands first.
            # Waking promptly on stop is what lets the finally below disconnect
            # before the process exits, so the device gets a clean GATT
            # disconnect and returns to its waiting screen rather than being
            # left frozen on stale data.
            await _wait_first(session.refresh_requested, stop_event, timeout=TICK)
    finally:
        # The clean GATT disconnect is what tells the peripheral the link is
        # gone. WinRT can surface a raw OSError or a bare AssertionError here,
        # so swallow both -- the link tears down regardless once we exit.
        try:
            await client.disconnect()
        except (BleakError, OSError, AssertionError):
            pass

    log("Device disconnected" if not stop_event.is_set() else "Stopping")
    return used_successfully


def _next_backoff(current: int, cap: int) -> int:
    """Double the current backoff, clamped to cap. Pure, so it is unit-testable."""
    return min(current * 2, cap)


async def main(tray_state=None) -> None:
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()

    # Publish the loop and stop_event so the tray can route Quit through
    # loop.call_soon_threadsafe: asyncio.Event is not thread-safe.
    if tray_state is not None:
        tray_state.loop = loop
        tray_state.stop_event = stop_event

    def _stop(*_args: object) -> None:
        log("Daemon stopping")
        stop_event.set()

    # Signal handlers can only be installed from the main thread, and
    # loop.add_signal_handler is unsupported on Windows. Under the tray the
    # loop lives in a background thread and the tray owns clean shutdown, so
    # skip silently there.
    if threading.current_thread() is threading.main_thread():
        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                loop.add_signal_handler(sig, _stop)
            except NotImplementedError:
                try:
                    signal.signal(sig, _stop)
                except ValueError:
                    pass

    log("=== ORmeter host daemon (BLE, Windows) ===")
    log(f"Poll interval: {POLL_INTERVAL}s")

    # Two distinct backoff regimes: slow-search (device genuinely absent) and
    # fast-reconnect (a link that dropped and should come back quickly).
    search_backoff = 1
    reconnect_backoff = 1
    while not stop_event.is_set():
        device = await acquire_target()
        if not device:
            if tray_state:
                tray_state.set_scanning()
            log(f"Device not found, retrying in {search_backoff}s...")
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=search_backoff)
            except asyncio.TimeoutError:
                pass
            search_backoff = _next_backoff(search_backoff, 60)
            continue

        ok = await connect_and_run(device, stop_event, tray_state)
        if not ok:
            if tray_state:
                tray_state.set_scanning()
            log(f"Connection lost, reconnecting in {reconnect_backoff}s...")
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=reconnect_backoff)
            except asyncio.TimeoutError:
                pass
            reconnect_backoff = _next_backoff(reconnect_backoff, RECONNECT_BACKOFF_CAP)
        else:
            reconnect_backoff = 1
            search_backoff = 1


def _print_payload() -> int:
    """Fetch once and print the payload. No BLE, so it is safe to run while the
    daemon is running -- useful for checking a key or a config change."""
    try:
        payload = asyncio.run(poll_openrouter())
    except AuthError as e:
        print(f"key rejected (HTTP {e})", file=sys.stderr)
        return 2
    except SourceConfigError as e:
        print(f"source misconfigured: {e}", file=sys.stderr)
        return 3
    if payload is None:
        print("transient failure (network/timeout/5xx) -- nothing to print", file=sys.stderr)
        return 4
    print(json.dumps(payload, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="ORmeter host daemon")
    parser.add_argument(
        "--print-payload",
        action="store_true",
        help="fetch once, print the payload, exit (no BLE)",
    )
    args = parser.parse_args()

    if args.print_payload:
        sys.exit(_print_payload())

    if sys.platform != "win32":
        print(
            "Warning: running under Linux/WSL -- WinRT BLE will not be available.",
            file=sys.stderr,
        )
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
