# Board: Waveshare ESP32-S3-Touch-AMOLED-1.8

The **only** supported device. 368×448 portrait AMOLED, ESP32-S3, 16 MB flash,
QIO/OPI PSRAM, `default_16MB.csv`, 921600 upload.

Not supported, permanently: the 2.16" AMOLED, the C6 variants, the LCD-154, the
2.06" — the upstream's other five targets are out of scope.

## Two shipping revisions — same board, different panel module

This is the one thing "single board" does *not* let us skip. Two revisions of
this exact model exist and differ only in the bonded panel module:

| | original | later |
|---|---|---|
| Display | SH8601 (QSPI) | CO5300 (QSPI) |
| Touch | FT3168 @ I2C `0x38` | CST816 @ I2C `0x15` |

Everything else — PMU, IMU, IO expander, pinout — is identical. Detect at boot
by probing the touch controller's I2C address; its presence is a reliable proxy
for which panel module is fitted. **Keep the probe.** Picking wrong means no
display or no touch, and a unit's revision cannot be told from the outside -- it
takes either this probe or a look at the serial console.

### Confirmed: the reference unit is the ORIGINAL revision

Measured 2026-10-20 from the unit's own serial console at 115200:

```
Board revision: SH8601 + FT3168
Touch FT3168 ID=0x03 (addr 0x38)
Dashboard ready (Waveshare AMOLED 1.8, 368x448), waiting for data on BLE...
BLE: init complete, MAC=<device-mac>
```

So the development unit is **SH8601 + FT3168**, and the CO5300's 16 px column
offset does not apply to it. Keep the probe regardless -- a replacement board can
be the other revision -- but development no longer has to guess: the device's
actual behaviour is directly observable.

The same banner confirms every other fact on this page: `XCA9554 init OK`,
`AXP2101 init OK`, `QMI8658 init OK (rotation disabled on this board)`,
`chime: ES8311 ready`, and the panel geometry.

**The serial console is the fastest diagnostic channel on this board.** Opening
`COM7` at 115200 resets the board and prints this banner plus every BLE event
(`connparams`, `auth complete ... bonded=1 enc=1`, `req_char onSubscribe`,
`refresh requested`). Note that opening the port genuinely resets it
(`rst:0x15 USB_UART_CHIP_RESET`): the daemon's BLE link drops and re-establishes
within seconds, so it is harmless but not invisible.

Two habits that cost real time here: serial output is **not buffered**, so a
banner is lost unless you are reading *while* the board boots or resets; and a
failed probe floods the console with `i2cWriteReadNonStop ... ESP_ERR_INVALID_STATE`
spam that drowns every other error, so filter it when hunting something else.

### Vendor spec vs this unit -- Waveshare has moved to the other revision

The product page for this board (checked 2026-10-20) confirms the geometry, and
states the currently shipping module:

> Embedded with CO5300 Display Driver and CST820 Capacitive Touch Chip

Its spec table lists `Resolution 368 x 448 pixels` and `Driver IC CO5300`. So:

* **368 x 448 is correct** -- confirmed by the vendor in three places and by this
  unit's own boot banner.
* **The shipping revision is now CO5300 + CST820**, i.e. the LATER one. The unit
  this project develops against reports SH8601 + FT3168, the ORIGINAL one. The
  page does not mention SH8601 at all.

Consequences worth keeping in mind:

* The CO5300 needs a 16 px column offset; the SH8601 does not. **This unit does
  not.** Development can proceed without it, but the code must not assume it.
* **The boot-time revision probe is load-bearing, not paranoia.** A warranty
  replacement or a second board bought today would very likely be CO5300 +
  CST820. Firmware that hardcoded SH8601 would come up blank or wrong-coloured on
  it. Keep the probe.
* This file previously named the later touch chip `CST816`; the vendor says
  **CST820**. The `0x15` I2C address is inherited from the legacy firmware, not
  verified against a CST820 part. Treat the *name* as vendor-authoritative and
  the *address* as unverified until a CO5300-era unit can be probed.

### Probe ordering is load-bearing

**Probe the touch controller and initialise the display BEFORE bringing up the
PMU.** XPowersLib's `begin()` re-initialises the shared Wire bus, and after that
the revision probe fails — it falls through to its fallback and, on a CO5300
unit, would initialise the wrong panel driver. Verified both ways on hardware:
PMU-first ⇒ probe fails.

The probe reads an I2C **register** (FT3168 chip id at `0xA0`, CST8xx at `0xA7`)
rather than merely checking that an address ACKs. Both address and size must be
cast to `uint8_t`: casting the address to `int` selects a different `Wire`
overload and the read fails with `ESP_ERR_INVALID_STATE` (259), which silently
misreported this panel as a CO5300.

## Pins

| Function | Pins |
|---|---|
| QSPI display | CS 12, SCLK 11, SDIO0–3 = 4, 5, 6, 7 |
| I2C (touch + PMU + IMU + expander share one bus) | SDA 15, SCL 14 |
| Touch INT | 21 |
| I2S audio (ES8311 @ `0x18`) | MCLK 16, BCLK 9, WS 45, DOUT 8, DIN 10 |
| Amp enable | **GPIO 46** (HIGH = on) |
| BOOT button | GPIO 0 |

**Panel reset is not a GPIO.** It is routed through the XCA9554 IO expander
(EXIO1), so the expander must be initialised and reset released *before* the
display driver's `begin()` runs. Ordering is load-bearing.

## IO expander — XCA9554 @ `0x20` (PCA9554-compatible)

| Pin | Role |
|---|---|
| EXIO0 | touch reset, active LOW |
| EXIO1 | display reset, active LOW |
| EXIO2 | PA_EN — **must stay HIGH.** Driving it low kills FT3168 touch. It is *not* a usable amp switch. |
| EXIO4 | PWR button input, active HIGH |

## Power — AXP2101 @ `0x34`

Battery monitoring only; the PMU's PKEY IRQs are not wired here. PWR edges are
synthesised in software from the polled EXIO4 level:

- **short** — on release, if the hold was shorter than 1500 ms
- **long** — fired once when a hold crosses 1500 ms
- **release** — on every falling edge

Poll intervals: battery 2000 ms, charging 500 ms, PWR 50 ms.

**Hold the PWR button for ~10 s and the AXP2101 powers the board down.** That is
a PMU-level state, so it survives an ESP32 reset and is not cleared by unplugging
USB alone if a battery is attached. Worth remembering before blaming firmware for
a board that behaves oddly.

## Audio — ES8311 mono codec, I2S

Sample rate **44100 Hz, which must match the embedded PCM** data. Amp enable is
GPIO 46 only — see the EXIO2 warning above.

## Buttons

BOOT (GPIO 0) is the only direct GPIO button. PWR arrives via the expander.
There is **no secondary button** on this board.

## IMU

QMI8658 present and initialised for I2C bus health, but **rotation is disabled** —
the panel mounts in a fixed orientation inside the kit's enclosure. Do not build
layout code that assumes it can flip.

## BLE

NimBLE, peripheral role only, **max 2 simultaneous connections**: the OS holds a
HID keyboard link (BOOT button sends Space) while the host daemon holds its own
connection to the custom data service. Both must coexist — dropping to one
connection breaks the keyboard, and the host link is what carries the payload.

Peripheral Preferred Connection Parameters are set explicitly
(`MIN_CONN_INTERVAL=12`, `MAX_CONN_INTERVAL=24`, `SLAVE_LATENCY=0`,
`SUPERVISION_TMO=600`) — this is a fix for Windows supervision-timeout churn,
not a tuning preference.

### The advertisement MUST carry the HID service UUID (0x1812)

This is the single most expensive fact on this page, established the hard way on
2026-10-20 by building a firmware that omitted it and then spending hours
debugging the downstream consequences.

**OS Bluetooth device lists only surface a BLE-only peripheral whose advertising
packet explicitly includes the standard HID service UUID `0x1812`.** Without it
the device is discovered internally but **hidden from the user-facing
nearby-devices list** — on both Windows and macOS.

The failure cascade is what makes it so misleading:

```
no 0x1812 in the advertisement
  -> invisible in every OS device list, while a raw scanner (bleak, hcitool)
     still sees it perfectly -- so it looks like a host-side bug
  -> cannot be paired, so no bond is ever created
  -> the OS will not encrypt a link to an unpaired device
  -> the OS terminates every connection ~1 s after it forms
     (HCI reason 531 = remote-user-terminated)
  -> every GATT read, write and subscribe fails ("Unreachable")
  -> looks like a daemon bug, a firmware bug, or a broken radio
```

Advertise it like this, and keep the primary packet small:

| Packet | Contents | Why |
|---|---|---|
| primary (≤31 bytes) | flags (3) + appearance `HID_KEYBOARD` (4) + HID service `0x1812` as a **16-bit** UUID (4) + name (9) = 20 bytes | these are the fields that make the device *visible* |
| scan response | the custom 128-bit data-service UUID (18) | active scanners (the host daemon) merge both |

Related traps, each of which independently produced a silently broken device:

* **The Flags AD type (`0x06`) is mandatory.** NimBLE's `addServiceUUID()` and
  `setName()` helpers add it implicitly; hand-building `NimBLEAdvertisementData`
  drops it, and a packet with no Flags AD type is treated as non-discoverable.
  Call `setFlags(0x06)` explicitly and do not remove it.
* **The advertised name must fit about 8 characters** next to a 128-bit UUID, or
  NimBLE silently advertises *no name at all* (scans report `name=None`). This is
  why the device name must be 7 characters and not 10 — the longer name did
  not fit and was dropped entirely.
* **`setMinInterval`/`setMaxInterval` take 0.625 ms units, and `0` means "use the
  controller default"** — which is ~1.28 s. OS scanners listen in short windows
  and miss that repeatedly, so the device "takes many retries" to find. Use
  160/240 (100–150 ms); this board is desk-powered, so being findable matters far
  more than the power cost.
* Verify from inside the packet rather than by intent: log
  `adv->getAdvertisementData().getPayload().size()` at boot. Trusting the code
  over the measurement cost hours here.
* After clearing bonds, re-assert advertising and log `isAdvertising()` — a bond
  wipe can leave the device silently un-discoverable while the UI still claims
  "pairing mode".

The firmware therefore exposes a real `NimBLEHIDDevice` (report descriptor,
keyboard appearance, PnP info, HID info, battery level) alongside the custom data
service. The report descriptor includes an **LED output report**, which macOS
requires before it will present the device as a full keyboard.

### A bond is required for a working link

Bonding is enabled (`setSecurityAuth(true, false, true)`). An OS will not encrypt
a link to an unpaired device and drops the connection instead. Two failure modes:

* **No bond** — connects, then disconnects ~1 s later, with every GATT operation
  failing. The device logs `bonded=0, encrypted=0` and never logs
  `auth complete`.
* **A stale bond** — e.g. after the firmware changed, or where only one side was
  cleared. Produces a connect/disconnect storm at roughly 1 Hz.

Clear bond state on **both** sides when it goes wrong:

* device: `NimBLEDevice::deleteAllBonds()`
* Windows: the PnP device nodes *and*
  `HKLM\SYSTEM\CurrentControlSet\Services\BTHPORT\Parameters\Devices\<addr>`.
  Both need elevation — use `pnputil /remove-device`, because `Remove-PnpDevice`
  is not available on this machine. Windows also **hides already-known devices
  from "Add a device"**, so a stale entry can make a perfectly good device look
  undiscoverable until it is removed.

When it is correct the device logs `auth complete bonded=1 encrypted=1` and the
host link holds indefinitely.

Note on the advertised address: `<device-mac>` has `00` in the two leading
bits, which makes it a **public** address, not a random one.

## LVGL

Configured entirely through build flags (`LV_CONF_SKIP`), colour depth 16. The
upstream enables `LV_USE_SNAPSHOT`, which is what makes a headless render /
screenshot harness possible without a device.

Traps that cost time here:

* **Round the flushed area on `LV_EVENT_INVALIDATE_AREA`, not inside the flush
  callback.** The SH8601 over QSPI rejects odd-coordinate windows. Rounding during
  the flush while still passing the original `px_map` draws pixels that do not
  correspond to the area claimed, and reads past the end of the buffer.
* **Set the colour format explicitly** (`lv_display_set_color_format(RGB565)`).
  LVGL is compiled as a library and does not see this env's `-DLV_COLOR_DEPTH` —
  the same reason `LV_FONT_*` enables must live in the *library* build flags.
* **The first-frame render burst must run AFTER `ble_init()`.** Moving it earlier
  left the panel permanently blank while every init log still reported success.
* A UI on a black background makes a total flush failure look identical to a
  powered-off panel. Keep a `PANEL_SELFTEST` that paints colours directly through
  the panel driver, so "dark screen" can be told from "dark UI" in one boot.
* A centred `LV_LABEL_LONG_CLIP` label clips glyphs off **both** ends
  ("PAIRING MODE" → "AIRING MODE"); use `LV_LABEL_LONG_WRAP`.
* Keep the first glyph clear of the panel's rounded corner — 16 px of row padding,
  not 6, or the "W" in "Wallet" gets cropped.

## Changing the GATT UUIDs needs a host re-pair

The service and characteristic UUIDs are the wire contract between the daemon and
the firmware. Both ends are ours, so they can be changed -- but a change takes
more than a reflash.

Windows caches each bonded device's GATT database. After a firmware change that
alters the UUIDs, Windows keeps handing the OLD attribute table to every
connection. The daemon connects fine, then fails with:

    Write failed: Characteristic <new-uuid> was not found!

on a device that is genuinely advertising that UUID. The stale table sits below
the BLE library, so reconnecting and retrying never clears it. It is even visible
in the device's record in Windows:

    BTHLEDEVICE\{4C41555A-4465-7669-6365-000000000001}_DEV_VID&0102E5_...

That embedded UUID is the one Windows enumerated the device under when it was
paired. It does not update until the bond does.

To fix it: remove the device in Windows Settings, clear the bond on the meter by
holding BOOT for 3 seconds and releasing, then pair again.
