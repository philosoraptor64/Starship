# Netplay notes (philosoraptor64 fork)

Goal: deterministic input-exchange netplay (lockstep, then rollback) through
[n64net](https://github.com/philosoraptor64/n64net), Mac and iPhone first, then an i64 module, the same approach as
our SpaghettiKart (Mario Kart 64), BattleShip (Smash 64) and perfect_dark forks. Online target: Versus mode
(2-4 players).

## Status
- Branch `netplay` (default): builds/boots on macOS 27 (Studio, 2026-11-10): Metal shader fix
  (`port/shaders/metal/default.shader.metal`, no `thread` on by-value texture/sampler params), Torch spdlog pin +
  BUILD_SM64=OFF, `STARSHIP_STRICT_FP` (default ON, `-ffp-contract=off`). Submodules -> philosoraptor64/libultraship and
  philosoraptor64/Torch, branch `starship-netplay`.
- **MacBook build (2026-11-10 night):** works. Deps: `brew install libogg libvorbis vorbis-tools libpng` (+ Brewfile).
  ROM: Star Fox 64 US Rev 1 (SHA-1 09f0d105f476b00efa5303a3ebc42e60a7753b7a) symlinked as `baserom.z64` in the repo
  root (git-ignored; never commit it or the .o2r files). Commands:
  ```
  cmake -H. -Bbuild-cmake -GNinja -DCMAKE_BUILD_TYPE=Release     # ~5 min first time
  cmake --build build-cmake --target ExtractAssets               # sf64.o2r from the ROM
  cmake --build build-cmake --target GeneratePortO2R             # starship.o2r
  cmake --build build-cmake -j14                                  # -> build-cmake/Starship
  ```
  Run from `build-cmake/` (`./Starship`; background test runs: `SDL_MAC_BACKGROUND_APP=1
  SDL_WINDOW_NO_ACTIVATION_WHEN_SHOWN=1`, plus `-ApplePersistenceIgnoreState YES`). Boots to the title scenes, no errors.
  Not yet played by the user.

## Determinism survey (Studio, 2026-11-10)
- One logic tick per `Graphics_ThreadUpdate` (30/s); 60 fps is render interpolation.
- Main RNG `sRandSeed1-3` is clock-seeded in `Rand_Init` (src/engine/fox_game.c ~58): seed from the match config.
- sinf/cosf come from the system libm (fine Mac <-> iPhone, a risk vs. PC).

## Tick structure + clock reads (MacBook, 2026-11-10 night)
- One tick = `Graphics_ThreadUpdate` (src/sys/sys_main.c): `Controller_UpdateInput()` (copies last tick's read into
  gControllerHold/gControllerPress, dead zone) -> `Controller_ReadData()` (src/sys/sys_joybus.c: `osContGetReadData(
  sNextController)`, unless gControllerLock) -> `Game_Update()` -> ... -> `Audio_Update()`, all on one thread.
  Hook points: input filter right after `osContGetReadData(sNextController)`; state hash after `Game_Update()`.
  Note the pads read at tick N are used at tick N+1 (UpdateInput runs first).
- Clock reads that reach gameplay (must be replaced in deterministic mode):
  1. `Rand_Init` (src/sys/sys_math.c) seeds sRandSeed1-3 from osGetTime() -> seed from the match config. (The
     Switch/Linux "seed is 0" workaround also re-seeds from the clock; not compiled on Apple.) There is also a seeded RNG
     (`Rand_SetSeed` -> sSeededRandSeed*), set by game code.
  2. **Versus time limit**: `Versus_UpdateTimer` (src/engine/fox_versus.c ~299) counts gVsCountdown down by the real
     time between draws (`sDrawTime = osGetTime()` ~1657) -> count ticks instead (1/30 s per tick) online.
  3. Boot logo: `Timer_CreateTask(MSEC_TO_CYCLES(1000), Timer_Increment, &gGameState)` (src/engine/fox_game.c ~402)
     leaves the logo after 1 real second (port timers run on SDL_GetTicks, src/port/Engine.cpp) -> a fixed tick count.
- Audio only: `gAudioRandom` from osGetCount (src/audio/audio_thread.c); not read outside src/audio. Check that no game
  code reads audio state back (perfect_dark had an AI command that did).
- Not yet checked: the render/camera aspect feeding the simulation (it did in perfect_dark: there a fixed render
  aspect was needed), widescreen/enhancement cvars that change gameplay.

## Deterministic mode (step 3a, 2026-11-10 night, MacBook)
`src/port/ReplayCheck.{c,h}`: `SF64_DETERMINISTIC=1` (later also implied by SF64_REPLAY_CHECK/RECORD/PLAY):
Rand_Init seeds from `SF64_SEED` (default 1), Versus_UpdateTimer advances 100/30 centiseconds per tick, the boot logo
waits 30 ticks (ReplayCheck_EndTick, called after Game_Update in Graphics_ThreadUpdate) instead of the 1 s timer task.
Verified only that it builds and boots to the title. NOT yet: input record/play, state hash, a replay comparison.

## User test on the Mac (2026-11-10 night)
- "Everything works." Launcher: `~/Developer/N64/Play Star Fox.command` (local, not in git: `cd build-cmake && ./Starship`;
  double-clicking build-cmake/Starship in Finder starts in ~ and asks for a ROM because it can't find sf64.o2r).
- Xbox controller: C buttons were only on the right stick / triggers / B / Y; the user wanted them on the D-pad. Done in the
  user's local config only (build-cmake/starship.cfg.json, backup .bak-before-dpad): SDL buttons 11-14 added to C-Up/Down/
  Left/Right. **Now the default** (2026-11-11, Studio, src/port/Engine.cpp SDLButtonToButtonMappings: D-pad also on
  C-Up/Down/Left/Right, the existing Y/B/trigger/right-stick C mappings kept). Defaults only apply to a controller with
  no saved mapping (new installs, i64 module, iOS); existing configs keep theirs. Not yet tested with a pad.
- VS mode didn't start with one controller (it needs a second plugged pad for player 2): netplay must make the remote
  player's port read as plugged (gControllerPlugged / osContGetStatus in Controller_UpdateInput, sys_joybus.c).

## Next steps (small, one commit each)
1. (done) User plays it on the Mac.
3b. **First test PASSED (2026-11-10 night):** 3 idle runs from boot x 600 ticks byte-identical (logo -> title at tick
   33 every time; hashes in build-cmake/replay/). NEXT: scripted/human input through the menus into a level and into
   Versus, then the iPhone build. Details: SF64_REPLAY_CHECK/RECORD/PLAY/FRAMES in ReplayCheck.c
   (pads hooked after osContGetReadData in sys_joybus.c, hash in ReplayCheck_EndTick, `_Exit(0)` after N ticks).
   First test to run: idle from boot, 3 runs identical:
   `cd build-cmake && SF64_REPLAY_CHECK=/tmp/a.hash SF64_REPLAY_FRAMES=1500 ./Starship` (x3, compare), then scripted
   presses / a human recording (SF64_REPLAY_RECORD) played back. Original plan text: Replay check like perfect_dark's `port/src/replaycheck.c`: record/play pads right after
   `osContGetReadData(sNextController)` in Controller_ReadData (src/sys/sys_joybus.c), per-tick hash in
   ReplayCheck_EndTick (sRandSeed*, gGameFrameCount, gPlayer[] pos/rot/health/state, actors/bosses/items), quit after N
   ticks; a script like perfect_dark's tools/replay/replay-test.sh. Then a Versus boot (4 players, 3 fed by a script).
3. (old text) Deterministic mode + replay check like perfect_dark's `port/src/replaycheck.c` (fixed seed, record/play pads,
   per-tick state hash), Versus mode boot.
4. iOS build (LUS iOS path from SpaghettiKart/BattleShip), then the i64 module.

## Input determinism + 2-player netplay (2026-11-11, Studio)
- **Bot determinism:** `SF64_TEST_BOT=seed` (controller 1; `SF64_TEST_BOT2..4` the others) mashes stick/fire/bombs/
  boost/menus. Seeds 5, 9, 13 (+BOT2), 3600-5400 ticks each run twice/three times: byte-identical per-tick hashes;
  the bots get into gameplay (GSTATE_PLAY ~4200 ticks, up to 16 objects).
- **SfNet** (`src/port/SfNet.{c,h}`, n64net submodule `external/n64net` @ 39dccc9): lockstep, the whole game shared
  from boot like two controllers on one console (each machine's own pad = its controller 1 -> host P1, guest P2;
  pads 3-4 unplugged). Hooks: ReplayCheck Init (connect, the host's seed wins), ReplayCheck_FilterPads (after
  osContGetReadData; ticks without a read get a filler input so the ledgers stay in step), ReplayCheck_EndTick
  (sync check every 30 ticks with the replay hash), Save_ReadData/WriteData (the guest plays on the host's save
  'SFSV', never writes). Handshake 'SFNH' compares the build fingerprint (`SFNET_BUILD_ID`, cmake/NetBuildId.cmake,
  every build); room codes via n64net rendezvous (game id `sf64-us+<build>`); BYE / 20 s silence -> play on offline.
  **i64-ready** like perfect_dark: `SfNet_SessionBlob` (host's save, 520 bytes), `SfNet_InitExternal`,
  `SfNet_ExternalEvent` (away = wait without a limit).
- `tools/net/pair-test.sh` (two copies in build-cmake, bots, compares sync checks AND both full per-tick hash
  logs): direct 1800 ticks (1449 in gameplay) IN SYNC, every tick's hash identical; DESYNC_TEST=400 caught at 420;
  EXTERNAL=1 (i64 stand-in) IN SYNC; SERVER=local room code direct IN SYNC.
- NEXT: Versus with both players (menu path to VS for the bots; check that P2's input drives P2), rollback
  (snapshots like perfect_dark: section pragma for the game's C globals + whatever heaps the game uses; Starship
  has C++ port code, check the state there), time sync, iOS build, then the i64 module (MacBook agent).
- **Versus online works (2026-11-11):** upstream disabled VS (`DEBUG_VERSUS 0`, commit 18631e72 "disable versus mode
  for now"); our fork sets `DEBUG_VERSUS 1` (include/mods.h). Online exactly controllers 1-2 count as plugged
  (sys_joybus.c osContGetStatus), so the handicap screen needs only the two players' A (offline every port counts as
  plugged and ports 3-4 can never confirm: that was the "VS needs a second pad" report). `VS=1 tools/net/pair-test.sh`:
  scripted menus (tools/net/vs-host.txt / vs-guest.txt: title Start x2, down x2, VS, Point Match, both confirm, stage)
  then both bots fly (SF64_NET_BOT_FROM=1500): VS_INIT -> 2281 ticks of a 2-player Point Match on Corneria, every
  per-tick hash identical; screenshots: both windows show the same 4-way split (Fox + Peppy flying, 2 spectator
  "Camera" quarters). Debug: SF64_NET_TRACE=from:to logs local/used pads. Bug fixed on the way: the netplay pad
  filter ran before a played-back script's pads were applied.
- NEXT for Star Fox: one full-screen view per device in VS (each player sees their own quarter full screen; like
  perfect_dark's full views, check what the camera/HUD do), rollback, iOS build + simulator test, i64 module.
  Why upstream disabled VS is unknown: watch for crashes in longer VS sessions (Battle Royal, Time Trial, other stages).
- **Full view per device in online Versus (2026-11-11):** `SfNet_FullView(gCamCount)` (default online with 4
  cameras; `SF64_NET_VIEW=split` = classic 4-way): fox_game.c draws only the local pilot (`Game_Draw(local)`) with
  the 1-camera full-screen viewport, skips the other views/dividers, draws the local glare/damage flash full screen,
  and sets `gPlayerNum = 1` afterwards (what drawing views 0,3,2,1 leaves on every machine: Game_Draw sets it).
  fox_versus.c skips other slots' labels (the empty slots' "Camera"). All four pilots are still simulated on both.
  VS pair test: IN SYNC, every per-tick hash identical (so drawing one view doesn't touch the simulation).
  **Still quarter-sized:** the local pilot's HUD (radar box, health bar, portrait, player number, boost gauge) stays
  at its quarter's position (P1 top-left, P2 top-right), drawn in the per-view Display/HUD code with quarter
  coordinates. Next: a full-screen layout for those (ideally the 1-player HUD positions).
