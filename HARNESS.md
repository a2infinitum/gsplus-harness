# GSplus test harness

A fork of [digarok/gsplus](https://github.com/digarok/gsplus) adding a headless
test harness: unattended boots, screenshots, breakpoints and crash-state dumps,
driven entirely from the command line.

It exists to answer "does this disk still boot, and does it look right?" in an
automated gate rather than by eye. It was built for the
[GS/OS 6.0.1 rebuild](https://github.com/a2infinitum/mpw), where a one-byte error
in a load file shows up as a black screen and nothing else.

Everything lives in `gsplus/src/harness.c` / `harness.h`; the hooks in the
emulator core are five small call sites.

## Building

```sh
cmake -S gsplus/src -B gsplus/build -DCMAKE_BUILD_TYPE=Release -G Ninja
ninja -C gsplus/build gsplus-sdl
```

Runs fully headless under `SDL_VIDEODRIVER=dummy`. Screenshots are taken from the
internal framebuffer (`g_mainwin_kimage`), **not** `SDL_RenderReadPixels`, so
they work with no display attached.

## Flags

| Flag | Effect |
|---|---|
| `-hdir <dir>` | Artifact output directory |
| `-hsecs <secs>` | Quit with 0 after N emulated seconds |
| `-hshot <secs>` | Screenshot every N emulated seconds |
| `-hbp <bb/aaaa>` | Code breakpoint → dump state, exit 2 |
| `-hbpw <bb/aaaa>` | Write breakpoint → dump state, exit 2 |
| `-hdump <adr:len>` | Include a memory region in state dumps |
| `-hbrk` | Halt on `BRK` — during an unattended run a BRK is a crash |
| `-hstall <secs>` | Exit 3 if the screen has not changed for N seconds |
| `-hturbo` | No frame pacing; run flat out (90 emulated secs in a few wall secs) |

## Exit codes

| Code | Meaning |
|---|---|
| `0` | `-hsecs` timeout reached |
| `2` | Halt — breakpoint or trapped BRK. State is in `state-halt.txt`: registers, disassembly, stack, text pages, plus any `-hdump` regions |
| `3` | Screen settled under `-hstall`. **For a boot test this is the good ending** — it means the machine reached an idle desktop |

## Example

Boot a disk headless, screenshot every 10 emulated seconds, treat a BRK as a
crash, and call it settled after 4 idle seconds:

```sh
SDL_VIDEODRIVER=dummy gsplus/build/GSplus.app/Contents/MacOS/GSplus \
    -hdir out -hturbo -hbrk -hshot 10 -hstall 4 -hsecs 90
```

Compare screenshots with PIL rather than by file hash — crop the bottom ~70px
status bar (it carries a timer) and compare pixels, since raw PNG bytes always
differ:

```python
from PIL import Image
def load(p):
    im = Image.open(p).convert("RGB"); w, h = im.size
    return list(im.crop((0, 0, w, h - 70)).getdata())
assert load("ours.png") == load("stock.png")
```

## Gotchas

- **GSplus rewrites its config file on exit.** Regenerate it every run, or the
  drive comes up empty and it looks like a boot failure.
- **Boot a copy of your disk image.** A GS/OS boot writes `Finder.Data`,
  `Font.Lists` and `CDev.Data` back to the startup volume.
- **Pass an absolute image path** — the emulator runs from the artifact directory.
- **Always run a known-good control** before believing a boot failure.
