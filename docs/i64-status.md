# Star Fox 64 in i64: status

ROM: Star Fox 64 (USA) Rev 1, SHA-1 09f0d105...  
Route: A, HarbourMasters Starship (libultraship)  
Owner: Studio agent  
Process and gate definitions: https://github.com/philosoraptor64/n64net/blob/main/docs/new-game-process.md

| Gate | Status | Evidence |
|---|---|---|
| 0 Intake | done |  |
| 1 Code map | n/a | existing port |
| 2 Builds | done | module build Mac + iOS |
| 3 Boots and plays | done | single player, Versus |
| 4 Correctness | partial | pause-menu-on-return (a70101c4) not yet seen in a mission |
| 5 Determinism | done | replay check, Versus per-tick hashes identical |
| 6 i64 module | done | 78 MB arena block (b76dd983); 0.17 MB/run left |
| 7 Online (lockstep) | done | pair tests in sync |
| 8 Rollback | done | SyncTest in Versus; 4000 ticks at 40+20 ms in sync |
| 9 Multiplayer features | partial | Versus 2 pilots online; more players pending sessions >2 |
| 10 iPhone | partial | 14 Pro single player at 60 fps; two phones not yet |

Status: done / partial / not started / n/a. Updated 2026-11-13 (Studio agent; owners please correct).

Known issues / open:
- crash-safe saves added (d02143df)
