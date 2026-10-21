# Flashing the meter

## The rule: flash the APP, not the merged factory image

```
esptool --chip esp32s3 --port COM7 --baud 921600 write_flash 0x10000 firmware.bin
```

Use `firmware.bin` at `0x10000`. **Do not** flash `firmware.factory.bin` at `0x0`.

### Why

`firmware.factory.bin` is a *merged* image: it spans `0x0` all the way to the end
of the application (~1.1 MB). That address range **includes `0x9000`, the NVS
partition**, which is where the ESP32 stores its BLE bond.

So writing the factory image at `0x0` erases the bond on every single flash. The
consequences are disproportionate to the cause:

* the device can no longer complete the encryption the OS expects
* the OS therefore drops every connection ~1 s after connecting
  (HCI reason 531, remote-user-terminated)
* every GATT read, write and subscribe fails with `Unreachable`
* the device advertises fine and scans fine, so it looks like a daemon, pairing
  or firmware bug

It is also **invisible to A/B testing**: flashing a different firmware version
breaks it exactly the same way, so "the old firmware fails too, therefore it is
not my code" is a *false* conclusion while this is happening. That specific
detour cost an hour.

### If the bootloader or partitions actually changed

Then write the parts individually, which still never touches `0x9000`:

| offset | image |
|---|---|
| `0x0` | `bootloader.bin` |
| `0x8000` | `partitions.bin` |
| `0xe000` | `boot_app0.bin` |
| `0x10000` | `firmware.bin` |

## Recovering a wiped bond

If the bond has already been erased, app-only flashing preserves it but cannot
restore it. Windows still holds its half of the key pair, so the OS shows the
device as *paired* while refusing to encrypt — which is a confusing state to
debug. Clear **both** sides and pair again:

1. Windows: remove the `ORmeter` entry in Settings → Bluetooth & devices.
   Check for stale entries too — Windows hides already-known devices from
   "Add a device", so a leftover record makes a healthy device look
   undiscoverable.
2. The meter: hold **BOOT** for 3 s and release, or use whatever on-device
   pairing control exists, to clear its own bonds.
3. Add device → Bluetooth → `ORmeter`.

When it works, the device's serial prints:

```
BLE: auth complete bonded=1 encrypted=1
```

That line is the proof. If it never appears, the link is not encrypted, whatever
the OS UI claims.

## Confirming the device's own view

The firmware prints `bonds=` on the boot line and shows a live
`adv:on bonds:N conn:N` status on screen. `bonds:0` means the device holds no
key and cannot satisfy an encryption request — no amount of host-side pairing
will fix that on its own.

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
