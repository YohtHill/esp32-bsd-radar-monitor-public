# Radar Serial Protocol and Android Telemetry

[简体中文](radar-protocol.zh-CN.md) · [Project home](../README.md) · [Firmware design](design.md)

## Two UART roles

USB serial uses **115200 baud** for logs and bench commands. The radar uses **UART2 at 921600 baud, 8N1**. Connect radar TX to GPIO16 and radar RX to GPIO17. At startup the firmware cycles through version query `FE`, sensing status query `D0` and sensing enable `D1` at 1500 ms intervals. A checksum-valid reply or report stops periodic probing. The reply handler can still request `D1` if sensing needs to start. Protocol lock shows that a frame passed framing and checksum. Updating targets requires a valid BSD report. [UART setup and probe code](../src/main.cpp#L1070-L1160)

| Header | Direction | Implementation |
| --- | --- | --- |
| `0x58` | ESP32 → radar | Command, parameter length, parameters and 16-bit little-endian additive checksum |
| `0x59` | Radar → ESP32 | Reply command, parameter length, parameters and 16-bit little-endian additive checksum |
| `0x5A` | Radar → ESP32 | Payload length, payload and 8-bit additive checksum. `TYPE 0x07` carries BSD targets |

## Length and checksum come before target decoding

The ESP32 feeds incoming bytes into a 256-byte receive buffer. It skips bytes before a `0x59` or `0x5A` header and waits for the full candidate specified by the length field. An oversized candidate or bad checksum discards its first byte and restarts header search. A checksum-valid report still needs the correct type, a supported target count and enough bytes for those targets. This handles frames split across multiple reads and consecutive frames in the byte stream. [Buffer, resynchronization and checksum code](../src/main.cpp#L1732-L1812)

~~~mermaid
flowchart TD
    A["Append a UART2 byte"] --> B["Find 0x59 or 0x5A"]
    B --> C{"Candidate complete?"}
    C -->|Wait for remaining bytes| A
    C -->|Yes| D{"Checksum valid?"}
    D -->|No, drop the candidate's first byte| B
    D -->|Yes| E{"Frame type"}
    E -->|0x59| F["Process command reply"]
    E -->|0x5A and TYPE 0x07| G["Check the target count and decode each record"]
    G --> H["Clear empty reports or update the latest target frame"]
~~~

## Decode the capture from the defense

~~~text
5A 09 07 01 00 00 00 02 37 00 00 A4
~~~

| Byte offset | Data | Meaning |
| ---: | --- | --- |
| 0 | `5A` | Unsolicited report header |
| 1 | `09` | Nine payload bytes and 12 bytes in the whole frame |
| 2 | `07` | BSD report type |
| 3–4 | `01 00` | Little-endian 16-bit count of one target |
| 5–6 | `00 00` | Reserved field after the count, skipped by the parser |
| 7 | `02` | Range of 2 m |
| 8 | `37` | Bearing of +55°, in the firmware's right zone |
| 9 | `00` | Radial speed of 0 m/s |
| 10 | `00` | Target ID 0 |
| 11 | `A4` | Low eight bits of the sum of the preceding 11 bytes |

Each target uses four bytes in the order range, angle, velocity and ID. The firmware interprets the first three as signed 8-bit values in metres, degrees and m/s. It stores the ID as a byte. Minimum payload length is `1 + 4 + 4 × N` for up to eight targets. The parser rejects shorter payloads and counts above the limit. It accepts payload bytes beyond that minimum. [BSD object parsing](../src/main.cpp#L1604-L1635)

For example, a velocity byte of `FE` means −2 m/s. The ESP32 multiplies by 3.6 before sending it to Android, which displays −7.2 km/h. Negative speed is the approach convention for this mounting and firmware. Check the sign with a target moving in a known direction after changing the mount.

## Android display and commands

Android connects to `ESP32_BSD_MONITOR` using Bluetooth Classic RFCOMM/SPP and reads newline-terminated records on a background thread. The ESP32 sends comma-separated `key:value` fields. A summary identifies the selected target and overall state. The `T0_` through `T7_` fields describe the target list. An accepted report can trigger an immediate update, and the ESP32 also schedules updates about every 250 ms while connected. That scheduling interval does not measure end-to-end latency.

| Fields | Purpose |
| --- | --- |
| `STATE`, `RISK`, `LEVEL` | System state, fused risk and display level |
| `ZONE`, `DIST`, `SPEED`, `ANGLE`, `ID` | Selected target and its readings |
| `CNT`, `T{n}_ID`, `T{n}_ZONE`, `T{n}_DIST`, `T{n}_SPEED`, `T{n}_ANGLE`, `T{n}_LEVEL` | Count and per-target records |
| `COLLISION`, `C_COUNT`, `BUZZER` | Impact state, hit count and buzzer state |
| `MSG` | Notification sent in collision confirmation, alarm or false-alarm states |

The app turns these fields into sector graphics, status colours and target rows. It writes a newline after each `MUTE`, `ALARM`, `FALSE` or `RESET` command. The same commands are accepted over USB serial for bench tests without a phone. [ESP32 status encoder](../src/main.cpp#L922-L1004) · [Android reader and UI update](../android_app/app/src/main/java/com/example/bsdmonitor/MainActivity.java#L622-L715)
