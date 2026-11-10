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

### 3D colour (2026-11-10)

In-game 3D first came out solid green, then green-tinted. Three renderer fixes, each found from frame dumps:
- **10-bit EDRAM aliasing.** The scene renders as 2_10_10_10_FLOAT (7e3, host half floats) and the game rebinds the same EDRAM base as 2_10_10_10 / 10_10_10_10 to resolve the raw bits; a post shader decodes the 7e3 itself. `sync_color_alias()` carries the newest contents between those formats (re-encoding half floats to 7e3 bits and back, Xenia's conversion).
- **Colour grading table.** The tone-map pass ends in a lookup into a 16x16x16 table that the game draws as 16 slices of 16x16 (each padded to 32 rows) and samples with a 3D fetch (fetch constant: 2D, stacked). The translator had treated it as 2D and ignored blue. Shader 3D fetches of resolved surfaces now blend the two nearest slices (`tfp.w` depth, `tfi.z` slice stride).
- **Small targets above render scale 1.** The table's shader works from interpolated coordinates per guest pixel; at scale 2 host pixel centres sit a quarter guest pixel off, so the first row of each slice took the previous slice's padding (green = 255) and black came out bright green. For targets 256 pixels wide or less: the viewport uses a guest half pixel, the vertex nudge for upscaling is off, and interpolators and the pixel position are moved to the guest pixel's centre (`vpW.z`, along `ddx_fine`/`ddy_fine`).
- Also: 16_16 and 16_16_16_16 targets hold -32..32 as signed 16-bit on the console. Pixel output to them is scaled to the unorm an unsigned fetch of those bits reads (value / 64); the game fetches them unsigned with no exponent bias. `ASURA_NOFIXED16` turns this off. Not checked against hardware.

Checked with scripted input (episode 1 opening, logs\auto9): space, planet, explosions, the title card and the HUD in natural colours. Still seen in earlier dumps: green/black vertical streaks at the right edge of the scene (in the scene buffer, before post-processing; not yet looked at). The frame dumper writes 10-bit and float surfaces as raw bytes, so decode them by hand.

Scripted input: `ASURA_AUTOINPUT="S.....A.....A.....A.....A.....A.....A.....|U.A....b..D.A..U.A........"` with `ASURA_AUTOINPUTMS=1000` reaches the opening of episode 1 from the completed save, whatever state the "Start episode?" dialog is in. `ASURA_CMDLINE=BC-S000` (map name) is ignored; the front end loads anyway.

### Input (2026-11-09)

The pad works (XInput slot 0 is user 0). The main menu had stopped responding because the game opens a system message box ("saved data does not exist") and then ignores input until it hears the system UI close: every XamShow*UI now sends XN_SYS_UI 1 then 0 to the listeners. After that: the save is created (`saves\SAVEDATA.DAT`), all five DLC packs mount (DLC0-DLC4) and the Episode Menu works. Seen: a checkered garbage band along the top of the Episode Menu (a rendering bug, not yet looked at).

### Testing aids and saves (2026-11-09)

- `ASURA_SKIPINTRO`: the autosave notice and the four logo movies are served as one-frame copies made from the originals (`cache\intro\`, built by the host on first use; `tools\bink1frame.py` does the same by hand). A missing or empty movie is a fatal "I/O failure" in this game, so they must stay valid Bink files.
- The user's completed save: `C:\dev\asura\SAVEDATA.DAT` is a console `CON` package; its one file (`SAVEDATA.DAT`, 50,193 bytes) is installed as `saves\SAVEDATA.DAT\SAVEDATA.DAT` and the game accepts it (the Episode Menu opens on Part IV, Episode 19). The game's own fresh save is kept in `saves_backup\`.

### Episode Menu ring (open)

The user reports the portrait ring "revolving around a fixed point instead of rotating around the portrait"; captures show the ring and the portrait apart, with a white disc where the portrait should be. Ruled out so far (frame dumps at the Episode Menu, `ASURA_DRAWPRINTFULL=4213EBD96F9DECE3`): the Scaleform vertex shader translation (its odd swizzles compose to a plain 2D transform), the vertex position decoding (16_16, high half is x), and the matrices themselves (axis-aligned scales with 16:9 aspect; the rotated element is a proper rotation about its own origin). So the placement comes from the game's CPU-side UI logic. Also ruled out: vertex shader ABC6D461 (a real constant-output shader, its draws are empty on hardware too), `mfocrf` (XenonRecomp hard-codes cr6, but this game only uses cr6), the unaligned vector loads (`lvlx`/`lvrx`, standard Unleashed versions), the offscreen passes (the 328x184 resolves come out black but don't move anything).

Reference (user's Xenia shots): the ring, the petal frame and the portrait share one centre at about (1100, 360), and five episode rows with thumbnails show. Here, on the episode detail screen (frame dump, `logs\run38.log`): ring pieces at (1079.5, 371.5) except one piece exactly (-128, -128) px off, the two big petal-frame shapes (2298 and 1146 vertices) at (1172, 72) and (1173, 4), the portrait and a white backing disc elsewhere. A clean 128 px offset on some clips points at wrong data in some display-list matrices (CPU side), not at the renderer. Next: find the Scaleform code that composes a clip's world matrix and watch the -128 clip's matrix being written (hardware watchpoint on its guest address), then check the instructions on that path, starting with the vector ones this project or BCR2 added to XenonRecomp (vpkd3d float16_2, vctuxs, vpkuwus/vpkuhus, ...). Also seen: a checkered band along the top of the Episode Menu, and only the selected episode row drawn.

### Sound (2026-11-09)

Working, confirmed by ear by the user: title music (speed, pitch) and menu sound effects. `audio_out.cpp` is BCR2's WASAPI output (48 kHz stereo, 5.1 folded down, the device's buffer paces the game's audio callback). `xma.cpp` emulates the XMA decoder with FFmpeg's XMA2 decoder (`thirdparty\ffmpeg`: BtbN ffmpeg-n8.1-latest-win64-lgpl-shared-8.1, 80,304,622 bytes, SHA-256 7f2cd74c2805437d7f07739fdd7bea7c9d851ee2dba9503fc8e7d60c24a57447; out of git, DLLs copied next to the exe by the build).

Things that mattered:
- XMA registers are little-endian (the game uses lwbrx/stwbrx). 0x600 is the context array's physical address, read once at start-up, so it's set before the game boots. Kick 0x650-0x659, lock 0x690-0x699, clear 0x6A0-0x6A9, one bit per context; XAudio2 locks and kicks each stream every audio frame.
- Register writes are events, so the register page (0x7FEA1000) is read-only and writes are trapped (fault, single step, apply). Lock order: the trap critical section, then the context lock. The worker must never write the page itself while it is protected (it did, and aborted the process).
- Context fields follow Xenia's XMA_CONTEXT_DATA; context updates are compare-and-swap on the big-endian words.
- Not done: Bink movie audio (none heard yet), gameplay voice and music not yet checked, `ASURA_NOXMA` turns the decoder off.

### Wanted later (from the user, 2026-11-09)

- 60 fps play, and a 24 fps mode for FreeSync/G-Sync displays, if the game's timing allows it.
- Watch mode: quick-time events and button prompts complete themselves, and the on-screen display is hidden, to watch the game as a film.

### Next

1. Input past the title screen (XInput works; keyboard and mouse are BCR2's kbm.cpp, not ported), then play into the first episode and look for rendering bugs.
2. Audio: WASAPI output from BCR2, then XMA decoding (UE3 plays its sound as XMA; BCR2 never needed it).
3. Saves and the `cache:` utility drive, then the menus with real input.
