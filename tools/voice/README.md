# CMCC 语音 → WAV

固件在按住语音键并开麦后，经 USB-CDC 输出：

```
# VOICE 1
A <40 hex of 20B GATT notify>
...
# VOICE 0
# A END
```

PC 侧拼帧 → ICO 解白化（置换 + XOR `0x0416` + 字节交换）→ `decode.exe`（G.722.1 16kbps）→ 16kHz mono WAV。

## 用法

```bat
pip install pyserial
cd tools\voice
python voice_cap.py list
python voice_cap.py COM3 --auto
```

按住遥控器语音键说一句，松手后约 2.5 秒自动出 `out\voice_*.wav`。

## 说明

- USB 麦克风（UAC）尚未做；当前先落地可听 WAV。
- `decode.exe` 基于 [freeswitch/libg7221](https://github.com/freeswitch/libg7221)（GPL-2.0，独立进程调用）。

## 来源与许可证（GPL-2.0）

本目录中与遥控 BLE 音频解码相关的若干文件来自
[jackielozet/cmcc-voice-remote-bridge](https://github.com/jackielozet/cmcc-voice-remote-bridge)
（GPL-2.0）。详见 [`NOTICE`](NOTICE) 与 [`LICENSE`](LICENSE)。

仓库其余代码默认仍为根目录 MIT（见 [`../../LICENSE`](../../LICENSE)）；上述 GPL 文件及其衍生部分单独按 GPL-2.0 授权。
