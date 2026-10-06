# Vita3K on the PS5

Vita3K builds as a PS5 title (jailbroken console). It uses the
[ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk), with the platform layer from
[PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK), and Vulkan from RADV on a PS5 winsys
([PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) + [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa)).
The desktop and Android builds are unchanged when `VITA3K_PS5_PORT` is off.

## Building

The toolchain lives in WSL (Ubuntu 24.04): LLVM 18, CMake 3.28 or newer, Ninja, Perl, and Meson 1.4 or newer for RADV.

1. Clone `PS5_Vulkan`, `PS5_Mesa` and `PS5_PayloadSDK` side by side. Then, in `PS5_Vulkan`, run
   `tools/setup-native-dependencies.sh` and `tools/build-radv.sh release`.
   RADV's host tools also need `libllvmspirvlib-18-dev`, `libclc-18-dev` and `libclang-18-dev`.
   Then run `tools/rebuild-libc.sh`. This produces:
   - the SDK fork in `.deps/native/ps5-payload-sdk`
   - `libvulkan_radeon.ps5.a` in `.deps/native/radv-release/lib`
   - the title's `runtime/libc.prx`, and `build/runtime-shim/ps5-native-tool`, which converts and signs titles
2. Configure and build Vita3K against the SDK fork. The upstream SDK lacks `libps5platform`
   and the console's `ucontext_t` layout, so it will not work:

   ```sh
   cmake -S . -B build-ps5 -G Ninja \
       -DCMAKE_TOOLCHAIN_FILE=cmake/ps5-payload.cmake \
       -DPS5_PAYLOAD_SDK=<PS5_Vulkan>/.deps/native/ps5-payload-sdk \
       -DCMAKE_BUILD_TYPE=Release -DUSE_LTO=NEVER
   ninja -C build-ps5
   ```

   The build links `bin/Vita3K` with PS5_Vulkan's title recipe (`cmake/ps5-title.cmake` reads
   `tools/radv-link.sh`). It converts and signs the result, then assembles the title in
   `build-ps5/title/PPSA99300/`: `eboot.bin`, `sce_sys/param.json`, `sce_module/libc.prx`, `data/` and
   `shaders-builtin/`. `PS5_VULKAN_ROOT` defaults to the checkout that holds the SDK.

## Running

1. Put the title folder where the installer scans, such as `/mnt/usb0/PPSA99300/` for ShadowMountPlus or
   `/data/homebrew/PPSA99300/` (for example with `PS5_Vulkan/tools/deploy-title-folder.py`).
   ShadowMountPlus registers a title only when it has `sce_sys/icon0.png`.
   The console's ftpsrv serves SELFs decrypted, so an `eboot.bin` read back over FTP is the unpacked ELF, not the signed file.
2. Install a Vita app or game into `/data/Vita3K/vita/ux0/`, or install firmware and packages with a desktop
   Vita3K and copy its file system over.
3. Write the app's title ID in `/data/Vita3K/boot.txt`.
4. Launch the title from the jailbreak's homebrew launcher.
5. Follow the log with `PS5_Vulkan/tools/ps5_console.py klog`. Lines are prefixed `[Vita3K]`.

Under the toolchain, the dependencies resolve like this:

| Dependency | Source on PS5 |
| --- | --- |
| Boost | cross-built with b2 (`clang-ps5` toolset, `target-os=freebsd`) |
| OpenSSL | 4.0.1 cross-built from source (`BSD-x86_64`, static, no secure memory) |
| FFmpeg | 7.1.1 cross-built from source with ffmpeg-core's component set (`cmake/ps5-ffmpeg.cmake`). ffmpeg-core's "freebsd" prebuilt is built on Linux and imports glibc |
| SDL3 | `SDL_UNIX_CONSOLE_BUILD`, no X11, Wayland, HIDAPI or OSS |
| curl | built from source against the OpenSSL above |
| Qt, nativefiledialog, Discord | not built |

## Runtime layout

- `/app0`: static assets (`data/`, `shaders-builtin/`), packaged with the title.
- `/data/Vita3K`: everything writable:
  - `vita/`: the emulated file system
  - `config/`, `logs/`, `cache/`, `patch/`, `shared/`
- `/data/Vita3K/boot.txt`: the title ID to boot. A title gets no command line.
- The log goes to `logs/` and, through `ps5_klog_capture_stderr`, to klog.

## Code map

- `vita3k/ps5/main.cpp`: the entry point. It follows the Android boot flow without a UI
  (`AppSessionController`, plus a frame host that presents to VideoOut).
- `vita3k/platform`: the PS5 and desktop runtimes (directories, klog capture).
- `renderer::Ps5DisplayHandle`: the renderer seeds the vulkan.hpp dispatcher from RADV's
  `vk_icdGetInstanceProcAddr` (there is no loader). It enables `VK_KHR_display` and creates a
  display-plane surface on VideoOut's first mode.
- `mem.cpp`: the access-violation handler reads `mc_err` from the console's `ucontext_t`
  (asserted through `ps5platform/context.h`) and also listens for `SIGBUS`.
- `mem/src/ps5_host_memory.cpp`: guest memory. A title cannot mmap anonymous memory, so the 4 GiB range
  is reserved at `0x10_0000_0000` and backed with direct memory in 64 KiB chunks as it is committed. Chunks
  stay backed until shutdown.
- dynarmic's code cache (`external/dynarmic`, `block_of_code.cpp`) comes from `ps5_exec_allocate`. The
  console refuses `sceKernelJitCreateSharedMemory` to titles. This is a patch to the Vita3K/dynarmic
  submodule, to be sent upstream.
- `vita3k/platform/src/ps5_libc.cpp`: `posix_fadvise`, `getresuid` and `getresgid`, which the console's
  libc declares but does not export. The title link binds the libc names to these.

## Status

- [x] RADV builds from the pinned Mesa fork.
- [x] The full tree (minus Qt) configures, compiles and links for the PS5 with the SDK's default link. The only
      undefined symbol is RADV's `vk_icdGetInstanceProcAddr`, which the title link below supplies.
- [ ] swscale maps executable memory with `mmap`, which the title sandbox refuses; check that it falls back.
- [x] The title links against RADV and is converted, signed and packaged. `ps5-native-tool self --inspect`
      reports its integrity as valid. Every one of its 438 imports comes from a system module, and it has no
      undefined weak symbols (`--no-dynamic-linker`; see PS5_Vulkan's `docs/M5_PHASE_B.md`).
- [x] Guest memory and the dynarmic code cache on direct memory. Built only, not yet run.
- [ ] Input: SDL has no PS5 joystick driver. Read the DualSense with `scePad` (PS5SX2's
      `orbis-shims` have one) and feed it to `ctrl`.
- [ ] Audio: SDL has no PS5 audio driver. Add an `sceAudioOut` backend.
- [ ] First boot on hardware. Check in klog, in order:
      1. that the log starts, and that `/data/Vita3K` gets created
      2. that config and app init succeed
      3. the Vulkan device and display (`Presenting to ...`)
      4. guest memory, then the JIT, then a homebrew app, then a commercial one

## Notes

- PS5 code uses the compiler's `__PROSPERO__` (and `__FreeBSD__` for userland differences),
  as the rest of the code base uses `__ANDROID__` and `__APPLE__`. `VITA3K_PS5_PORT` is CMake only.
- [ps5-vulkan](https://github.com/mpereiraesaa/ps5-vulkan) is a separate, bounded Vulkan 1.3
  driver. Its native build needs companion repositories that are not all public, so RADV,
  a complete Vulkan 1.4 driver, is the one used here.
