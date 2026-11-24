# 手绢2000 (Handkerchief 2000) — Nintendo Switch 移植版

An unofficial, non-commercial port of the Windows game *Handkerchief 2000* (手绢2000, OPUS Corp. / ASK Co., Ltd., 1999-2006) to the Nintendo Switch. The original executable is statically recompiled from x86 to C, so the game's own code runs unchanged; this repository supplies the Windows/DirectX environment it expects. **You need your own copy of the game.**

《手绢2000》是横版阵营攻防动作游戏（六支以动物命名的队伍争夺战地领域）。本项目把原版 `HDKWIN.EXE` 的 x86 机器码静态转译为 C 后在 Switch 上运行——游戏逻辑 100% 是原版代码，移植层只实现它依赖的 Windows API 与 DirectDraw/DirectSound。

## 移植状态（NX-2.0）

| 功能 | 状态 |
|------|------|
| 游戏本体 | 标题、关卡、结算、25 FPS 原速运行 |
| 画面 | DirectDraw 8-bit 软件模拟；4:3、拉伸 16:9、原生宽屏 848×480（关卡视野更宽） |
| 中文 | 资源字符串/菜单/对话框按 GBK 码页转换，内置 GB2312 宋体点阵渲染 |
| 存档 | 载入/保存/删除界面完整可操作；多槽位写入（路径规范化）；槽位列表按磁盘实际状态显示 |
| 音频 | Miles 接口层 + 软件混音（连接原版 DirectSound） |
| 输入 | 手柄映射，界面方向键导航；游玩时 X/B 可对调；命名输入使用系统软键盘 |
| 附加 | 暂停菜单（宽屏开关、画面比例、音量、FPS、原版秘技） |

## 操作

| 按键 | 游玩 | 菜单/对话框 |
|------|------|-------------|
| A | 射击 | 确认 / 点击 |
| B | 跳跃 | 取消 |
| X | 切换与旋转（Switch & Rotation） | — |
| Y | 变身炸弹（Turncoat Bomb） | 等于「确定」 |
| L / R | Shift / Ctrl | — |
| − | 游戏菜单 / Esc | 取消 |
| + | 暂停菜单 | — |
| 左摇杆 / 十字键 | 移动 | 焦点导航 |

存档界面：方向键移动焦点，A 选中槽位/按下按钮，Y 等于「确定」（保存/载入），B 取消。光标在「删除」上按 A 或 Y 弹出确认框，确认后清空该槽存档（主文件与附属文件一并删除）。

## 构建

依赖：

- Python 3（`capstone`、`pefile`）
- [devkitPro](https://devkitpro.org)（libnx）与 aarch64-none-elf GCC
- SDL2（Switch 静态库）
- Ghidra 11.x（一次性导出函数表）

```
# 1. 用自备的正版 exe 导出函数表（一次）
analyzeHeadless <proj_dir> hk -import original-data/HDKWIN.EXE \
    -scriptPath re/ghidra -postScript ExportFunctions.java build/hk-recomp/re/functions.json

# 2. 静态重编译：x86 -> C
HK_EXE=original-data/HDKWIN.EXE HK_FUNCS=build/hk-recomp/re/functions.json \
HK_GEN=build/hk-recomp/gen python -I recomp/x86rec.py
python -I recomp/gen_import_stubs.py

# 3. 编译、链接、打包 nro
python build_hk.py build link nro
```

工具链路径可用环境变量覆盖（见 `build_hk.py` 顶部：`HK_TC`、`DEVKITPRO`、`HK_NEWLIB`、`HK_LIBS`、`HK_SDL_INC`）。

> **重编译产物（`build/hk-recomp/gen/`）与由它构建的任何二进制都派生自商业可执行文件：仅限本地使用，不得再分发。**

Linux 下也可先跑 PC 版验证逻辑：`make -f port/Makefile.pc -j8 && build/hk-pc/handkerchief <游戏目录>`。

## 目录

| 路径 | 内容 |
|------|------|
| `recomp/` | x86 → C 静态重编译器（派生自 [handkerchief-vita](https://github.com/IvandersonBordin/handkerchief-vita)，其重编译器派生自 [sh3vita](https://github.com/matfsouza/sh3vita)），以及移植所需的补丁与钩子表 |
| `re/ghidra/` | 导出函数表用的 Ghidra 脚本 |
| `port/` | 移植层：Win32/DirectDraw/DirectSound 模拟、对话框与菜单、输入、宽屏合成等（GPL-3.0） |
| `switch/` | Switch 入口（SDL2 + libnx）、按键映射、暂停菜单 |
| `tools/` | NRO 打包、字体/码表生成、原版资源解析等辅助脚本 |
| `docs/` | 设计文档（静态重编译、引擎结构、HDK 格式、存档机制、宽屏规划） |

## 许可与版权

- 移植层与重编译器：**GPL-3.0**（见 `LICENSE`、`THIRD_PARTY_NOTICES.md`）
- 游戏本体、美术、音乐：© OPUS Corp. / ASK Co., Ltd.，版权归权利人。**本仓库不包含任何游戏文件**（exe、Bitmap/System/Wav/User 资源、重编译产物）
- 移植：千叶黑猫 / lsyymmilo

本项目与 OPUS Corp. / ASK Co., Ltd. 无任何关联，仅供学习与技术研究。使用需自备正版游戏。

> AI生成
