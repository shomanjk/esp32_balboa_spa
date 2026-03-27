# OTA + Live Logging Workflow (M5AtomLite-tub)

Use this guide when you want to push firmware OTA and watch live logs at the same time.

## Preconditions

- Device is on the same LAN as your computer.
- Firmware is built with `TELNET_LOG` enabled (default in `M5AtomLite-tub` / `M5AtomLite-tub-ota`).
- You know either:
  - hostname (for example `spa-XXXXXXXXXXXX.local`), or
  - IP address (for example `192.168.68.68`).

## 1) Start live logs in terminal A

Use hostname:

```bash
nc spa-XXXXXXXXXXXX.local 23
```

Or use IP:

```bash
nc 192.168.x.x 23
```

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

The telnet connection may drop during reboot; reconnect after 5-15 seconds:

```bash
nc spa-XXXXXXXXXXXX.local 23
```

## Troubleshooting

### OTA upload cannot find device

- Set `upload_port` in `platformio.ini` to the current hostname or IP.
- Prefer IP if mDNS (`.local`) is unreliable on your network.

### No telnet logs shown

- Verify build includes `TELNET_LOG`.
- Confirm device is connected to Wi-Fi and reachable.
- Retry with device IP instead of hostname.

### Serial upload conflict errors

If USB upload fails with serial-access errors:

- Stop all serial monitors/tools using the port.
- Replug USB cable.
- Retry with explicit port:

```bash
~/.platformio/penv/bin/pio run -e M5AtomLite-tub -t upload --upload-port /dev/cu.usbserial-XXXX
```

## Notes

- OTA transport (`espota`) itself is not a log console; telnet is the live log channel.
- For immediate firmware identity checks, use:
  - `/state` in the web portal (Firmware Version/Build), or
  - `/api/version` JSON endpoint.
