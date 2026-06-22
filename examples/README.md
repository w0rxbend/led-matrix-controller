# Examples

These examples use the existing TCP client in `tools/matrix_client.py`.

Set the target device IP once:

```bash
export MATRIX_HOST=192.168.1.127
```

Run a preset:

```bash
bash examples/presets/rainbow.sh
bash examples/presets/fire.sh
bash examples/presets/border_chase.sh
```

Set a static color:

```bash
bash examples/static/razer_green.sh
bash examples/static/toxic_green.sh
bash examples/static/cyan.sh
bash examples/static/warm_white.sh
```

Override timing/color without editing scripts:

```bash
MATRIX_INTERVAL=70 MATRIX_R=0 MATRIX_G=180 MATRIX_B=255 bash examples/presets/scanner.sh
MATRIX_R=68 MATRIX_G=214 MATRIX_B=44 bash examples/static/green.sh
```

Draw a custom heart frame:

```bash
python tools/matrix_client.py heart --r 255 --g 0 --b 80
python tools/matrix_client.py heart-beat
python tools/matrix_client.py heart-beat --layout h-tl
bash examples/heart_frame.sh
MATRIX_R=57 MATRIX_G=255 MATRIX_B=20 bash examples/heart_frame.sh
bash examples/heart_pulse.sh
bash examples/heart_beat.sh
MATRIX_ROTATION=90 bash examples/heart_beat.sh
```

The heart helpers use display-space coordinates and compensate for the fixed
firmware's alternating row mirror. Use `--transport frame` only when testing
raw 192-byte payload behavior.

Upload and start a generated custom animation:

```bash
bash examples/custom_animation.sh
MATRIX_MODE=fire MATRIX_DELAY=90 bash examples/custom_animation.sh
bash examples/custom/fire.sh
bash examples/custom/flame.sh
bash examples/custom/stardust.sh
bash examples/custom/warp.sh
bash examples/custom/bell.sh
bash examples/custom/notification.sh
bash examples/custom/rocket.sh
bash examples/custom/air_raid_alert.sh
bash examples/custom/telegram.sh
bash examples/custom/scala_logo.sh
bash examples/custom/ubuntu_logo.sh
bash examples/custom/matrix_rain.sh
bash examples/custom/lofi.sh
bash examples/custom/robot_face.sh
bash examples/custom/eyes.sh
bash examples/custom/sleepy_eyes.sh
```

The custom animation example builds 192-byte physical-order frames in Python and
sends them with protocol command `0x09`. Available generated modes are `dot`,
`wipe`, `fire`, `flame`, `stardust`, `warp`, `bell`, `notification`, `rocket`,
`air_raid_alert`, `telegram`, `scala_logo`, `ubuntu_logo`, `matrix_rain`, and
`lofi`, `robot_face`, `eyes`, and `sleepy_eyes`. The robot mode draws white
animated eyes and a curved mouth by default; `eyes` and `sleepy_eyes` draw only
white eye shapes. Most modes use `MATRIX_R`, `MATRIX_G`, and `MATRIX_B` as their
color tint. All generated custom frames are rotated `-90` before upload.
