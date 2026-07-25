# Sanitizers (n64san)

[n64san](https://github.com/AngheloAlf/n64san) is a port of parts of LLVM's `compiler-rt`
to the N64. It lets a gcc-compiled build of the game report undefined behaviour at
runtime instead of silently misbehaving. It is wired into this repo as the submodule
`tools/n64san`, plus a DKR-specific implementation of n64san's "N64 Wrapper" API in
[src/n64san/dkr_n64_wrapper.c](../src/n64san/dkr_n64_wrapper.c).

Sanitizers only work with `COMPILER=gcc` (which implies `NON_MATCHING=1`). The ido
compiler has no sanitizer support, so matching builds are unaffected by everything below.

## Quick start

```sh
git submodule update --init --recursive
make COMPILER=gcc SANITIZE=all SANITIZE_FILES="src/racer.c src/camera.c" -j$(nproc)
```

Then run `build/dkr.us.v77.z64` in an emulator with IS-Viewer support (see
[Report sinks](#report-sinks)) and watch its debug output. A report looks like:

```
SanitizerTool: runtime error: shift exponent 32 is too large for 32-bit type 'int'
```

Changing any `SANITIZE*` option rebuilds everything automatically —
the options are recorded in `build/.debug_opts` and every object depends on it, because
make otherwise tracks only sources and headers and would reuse objects built with the
old flags.

## Options

| Variable | Default | Meaning |
|---|---|---|
| `SANITIZE` | *(empty)* | Space or comma separated list of `-fsanitize=` checks. `all` enables everything the runtime supports. Empty disables sanitizers entirely. |
| `SANITIZE_FILES` | *(empty)* | Which source files to instrument, e.g. `"src/racer.c src/camera.c"`. Empty instruments every gcc-compiled file — see [Size](#size). |
| `SANITIZE_OUTPUT` | `isviewer` | Where reports go: `isviewer`, `screen`, or `both`. |
| `SANITIZE_ABORT` | `0` | `1` adds `-fno-sanitize-recover=all`, so the first report halts the game instead of letting it continue. |
| `SANITIZE_DEBUG` | `0` | `1` builds the runtime with `SAN_DEBUG=1` for tracing from inside the sanitizer itself. |

### Supported checks

The runtime only implements part of ubsan, so `SANITIZE=undefined` will not link — the
build rejects unsupported names up front. `SANITIZE=all` expands to exactly:

`shift`, `integer-divide-by-zero`, `null`, `signed-integer-overflow`, `bounds`,
`alignment`, `object-size`, `float-divide-by-zero`, `float-cast-overflow`,
`pointer-overflow`

### Report sinks

- `isviewer` — the IS-Viewer 64 debug port at `0xB3FF0000`, written a word at a time
  through the PI. Nothing is emitted if the emulator or flashcart doesn't implement it.
- `screen` — DKR's own on-screen debug text (`render_printf`). That buffer is only
  0x800 bytes and is flushed once a frame, so it only really shows the first few reports
  of a frame, but it needs no emulator support.

#### On real hardware

The IS-Viewer was a devkit peripheral, so a stock console shows nothing. Reading it back
needs a flashcart that emulates the device and forwards the buffer over USB:

- **SummerCart64** — has an IS-Viewer implementation; enable it when deploying
  (`sc64deployer upload --isv 0x03FF0000 rom.z64`) and read it with `sc64deployer debug`.
- **64drive** — its firmware emulates IS-Viewer 64; the text shows up over USB.
- **EverDrive-64 X7** — no IS-Viewer emulation. Use `SANITIZE_OUTPUT=screen` instead, or
  swap the sink for the cart's own USB protocol via UNFLoader.

If you have no supporting cart, `SANITIZE_OUTPUT=screen` needs nothing but the console —
that's the reason the sink exists.

#### Emulators

| Emulator | Works | Notes |
|---|---|---|
| [gopher64](https://github.com/gopher64/gopher64) | yes | `src/device/is_viewer.rs`. Output goes to **stdout**, so launch it from a terminal. Only mapped for ROMs under ~64 MB, which this one is. |
| [ares](https://ares-emu.net/) | yes | Output goes to the debugger's IS-Viewer tracer notification, so it has to be enabled in the debugger UI rather than appearing on stdout. |
| cen64 | yes | |
| simple64 | — | Archived upstream; use gopher64, which is its successor. |

There are two incompatible readings of the IS-Viewer `put` register: libultra treats it
as a ring-buffer write cursor, libdragon as a plain "number of bytes to print". Both
gopher64 and ares sidestep this by never storing writes to that register, so the wrapper
always sees a cursor of 0 and every message is written from the start of the buffer. The
result is correct under either interpretation. A flashcart or emulator that *does* store
the cursor would replay earlier text on each report.

## Debugging reports with gdb

A report tells you what happened; a breakpoint on the handler tells you where. The
gcc build carries DWARF (`-g` is in `GCC_CFLAGS`, and
[mods/dkr.custom.ld](../mods/dkr.custom.ld) keeps the `.debug_*` sections from the
`/DISCARD/` rule), so gdb resolves file, line and locals in the instrumented frame.

In ares: **Settings → System → Debugging → GDB debugging**, plus **IPv4 mode** to listen
on `127.0.0.1:9123` rather than `[::1]:9123`. The status bar shows `GDB Listening ...`.

```sh
gdb-multiarch build/dkr.us.v77.elf
(gdb) target remote 127.0.0.1:9123
(gdb) rbreak ^__ubsan_handle_      # or a single one, e.g. __ubsan_handle_out_of_bounds
(gdb) continue
```

gcc calls the plain handler normally and the `_abort` variant under `SANITIZE_ABORT=1`.
At the breakpoint, `$a0` is the check's `...Data *` (whose first member is always a
`SourceLocation`: filename pointer, line, column) and the remaining argument registers
are the offending values. `frame 1` is the code that tripped the check.

Don't combine this with `SANITIZE_ABORT=1` — that halts in `N64Wrapper_Assert` *after*
the report, one frame further from the bug. `SANITIZE_ABORT` is for when no debugger is
attached.

### From a devcontainer

ares' GDB server binds loopback only, so a container on Docker's default bridge network
cannot reach it — `host.docker.internal` does not help. Put the container in the host's
network namespace instead, in your `devcontainer.json`:

```json
"runArgs": ["--network=host"]
```

Then `127.0.0.1:9123` inside the container is the host's ares. The container also needs
`gdb-multiarch` (`apt-get install -y gdb-multiarch`); the mips toolchain's binutils do
not include a debugger.

If host networking isn't an option, bridge the port on the host instead and point gdb at
the gateway address:

```sh
socat TCP-LISTEN:9124,fork,reuseaddr,bind=0.0.0.0 TCP:127.0.0.1:9123
```

[.vscode/launch.json](../.vscode/launch.json) has two `cppdbg` configurations for this:
a plain attach, and one that sets the `rbreak` above on connect.

## Size

Instrumentation is expensive. Measured on `us.v77` with all checks enabled:

| Build | `.main` text | `.main` data |
|---|---|---|
| `COMPILER=gcc` | 0x0ABE00 | 0x00A840 |
| `+ SANITIZE=all SANITIZE_FILES="src/racer.c src/camera.c"` | 0x0F1860 | 0x02E5B0 |
| `+ SANITIZE=all` (whole game) | 0x2B1A80 | 0x10E8D0 |

The data growth is the per-check type descriptors and source location strings. A
whole-game build grows `.main` to roughly 4 MB, which will not leave a usable heap on a
stock console and can crash during boot or lose entities. **Always narrow the build with
`SANITIZE_FILES` to the handful of files you're actually investigating**, and prefer a
single check (e.g. `SANITIZE=shift`) over `all` when you can.

## Known limitations

- `-fwrapv` is part of DKR's normal gcc flags, and it defines away signed overflow, which
  would silence `-fsanitize=signed-integer-overflow` completely. When that check is
  requested the build drops `-fwrapv` from the instrumented files only (and says so).
  This changes codegen for those files, so an overflow bug may present differently than
  in a normal gcc build.
- The runtime formats reports with DKR's own `vsprintf`, which doesn't understand the
  `%z` length modifier. The two `%zx` uses in the runtime's internal `CHECK` failure path
  will print garbage; ordinary UB reports are unaffected.
- Reports are emitted from whatever thread hit the check, using ~600 bytes of its stack
  plus whatever `vsprintf` needs. A check firing on a thread with a very small stack can
  turn a report into a crash.
- `src/n64san/dkr_n64_wrapper.c` is deliberately never instrumented, and it holds a
  reentrancy guard: UB detected while a report is being formatted is dropped rather than
  recursing.
- The sanitizer runtime is linked from `tools/n64san/libn64_ubsan.a`, which
  [mods/dkr.custom.ld](../mods/dkr.custom.ld) places in the `.main` segment via
  `*libn64_*.a:*(...)` patterns. That script ends in `/DISCARD/ : { *(*); }`, so any new
  section the runtime starts emitting would be silently dropped rather than erroring.
