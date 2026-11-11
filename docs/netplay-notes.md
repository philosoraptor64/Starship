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

## Next steps (small, one commit each)
1. User plays it on the Mac (picture, sound, controller).
2. Find the controller read (port side of osContGetReadData) and the end of a logic tick; list clock reads.
3. Deterministic mode + replay check like perfect_dark's `port/src/replaycheck.c` (fixed seed, record/play pads,
   per-tick state hash), Versus mode boot.
4. iOS build (LUS iOS path from SpaghettiKart/BattleShip), then the i64 module.
