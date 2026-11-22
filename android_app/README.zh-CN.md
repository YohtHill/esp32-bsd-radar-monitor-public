# 安卓蓝牙客户端

[English](README.md) · [项目首页](../README.zh-CN.md)

使用 JDK 17、Android SDK Platform 35、Gradle 8.7 与 Android Gradle Plugin 8.6.1。可用 Android Studio 打开本目录，或在本目录执行下方命令。

~~~powershell
.\gradlew.bat assembleDebug
~~~

安装 `app/build/outputs/apk/debug/app-debug.apk`。在安卓设置中与 `ESP32_BSD_MONITOR` 配对，按提示授予附近设备或蓝牙权限。刷新已配对设备列表，选择 ESP32 并连接。App 显示左、中、右分区，以及各目标的带方向速度（千米/小时）、距离、角度和风险等级。参见[原始画面及说明](../demo/README.zh-CN.md)和[指令行为](../docs/design.zh-CN.md)。

API 35 调试构建已生成 APK。手机与板卡联调可按[台架测试步骤](../docs/reproduce-and-test.zh-CN.md)复现。

[安卓状态字段与通信格式](../docs/radar-protocol.zh-CN.md)
