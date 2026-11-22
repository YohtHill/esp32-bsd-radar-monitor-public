# ESP32 60 GHz Millimeter-Wave Blind-Spot Monitor

[简体中文](README.zh-CN.md)

This project builds a **rear blind-spot warning prototype for bicycles and e-bikes**. A millimeter-wave radar reports target range, relative speed and direction. The ESP32 assigns targets to left, centre and right zones, evaluates risk, and drives an OLED, an optional TFT, a buzzer and an Android display. A vibration sensor supplies a separate mechanical trigger. The phone can mute the buzzer, dismiss a false alarm, set the local alarm state or resume monitoring.

**What worked.** The team fabricated and assembled a two-layer PCB. An archived serial capture decoded a right-zone target at **2.00 m** and **+55°**. The OLED displayed two targets at approximately **8 m and 10 m**. A separate Android capture showed two targets, including a left-zone warning of **4/5** for the target at **10 m, −7.2 km/h and −16°**. The three captures are from different scenes. The ESP32 firmware and Android debug APK both built from the repository.

![Demonstration assembled from original board, OLED and Android captures](demo/demo.gif)

[Demo walkthrough](demo/README.md) · [Editable PCB and manufacturing files](hardware/README.md) · [Firmware design](docs/design.md) · [Protocol byte by byte](docs/radar-protocol.md) · [Testing and improvements](docs/reproduce-and-test.md)

## Why the team built it

The coursework started with a rear-end collision on campus. Low speeds still leave cyclists with limited visibility to the side and rear. The project uses the MS60-1211S80M-BSD radar because it directly reports range, radial relative speed and bearing without relying on the brightness of a camera image.

The prototype has two feedback paths. **Radar targets** drive tracking and warnings. The OLED and buzzer can work with the phone disconnected. **Mechanical vibration** enters collision confirmation, with an app control for human handling. Both paths return to the monitoring loop. The alarm is a local state with no connection to an external emergency service.

~~~mermaid
flowchart LR
    R["MS60 radar<br/>Range · speed · angle"] -->|UART2 921600| F["ESP32 frame validation and target parser"]
    F --> S["Risk rules and state machine"]
    V["YL-99 vibration sensor"] -->|GPIO27| S
    S --> O["OLED / ST7789 / buzzer"]
    S -->|Status and targets| A["Android app · Bluetooth Classic SPP"]
    A -->|Mute / alarm / false alarm / reset| S
~~~

## Hardware implementation

The V1 two-layer carrier is approximately 121 × 80 mm. It mounts an ESP32-WROOM DevKit, MS60 radar, 0.96-inch SSD1306 OLED, 1.54-inch ST7789 TFT, YL-99 vibration module and an active-low buzzer. A 5 V input feeds DevKit VIN. The carrier uses the DevKit's 3.3 V supply for the corresponding peripherals. The OLED uses I²C for a quick numerical view. The TFT uses SPI for more detailed zones and state. Radar UART and the vibration GPIO can be brought up separately.

<img src="demo/assets/pcb-layout.png" alt="Public top-side PCB render, with displays on the left and ESP32 in the middle" width="680">

| Material | What it covers |
| --- | --- |
| [EasyEDA Pro project](hardware/esp32-bsd-monitor-v1.epro2) · [Gerber and drill ZIP](hardware/esp32-bsd-monitor-v1-gerbers.zip) | Editable design and fabrication layers with personal markings removed |
| [Schematic and wiring](docs/hardware-wiring.md) · [PCB layout and rear photo](hardware/README.md) | Supply, crossed UART signals, manual jumpers and TFT backlight |
| [PlatformIO firmware](src/main.cpp) · [Android build and pairing](android_app/README.md) | Flash the board and connect the client |

## From a radar frame to a warning

1. **Measurement.** The radar module estimates range, radial speed and angle from the beat frequency, phase across chirps and phase across receive channels. The ESP32 takes target-level reports and handles parsing, decisions and display.
2. **Frame parsing.** The ESP32 locates a frame header, waits for the bytes specified by the length and validates the checksum. A `TYPE 0x07` report contains the target count and four fields per target. The firmware stores up to eight. The [protocol walkthrough](docs/radar-protocol.md) annotates the captured 12-byte frame.
3. **Multi-target risk.** Each target is scored using its range, approach speed and the vibration input. The summary uses the highest risk, then the closest target on ties. A fresh target can enter the `WARNING` state. An audible radar warning also requires fused risk `WARN`.
4. **Display and control.** The OLED pages through targets. The TFT and Android app visualise the zones. The phone receives both summary and per-target data and sends four commands back to the ESP32. The firmware resumes monitoring when the current observations and latched state allow it.

## Interpreting the results and improving V1

The report and defense show the prototype's functional data path. They do not establish maximum detection range, calibrated accuracy or a long-run false-alarm rate. Late SPI display rework damaged the controller. The original assembly could not continue testing and the electrical root cause was not isolated. Start the next revision with removable peripherals and current-limited supply branches, then add serial replay and measured-distance tests. Use those data to tune the thresholds and controls. [Test matrix and improvement order](docs/reproduce-and-test.md)

## Quick build

Check the [wiring and supplies](docs/hardware-wiring.md). The ESP32 firmware uses Arduino with PlatformIO. Pair the Android phone with `ESP32_BSD_MONITOR` over Bluetooth Classic. APK installation and stepwise board testing are in the [Android build guide](android_app/README.md) and [bench guide](docs/reproduce-and-test.md).

~~~sh
pio run
pio run -t upload
pio device monitor -b 115200
~~~
