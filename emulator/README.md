This is a very quick and nasty spa emualtor to assist in bench testing and development.  I just used a spare USB serial device connected to the ESP32 RX2 and TX2 pins.

It will send a rudemntary config and status updates ( time )

## Bridge TCP client (`bridgeClient.js`)

Connects to the tub gateway bridge on port **4257**. Target host is **not** hardcoded:

```bash
SPA_BRIDGE_HOST=spa-XXXXXXXXXXXX.local node emulator/bridgeClient.js
# or
node emulator/bridgeClient.js spa-XXXXXXXXXXXX.local
```

Defaults to **`127.0.0.1`** for local testing.