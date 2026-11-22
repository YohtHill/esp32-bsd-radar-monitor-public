# 原型演示

[English](README.md) · [项目首页](../README.zh-CN.md)

动图由课程报告中的三张静态画面组成，每张画面对应各自的测试场景。

![PCB、OLED 和安卓画面演示](demo.zh-CN.gif)

| 素材 | 画面内容 |
| --- | --- |
| [原型实物背面](assets/assembled-back.jpg) | 原始照片可见载板焊点。[顶层布线图](assets/pcb-layout.png)标明主控、两块屏和传感器在板上的位置。 |
| [OLED 截图](assets/oled-two-targets.jpg) | 同时显示约 8 米和 10 米的两个目标，两者显示速度均约为 −2 米/秒。 |
| [安卓预警截图](assets/android-warning.jpg) | 目标列表包含两条风险记录。左侧目标为 10.00 米、−7.2 千米/小时、−16°，风险 4/5；中间目标为 4.00 米、−3.6 千米/小时、3°，风险 2/5。 |

报告的串口截图还记录了一条类型为 0x07 的单目标上报，距离 2.00 米、角度 +55°。需要重现实验时可参照[台架测试](../docs/reproduce-and-test.zh-CN.md)。
