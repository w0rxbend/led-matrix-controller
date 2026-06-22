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

Upload and start a generated custom animation:

```bash
bash examples/custom_animation.sh
```

The custom animation example builds 192-byte physical-order frames in Python and
sends them with protocol command `0x09`.
