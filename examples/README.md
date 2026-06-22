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

Override timing/color without editing scripts:

```bash
MATRIX_INTERVAL=70 MATRIX_R=0 MATRIX_G=180 MATRIX_B=255 bash examples/presets/scanner.sh
```

Upload and start a generated custom animation:

```bash
bash examples/custom_animation.sh
```

The custom animation example builds 192-byte physical-order frames in Python and
sends them with protocol command `0x09`.
