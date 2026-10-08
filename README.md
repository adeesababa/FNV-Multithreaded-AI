# FNV Multithreaded AI (EXPERIMENTAL)

An xNVSE plugin for **Fallout: New Vegas** that spreads the game's NPC detection work over several CPU cores, so big
crowds of NPCs stop freezing the game when they notice each other.

> [!WARNING]
> **This is an experimental plugin. It may crash or freeze your game.**
> - **Do not use it with saves you care about.** Back up your saves before trying it.
> - It has been tested on one PC only. Earlier builds crashed in very big fights with human NPCs (250+ at The Fort).
>   The cause was narrowed down and this build avoids it, and it ran clean in that same fight, but it has not been
>   tested long enough to call it stable.
> - Removing it is always safe: it **writes nothing to your saves**. Delete the DLL and the game is exactly as before.

## What it does

Every NPC near you regularly checks which other actors it can see (line-of-sight ray casts). The game does these checks
one after another on a single thread. With a few dozen NPCs that is fine; with hundreds (big battles, spawned hordes,
crowd mods) it can take seconds per frame, and the game freezes.

This plugin runs the slow part of those checks, the line-of-sight ray casts, **at the same time on several CPU cores**
and hands the results back to the game. Same checks, same inputs, same results: NPCs behave exactly as they would
without it, just without the freeze.

Measured on the author's PC (Ryzen 7 9800X3D, 16 threads, 8 helper threads):

- **Big fight at The Fort (250+ human NPCs), this build:** at the peak, about 600 checks per frame took **23 ms**
  instead of **203 ms** (8.7x faster).
- **~300 hostile feral ghouls, an earlier test build:**

| | Without | With the plugin |
|---|---|---|
| Worst freeze when the horde appears | 8 s | 1.2 s |
| Average frame during the fight | 140 ms (~7 fps) | 45 ms (~22 fps) |

With normal numbers of NPCs you will not notice a difference; it only matters with big crowds.

## Requirements

- Fallout: New Vegas **1.4.0.525** (the current version; tested on Steam)
- [xNVSE](https://github.com/xNVSE/NVSE)
- A CPU with **4 or more threads** (it uses your thread count minus 3, up to 8 helper threads; with 3 or fewer threads it
  stays idle and the game runs as before)

## Install

Copy into your game's `Data\NVSE\Plugins\` folder:

- `MultithreadedAI.dll`
- `MultithreadedAI.ini` (settings)
- `MultithreadedAI.pdb` (optional; makes crash logs readable)

With a mod manager, install the zip like any other NVSE plugin.

**Uninstall:** delete those files. Nothing else is changed.

## Settings (`MultithreadedAI.ini`)

| Setting | Default | What it does |
|---|---|---|
| `Enabled` | `1` | On when the game starts. `0` = off (Scroll Lock can still switch it on). |
| `ScrollLockToggle` | `1` | The **Scroll Lock** key switches it on/off while playing, so you can compare. |
| `Threads` | `0` | Helper threads. `0` = automatic (logical cores minus 3, at most 8). |
| `MinChecks` | `12` | A lone NPC's check run smaller than this is left to the game (not worth waking the helpers). |
| `SerialChecks` | `0` | Diagnostic only: runs the checks one by one again (no speed-up). |
| `LogSummarySeconds` | `10` | How often the log gets a line of numbers while it works. `0` = never. |
| `CheckLock` | `2` | How much of each check runs in parallel. `2` = only line of sight (the slow part), the rest one at a time. `1` = whole checks one at a time (little speed-up). `0` = everything in parallel: **test only, this crashed** in big human fights. |
| `NoHelpers` | `0` | Diagnostic only: no helper threads at all (no speed-up). For crash hunting. |
| `PickLock` | `0` | Diagnostic only: the ray casts themselves run one at a time (less speed-up). For crash hunting. |

## If something goes wrong

The plugin writes `MultithreadedAI.log` in your game folder (replaced at each launch). If the game crashes or freezes
with the plugin, please open an issue with:

- `MultithreadedAI.log`
- your crash log, if you use a crash logger (e.g. CrashLogger's `Crash Logs\CrashLogger.*.log`)
- what was happening (how many NPCs, which kind, where)

## Compatibility

- At start it checks every piece of game code it hooks. If another plugin has changed any of it, it **does nothing**
  and says so in its log; the game then runs as before.
- Other plugins' hooks inside the detection check (for example JIP LN's) are listed in the log, because they then also
  run on the helper threads.
- It only acts on NPCs' detection of other actors. Everything else in the game's AI is untouched.

## How it works (technical)

- At the start of each detection pass (`ProcessLists::RunDetectionForAllActors`, on the game's AI thread), the plugin
  collects every check the pass is about to make, using the game's own filters and arguments, and runs them all at
  once: on helper threads created as the game's own AI worker threads (`AILinearTaskThread`, with the same Havok
  thread memory set-up) plus the AI thread itself. Each NPC's detection loop then takes its answer from that batch;
  anything not in the batch is checked by the game as before.
- Batches run only inside that detection pass. The game also runs detection elsewhere (for example for a leader's
  followers on the other AI thread, while physics may be updating); those calls are left to the game.
- Only the line-of-sight part of each check (`Actor::LineOfSight`, about 99% of a check's time) runs on several threads
  at once; the rest of each check runs one at a time under a lock. Running whole checks at once crashed in big fights
  with human NPCs (Havok heightfield data overwritten); with whole checks one at a time, and with this mode, the same
  fight ran clean.
- One part of the check is not read-only: looking up the target's worn armor can delete an inventory entry in a rare
  case. That call is made one thread at a time.
- Havok (the physics engine) has room for 32 threads in its memory system; the plugin always leaves 10 of them free.
- The source has detailed comments with every game address used (`src/ParallelDetection.inl`).

## Building

Visual Studio 2019 (v142 toolset), Win32 Release: open `MultithreadedAI.vcxproj` and build, or

```
msbuild MultithreadedAI.vcxproj /p:Configuration=Release /p:Platform=Win32
```

Offline tests (no game needed): `tests\run_offline_test.ps1`.

## Credits

- [xNVSE](https://github.com/xNVSE/NVSE) team, for the plugin API.
- **WallSoGB**, for the IDA Pro database / symbol map of the game, which this work's reverse engineering relies on.
- Part of the work on [NVR Unofficial Optimized](https://github.com/adeesababa/NVR-Unofficial-Optimized).

## License

GPL-3.0 (see [LICENSE](LICENSE)). Provided as is, without warranty of any kind.
