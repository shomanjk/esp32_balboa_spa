# OTA + Live Logging Workflow (M5AtomLite-tub)

Use this guide when you want to push firmware OTA and watch live logs at the same time.

## Preconditions

- Device is on the same LAN as your computer.
- You know either:
  - hostname (for example `spa-XXXXXXXXXXXX.local`), or
  - IP address (for example `192.168.68.68`).

**Default shipped builds** (checked-in [`platformio.ini`](platformio.ini) **`M5AtomLite-tub`** / **`M5AtomLite-tub-ota`**) **do not** define **`TELNET_LOG`**, so there is **no** Telnet listener on port **23**. Live logs are **USB Serial** and/or the portal **`/logs`** page.

## 1) Start live logs in terminal A (recommended: USB serial)

Connect USB and run PlatformIO’s serial monitor (baud **115200** for these envs):

```bash
~/.platformio/penv/bin/pio device monitor -e M5AtomLite-tub -b 115200
```

**Without USB:** open **`http://<hostname-or-ip>/logs`** in a browser, or poll **`GET /api/logs`**.

### Optional: `TELNET_LOG` + `nc` on port 23

If you add **`-DTELNET_LOG`** to your PlatformIO env (see commented line in [`platformio.ini`](platformio.ini)), the firmware starts **TelnetStream** on TCP **23** after Wi‑Fi connects. **`Log`** still goes to **Serial** and the web log ring (it is not repointed to Telnet). You can **`nc <host> 23`** for an interactive session, but **do not expect a full log mirror** over Telnet with the current code.

## 2) Push OTA firmware in terminal B

```bash
~/.platformio/penv/bin/pio run -e M5AtomLite-tub-ota -t upload
```

## 3) Expected OTA log events

During upload, you should see lines similar to:

- `[WiFi]: OTA auth ...`
- `[WiFi]: Arduino OTA Update Start`
- `[WiFi]: OTA Progress: <n>%`
- `[WiFi]: Arduino OTA Update Complete`

The device reboots after OTA. If you are on **USB serial**, restart the monitor (or reconnect) after a few seconds. If you use **`TELNET_LOG`** + **`nc`**, reconnect after reboot the same way.

## Troubleshooting

### OTA upload cannot find device

- Set `upload_port` in `platformio.ini` to the current hostname or IP.
- Prefer IP if mDNS (`.local`) is unreliable on your network.

### No serial output in monitor

- Confirm baud **115200** and the correct **`/dev/cu.…`** (or COM port).
- Stop other tools holding the serial port.

### Optional Telnet: nothing on port 23

- Confirm your build actually defines **`TELNET_LOG`** (default envs do **not**).
- Confirm Wi‑Fi is up and you are using the device IP or resolvable hostname.

### Serial upload conflict errors

If USB upload fails with serial-access errors:

- Stop all serial monitors/tools using the port.
- Replug USB cable.
- Retry with explicit port:

```bash
~/.platformio/penv/bin/pio run -e M5AtomLite-tub -t upload --upload-port /dev/cu.usbserial-XXXX
```

## Notes

- OTA transport (`espota`) itself is not a log console; use **USB serial**, **`/logs`**, or **`/api/logs`** for live output.
- For immediate firmware identity checks, use:
  - `/state` in the web portal (Firmware Version/Build), or
  - `/api/version` JSON endpoint.
