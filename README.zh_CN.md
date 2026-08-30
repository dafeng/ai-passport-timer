<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# FoloToy 倒计时提醒

AI Passport 上的倒计时玩法：开机进入选档，到点蜂鸣并闪烁 **TIME UP**。

预设：**30 秒 / 5 / 10 / 15 / 20 / 30 分钟**。

## 按键

| 状态 | 上 / 下 | 确定短按 | 确定长按 |
| --- | --- | --- | --- |
| 选档 | 切换预设 | 开始 | — |
| 计时中 | — | 暂停 | 返回选档 |
| 已暂停 | — | 继续 | 返回选档 |
| 到时 | — | 再选一次 | — |

界面文案为英文（设备字体不含中文汉字）。电量在右上角白云下方；读不到电量时不显示。

## 目录

```text
main/                 倒计时逻辑、界面、启动
components/bsp/       显示 / 按键 / 音频 / 电池
bootloader_components  Recovery 按键钩子（刷机兼容）
tests/                倒计时状态机主机测试
tools/                校验与固件检查
```

## 构建

需要 **ESP-IDF 5.5.3**，目标 ESP32-C3。

```bash
idf.py --version
./tools/validate.sh --static
./tools/validate.sh --firmware
```

合并固件：`build/FoloToy-AI-Passport-full.bin`。
