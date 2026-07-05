# Emperor's-pad

Modern gamepad support for **Indiana Jones and the Emperor's Tomb** (2003).

Tested with DualSense and 8BitDo. Works on both GOG and Steam versions.

---

## Quickstart

1. Download the latest Emperor's-pad release from GitHub.
2. Extract the archive contents into `...\GameData\bin` (next to `indy.exe`).
3. Run `init.bat` as Administrator — it copies the original system `dinput8.dll` alongside the exe and renames it to `dinput8_orig.dll`.
4. **Steam only:** right-click the game in your library → Properties → Controller → set to **Disable Steam Input**.
5. Plug in your controller, then launch the game.
6. In-game: **Options → Controls → Enable Gamepad: ON**, then bind the controls as shown below.

---

## Recommended control layout

This layout is taken from the original PS2 version of the game.

| Action | Button |
|---|---|
| Primary Attack | A / Cross |
| Secondary Attack | X / Square |
| Jump | B / Circle |
| Action | Y / Triangle |
| Draw / Pack | LT / L2 |
| Reload | RT / R2 |
| Somersault (Roll) | LB / L1 |
| Inventory Open | D-pad Up |
| Inventory Previous | D-pad Left |
| Inventory Next | D-pad Right |
| Inventory Close | D-pad Down |
| Guard Mode | RB / R1 |
| 1st Person | RS / R3 |
| Wall Hug Peek Left | Select / Back |
| Wall Hug Peek Right | Start |
| Walk | *(leave blank, or bind to a keyboard key)* |

> **Note:** Walk doesn't need a gamepad binding — movement is analog, so the left stick already controls both walking and running by how far it's pushed.

> **Note:** The Menu button is hardcoded to LS / L3 and cannot be rebound. Reset Camera is hardcoded to RS / R3 — tap R3 to reset the camera, hold it for first-person view.

---

## How it works

The engine communicates with the controller through DirectInput, but modern
pads (especially DualSense over native USB/Bluetooth) can come through
Windows' legacy HID layer with a broken or incomplete axis layout — the
right stick may not show up at all, or gyro/accel data can leak into the
axes. Emperor's-pad inserts a `dinput8.dll` proxy between the game and the
system library. By default it reads the controller through SDL2's
GameController API (which parses the HID report correctly) and hands the
game a clean, standard layout. Same approach as Pad-Within (Prince of
Persia: Warrior Within) and Gamepads of Valhalla (Rune).

The engine also has its own quirks:
- Camera axes are read from hardcoded byte offsets, not standard DirectInput
  axis semantics. Confirmed by testing: horizontal turn reads from Z/Rz, 
  vertical look from Ry.
- POV/hat and analog triggers are never read by the engine — only digital
  buttons. The D-pad and triggers are synthesized as extra buttons so they
  can be bound in the game's own control menu.
- The control menu can only bind buttons, not axes.

---

## Configuration (`dinput8.ini`)

All settings can be changed without rebuilding.

| Key | Default | Description |
|---|---|---|
| `MoveDeadzone` | 5000 | Left stick deadzone, SDL units (0–32767) |
| `CameraDeadzone` | 5000 | Right stick deadzone, SDL units (0–32767) |
| `CameraSensitivity` | 65 | Right stick sensitivity — 50 = as-is, 100 = 2×, 25 = 0.5× |
| `MoveStickRange` | 100 | Left stick physical travel as % of full range |
| `CameraStickRange` | 100 | Right stick physical travel as % of full range |
| `MoveRadialScale` | 1 | Circle-to-square correction so diagonals reach full per-axis output |
| `MoveSmoothing` | 40 | Left stick EMA smoothing — 0 = off, higher = smoother but more lag |
| `AxisSnapRatio` | 15 | Right stick cross-axis snap threshold in % |
| `UseHIDAPI` | 0 | Set to 1 to use SDL's proprietary PS4/PS5 HIDAPI parser |

---

## Recommended companion mods

Emperor's-pad only restores controller functionality. For a more
comfortable overall experience, it's recommended to also use the
**Widescreen WSGF mod**.

**For a stable 60 FPS:** in `.../GameData/Indy/vars.cfg`, don't set
`fpsLimit` to 60 — set it to **0** instead, and cap the framerate
externally with RivaTuner Statistics Server, or through DgVoodoo2's own
frame limiter options.

**DgVoodoo2** does a decent job smoothing out the engine's aggressive draw
calls in DX12 mode, but test which backend actually runs best on your
system — your mileage may vary. DgVoodoo2 also stretches cutscenes and
loading screens to fill the whole screen, which is a nice bonus.

---

## Troubleshooting

Set `Log=1` and optionally `DumpAxes=1` in `dinput8.ini` — this writes
`dinput8_proxy.log` next to the exe. Key lines to look for:

- `SDL: opened controller #N: <name>` — SDL found and opened the pad.
- `SDL: backend active` — SDL path is live.
- `SDL: ... falling back to DirectInput` — something's wrong (missing SDL2.dll, pad not recognised as a GameController, etc.).

---

## Rebuilding from source

```
i686-w64-mingw32-gcc -shared -O2 -static-libgcc -o dinput8.dll \
    src/dinput8_proxy.c src/dinput8_proxy.def -ldxguid -lole32 -lm
```

`-static-libgcc` is required — without it the build can produce a runtime
dependency on `libgcc_s_dw2-1.dll`, which is not present on a stock Windows
install and will silently prevent the game from starting.
