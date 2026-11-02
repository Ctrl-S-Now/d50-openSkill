# d50-openSkill

**D50 的开源 Skill**：让你的 AI 助手（Codex、Claude Code、Cursor 等）带你把 D50 用起来 —— 编程指导、引脚与原理图说明、联网配置、排障和售后。
Skill 自带一份最简的开放基础固件，开机即做外设自检，你可以直接在它上面开发。

## 怎么用

1. 克隆本仓库：`git clone <本仓库地址>`
2. 对你的 AI 助手说：**"读一下 SKILL.md，然后帮我……"**
   - Claude Code：也可以把整个仓库目录复制到 `~/.claude/skills/d50/`，之后直接提 D50 相关的问题即可。
   - 其他助手（Codex、Cursor 等）：打开仓库后让它先读 `SKILL.md`（`AGENTS.md` 会指向它）。

## 里面有什么

| 路径 | 内容 |
| --- | --- |
| `SKILL.md` | Skill 入口：助手首先读它 |
| `references/` | 助手按需读取的文档：环境烧录、引脚、编程指南、联网、排障、售后 |
| `firmware/` | 开放基础固件工程（ESP-IDF v6.0.2，ESP32-P4）：开机画面、外设自检、社区二维码、K2 小恐龙、静默联网 |
| `firmware/c2_firmware/` | 板载 Wi-Fi 芯片（ESP8684）的从机固件（出厂已自带，一般不用管） |

固件细节见 [firmware/README.md](firmware/README.md)。预编译的整包固件放在本仓库的 **Releases** 页面（不需要装开发环境）。

## 联系我们

保修、硬件问题、商务合作：**xiangjinbo@vijimgroup.com**。软件问题请先让你的 AI 助手按 `references/05-troubleshooting.md` 排查，
仍无法解决再到 Issues 里反馈，并附上开机自检页面的照片。

## 许可

（待补充）
