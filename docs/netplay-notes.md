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
