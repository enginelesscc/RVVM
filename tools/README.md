# Guest framebuffer and input control

Start RVVM with `-gui none -control 1337` to keep the emulated display and HID devices without opening a host window. Other GUI backends can also use `-control`. Use `-usb-input` when the guest needs USB keyboard and mouse devices. `-nogui` removes the default display and is not a substitute for `-gui none`.

The optional control server binds only to `127.0.0.1`. It permits one connection at a time and disconnects after ten idle seconds. Treat it as a local debugging interface: any local process can connect. Disconnecting or issuing `release` releases injected keys and mouse buttons.

The Python client needs Python 3 and Pillow for PNG capture:

```powershell
python tools/guest_control.py frame frame.png
python tools/guest_control.py click 500 199
python tools/guest_control.py chord 0xe0 0xe1 0x29
python tools/guest_control.py text 'taskmgr'
```

The chord example sends left Control + left Shift + Escape. Keys use USB HID keyboard usage codes. Text entry assumes a US keyboard layout. Mouse coordinates refer to the guest framebuffer, independent of host window position or scaling. Add `--port PORT` before the command to select another port.

Scripts can import `Guest` from `guest_control`, then use it as a context manager. Methods include `frame(path)`, `key(code, down)`, `chord(*codes)`, `mouse(x, y, buttons=0)`, `click(x, y, button=1)`, `text(value)`, and `request(command)`.

The socket accepts newline-terminated ASCII commands:

| Command | Result |
| --- | --- |
| `frame` | `FRAME width height stride format byte_count\n`, followed by exactly `byte_count` raw framebuffer bytes |
| `key usage down` | Press (`1`) or release (`0`) a HID key |
| `mouse x y buttons` | Set absolute position and left/right/middle button bits (`1/2/4`) |
| `scroll amount` | Relative wheel motion, from -127 to 127 |
| `release` | Release all injected input |

Input commands reply `OK\n` or `ERROR reason\n`. Requests are limited to 255 bytes and frames to 64 MiB. Input and socket work run in the existing display poll loop. Frame capture copies the current scanout; it does not pause guest drawing, so an actively changing frame can tear. Move the cursor away and allow the UI to settle when comparing screenshots.
