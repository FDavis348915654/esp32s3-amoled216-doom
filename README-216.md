# Doom on ESP32-S3-Touch-AMOLED-2.16

把 Doom（doomgeneric）跑在微雪 2.16 寸 AMOLED 板上的移植工程。
源工程是 [Sethyrus/ESP32Watch-Doom](https://github.com/Sethyrus/ESP32Watch-Doom)（2.06 手表版），本目录是改到 2.16 的版本。

> 最后更新：2026-11-16 晚。当前**能玩**，触摸与音量还差一步，见「待办」。

---

## 1. 现状

| 项 | 状态 |
|---|---|
| 编译（IDF 6.0.2 + GCC 15） | ✅ 通过，产物 `ESP32WatchDoom.bin` ≈ **660 KB** |
| 启动 / 渲染 | ✅ `fps=27`，单帧绘制 `26.3 ms`（480×480 屏，游戏区 480×360） |
| 音效 | ✅ ES8311 出声，22050 Hz，8 声道混音 |
| WAD | ✅ 编译期嵌入 `storage` 分区（6 MB FAT），不依赖 SD 卡 |
| SD 卡 | ✅ 自动挂载为存档区（`SL32G` 实测），没卡也能玩 |
| 触摸 | ⚠️ 报 `ESP_ERR_INVALID_RESPONSE`，修复版**已编译但尚未烧录验证** |
| 音量 | ⚠️ 出厂设 60，实测**很吵**，建议 20~25 |
| 画面方向 | ⚠️ 沿用 2.06 的 90° 软件旋转，方形屏上是否正立**待肉眼确认** |

启动日志关键行（已实测）：

```
I (2032) doom_app: Internal FATFS mounted successfully
I (2114) doom_app: SD mounted: SL32G
I (2114) doom_app: Starting DoomGeneric with /internal/doom1.wad
                            DOOM Shareware
I (10451) doom_port: DG_DrawFrame: frames=138 fps=27 draw_avg_us=26269 draw_max_us=26347
I (7237) doom_sound: audio: buffers=43/s write_avg_us=22947 write_err=0
```

---

## 2. 硬件要点（改代码前先看）

| 项 | 参数 |
|---|---|
| 主控 | ESP32-S3R8：双核 240 MHz，**8 MB OCT PSRAM**，**16 MB flash**（2.06 是 32 MB，别照抄它的分区表） |
| 屏 | CO5300，QSPI：CS12 / PCLK38 / D0-3 = 4/5/6/7，RST39，480×480，RGB565 + 字节交换 |
| 触摸 | CST9217，I2C 地址 **0x5A**，SDA15 / SCL14，RST40 / INT11，**swap_xy + mirror_y** |
| 音频 | ES8311（I2S：MCLK42 / SCLK9 / LCLK45 / DOUT8 / DSIN10，功放使能 GPIO46） |
| 电源 | AXP2101（I2C 0x34）；ALDO1 只给麦克风，屏/触摸默认供电 |
| SD | **SPI 模式**：CMD1 / CLK2 / D0=3 / CS41 |

---

## 3. 相对源工程的改动（逐个文件）

| 文件 | 改了什么 |
|---|---|
| `main/idf_component.yml` | 删 `watch_board`（2.06 手表按键组件）；BSP 由 `esp32_s3_touch_amoled_2_06 ^1.0.6` 换成 **`waveshare/esp32_s3_touch_amoled_2_16 ^2.0.1`**；`idf` 约束从 `>=5.5,<5.6` 放宽到 `>=5.5` |
| `main/main.c` | 删 `watch_launcher_boot_once()`（我们不做 launcher），直接 `doom_app_start()` |
| `main/CMakeLists.txt` | `REQUIRES doom_app watch_board` → `REQUIRES doom_app` |
| `components/doom_app/CMakeLists.txt` | 去 `watch_board`；BSP 组件名换 `waveshare__esp32_s3_touch_amoled_2_16`；加 `driver`（GPIO） |
| `components/doom_app/doom_app.c` | BSP 头文件换 2_16；`bsp_touch_new()` 传 `swap_xy=1, mirror_y=1`（原代码传 `NULL`，而 2.16 BSP 里有 `assert(cfg != NULL)`，必崩） |
| `components/doom_app/doom_port.c` | 删 `watch_buttons.h`，BOOT 键改本地 `GPIO0` 上拉输入；去掉 AXP2101 的 PWR 中断逻辑（菜单用触屏右上角）；游戏区高度 `376 → 360` |
| `components/doomgeneric/CMakeLists.txt` | 加 `-Wno-error`：1993 年的代码在 GCC 15 下会因新告警 × `-Werror` 大面积失败 |
| `partitions.csv` | 换成 16 MB 布局（见下） |
| `sdkconfig.defaults` | `FLASHSIZE_16MB`；加 `CONFIG_COMPILER_DISABLE_GCC15_WARNINGS=y`；去掉 RGB 屏专用项 |
| `managed_components/waveshare__esp32_s3_touch_amoled_2_16/…c` | **本地补丁**：I2C 加 `glitch_ignore_cnt=7` + `enable_internal_pullup`（官方 BSP 没有，疑似触摸读不到的原因） |

### 分区表（16 MB）

```
# Name,   Type, SubType, Offset,   Size
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
factory,  app,  factory, 0x10000,  0x200000,   # 2 MB，实际用 660 KB
storage,  data, fat,     0x210000, 0x600000,   # 6 MB，放 WAD
```

### ⚠️ BSP 补丁会丢

`managed_components/` 在 `.gitignore` 里，换机器 / 干净 checkout 后组件管理器会重新下载**未打补丁**的官方组件，触摸就又读不到了。工程里有两处补丁，都已备份：

| 备份目录 | 补丁内容 |
|---|---|
| `vendor_bsp/waveshare__esp32_s3_touch_amoled_2_16/` | I2C 初始化补上内部上拉 + 毛刺滤波 |
| `vendor_cst9217/waveshare__esp_lcd_touch_cst9217/` | `0xD109` 回报文页、ACK 不阻断、多点解析（5 点）、填充 `track_id` |

恢复：

```powershell
# 先跑一次 build，让 managed_components 生成出来，再执行：
.\restore_patches.ps1
```

---

## 4. 在新电脑上继续

### 4.0 带什么走

拷整个 `doom-216/` 目录即可，但注意三项**不在源码控制里**：

| 项 | 说明 |
|---|---|
| `wad/doom1.wad` | 4,196,020 字节，被 `.gitignore` 排除，必须单独拷（或按第 5 节重下） |
| `managed_components/` 里的组件补丁 | 已备份到 `vendor_bsp/` 与 `vendor_cst9217/`，新机器 build 一次后跑 `restore_patches.ps1` |
| `sdkconfig` | 由 `sdkconfig.defaults` 生成，不用带；`build/` 更不用带 |

本目录已初始化 git 仓库，`.gitignore` 排除 build / managed_components / sdkconfig / *.wad。

### 4.1 环境

- ESP-IDF **v6.0.2**，工具链 `esp-15.2.0_20251204` / cmake 4.0.3 / ninja 1.12.1 / ccache 4.12.1
- Python 环境 `idf6.0_py3.13_env`（IDF 自建）
- `build.ps1` 是照着 `mota-lite` 那套写的，**顶部三个路径写死**，新机器要么装到同样位置，要么改这三行：

```powershell
$IdfRoot = "C:\esp\esp-idf"
$Tools   = "$env:USERPROFILE\.espressif\tools"
$PyEnv   = "$env:USERPROFILE\.espressif\python_env\idf6.0_py3.13_env"
```

安装过程见主文档 `../微雪ESP32S3-AMOLED-2.16-开发记录.md` 第 4 节 / 第 10 节（含代理、submodule 的坑）。

### 4.2 构建

```powershell
cd <本目录>
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
.\build.ps1 set-target esp32s3     # 首次；要拉 22 个组件（LVGL 整棵依赖树）
.\build.ps1 build
```

首次 configure 约 3~5 分钟（组件下载 + CMake），全量编译约 8 分钟；之后增量 1 分钟内。
编译时会打印 `DOOM WAD Detected: … Embedding into FATFS partition 'storage'`。

### 4.3 烧录

```powershell
.\build.ps1 -p COM4 flash        # 首次 / 改分区：连 6 MB storage 一起烧，约 40 秒
.\build.ps1 -p COM4 app-flash    # 只改代码：快，约 6 秒
```

端口在设备管理器看（本机是 COM4，`VID_303A&PID_1001`；换 USB 口重新枚举）。

---

## 5. WAD

`wad/doom1.wad` **不在 git 里**（`.gitignore` 排除），换机器要单独带或重新下：

| 项 | 值 |
|---|---|
| 文件 | shareware `doom1.wad`（DOOM 1.9 试玩版，可自由分发） |
| 大小 | **4,196,020** 字节 |
| 校验特征 | 头 4 字节 `IWAD`，1264 个 lump，含 `E1M1` / `PLAYPAL` |
| 本次来源 | `https://raw.githubusercontent.com/samrude005/doom1.wad/HEAD/doom1.wad` |

放好后重新 `.\build.ps1 build`（CMake 在 configure 阶段抓 WAD，文件是后加的要 `idf.py reconfigure`）。
也可以不用嵌入：`menuconfig → Doom → Embed the WAD` 关掉，把 WAD 放 SD 卡根目录。

---

## 6. 待办（按优先级）

1. **验证触摸**（最高优先）。修复版已编译，未烧。烧完看日志是否还有 `Touch read failed`，再用手指试：中间 160×130 区域 = 使用/确认，上下 = 前进后退，左右 = 转身，BOOT = 开火。
   若仍失败，下一步换 100 kHz 试（`CONFIG_BSP_I2C_FAST_MODE=n`），或直接用 mota-lite 那套自建 I2C 总线（`enable_internal_pullup=true`，已验证可用）。
2. **降音量**：`components/doom_app/doom_app.c` 里 `esp_codec_dev_set_out_vol(spk, 60)` → 20~25。
3. **确认画面方向**：不正立就把 `doom_port.c` 的 `DOOM_PORT_LANDSCAPE_CLOCKWISE` 取反。
4. 性能（可选）：`draw_avg 26 ms` 是瓶颈。可试 `DOOM_PORT_CHUNK_LINES` 8 / 40、或把「物理行→源行」映射再简化（方形屏不需要旋转时可整段去掉）。
5. 手感（可选）：目前只有 BOOT 一个实体键，PWR 键（AXP2101 中断）没接；也可以加「双击 BOOT = ESC」。

---

## 7. 踩过的坑（省得再查）

| # | 现象 | 原因 / 处理 |
|---|---|---|
| 1 | `error: initializer-string … lacks 'nonstring' attribute` | GCC 15 新告警 + IDF 的 `-Werror`，出在 `d_main.c` 的 `char name[23][8]`。组件级 `-Wno-error` 解决 |
| 2 | `error: taking absolute value of unsigned type` | 同上，`r_segs.c`。`CONFIG_COMPILER_DISABLE_GCC15_WARNINGS=y` 只挡 GCC15 **新增**的那几条，GCC14 及更早的老告警仍会报错 |
| 3 | 触摸必崩 / `assert failed` | 2.16 BSP 的 `bsp_touch_new` 有 `assert(cfg != NULL)`，不能传 `NULL` |
| 4 | 触摸 `ESP_ERR_INVALID_RESPONSE` | I2C 读到了数据但 ACK 字节不对；官方 BSP 没开内部上拉 |
| 5 | 分区表放不下 | 2.06 是 32 MB flash、7 个 OTA 槽；2.16 只有 16 MB，必须重写 |
| 6 | `storage` 镜像每次 `flash` 都重写 6 MB | 正常。只改代码用 `app-flash` |
| 7 | IDF 6 下 BSP 多一个 `usb` 依赖 | BSP 2.0.1 已按 `idf_version >= 6.0` 自动处理，无需干预 |

---

## 8. 目录结构

```
doom-216/
├─ build.ps1                 固化 IDF 环境的构建入口（从 mota-lite 抄的）
├─ restore_patches.ps1       把 vendor_bsp / vendor_cst9217 里打过补丁的组件还原到 managed_components
├─ partitions.csv            16 MB 分区表
├─ sdkconfig.defaults        16 MB / OCT PSRAM / 关 GCC15 新告警
├─ main/                     app_main，只负责起 doom
├─ components/
│  ├─ doom_app/              板级层：显示、触摸、按键、音频挂接、WAD 挂载
│  └─ doomgeneric/           引擎（vendor，只编译用到的源文件）
├─ wad/                      doom1.wad 放这里（git 忽略）
├─ vendor_bsp/               打完补丁的 2.16 BSP 备份
└─ docs/DOOM_PORT.md         原作者的移植笔记（西班牙语，仍有参考价值）
```

引擎与渲染路径与原工程一致：`DG_ScreenBuffer`（320×240，PSRAM）→ 逐像素查 256 色板转 RGB565 + 字节交换 → 按行分块 `esp_lcd_panel_draw_bitmap()`。
