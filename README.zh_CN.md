<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# FoloToy 倒计时提醒

AI Passport 上的倒计时玩法：开机进入选档，到点蜂鸣并闪烁 **TIME UP**。

预设：**5 / 10 / 15 / 20 / 25 / 30 分钟**。

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

## GitHub Actions 出包

与上游 [CI 构建说明](https://github.com/FoloToy/ai-passport/blob/main/docs/development/CI-build-and-release.zh_CN.md) 相同：日常 push **不会**发 Release；打 tag 才会编译并挂上固件。

1. 仓库 **Settings → Actions** 打开 Actions（fork 默认关闭）。
2. 推送 tag，例如 `v0.1.0-timer`：

```bash
git tag v0.1.0-timer
git push origin v0.1.0-timer
```

3. `Build firmware` 用 ESP-IDF 5.5.3 / ESP32-C3 跑 `./tools/validate.sh --firmware`。
4. 成功后创建 GitHub Release，附件为 `FoloToy-AI-Passport-full.bin`。
5. 也可在 Actions 页对 `Build firmware` 点 **Run workflow** 只出 artifact、不发 Release。

`main` / PR 上的 `Pull request checks` 也会编固件，artifact 保留 7 天，不自动发 Release。

刷机：打开 [在线刷机工具](https://ai-passport.folotoy.cn/tools/web-flasher/)，选 Release 里的 `FoloToy-AI-Passport-full.bin`，从 `0x0` 写入 8 MB 设备。
