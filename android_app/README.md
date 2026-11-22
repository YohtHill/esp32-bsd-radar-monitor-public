# Android Bluetooth Client

[简体中文](README.zh-CN.md) · [Project home](../README.md)

Build with JDK 17, Android SDK Platform 35, Gradle 8.7 and Android Gradle Plugin 8.6.1. Open this folder in Android Studio or run the wrapper from this directory.

~~~powershell
.\gradlew.bat assembleDebug
~~~

Install `app/build/outputs/apk/debug/app-debug.apk`. Pair with `ESP32_BSD_MONITOR` in Android settings. Grant nearby device or Bluetooth permissions when prompted. Refresh the paired-device list, select the ESP32 and connect. The app presents the left, centre and right zones, signed speeds in km/h and each target's distance, angle and level. See the [archived screen and walkthrough](../demo/README.md) and [command behaviour](../docs/design.md).

The API 35 debug build produced an APK. On-device checks can be repeated using the [bench procedure](../docs/reproduce-and-test.md).

[Android telemetry field map and wire format](../docs/radar-protocol.md)
