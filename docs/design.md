# Sensing, Risk Fusion and State Machine

[简体中文](design.zh-CN.md) · [Project home](../README.md) · [Serial protocol walkthrough](radar-protocol.md)

## What the radar measures

The project uses the 60 GHz MS60-1211S80M-BSD FMCW module. Beat frequency tied to echo delay is used to estimate range. Phase across consecutive chirps gives radial speed. Phase across receiver channels supports angle estimation. The defense records one transmit and four receive channels. With no antenna geometry, raw ADC samples or point cloud available to the application, ESP32 works on the module's target reports.

| Parameter | Module manual | How the project uses it |
| --- | --- | --- |
| Band and bandwidth | 59–64 GHz, with adjustable 2 GHz bandwidth | Sensor context. The ESP32 reads target reports. |
| Nominal field of view | ±60° horizontal and vertical | The firmware uses horizontal angle for zones. Mounting and obstruction effects need measurement. |
| Nominal detection ranges | Cars 0.5–40 m, motorcycles 0.5–30 m, bicycles 0.5–20 m and pedestrians 0.5–15 m | Module specifications. The prototype has no measured limit by target class. |
| Update and acquisition time | 100 ms update and 5 s acquisition listed in the manual | Separate from ESP32 and Android scheduling. End-to-end latency needs measurement. |

The module field of view, the illustrative sector drawing in the report and the software thresholds serve different purposes. The code uses **−60° up to −15° exclusive** for left, **−15° through +15°** for centre, and **above +15° through +60°** for right. Other angles get no zone. A software zone is an angle category. Installed beam coverage has not been characterised. [Zone selection](../src/main.cpp#L320-L337)

## How the ESP32 loop works

The program records the latest `RadarFrame` and its arrival time. A frame can contain up to eight `RadarTarget` records. The main loop samples the vibration input, reads USB and Bluetooth commands, drains the radar UART and updates state, the buzzer and displays. A fresh BSD frame can also update Bluetooth and the display immediately. [Main loop](../src/main.cpp#L1901-L1922)

| Schedule | Purpose |
| --- | --- |
| 25 ms debounce and 3 s recent-hit period | A debounced active edge increments the impact count. Human confirmation may take longer than 3 s. |
| 2 s radar freshness window | An accepted non-empty frame is aged by its receive time. A valid empty report clears targets immediately. |
| 250 ms OLED and 1200 ms page rotation | Up to four targets fit on a page. |
| 1000 ms TFT and 250 ms Bluetooth | Timed refresh and delivery. Some events can force updates. |
| 5 s serial link summary | Check cumulative received bytes, valid frames and discarded bytes. |

## Distance and speed rules

The mechanical input has the highest priority. In the absence of a hit, the firmware evaluates signed radial velocity. Negative speed means approach for this setup. The rules below run in order. [Fusion rules and target selection](../src/main.cpp#L198-L258)

| Priority condition | Fused risk |
| --- | --- |
| The vibration input is still active or the hit is recent | `HIT` |
| No target | `OK` |
| Range at or within 4 m | `NEAR`, as a conservative response to unreliable near-field returns |
| Range beyond 4 m and at most 8 m while approaching at 1 m/s or more, or range beyond 4 m while approaching at 2 m/s or more | `WARN` |
| Other targets within 8 m or approaching at 1 m/s or more | `NEAR` |
| Other targets | `TRACK` |

The summary uses the highest target risk, with the closer target winning ties. In the defense's two-target capture, the **centre target at 4 m and −3.6 km/h** falls under the close-range rule. The **left target at 10 m and −7.2 km/h** reaches the fast-approach threshold. The more distant left target can therefore drive the warning.

Fused risk, top-level state and the displayed level have different roles. `WARNING` indicates a fresh target. An audible radar warning also requires `WARN`. The per-target `LEVEL` takes the higher of the risk score and a separate near-distance score. A displayed level of 5 alone cannot establish impact. Collision confirmation or the local alarm forces the summary level to 5. [Display level calculation](../src/main.cpp#L588-L635)

## Human confirmation and return to monitoring

~~~mermaid
flowchart LR
    S["SAFE"] -->|Fresh target| W["WARNING"]
    W -->|Targets empty or expired| S
    S -->|Debounced impact| C["COLLISION_CONFIRM"]
    W -->|Debounced impact| C
    C -->|ALARM| A["ALARMING"]
    C -->|FALSE| F["FALSE_ALARM"]
    F -->|After 2 s| N["Select current monitoring state"]
    C -->|RESET| N
    A -->|RESET| N
    N -->|No fresh target| S
    N -->|Fresh target| W
~~~

The diagram shows the usual handling path. The command handler also accepts ALARM, FALSE, MUTE and RESET from other states.

| State or action | Shipped firmware behaviour |
| --- | --- |
| `SAFE` / `WARNING` | Regular monitoring chooses the state according to whether a fresh target exists. Expired targets can return regular monitoring to `SAFE`. |
| `COLLISION_CONFIRM` | A debounced mechanical hit increments the count and enters confirmation. The state can await a command beyond the recent-hit period. |
| `ALARM` | Enters `ALARMING`, switches the current buzzer output off and sends app status. This is a local notification. |
| `FALSE` | Enters `FALSE_ALARM`, clears the recent-hit timer and mutes the buzzer. After 2 s it selects `SAFE` or `WARNING` according to the current target data. |
| `MUTE` | Stops the buzzer without changing state. A later state transition clears the mute flag. |
| `RESET` | Clears the recent-hit timer and mute flag and resumes monitoring using the latest targets. The cumulative collision counter is retained. |

When unmuted, `WARNING` with fused risk `WARN` toggles the buzzer with a 500 ms interval. Collision confirmation uses 100 ms. The OLED's simple safe flag uses target freshness and vibration state. Android also shows the full application state. Read each display according to the fields it presents. [State and buzzer handling](../src/main.cpp#L356-L423) · [Command handlers](../src/main.cpp#L1299-L1350)

This guide follows Sections 1–3 of the coursework report and Slides 4–11 of the defense. Boundaries and timings come from the current firmware.
