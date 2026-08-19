# GSplus test harness

A fork of [digarok/gsplus](https://github.com/digarok/gsplus) adding a test
harness with two layers:

- a **batch harness** (`gsplus/src/harness.c`): unattended boots, screenshots,
  breakpoints and crash-state dumps, driven entirely from the command line and
  reporting pass/fail through the exit code. Built for the
  [GS/OS 6.0.1 rebuild](https://github.com/a2infinitum/mpw), where a one-byte
  error in a load file shows up as a black screen and nothing else.
- an **interactive control channel** (`gsplus/src/sdl_driver.c`): one command
  per line on stdin — press keys, move a virtual joystick, read and poke
  memory, watch writes, take screenshots — while the machine runs. Built for
  driving game testing (an Elite port) from shell scripts.

They compose: a batch run can take a `-script` file of control-channel
commands, and an interactive session still honors the `-h*` flags.

## Building

```sh
cmake -S gsplus/src -B gsplus/build -DCMAKE_BUILD_TYPE=Release -G Ninja
ninja -C gsplus/build gsplus-sdl
```

Runs fully headless under `SDL_VIDEODRIVER=dummy`. Screenshots are taken from the
internal framebuffer (`g_mainwin_kimage`), **not** `SDL_RenderReadPixels`, so
they work with no display attached.

## Batch flags

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
| `-script <file>` | Run control-channel commands from a file, one per line |
| `-timeout <secs>` | Quit with 0 after N **wall-clock** seconds — the backstop for a run that never advances emulated time (`-hsecs` is the deterministic form) |

### Exit codes

| Code | Meaning |
|---|---|
| `0` | `-hsecs` timeout reached |
| `2` | Halt — breakpoint or trapped BRK. State is in `state-halt.txt`: registers, disassembly, stack, text pages, plus any `-hdump` regions |
| `3` | Screen settled under `-hstall`. **For a boot test this is the good ending** — it means the machine reached an idle desktop |

### Example

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

## Control channel

The SDL front end reads one command per line from **stdin**, every frame,
running or halted. The same commands can come from a file via
`-script <file>`. Anything the harness does not claim falls through to the
65816 monitor, so its commands (`0/2000.20ff` etc.) still work. `help` prints
this list in the emulator:

    key <spec>[:frames] ...   tap keys — a char, a name (left right up down
                              ret esc space tab del), or $hh raw ADB code;
                              ":n" holds the key for n frames
    type <text>               tap out each character
    hold <spec> / release     press and keep held — this is what game flight
                              controls need (it sets any-key-down)
    joy <x%> <y%> | joy off   virtual joystick, -100..100, 0 centered;
                              overrides any real controller until "joy off"
    joybtn <n> <0|1>          joystick button up/down
    mem <bank>/<addr> [len]   hex dump, live, no halt needed (len in hex)
    <bank>/<addr>:<bytes>     poke memory (hex, case-insensitive)
    save <bank>/<addr> <len> <file>   raw region dump, for byte-comparing
    watch <bank>/<addr> [len] [n|stop]  report every write to the region,
                              with the value and the writing PC, and keep
                              running; "n" also dumps n instructions of
                              trace per hit; "stop" halts on the write and
                              dumps an interleaved PC+data trace to
                              logpc_out instead. "watch off" clears both
                              the flags and the underlying write breakpoint.
                              Exact byte range, not page-granular.
    trace [n]                 dump the last n instructions (default 24)
    text                      the 40/80-column text screen as text — read a
                              text-mode screen without OCRing a screenshot
    shot [path]               screenshot now
    wait <frames>             hold off later commands (60 frames = 1
                              emulated second)
    speed <0-3>               set g_limit_speed live: 0 unlimited, 1 1MHz,
                              2 2.8MHz, 3 8MHz
    echo <text>               sync marker on stdout
    halt / reset / quit       enter the monitor / reset the IIgs / exit

Semantics worth knowing:

- **Keys are injected as real ADB key-down/key-up events, never through the
  paste buffer.** Paste only fills the `$C000` latch; software that gates on
  `$C010` bit 7 (any-key-down) never sees a pasted key.
- **`echo` is a true sync barrier.** One command runs per frame, and none
  while a `wait` counts down or queued keys are still playing out — so when an
  `echo` marker appears on stdout, everything before it has actually happened
  on the emulated machine.
- **The poke form is claimed by the harness on purpose.** The 65816 monitor's
  Apple-II-style parser takes only lowercase hex — uppercase letters are
  monitor commands — so `04/294E:ff` passed through to it silently truncates
  to address `$0294` and writes there. The harness parser is case-insensitive.
- **`watch` and `trace` need `-logpc`** (the PC ring is only filled by the
  logging engine variants). A trapped BRK dumps the last 24 instructions
  automatically. Caveat: the ring's in-flight entry (the last line) has a
  valid PC and opcode but stale registers — they are filled at retire — so
  treat watch's "by PC" attribution as a strong hint, not proof; the retired
  lines above it are solid.
- **`speed` exists because unattended games run away at unlimited speed.**
  Boot at `-g_limit_speed 0` (seconds instead of minutes), then `speed 2`
  before doing anything real-time.

### Config variables as CLI flags

Any unrecognized `-<name> <value>` argument is applied as a config-variable
override (`config_add_argv_override`), so every config variable is a flag and
nothing needs to be hand-edited into the config file. Useful ones:
`-ssevery <secs>` / `-ssfile <path>` (auto-screenshots to a fixed, overwritten
path), `-g_status_enable 0` (hide the status lines), `-g_limit_speed 0`
(unlimited speed), `-audio 0`. `-h` lists them.

Caveat: **overrides are written back into the config file on exit**, so a
`-g_limit_speed 0` test run leaves that setting behind for the next
interactive launch. This is why `-script` is a plain flag rather than a config
variable — a stale script line would silently replay a test on the next boot.

## Boot-slot fallback

The ROM's slot-7 boot fallback goes to slot 5 and sits at an empty drive
forever. This fork falls through to slot 6 when s5d1 is empty and s6d1 has a
disk, so a 5.25" image boots unattended. (Selecting the boot slot via BRAM
bytes in the config doesn't work — the ROM checksums BRAM and resets
hand-edited values.)

## Gotchas

- **GSplus rewrites its config file on exit.** Regenerate it every run, or the
  drive comes up empty and it looks like a boot failure.
- **Boot a copy of your disk image.** A GS/OS boot writes `Finder.Data`,
  `Font.Lists` and `CDev.Data` back to the startup volume.
- **Pass an absolute image path** — the emulator runs from the artifact directory.
- **Always run a known-good control** before believing a boot failure.
- **Relative paths in the config file resolve from the config file's
  directory**, not the current directory.
- **Screenshots are large and all exactly the same size**: `png_write.c` links
  no zlib and emits stored deflate blocks. Compare content, never sizes.
