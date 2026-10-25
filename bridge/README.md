# Dust bridge

Any ESP32 with Wi-Fi, plugged into the Jetson by USB. It passes every ESP-NOW packet it hears
up to the Jetson, and sends whatever the Jetson gives it out over ESP-NOW, either to everyone
or to one tile. It knows nothing about Dust, so it never needs reflashing when the protocol
changes.

```
idf.py set-target esp32s3            # or esp32, esp32c3, esp32c6, ...
idf.py menuconfig                    # "Dust bridge": the wall's channel (CONFIG_DUST_CHANNEL), the link
idf.py -p PORT flash
```

- **Channel:** it must match the tiles' `CONFIG_DUST_CHANNEL` (default 6). The Jetson can also
  move it with `--channel`.
- **Link:** UART0 at 921600 baud works on any dev board through its USB-to-serial chip. On
  boards with native USB (S3, C3, C6) you can choose USB Serial/JTAG instead. Logging is
  switched off, so the port carries only frames.
- **Frames** (`main/link_frame.h`): kind, MAC, RSSI, payload and a CRC-16, COBS-encoded and
  ended by `0x00`. A garbled frame is dropped and the next one is read normally. Once a second
  it reports its channel and packet counts (heard, sent, failed, dropped).
- Needs ESP-IDF 5.3 or newer. Built from the same frame code that `host/net_test` checks, but
  not yet built with ESP-IDF or run on a board.
