# Asura's Wrath recomp: working notes

Title 43430817, TU applied (execution info version 0x105). Unreal Engine 3 (`BCGame-Xbox360-Shipping.pe`), SDK 2.0.20871.

## Layout

| Path | Contents |
|---|---|
| `gamefiles\` | User's originals (ISO, DLC in `43430817\00000002`, title update in `Cache`). Read only. |
| `private\default.xex` | From the ISO (`tools\xdvdfs.py`) |
| `private\default.xexp` | From the TU package (`tools\stfs.py`) |
| `private\default_patched.xex` | TU applied, decrypted, uncompressed (made by XenonRecomp) |
| `private\image.bin` | Flat memory image at 0x82000000 (`tools\xexinfo.py`) |
| `private\imports*.txt` | Import records; `imports_named.txt` marks each as impl / bcr2-stub / NEW against the BCR2 host |
| `game\disc\` | Disc data (`BCGame\CookedXbox360`, `BCGame\Movies`, ...), 6.1 GB |
| `game\dlc\<package>\` | The five DLC packages unpacked (data only, no code) |
| `config\asura.toml` | XenonRecomp config |
| `config\switch_tables.toml` | 101 jump tables from `tools\jumptables.py` |
| `ppc\` | Generated C++ (208 files, 52,376 functions) |
| `recomp_check\` | Compile check: `build.cmd` builds `asura_ppc.lib` |

DLC packages: 78E95E38 = Episode Pack: Part IV (DLCBIGEXPAND), 8A6811EC = Episode 11.5 (DLC120), 981ED336 = Lost Episode 1 (DLCCOLLABO01), E2AFB00E = Episode 15.5 (DLC170), F56F8734 = Lost Episode 2 (DLCCOLLABO02).

## Regenerating

```
cd C:\dev\asura\config
C:\dev\XenonRecomp\out\XenonRecomp\XenonRecomp.exe asura.toml C:\dev\XenonRecomp\XenonUtils\ppc_context.h
```

Python is `C:\Users\deuce\AppData\Local\Programs\Python\Python313\python.exe` (run tools with `-I`).

## Image

- Code: `.text` 82220000-82E3D2D4, `BINK` 82E3D400-82E50868, `RADCODE` 82E50A00-82E514AC. Entry 82B75160.
- Register helpers: savegprlr_14 82B58EE0, restgprlr_14 82B58F30, savefpr_14 82B597A0, restfpr_14 82B597EC, savevmx_14 82B598A0, restvmx_14 82B59B38, savevmx_64 82B59934, restvmx_64 82B59BCC.
- 37,915 `.pdata` entries; TLS has 64 slots, no TLS data. Default stack 256 KB.

## Things found

- XenonAnalyse finds no jump tables: this compiler orders `lis, rlwinm, addi, lwzx`. `tools\jumptables.py` tracks registers instead: 74 absolute, 13 byte-offset, 14 halfword-offset tables (two inline tables with no bound check nearby, sized by reading entries).
- 17 `lbz`-indexed `bctr`s go through tables at 8323A9E8..8323E9E8 in `.data`: filled at run time (UnrealScript `GNatives`-style dispatch). They must stay indirect calls.
- SEH: 48 `__try` functions have an 8-byte header (handler 82E3C8C4 + scope table) in front of their `.pdata` start. XenonRecomp's gap scan decoded it as code and split the functions, which broke branches from `__except` funclets back into their parents. Skipped through `invalid_instructions`. Exceptions themselves are still not supported; the funclets are dead code unless something raises.
- Added to XenonRecomp (shared with BCR2, additive): vpkuhus(128), vpkuwus(128), vctuxs/vcfpuxws128, vsubshs, stbux, dcbst, mulhd, mulhdu, vpkd3d128 type 3 (float16_2).

## Imports

288 functions + 13 variables. Against the BCR2 host: 134 implemented, 111 imported by BCR2 but stubbed there, 43 new (mutants, NtSuspendThread, NtWriteFileGather, XexLoadImage/XexGetModuleSection, XAudio ducker, XNet QoS, XMsg async IO, launch data, more Xam UI). Static libs include XAUDIO2, XMP, D3D9LTCG, XHV2, Bink.

## Host (started 2026-11-09)

`host\` is a new runtime written for this game, porting the kernel logic of the BCR2 host (`C:\dev\bcr2\host\imports.cpp`) without its game-specific debugging. Build with `host\build.cmd` (clang-cl + Ninja, all of `ppc\` compiled into `asura.exe`, about 2 minutes on 24 cores from clean, seconds for host-only changes). It loads `private\image.bin` and `private\default_patched.xex` and the unpacked game from `game\` (folder found next to or above the exe, or `ASURA_ROOT`).

| File | Contents |
|---|---|
| `main.cpp` | guest memory (4 GB, physical windows aliased), image and function table, imports, XEX header and loader entry, crash report |
| `kernel.cpp` | imports, memory arenas, dispatcher objects and waits, APCs, threads, TLS, time, XEX module calls, thread monitor |
| `kfile.cpp` / `kstring.cpp` | file system (disc, DLC, saves, symbolic links) / strings and the printf family |
| `xam.cpp` | input (XInput, `ASURA_AUTOINPUT`), profile, content (saves, DLC), enumerators, notifications, UI stubs, XMsg, net stubs |
| `gpu.cpp` | command processor: Vd* calls, ring, PM4 interpreter (fences, memory writes, waits, interrupts, swaps), 60 Hz vblank; hands registers and packets to the renderer |
| `xenos.cpp` | the D3D11 Xenos renderer, copied from BCR2 by `tools\port_renderer.py` (switches renamed `ASURA_*`, guest reads through `gpu_phys_to_host`); keep it close to BCR2's so they can merge |
| `xenos_glue.cpp`, `gpu.h`, `kbm.h`, `paths.h`, `shaderpack.*` | what the renderer expects from its host (BCR2's settings, keyboard and data paths, stubbed or mapped) |
| `audio.cpp` | null XAudio driver (client called every 5.33 ms, output discarded), XMA contexts allocated but not decoded |
| `hooks.cpp` | overrides of game functions (the UE3 fatal-error handler is logged) |

Run: `host\build\asura.exe --log` (log in `logs\`; `ASURA_LOGFILE` names another file). Debug switches: `ASURA_STRICT` (stop at the first unimplemented import), `ASURA_WATCH=addr,...` (words printed by the 10 s thread monitor), `ASURA_CRASHDUMP=addr,len`, `ASURA_AUTOINPUT`, `ASURA_CMDLINE` (UE3 command line), `ASURA_NOAUDIO`.

State at the end of the first host session (null GPU): boots through the logos (Capcom, CyberConnect2, UE3, SpeedTree) to the title screen's background movie (`TitleMenu_Bg00.bik`) and runs at a steady 30 frames per second, with nothing drawn (null GPU) and no sound.

### Bugs found while booting (each was a hang or crash)

- Address ranges carry meaning: UE3's allocator sends pointers below 0x40000000 and from 0xE0000000 to its large-block path. 4K-page virtual memory comes from 0x00100000-0x3FFFFFFF, 64K pages (MEM_LARGE_PAGES) from 0x40000000; physical blocks are returned in the window their page size implies.
- `NtQueryVirtualMemory` must end the region at the allocation's end. Windows merges neighbouring committed blocks, and UE3's realloc grew a block in place over its neighbour (a corrupted string map).
- `memset` (0x82B594C0) was cut short at its first `bdzlr` by XenonRecomp, as in BCR2: fixed with a whole-function entry in `config\asura.toml`. `tools\trunc.py` finds such functions (strip the `+`/`-` branch hints).
- The 0xE0000000 window is 0x1000 ahead: virtual E0000000+x is physical x+0x1000, and the GPU writes D3D's fence (read at E0012000) to physical 0x13000. Views can't be shifted by 4K, so E0 blocks get a spare page and the GPU translates their physical pages through a per-page table (`gpu_phys_to_host`). `MmGetPhysicalAddress` adds the 0x1000.
- I/O completion routines must queue as APCs and run at the thread's next alertable wait: XAPI's `ReadFileEx` path does `SleepEx(INFINITE, TRUE)` and waits for it. `KeDelayExecutionThread` with INT64_MIN is forever.
- `XMsgInProcessCall` results are HRESULTs: a positive Win32 code reads as success. Unknown messages (app 0xFC, 0x58004/0x58020/0x58046) now fail with 0x80070490; returning 0x490 made the game allocate an unwritten size (an "OOM 1074833184 bytes" fatal error).
- UE3's fatal errors end in `Sleep(INFINITE)`; the message goes nowhere unless `sub_8229BB28` is hooked (it is).
- `VdInitializeScalerCommandBuffer` takes its destination as the 11th argument (r1+0x64), with a dword limit at r1+0x6C.
- File information class 26 (sector of the file) is asked for every package; answered with a stable per-path number.
- Drives seen: `game:`/`D:` (disc), `UPDATE:` (title update data: empty here, `game\update`), `cache:` (utility drive, not mounted yet).

### Renderer (2026-11-09)

BCR2's renderer runs unchanged apart from the port script's edits. The boot sequence and the title screen ("PRESS START" over the background movie) draw correctly at 30 fps, 140-240 draws per frame. Frame captures: `ASURA_SHOTDIR=<dir>` with `ASURA_SHOTEVERY=<frames>`; `tools\bmp2png.py` makes PNGs or a contact sheet. Not yet checked: anything past the title screen, BCR2's precompiled-shader bundle (Asura's shaders compile on first use), VRR and display pacing (the 60 Hz tick here is free-running).

Fixed on the way:
- The swap packet must carry the front buffer's physical address (E0 window +0x1000): with the virtual one the present found no surface (flat purple frames).
- XAPI's heap: `MEM_RESERVE` must not commit, `NtQueryVirtualMemory` must report the run of pages in one state (not the whole allocation), and commits in a 64K-page range cover whole 64K pages. Each was an intermittent heap crash on a worker thread.
- WAIT_REG_MEM on memory now waits (up to 3 s), EVENT_WRITE_EXT is packet 0x5A with full-screen extents, and occlusion queries are answered by the renderer, as in BCR2.

### Next

1. Input past the title screen (XInput works; keyboard and mouse are BCR2's kbm.cpp, not ported), then play into the first episode and look for rendering bugs.
2. Audio: WASAPI output from BCR2, then XMA decoding (UE3 plays its sound as XMA; BCR2 never needed it).
3. Saves and the `cache:` utility drive, then the menus with real input.
