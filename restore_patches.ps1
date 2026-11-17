# restore_patches.ps1 —— 把打过补丁的组件还原到 managed_components。
#
# 背景：managed_components/ 被 .gitignore 排除，干净 checkout 后组件管理器会重新下载
#       未打补丁的版本。本工程有两处补丁必须还原，否则功能直接坏掉：
#
#   1) waveshare__esp32_s3_touch_amoled_2_16
#      官方 BSP 的 I2C 初始化没开内部上拉，CST9220 在 400 kHz 下读回来的 ACK 字节不对。
#
#   2) waveshare__esp_lcd_touch_cst9217
#      - 初始化进了 0xD101 命令页却没切回 0xD109 报文页 → 之后每次读到的都是同一帧
#        命令页内容，触摸完全不响应（日志刷 Touch read failed）
#      - ACK 校验（0xAB）对本板 0x542F 老固件不适用，改为不阻断解析
#      - 只解析 1 个触点 → 放开到 5 点，并把报文的 track_id 填进去供多指跟踪
#
# 用法：先跑一次 .\build.ps1 build（让组件管理器把依赖下载完），再执行本脚本，然后重新 build。

$ErrorActionPreference = "Stop"

$root = $PSScriptRoot

$targets = @(
    @{
        Src  = "vendor_bsp\waveshare__esp32_s3_touch_amoled_2_16"
        Dst  = "managed_components\waveshare__esp32_s3_touch_amoled_2_16"
        File = "esp32_s3_touch_amoled_2_16.c"
        Mark = "enable_internal_pullup"
    },
    @{
        Src  = "vendor_cst9217\waveshare__esp_lcd_touch_cst9217"
        Dst  = "managed_components\waveshare__esp_lcd_touch_cst9217"
        File = "esp_lcd_touch_cst9217.c"
        Mark = "back_to_report"
    }
)

foreach ($t in $targets) {
    $src = Join-Path $root $t.Src
    $dst = Join-Path $root $t.Dst

    if (-not (Test-Path $src)) {
        throw "找不到备份：$src"
    }
    if (-not (Test-Path $dst)) {
        throw "找不到 $dst。先执行 .\build.ps1 build 让组件管理器把依赖下载下来。"
    }

    Copy-Item (Join-Path $src "*") $dst -Recurse -Force

    $target = Join-Path $dst $t.File
    if (Select-String -Path $target -Pattern $t.Mark -SimpleMatch) {
        Write-Output "OK   $($t.Dst)"
    } else {
        Write-Output "WARN 复制完成但没找到补丁标记，请手工确认 $target"
    }
}
