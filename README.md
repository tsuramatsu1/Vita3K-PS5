# Vita3K for PS5

A port of the [Vita3K](https://github.com/Vita3K/Vita3K) PlayStation Vita emulator to the jailbroken
PlayStation 5, running as a signed title (`PPSA99300`) with its own console front end.

This is a fork. Everything the emulator does — CPU, GXM, modules, the Vulkan renderer — is upstream Vita3K's
work. What is added here is the platform layer that lets it run inside a PS5 title sandbox, and a controller-driven
interface to replace the desktop one.

> **Status: experimental.** Games boot, run and are playable, but several render incorrectly. See
> [Known problems](#known-problems) before you set your expectations. The desktop and Android builds are untouched:
> nothing in this fork changes them.

## What the port adds

**A console front end.** Upstream's desktop interface assumes a mouse and a window manager, so it is replaced by
one built for a controller on a television:

- A game list laid out like the Vita's home screen — glossy bubbles in rows of three, four and three, ten to a page.
- An installer that browses the file system, installs `.vpk`, `.zip`, `.pkg` and firmware `.pup`, and shows overall
  and per-file progress. It can offer a choice of destination, but in practice there is only ever one — see
  [Known problems](#known-problems).
- Game deletion, a settings screen, a splash screen and the Vita's launch chime.

**A platform layer** (`vita3k/platform/`) for the things the console does not provide the way a desktop does:

| Area | What the console needs |
| --- | --- |
| Controller | SDL has no driver for the DualSense here, so `scePad` is read directly |
| Audio | SDL's backends reach no hardware; `sceAudioOut` is used instead |
| File removal | Boost's `remove_all` is refused in the sandbox; a direct `unlinkat` walk replaces it |
| Memory | The kernel protects memory in 16 KiB granules while `sysconf` reports 4096 |
| Power | `sceSystemServicePowerTick`, so a long install is not cut short by the idle timer |

**Controls.** The DualSense stands in for a Vita that has a touchscreen and no second trigger:

| Input | Does |
| --- | --- |
| Touchpad, dragged | Moves an on-screen pointer, drawn over the game |
| Touchpad, clicked | Taps the Vita's screen wherever the pointer is |
| R2 | SELECT — the Create button never reaches a title, and the touchpad click is taken |
| L3 + R3, held | Returns to the game list, rather than to the PS5's home screen |

## Building

Builds from WSL or Linux. You need LLVM 18, CMake 3.28+, Ninja, Perl, and Meson 1.4+ for RADV.

Vulkan comes from RADV on a PS5 winsys, so that has to be built first. Clone
[PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan), [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa) and
[PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK) side by side, then in `PS5_Vulkan`:

```sh
tools/setup-native-dependencies.sh
tools/build-radv.sh release      # also needs libllvmspirvlib-18-dev libclc-18-dev libclang-18-dev
tools/rebuild-libc.sh
```

Then build the emulator against **that** SDK fork — the upstream ps5-payload-dev SDK lacks `libps5platform` and the
console's `ucontext_t` layout, and will not work:

```sh
cmake -S . -B build-ps5 -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=cmake/ps5-payload.cmake \
    -DPS5_PAYLOAD_SDK=<PS5_Vulkan>/.deps/native/ps5-payload-sdk \
    -DCMAKE_BUILD_TYPE=Release -DUSE_LTO=NEVER
ninja -C build-ps5
```

The result is a complete title in `build-ps5/title/PPSA99300/`, already converted and signed.

[`docs/ps5-porting-plan.md`](./docs/ps5-porting-plan.md) covers how each dependency is cross-built and how the
runtime is laid out.

## Running

### Requirements

The **PS5SX2 Helper**, from the releases of [PS5SX2](https://github.com/Swordpdf/PS5SX2), loaded along with kstuff.
A title starts in a sandbox that can see only its own mounts, so Vita3K asks to be let out and waits for the Helper
to act on it. Without it Vita3K still runs, but its files go to `/download0/Vita3K` inside the title's own sandbox,
where nothing else on the console can reach them — not even FTP — so anything you install is effectively invisible.

[ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) to register and launch the title.

### Steps

1. Copy `build-ps5/title/PPSA99300/` to where ShadowMountPlus scans — `/mnt/usb0/PPSA99300/` or
   `/data/homebrew/PPSA99300/`. It only registers a title that has `sce_sys/icon0.png`.
2. Add `PPSA99300` to `/data/whitelist.txt`, or the Helper will not answer Vita3K's request.
3. Load kstuff and the PS5SX2 Helper, then launch Vita3K from the home screen.
4. Install the Vita firmware from a `.pup`, then your games, using the installer in the front end. A game folder
   copied straight into `/data/Vita3K/vita/ux0/app/` is picked up without installing.

Commercial games need their licence (`work.bin`) installed alongside them, exactly as on desktop Vita3K. Without it
the game fails to boot and the log says `No klic provided for encrypted App`.

The log is at `/data/Vita3K/logs/vita3k.log`, with the previous run kept beside it as `vita3k.log.previous` —
worth knowing, because returning to the game list restarts the title and would otherwise overwrite the log you
wanted to read.

## Known problems

- **Some games render incorrectly.** Uncharted: Golden Abyss draws most of the frame correctly but with large black
  rectangles and bands of corruption. Under investigation; several fixes are in and have not resolved it.
- **"Rendering: accurate" renders nothing.** The setting switches to shader interlock, which produces a blank screen
  on RADV while the game keeps running and playing audio.
- **"Memory mapping: disabled" crashes.** Leave it on `double buffer`.
- **USB drives are invisible to the running title.** The jailbreak grafts `/data` into the sandbox but does not
  repoint the process root, so `/mnt/usb0` cannot be reached from inside. This is also why the installer never
  actually asks where to put a game: the only destination it can find is console storage, so it stops offering a
  choice and everything lands in `ux0`. The code for choosing is there and will work once a drive can be reached.
- **Installing is slower than on a PC.** Small writes to `/data` are expensive, and a package install writes the
  game twice — once unpacked, once decrypted.

## Settings

The settings screen exposes the levers that matter for diagnosing a misbehaving game. Each takes effect the next
time a game is launched:

| Setting | Why you would change it |
| --- | --- |
| Rendering | `accurate` drops two render-target shortcuts — currently broken, see above |
| Surface write-back | Copies what the GPU drew back into the game's memory; a game that reads its render targets needs it |
| Memory mapping | How the game's memory reaches the GPU |
| Shader compilation | Background compiling leaves not-yet-compiled geometry undrawn for a moment |
| Stick sensitivity | Raise it when a character walks where it should run |
| Touchpad acts as | Which of the Vita's two touch panels the touchpad stands in for |

## License

GPLv2, the same as upstream Vita3K.

## Thanks

To the Vita3K team for the emulator, and to mihawk-99 for PS5_Vulkan, PS5_Mesa and PS5_PayloadSDK, without which
there would be no Vulkan and no toolchain on the console.

To [Swordpdf](https://github.com/Swordpdf) for [PS5SX2](https://github.com/Swordpdf/PS5SX2) and its Helper payload,
which is what lets a title reach anything outside its own sandbox, and to
[drakmor](https://github.com/drakmor) for [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus).

## Note

The purpose of this emulator is not to enable illegal activity. You can dump games from a Vita with
[NoNpDrm](https://github.com/TheOfficialFloW/NoNpDrm) or
[FAGDec](https://github.com/CelesteBlue-dev/PSVita-RE-tools/tree/master/FAGDec/build), and get homebrew from
[VitaDB](https://www.rinnegatamante.eu/vitadb/#/).

PlayStation, PlayStation Vita, PlayStation 5 and PlayStation Network are registered trademarks of Sony Interactive
Entertainment Inc. This emulator is not related to or endorsed by Sony, nor derived from confidential materials
belonging to Sony.
