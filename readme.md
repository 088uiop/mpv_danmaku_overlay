# mpv 弹幕覆盖层（danmaku_overlay）

## 启动命令与输入参数：

通过命名管道等待 lua 脚本连接并下发弹幕数据。

| 参数 | 说明 |
| --- | --- |
| `--mpv-pid` | mpv 进程 ID，用于反查窗口句柄 |
| `--mpv-hwnd` | 直接传窗口句柄（十六进制，可选） |
| `--pipe` | 命名管道名，默认 `\\.\pipe\danmaku_overlay_<pid>` |
| `--fps` | 渲染帧率，默认 60 |
| `--log` | 日志文件路径，默认 `%TEMP%\danmaku_overlay.log` |

## 管道 JSON 命令

lua 通过管道按行发送 JSON，每个 `type` 对应一条命令：

| type | 说明 | 参数示例 |
| --- | --- | --- |
| `load_ass` | 加载 ASS 字幕文件路径，成功后回 `ready` | `{"type":"load_ass",<br>"filepath":"C:\\path\\danmaku.ass"}` |
| `sync` | 播放进度/状态同步，仅 seek、暂停、变速、暂停恢复时发送 | `{"type":"sync","time_pos":12.34,<br>"duration":300,"paused":false,"speed":1,"seeking":false}` |
| `set_delay` | 弹幕时间偏移（秒，±120 内） | `{"type":"set_delay","delay":0.5}` |
| `set_enabled` | 弹幕渲染总开关 | `{"type":"set_enabled","enabled":true}` |
| `set_hdr` | HDR 渲染开关 | `{"type":"set_hdr","enabled":true}` |
| `set_hdr_peak` | HDR 峰值亮度（nits，默认 203） | `{"type":"set_hdr_peak","hdr_peak":203}` |
| `set_config` | 渲染参数：透明度、字体缩放、弹幕上限 | `{"type":"set_config","opacity":0.8,<br>"font_scale":1.0,"max_danmaku":200}` |
| `clear_danmaku` | 清空当前所有弹幕 | `{"type":"clear_danmaku"}` |
| `shutdown` | 关闭 overlay 进程 | `{"type":"shutdown"}` |
| `ping` | 心跳探活，回 `pong` | `{"type":"ping"}` |

- `sync` 也接受 `update_state` 作别名，`time_pos` 为秒、`speed` 为倍速、`paused`/`seeking` 为布尔。
