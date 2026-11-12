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
- **Exponent biases and resolve conversion (dark, blurry picture; "missing" objects).** The post chain draws into a 16_16_16_16 target with colour exponent bias +5 (RB_COLOR_INFO bits 20-25), then copies it out twice: as 16_16_16_16_FLOAT (format 32, number 7), read back as format 29 (_EXPAND = 16-bit float, as in Xenia), and as 16_16_16_16 unorm (number 0) with copy exponent bias -5 (RB_COPY_DEST_INFO bits 16-21). The tone-map pass blends the sharp scene with that buffer using its alpha as the depth-of-field factor. The resolve used to copy host bits unchanged and neither bias was applied, so the factor came out about 1/64: nearly every pixel showed the dim blurred copy, which looked like a dark picture with objects missing. Now: the pixel shader applies the target's exponent bias, 16_16(_16_16) targets are half floats clamped to -32..32, resolves to the 16-bit formats (24-26, 30-32) make a surface of the destination's format and number type after scaling by the copy bias, and `texfix` decodes 16-bit data by what the surface holds (`tfi.w`: 1 unorm, 2 snorm, 3 float). Switches: `ASURA_NOEXPBIAS`, `ASURA_NORESOLVECONV`. Checked: logs\auto14 (before) and auto16 (after), frame 4500 of the shooting segment.
- Tone map (ps 0C21C8EC72734488) for reference: decodes the 7e3 scene itself as 1/8 of the standard value (c511.w = 8), lerps toward the blurred buffer (read x4) by its alpha, adds bloom (322x182, x4, weighted by c263), then pow(1/2.2) (c508.y) into the 16^3 colour table (identity on the grey axis in the dumps).

Checked with scripted input (episode 1 opening, logs\auto9, auto12): space, planet, explosions, the title card and the HUD in natural colours. The scripted loop stops at the first LS/RS prompt, which the game shows over a dimmed scene. The "green/black streaks at the right edge" from the pilot3 dumps don't appear in any screenshot or in the auto11/auto12 scene buffers; they were most likely from decoding the old raw dumps by hand. The frame dumper now decodes each format (half floats, 10-bit, 16-bit, two-channel) to a 0..1 picture. Known dumper quirk: the 8_8_8_8 final frame (334000) comes out with red and blue swapped.

Scripted input steps added for episode 1's prompts: `W` both triggers, `K` both sticks pushed down. Scripted input: `ASURA_AUTOINPUT="S.....A.....A.....A.....A.....A.....A.....|U.A....b..D.A..U.A..W.K.W.K."` with `ASURA_AUTOINPUTMS=1000` reaches the opening of episode 1 from the completed save, whatever state the "Start episode?" dialog is in. `ASURA_CMDLINE=BC-S000` (map name) is ignored; the front end loads anyway.

### Objects culled as occluded: E0 window alias (fixed 2026-11-10)

Missing geometry (distant targets, the Gohma popping out, distant cutscene characters, Asura in places, the planet's corruption layer) was UE3's occlusion culling. Found by tracing the decision: sub_8273D208 (per object, per view: distance limits, all infinite here), frustum test sub_822C3450, then sub_82748850 (occlusion history) which asks D3D's query GetData (sub_8299FAF8 via sub_828D1480) for the sample count; zero = occluded. GetData sums, per sample block, ZPass at +16/+20 minus the begin report at +48/+52, but it reaches the block through the C0000000 alias computed the console way: an E0000000 address x becomes C0000000 + x + 0x1000 (the E0 window is 0x1000 ahead of physical on the console). The host maps all windows at the same section offset (64K view alignment), so that read landed one page past the block, on zeros, and every occlusion-tested object was culled. `tools\patch_e0_alias.sh` (run after regenerating ppc\) turns the +0x1000 off at the 9 places where that conversion feeds a C0 pointer; the same idiom feeding GPU addresses (command buffers) is left alone. User check after the fix: "all the geometry is rendering".

Tools that found it (hooks.cpp, env-gated): `ASURA_CULLDUMP`, `ASURA_CULLWATCH` (write-protects a table and logs who sets an entry), `ASURA_DISTDUMP` (per-object distance, frustum, occlusion and GetData results); the profiling build (`cmake -DASURA_FUNCPROF=ON` in host\build_prof, `ASURA_FUNCPROF="first,last"` logs per-frame call counts of every game function; copy host\build\shadercache to build_prof first) to find functions whose counts change when an object disappears.

### Distant objects not drawn (fixed, see above; kept for the trail)

Models far from the camera are not drawn: shooting-segment targets, characters in long cutscene shots (appear after a cut to a closer shot), the Gohma in front of the flagship (frame ~6816 of the pilot7 replay), the corruption layer on the planet. Traced (logs\replay4): the Gohma's draws (vs 42F7DBF5, 1F5B1899, 1B63BD5A, 14617A51) are issued every frame until 6816 and not at all from 6818; no lower-LOD draw replaces them. So the game drops them itself. Not occlusion queries: every EVENT_WRITE_ZPD end report in those frames gets the fixed 1000 samples (`[zpd]` lines with a trace on, logs\replay6). Next: the game's CPU-side distance / draw-distance / LOD screen-size test, and the VMX instructions it uses (the Episode Menu's -128 matrix offset may be the same kind of bug).

Ruled out since (2026-11-10, later): VMX translations used in culling (vmsum3/4fp128, vcmpbfp with CR6, the record-form compares, unpack/pack, conversions, rounding, vsldoi, vupkd3d types 0 and 1 are the only ones used), scalar FP (fmadd family operand order and signs, fsel, conversions, estimates), the video mode (1280x720), physical allocation failures (none), host-replaced game functions (only the fatal-error hook), occlusion query byte order (`ASURA_OCCLUSIONBE` experiment: no change), PM4 predication (`ASURA_PREDLOG=1` counts draws predicated out: none), and draws dropped by the renderer (every draw is logged first). The Gohma is at full detail (constant index counts) until it vanishes; no lower LOD replaces it. Asura also fades out in the opening cinema (replay frames 2564 to 2570, translucent first) and is missing inside his flame aura in the shooting segment, close to the camera; a character pops out at pilot7 frame 8518. Separately, one area keeps the previous frames' image (nothing new drawn there): the "window" in one particular background, pilot7 frames 8250 to 8310 (the user: not a whole-screen issue). Lower priority than the culling.

### Broken small textures (fixed for explosion sprites, 2026-11-10)

Tiled textures whose fetch pitch is at least twice their width (64x32 DXT1 with pitch 128, 32x32 with pitch 128) hold the image at block (width, height) of the tile: decoding a raw tile (`ASURA_TEXDUMPMIPS` also saves `raw_*.bin` for 2:1 textures; `asura.exe --decode-dxt1 <file> <w> <h> <pitch> <tiled>`) showed a 64x32 image at texel (64, 32) and its small mips at the top left. get_texture now reads the base level from there (`ASURA_NOSUBRECT` turns it off). Explosion puffs and Gohma swarm glows draw correctly with it. Not derived from a known rule (Xenia's packing rule doesn't place it there), so other sizes may still be off. Ruled out on the way: stale E0 page marks (no texture is on an E0 page; `ASURA_TEXE0LOG`), mip addresses (always right after the base level). `ASURA_PHYSWATCH=<physical>` logs writes to a physical page through any window.

Distant ships glassy white, fading as they came closer: fixed by memexport (next section).

Update after memexport: every texture this rule touches now has its image at the origin and nothing at (width, height) (`ASURA_SUBRECTLOG=1` logs each with both counts), so the raw tiles above were textures caught mid-move. Always reading from (w, h) had removed Asura's flame aura in the shooting segment; the offset is now used only when (w, h) holds more data than the origin. Flames and round explosion puffs both check out (logs\flame3).

### Vertex streams split across ring buffers (fixed 2026-11-10)

One-frame flicker on faces, the Gohma torso and ships (wrong surface on the right shape, different frames every run): vb_alloc moves to the next of the vertex ring buffers when one fills, and a draw's streams were allocated one at a time. When the move fell between two streams, the draw bound the new buffer and read its earlier stream at the old offset from it. issue_draw now reserves room for all of a draw's streams first (vb_reserve). User check: "Flicker is gone".

### Debug aids added late 2026-11-10

- F9 (with --log) saves every frame, 105 before the press and 45 after (`ASURA_SHOTRINGAFTER`), plus draws_<frame>.txt: each draw's shaders, vertex stream, constant hashes and pixel texture addresses with content hashes. An input replay pauses its clock while a capture is saved, and hands the pad back when the recording ends.
- `ASURA_TEXFLIP=1`: one-frame texture swaps per draw, and draws present or missing for one frame (`[texflip]`, `[drawflip]`).
- `ASURA_MONSAMPLE=1`: samples guest thread stacks every 2 ms and logs where frames over 120 ms went (`[sample]`). Critical-section waits over 100 ms log how often others took the lock meanwhile (`[cs]`).
- `[texzero]` (on with --log): textures of 64x64 and up whose base level is all zero at upload. `ASURA_TEXDUMPONLY=addr,...` limits the texture dumps.

### Auto-QTE and test runs from the completed save (2026-11-10)

- `ASURA_SAVESEED=saves_complete\SAVEDATA.DAT` copies the user's completed save (extracted from the CON package `SAVEDATA.DAT` with tools\stfs.py; the copy in `saves\` had been rewritten by the game) over the installed one before the game reads it, so every test run starts the same. Use it for all test runs.
- `replays\complete_ep1.txt`: the user's run through all of episode 1 from that save (10,445 states, 19 min). Timed by wall clock, so it drifts late in the episode. `ASURA_INPUTPLAYUNTIL=ms` stops a replay there and hands over; `ASURA_INPUTCAPTURE=ms,...` triggers F9 captures at replay times (with `ASURA_SHOTRINGNOIMG=1` only the draw list is saved, `ASURA_SHOTRINGAFTER=n` shortens the wait).
- QTE prompts are UI draws (vs 4213EBD9) whose icon textures sit at fixed addresses in the UI package: B (01466000, 01496000, 014CA000, 014DA000, 014EE000), RT (01476000, 0147A000, 01482000, 014AE000, 014C2000), LT (014A6000, 014B6000), LS (01486000, 0148E000), RS (0148A000, 01492000), rotate arrows (01456000). Table kQteTex in xenos.cpp (address and content hash; LS and RS share a hash). The renderer publishes the prompt set each frame; `ASURA_AUTOQTE=1` makes xam.cpp add the answer: buttons tapped at 10 Hz (answers presses and mashing), triggers held, LS/RS pushed down, or circled counterclockwise with the rotate arrows. Checked (logs\aqte1): from 150 s of replay, the game ran through episode 1's QTE chain alone (LT+RT, LS+RS down, B, LS rotate, LS, two B mashes, RT) into the Augus cutscenes. Gameplay sections (shooting, fights) still need control. No A/X/Y prompts in episode 1, so those icons aren't in the table yet.

- Shorter test runs: `ASURA_SKIPMOVIES=AW_S000_A000.bik` serves episode 1's 18.2 s opening movie (the mountain and the logo) as one frame, like `ASURA_SKIPINTRO`'s logos (an empty movie is a fatal I/O error); `replays\complete_ep1_short.txt` is the episode 1 recording with every input after 32.2 s moved 18,167 ms earlier. Checked (logs\short1): every QTE comes 543 frames earlier and lasts the same, so the recorded answers still land.

### Speckle: 7e3 targets kept out-of-range values (fixed 2026-11-10)

Isolated bright (and some dark or coloured) single pixels on the planet at the start of episode 1 and over Asura in the shooting section. The scene's 2_10_10_10_FLOAT (7e3) colour targets are held as 16-bit float; the console's format can't hold more than 31.875, nor NaN or infinity (NaN becomes 0), but ours kept them, and bloom and tone mapping turned them into specks. The pixel shader epilogue now clamps colour target 0's output to 0..31.875 (NaN to 0, alpha 0..1) when that target is format 3 or 12 (flag in vpW.w; `ASURA_NO7E3CLAMP=1` to compare). Planet frames: 8 isolated bright pixels before, 1 after; Asura in the shooting section clean (logssura1 3800-3880). Note the per-pixel probe (`ASURA_PROBEQ`) only reads 8-bit targets, so it can't follow the scene passes.

### Afterimages: depth fast clear didn't clear the 7e3 scene target (fixed 2026-11-11)

The window behind Emperor Strada in episode 1 (short replay frames ~8200-8255) kept the previous shot and smeared the passing fleet into white trails. UE3 clears its scene colour target (base 2D0, format 3, 7e3) each frame through the depth-clear trick: a half-pitch 4x MSAA depth-only rectangle at the same EDRAM base, whose dword (here 0) lands in the colour bits too. fast_clear only cleared the 8-bit colour views (formats 0 and 1) at that base, so wherever no geometry drew (the sky through the window) the old picture stayed. It now clears every host colour target at that base and pitch: any format for a zero word, the 10-bit family decoded from the word otherwise (7e3 channels as floats), other formats left alone for non-zero words. `ASURA_FASTCLEAR8=1` restores the old behaviour. Checked (logs\win5): dark space with the fleet and the planet's embers, no trails. Found with `ASURA_DUMPFRAME`/`ASURA_DUMPCOUNT`/`ASURA_DUMPNOSURF` (per-draw trace with targets; summarised by render target). `ASURA_SHOTDIR` is now created if missing (a missing folder silently lost a capture).

The cut to Mithra right after: two frames (8232-8233 in win5, 8256-8257 in win3) draw no 3D scene at all (only the post-process and overlay passes), so they come out black; frame times stay at 33 ms. The same in every run, so a game-side decision; not yet known whether the console does it too.

A one-second freeze reported at this cut (frame 8275: 1047 ms, 89 texture uploads / 50 MB in one frame, ~960 ms of it in the draw path outside texture conversion) is intermittent: the user sees it in most single-instance runs, but one clean run had no stall there (worst frame 36.6 ms) and no 50 MB texture burst at the cut either. Suspect: the texture cache never frees host textures, so video memory fills over a session and a burst of new textures makes the driver page. `[vram]` lines (with each `[perf]`) show usage against the budget. `ASURA_DRAWPROF=1` now also prints a `[spikeprof]` phase breakdown for any single frame over 100 ms; `ASURA_SHOTFROM`/`ASURA_SHOTTO` limit `ASURA_SHOTEVERY` to a frame range.

### Graphics Options menu (2026-11-11)

A host-drawn menu (options.cpp) over the title screen and main menu, since the game's own menus are Scaleform movies (an in-menu version would mean patching the movie or its UnrealScript). Opens with the pad's Back/View button or F1 while a `TitleMenu_Bg*.bik` background movie is open (kfile.cpp `title_screen_active`); up/down, left/right, A, B/Back/Escape. While open the game gets a neutral pad (xam.cpp), and the press that closes it is held back until released. Rows: Internal Resolution (Auto or 1x-4x of 1280x720; read at launch, so "Restart to apply" relaunches the same command line), Vsync (live: Present interval 1), Gamma (50-150%, live, the present pass's `pow(col, 1/gamma)` from BCR2; lower is darker, for bright displays; `ASURA_GAMMA`). Saved to `asura.ini` in the data root (the repo root, C:\dev\asura\asura.ini, in the dev layout; keep it out of git); `ASURA_RESSCALE` / `ASURA_VSYNC` set by hand still win. Drawn with GDI text into a premultiplied RGBA panel uploaded only when it changes (xenos.cpp draw_options). `ASURA_OPTIONSTEST=1` opens it by itself once (checked: logs\opts1, frame 600).

Green picture at internal resolution 3x and 4x (2026-11-11, fixed): two causes, both in the colour grading table (16x512 target, pitch 80, resolved to 0AFD0000).
- 3x: the table's shader picks slice and row from interpolators moved to the guest pixel's centre (vpW.z); at the exact centre the first row of each slice sat on the boundary with the previous slice's padding (green 255), and at 3x it fell on the padding side, so dark colours graded to green. The snap now lands 0.02 guest pixel past the centre.
- 4x: rt_size gives that target 8192 rows, a 32768-row viewport at 4x, past D3D11's 32767 limit, and the table came out nearly empty. Target heights are now capped at 16384 / scale guest rows (the host texture was already capped at 16384 rows, so nothing reachable before is lost).
Checked with the short replay, frames 1350-3000 every 150, at 2x, 3x and 4x (logs\green2, green3b, green4b): no green.

Colour banding growing with internal resolution (2026-11-11, fixed): the table surface is stored at render scale, so each of its 16x16 grid cells is a scale x scale block of equal texels and the hardware filter only blended across the one-texel seam between blocks (flat for half of each cell at 2x, two thirds at 3x): the grading came out in steps. Stacked fetches of resolved surfaces now filter on the fetch's own grid (`gridlerp` in the shader header: four texel centres, the console's weights), then blend the two slices as before. `ASURA_LUTHW=1` for the old hardware filter. Checked: nebula backdrop at 3x, brightened 4x (logs\green3b frame 1650 vs band3 2100): the flat patches with hard edges are gone; 2x unchanged otherwise (logs\band2).

Open (2026-11-11): grey haze around background ships and remaining banding, waiting on a console/Xenia reference from the user. Frame 2100 of the short replay at 3x (logs\haze3, final as fleet_3x_ours.png): the scene before tone mapping (0B700000) is crisp; the haze comes in at the tone map, which mixes in the half-resolution blurred copy (0B130000) by its alpha, the depth-of-field amount. That alpha (`ASURA_DUMPALPHA=1` dumps it as grey; logs\haze3a) is ~0.9 on the foreground and ~0.3 on everything far away, ships and space alike. Either the game's own look or a mis-decoded factor; compare before changing. Banding: the colour table steps are fixed; a final-pass dither option is the fallback if the rest matches the console.

### 60 fps mode and FPS counter (2026-11-11)

The game holds itself to 30 fps with two caps, and both must go:
- UE3's frame limiter: `MaxSmoothedFrameRate=30` (with `bSmoothFrameRate=TRUE`) in the console sections of the engine config (`Xbox360-BCEngine.ini` inside `Coalesced_*.bin`, UTF-16, each key then its value as its own string; the PC sections say 62). kfile.cpp reads every such "30" as "60" (`ASURA_FPS60=1`; `[fps]` log line, 4 values in Coalesced_INT.bin). The file on disk is unchanged.
- Presenting every second vblank. gpu.cpp's vblank thread runs at `ASURA_VBLANKHZ` (default 60); 120 lets it present at 60.
Either alone stays at 30 (the vblank at 120 Hz alone: still 30 swaps a second). With both: 60 fps, p50 16.7 ms (logs\fps60c), 56-60 in the heaviest cutscenes at 1x. Game time follows real time: with the wall-clock input replay, the QTE prompts came at 1.96x the 30 fps frame numbers (8107/8350/11332 against 4141/4264/5797) and were all answered, through the shooting section. Not yet checked: movies (Bink) and the rest of the episode by eye.
Graphics Options: Frame Rate 30/60 (ini `fps`, sets both switches; restart to apply) and Show FPS (ini `showfps`, `ASURA_SHOWFPS=1`): `gpu_fps_text` in a small box in the top-left corner (options.cpp fps_picture, xenos.cpp draw_overlay, shared with the menu panel). `ASURA_OPTIONSTEST` now also ignores real pad and key input (a pad on the desk closed the menu in a test run).

### Per-object noise (2026-11-11)

Planet (episode 1 opening): fixed. Signed textures were read as unsigned and turned signed in texfix after filtering; bytes 7F (+1) and 80 (-1) are neighbours in unsigned order, so filtering across zero gave values near -1 (sparkle on clouds). Formats with an SNORM twin (R8, R8G8, R8G8B8A8, BC4, BC5) are now created typeless, and a fetch reading every channel signed binds the SNORM view (`[snorm]` log lines; `ASURA_NOSNORM=1` for the old way). Checked: logs\noise3 vs snorm1, frame 1770.

Bronze figure and bronze ship in the shooting section (short replay ~8790-9150): FIXED. Cause: texture fetch opcode 19, getWeights (the bilinear weights a fetch at that position would use), was not translated; the translator logged "unhandled fetch opcode 19" and emitted nothing. UE3's shadow projection (ps ED6BB71B465780A5) blends its nine point-sampled shadow-map taps with those weights, so it blended with whatever the registers held: per-pixel speckle wherever a surface shadowed itself. Now emitted as frac(texel position - 0.5) per axis on the fetch's own grid (tfi), with the instruction's offsets. Checked at 1x: logs\noise1x frame 8730 (before) vs weights1 frame 8940 (after), clean armour; no unhandled fetch opcodes left in the log. Found with `ASURA_DBGPSAT` (below): everything up to the comparisons was smooth, the noise came in with the weights.
`ASURA_DBGPSAT="<ps hash>@<marker>#<k>:<float4 expr>"` stops a pixel shader after the k-th body line containing <marker> and outputs <expr> (see the shader with `ASURA_SHADERDUMP`).
The trail, kept for the record (logs\bronze2, tint1, noz1, noshadow1, pcf1, smap1):
- The figure is ps 6038B5EB5A3D0277 (vs 95D2DD604874AD1A), after a depth pre-pass (vs 51038CE0FC71D810); arms ps DEA836E87AFA6260.
- Painted a flat colour with depth test off (`ASURA_TINTPS`, `ASURA_NOZPS`) it is still speckled: the noise comes from a later pass.
- It is UE3's modulated shadow projection, ps ED6BB71B465780A5 (a 36-vertex light volume with stencil, then this pass, blended over the scene). Forcing it to white (`ASURA_DBGPS=ED6BB71B465780A5:float4(1,1,1,1)`) gives clean bronze.
- Its PCF result (r0.z, `float4(r0.zzz,1)`) is itself speckled. Inputs: scene depth copy 0B35A000 (fmt 23, D24FS8 source) and shadow map 0BE31000 (fmt 22, D24S8 at EDRAM 5A0, 1120x688 atlas, nine point-sampled taps); both look smooth when dumped (`ASURA_DUMPRESOLVE=BE31000` writes the surface after every resolve into it in the traced frame).
- Ruled out: render scale (still noisy at `ASURA_RESSCALE=1`), 32-bit float depth buffers (`ASURA_DEPTH32=1`, experiment, off by default), polygon offset (the game sets none here), point-sample snapping to one host texel per guest texel (tried and removed), precise vertex shaders, Xenos 0*x=0 multiplies, loop-shader mip sampling.
- Resolved by the getWeights fix above.

Also made this session, kept as console behaviour: Xenos multiplies (0 times anything is 0: MUL, MAD, DP2/3/4, scalar MUL/MUL_PREV; `ASURA_IEEEMUL=1` for plain products), precise vertex-shader temporaries (`ASURA_VSIMPRECISE=1`), normal sampling in pixel shaders with loops (level-0 copy compiled if the compiler refuses; `ASURA_LOOPLOD0=1`), polygon offset from the faces a draw keeps (front or back registers; `ASURA_POLYFRONTONLY=1`). Test-run switches: `ASURA_MUTE=1` (silent, device still paces the game), `ASURA_MINIMIZED=1`, `ASURA_SHOTFROM`/`ASURA_SHOTTO`; `[texmip]` lines now show sign, swizzle and exponent bias; `[vram]` lines with each `[perf]`.

### Crowd lighting pops: fast clear wiped the whole shadow atlas (fixed 2026-11-11)

In episode 1's Shinkoku crowd scene ("Faithful people of Shinkoku! Offer your prayers!"), the rim light and shadows on the kneeling people switched on and off from one frame to the next, a different person each frame. Xenia shows them steady. UE3 renders every shadow caster's depth map into one atlas (EDRAM base 5A0, pitch 1120, up to 1088x672), resolves it to 0BE31000, then draws the projections (ps 9DA2268CD4E16980 / 0BB34C109B105A7C) that read their own tile. Before each caster it fast-clears only that caster's tile: a depth rectangle, at full pitch and again at half pitch with 4x MSAA. `fast_clear` cleared the whole host depth target, so each clear erased the tiles of the casters drawn before it, and only the last caster's shadow map survived. The draw order changes every frame, so the survivor changed too. `fast_clear` now reads the rectangle's three vertices (screen positions; the vertex count gives the stride), clips them to the scissor, scales them by the samples per pixel (2 for 4x) and the render scale, and clears only that part. Depth and stencil are cleared by a scissored blit whose depth comes from the viewport, colour targets at the same base by ClearView with the rect. Rectangles covering the whole target still take the old path. `ASURA_FASTCLEARWHOLE=1` restores the whole-target clear. Checked (logs\popF against popR, frames 7427-7430): every person casts a ground shadow, and the rim light stays put. Confirmed fixed by the user (2026-11-11).

Not the cause, for the trail: in the light pass, the first character's two meshes get the previous pass's c265 (`ASURA_CONSTWATCH=265` with a trace logs every type-0 write to it). That shader (1031109746444590) never reads c265, and the next shader to read it gets the right value. `ASURA_CONSTLOG="<ps hash>:<from>-<to>"` logs c256-c285 for each draw of one pixel shader. Also ruled out: words changed in an indirect buffer after a WAIT_REG_MEM (none in 89,500 waits), and the light pass draw count (it changes smoothly as the camera pans).

### QTE judge (2026-11-10)

QTEs are run by an `ABCInputJudgeSystem` actor (seen at 420F1000; vtable 8219BBC8; its native execCheckMissInput is sub_824BA210, which calls vtable+888 = sub_82968BC8 every frame of a QTE). It reads button state from BCPlayerInput's bookkeeping (natives SetPressedButton 829293F8, SetReleaseButton 82929518, SetRepeatButton 82929488, ResetRepeatButton 829295A8, UpdateButtonInputCondition 82929340, UpdateInputList 824B8218; name/function table at 831FF598). The "Synchronic" cinematic-track natives (IsInputOk 82950C80 etc.) are not used by episode 1's QTEs. Active prompts: TArray at judge+0x1C4 {data, count, max} (a second one at +0x1E4 during the rotate prompt), 0x60-byte entries: word 3's top byte is the kind (0 press, 1 mash, 3 simultaneous), word 4 the presses required (15 and 30 for the two B mashes), word 0 is another TArray (one item per input). Decoded from the judge's per-frame method sub_82968BC8: prompt entries are 92 bytes; entry+0/+4 = a byte list of input codes and its count; entry+12 = kind (0 one code at a time, the current one at entry+52; 1 mash; 3 all together; 5 special); entry+28 = how misses are judged. Each code becomes a bit of the pressed-input mask (judge+468 -> +1056) through an UnrealScript function called by name (sub_8291C430). Codes seen in episode 1: 7 LT (bit 6), 8 RT (bit 8), 30 B (bit 3), 17-20 left stick left/down/right/up (bits 16-19), 21-24 right stick (bits 20-23), 37 rotate the left stick (counterclockwise). A, X, Y, LB, RB not seen yet. `ASURA_AUTOQTE=1` now answers from the judge (xam.cpp qte_from_judge) and falls back to the prompt icons only for unknown codes (logged); `ASURA_AUTOQTE=icons` keeps the icon-only answers. Checked (logs\jqte2, replay stopped at 135 s): every episode 1 QTE answered from the judge, prompts gone in 15-26 frames (the user's took 26-48), the single LS prompt now gets "right" (the icon version pushed down). UE3 name indices for the pad keys (from the name table, index = stored word / 2): A A7C, B A7D, X A7E, Y A7F, LB A80, LT A81, LS A82, RB A83, RT A84, RS A85, D-pad A70-A73. `ASURA_QTELOG=1` logs the natives, dumps the judge on prompt changes ([qtejudge]) and its prompt list when it changes ([qtelist]).

### Renderer CPU cost in busy scenes (2026-11-10)

The fleet zoom-out at the start of episode 1 (1,500-1,900 draws a frame) ran at 20 fps: the renderer took 32 ms a frame. `ASURA_DRAWPROF=1` (per-phase time of issue_draw, `[drawprof]` every 300 frames) showed 20 ms in the "shaders" phase, which was really the F9 capture ring checksumming each draw's whole first vertex stream (up to 256 KB per draw, with --log) plus build_shader hashing the full microcode of both shaders for every draw. Now: the checksum samples 256 words; shader code that is reloaded unchanged keeps its generation and issue_draw reuses its lookup (cur_vs/cur_ps); vertex streams already copied into the current ring buffer this frame are reused (same buffer, no memexport since, sampled checksum matches; `ASURA_NOVBCACHE=1` to compare). Result: 15.6 ms for 1,861 draws, a steady 30 fps (6 of 441 frames over 40 ms, was 210). What remains per frame in that scene: textures 4-6 ms (hashing every bound texture once a frame), indices 2-4 ms (CPU index conversion), constants 1.5 ms: the next places to look for 60 fps.

### Scene-cut hitches: unfair critical sections (fixed 2026-11-10)

Camera cuts stalled 0.1 to 0.4 s. Stack sampling (`ASURA_MONSAMPLE=1`) showed the game's render thread waiting on one critical section (entered at 82238564) for the whole frame while a loader thread (loop in sub_82239588, name lookups in sub_8238E050) released and retook it: 48,324 times during one 141 ms wait. Host `std::recursive_mutex` lets the releasing thread barge back in. Guest critical sections are now fair (FIFO hand-off to waiters, no barging; kernel.cpp, `ASURA_UNFAIRCS=1` for the old one). Between frames 200 and 12,000 of the auto-QTE run: 10 frames over 90 ms before, 3 after (the episode load at 1023, the menu at 642, and one 146 ms frame at 10387 with the render thread polling occlusion queries). The game releases one section (40A2D7A4, from 82239430) on another thread than took it; the release is honoured, as before (refusing it hung startup at "checking for downloadable content").

Open: ships sometimes dark and without detail in some runs only (timing, likely streaming); colour banding.

### Memexport: UE3 moves texture memory with the GPU (fixed 2026-11-10)

White distant ships (wrong mip levels), one-frame texture swaps on ships, walls and the Gohma torso (even on a paused, still scene), and the Episode Menu's broken visuals all had one cause. The game runs a vertex shader (vs 32F7068C769B5B04, point lists of 64 to 16384 points) whose exports go to eA/eM (export registers 32 to 37) instead of a position: Xenos memexport. UE3's texture pool uses it to move and copy texture memory (8 KB to 1 MB per draw, 16 bytes per point, format 32_32_32_32_FLOAT). The renderer ignored those exports, so moved textures kept stale or other textures' data at their new place, worst in the small mips.

Now: the translator marks such shaders (`XShader::memexport`), keeps eA/eM in locals, and flushes them at each `alloc` and at the end (`mx_flush` in kMemExport, decoding eA as Xenia does: x = 0x40000000 | base in dwords, y = 2^23 + index, z = format word, w = 2^23 + count). Moves keep raw bits (`max(a,a)` becomes a copy in these shaders, or denormal patterns would flush to zero). The vertex shader writes (address, value, mask) records to a UAV (needs feature level 11_1); issue_draw draws the vertices as points with no target, reads the records back (a GPU sync per memexport draw) and writes them into guest memory; textures used earlier in the frame re-check their data (`g_mxEpoch`). `[mx]` lines log the first 20 draws (`ASURA_MXLOG=1` all of them); `ASURA_NOMEMEXPORT` turns it off for comparison. Shaders without memexport compile from the same source as before.

### Broken small textures (earlier notes)

Explosion sprites drawn as squares with scrambled content; likely the same on the pause screen. Those sprites use 2:1 DXT1/DXT5 textures (mostly 64x32, also 256x128 and 256x32) whose fetch pitch is 128 for a 64-wide texture. Their memory holds data laid out for twice the height (`ASURA_TEXDBGWIDE=1` logs an occupancy map per texture, one character per 256 bytes), with zero gaps where the image comes out black. Our tiled layout matches Xenia's (texture_util.cc), so: either the fetch describes a smaller mip of a larger streamed texture, or the data in memory is incomplete. `ASURA_NOMIPS` does not change them.

### Testing aids added 2026-11-10

- `ASURA_INPUTREC=<file>` records the pad (one line per change, ms since the first poll); `ASURA_INPUTPLAY=<file>` plays it back instead of the pad. Cutscenes line up within a few frames; logs\pilot7\input.txt is a recorded run through the first part of episode 1.
- Scripted input steps: `W` both triggers, `K` both sticks down, `C` left stick circling counterclockwise.
- The shader manifest (cache\shader_manifest.txt, out of git) records whole microcode for Asura ("V"/"P" lines) on every run (`ASURA_NORECORD` turns it off), and the next start compiles those shaders first. Keep it across all play sessions: the game is long, so it has to be collected while playing.
### Shader precompile (2026-11-10)

As in BCR2, the shipped manifest holds no game code. `shader_manifest.txt` (repo root, in git) lists `vs|ps <key> <paramGen> [index=value ...]`: the key is a hash of a shader container's microcode in the player's own game files, plus the words the game patched when it bound the shader (only vertex shaders: about 21 words each, their vertex fetches). On start the host loads the shader index, `cache\shader_index.bin` + `shader_code.bin` (out of git), building it first if it's missing (about 1.5 minutes, progress shown; `asura.exe --build-shader-index` does it by hand and reports coverage). Then it compiles every manifest shader. Every run records new shaders (`ASURA_NORECORD` turns it off): a reference line when the index holds the shader, and always a whole-microcode line in `cache\shader_manifest.txt`, which backs up shaders the game files don't hold and is converted to reference lines at the next start.

Where the shaders are (shaderindex.cpp):
- Cooked packages are UE3 compressed chunks (tag 9E2A83C1, block size, sizes, a size pair per block, the blocks), each block XMemCompress LZX with a 128 KB window in 32 KB frames (2-byte compressed size per frame, or FF + 2-byte output size + 2-byte compressed size). 14 packages are compressed whole (with a `.uncompressed_size` file); the rest have a plain summary and compressed body chunks. Decoded with libmspack's lzxd (LGPL 2.1, `thirdparty\libmspack`, in git).
- Global shaders: plain containers (magic 102A1100 pixel, 102A1101 vertex) in `GlobalShaderCache-Xbox360.bin`.
- Material shaders: inside each package's ShaderCache export, in compressed chunks `[uncompressed size][compressed size]` (big-endian) followed by an XMem LZX stream, each holding a run of containers. Found by tracing: the game's heap holds the unpacked chunk (`ASURA_SHADERFIND=<n>` logs where a new shader's microcode sits in guest memory), and LZX streams in the export with container magic in their output gave the layout.
- Coverage of the 465 shaders recorded so far: all 268 pixel shaders exactly, 187 of 197 vertex shaders by the closest container (same length, fewest different words); the other 10 (24 to 138 words) aren't in the packages (likely the D3D runtime's own shaders in the executable) and stay in the local record. 19,500 shaders in the index.
- Debug modes of asura.exe: `--unpack <package> <out>`, `--pkg-info <package> [class]` (UE3 v841 summary and exports), `--find-shaders <file>`, `--probe-lzx <package> <start> <length>` (hex).
- The F10/DUMPFRAME surface dumper decodes half float, 10-bit, 16-bit and two-channel formats. `ASURA_TEXDUMP` no longer runs out on movie frames.
- Fixed: the 10-second thread monitor read a stale guest stack pointer and the access violation ended the process a few frames later (two replays died silently); every read is checked now. Resolving one buffer in two formats each frame reuses the previous surfaces instead of creating textures twice a frame.

### Input (2026-11-09)

The pad works (XInput slot 0 is user 0). The main menu had stopped responding because the game opens a system message box ("saved data does not exist") and then ignores input until it hears the system UI close: every XamShow*UI now sends XN_SYS_UI 1 then 0 to the listeners. After that: the save is created (`saves\SAVEDATA.DAT`), all five DLC packs mount (DLC0-DLC4) and the Episode Menu works. Seen: a checkered garbage band along the top of the Episode Menu (a rendering bug, not yet looked at).

### Testing aids and saves (2026-11-09)

- `ASURA_SKIPINTRO`: the autosave notice and the four logo movies are served as one-frame copies made from the originals (`cache\intro\`, built by the host on first use; `tools\bink1frame.py` does the same by hand). A missing or empty movie is a fatal "I/O failure" in this game, so they must stay valid Bink files.
- The user's completed save: `C:\dev\asura\SAVEDATA.DAT` is a console `CON` package; its one file (`SAVEDATA.DAT`, 50,193 bytes) is installed as `saves\SAVEDATA.DAT\SAVEDATA.DAT` and the game accepts it (the Episode Menu opens on Part IV, Episode 19). The game's own fresh save is kept in `saves_backup\`.

### Episode Menu ring (open)

The user reports the portrait ring "revolving around a fixed point instead of rotating around the portrait"; captures show the ring and the portrait apart, with a white disc where the portrait should be. Ruled out so far (frame dumps at the Episode Menu, `ASURA_DRAWPRINTFULL=4213EBD96F9DECE3`): the Scaleform vertex shader translation (its odd swizzles compose to a plain 2D transform), the vertex position decoding (16_16, high half is x), and the matrices themselves (axis-aligned scales with 16:9 aspect; the rotated element is a proper rotation about its own origin). So the placement comes from the game's CPU-side UI logic. Also ruled out: vertex shader ABC6D461 (a real constant-output shader, its draws are empty on hardware too), `mfocrf` (XenonRecomp hard-codes cr6, but this game only uses cr6), the unaligned vector loads (`lvlx`/`lvrx`, standard Unleashed versions), the offscreen passes (the 328x184 resolves come out black but don't move anything).

Reference (user's Xenia shots): the ring, the petal frame and the portrait share one centre at about (1100, 360), and five episode rows with thumbnails show. A later Xenia shot (completed save, Part I): ring centre at about (1080, 375), the gold petal frame around it, the portrait inside lit by a yellowish glow that slowly fades in and out; five rows (Episode 1 highlighted in olive, the rest greyed with faint thumbnails behind the text), EASY/NORMAL/HARD columns with gold marks, a scroll bar right of the rows, no checkered band at the top. Here, on the episode detail screen (frame dump, `logs\run38.log`): ring pieces at (1079.5, 371.5) except one piece exactly (-128, -128) px off, the two big petal-frame shapes (2298 and 1146 vertices) at (1172, 72) and (1173, 4), the portrait and a white backing disc elsewhere. A clean 128 px offset on some clips points at wrong data in some display-list matrices (CPU side), not at the renderer. Next: find the Scaleform code that composes a clip's world matrix and watch the -128 clip's matrix being written (hardware watchpoint on its guest address), then check the instructions on that path, starting with the vector ones this project or BCR2 added to XenonRecomp (vpkd3d float16_2, vctuxs, vpkuwus/vpkuhus, ...). Also seen: a checkered band along the top of the Episode Menu, and only the selected episode row drawn.

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
