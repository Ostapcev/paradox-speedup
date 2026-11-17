# Paradox games speed-up — plugins

[Русская версия](README.ru.md)

Source code of DLL plugins that speed up **Europa Universalis IV 1.37.5 (Steam and GOG, Windows x64)**, including large mods such as Anbennar, **Hearts of Iron IV 1.19.5 (Steam, Windows x64)** and **Stellaris 4.5.4 (Steam, Windows x64)**. Plugins for other Paradox games will come later.

The easiest way to install the ready builds is the [installer](https://github.com/Ostapcev/paradox-speedup-launcher) (English and Russian). It finds the game, explains every plugin and sets it up.

## How it works

The plugins are ordinary DLLs in the `plugins` folder next to `eu4.exe`. They are loaded by `VERSION.dll`, the loader from EU4 Unicode Patch. `eu4.exe` on disk is never changed: each plugin changes the game's code only in memory, at start-up.

Before any change a plugin checks that this is the right game build:
- the build of `eu4.exe` (Steam or GOG 1.37.5, from its PE header);
- the bytes at every place it changes.

If anything differs, the plugin writes `REFUSED` to its log and touches nothing. Every plugin has an ini file with an `off`/`disabled` mode.

Many plugins can run in shadow mode (`shadow`): they compute their own way, compare with the game's result and log the number of differences. This is how their accuracy was checked.

## EU4 plugins

| Folder | What it does | Changes gameplay? |
|---|---|---|
| `eu4_render_patch` | Draws fewer frames at fast speeds (the largest speed-up) | no |
| `eu4_alloc_patch` | Fast memory allocation | **yes**: a different order of addresses |
| `eu4_country_patch` | Country calculations ahead of time on several threads; crash guard `relguard` | no; 4 rare calculations can be turned off |
| `eu4_ai_serial` | Caches in AI calculations | no |
| `eu4_knowledge_patch` | Parallel map knowledge | no |
| `eu4_modifier_patch` | Fast modifier search and merge | no |
| `eu4_gov_patch` | Remembers government reform properties | no |
| `eu4_pops_patch` | Removes needless waiting in Paradox online services | no |
| `eu4_load_patch` | Fast game and save loading | no |
| `eu4_loc_patch` | Fast localisation loading | no |
| `eu4_menu_patch` | "Exit to Menu" without restarting | no |
| `eu4_save_patch` | Fast saving | no |

Experimental and service folders (not in the installer):

| Folder | Purpose |
|---|---|
| `eu4_speed_patch` | Schedule of the army AI target search |
| `eu4_trigger_patch` | Parallel triggers, diagnostics only |
| `eu4_det_patch` | Fixed seed for reproducible comparisons |
| `eu4_sampler` | Profiler: stacks of the game's threads |
| `eu4_bench` | Speed measurement from the `-gamestatetimer` log |
| `eu4_port` | Steam → GOG address translation (see below) |

## HoI4 plugins

For HoI4 1.19.5.0 and 1.19.4.2 (Steam). The two builds differ only in version strings and one small text-parser function, which no plugin touches. `hoi4_common/hoi4_build.hpp` recognises both builds from the PE header of `hoi4.exe`; with any other build the plugins write `REFUSED` and change nothing. On a late save (1941) a game day takes about 800–860 ms instead of about 1020–1190 ms without plugins.

| Folder | What it does | Changes gameplay? |
|---|---|---|
| `hoi4_loader` | Our own plugin loader `VERSION.dll` | no |
| `hoi4_render_patch` | Draws frames and the 3D scene less often at fast speeds (province graphics and unit model throttles exist but are off: they made icons flicker) | no (with `supply_interval=1`, as the installer sets it) |
| `hoi4_supply_patch` | Reuses the supply path calculation of a country whose inputs did not change; byte for byte the game's result | no |
| `hoi4_tick_patch` | Runs 23 small parallel loops of the hourly tick in the calling thread, the game's own single-job path | no |
| `hoi4_load_patch` | Faster start-up and save loading: trigger registry, file index, country tag lookup | no |
| `hoi4_alloc_patch` | Fast memory allocation (port of `eu4_alloc_patch`) | **yes**: a different order of addresses |

Not in the installer: `hoi4_bench_helper` (switches to observer mode for benchmarks), `hoi4_sampler` (profiler), `hoi4_bench` (speed measurement from the `-gamestatetimer` log).

## Stellaris plugins

For Stellaris 4.5.4 (Steam, build 1b4f). `stellaris_common/stellaris_build.hpp` recognises it from the PE header of `stellaris.exe`; with any other build the plugins write `REFUSED` and change nothing. All plugins use the shared `stellaris_common/stellaris_plugin.hpp` (build check, byte check, log, ini). Many exact plugins were checked in shadow mode: they compute their own way next to the game and compare, 0 differences.

Measured over the same date span in the game's log: late game (5000-star galaxy, year 2446) ×2.07, early game (2200) ×1.52 with all installer plugins.

| Folder | What it does | Changes gameplay? |
|---|---|---|
| `stellaris_loader` | Our own plugin loader `VERSION.dll` | no |
| `stellaris_render_patch` | Draws frames from inside the day calculation less often | no |
| `stellaris_budget_patch` | Reads the timer less often in the frame-budget check | no |
| `stellaris_path_patch` | Distance table between all systems in one multi-threaded pass (load: ~20 s → ~2 s) | no |
| `stellaris_dijk_patch` | Our own path search with the same rules, continuing a repeated search | no |
| `stellaris_pathq_patch` | Writes paths into the shared cache without extra exclusive locking | no |
| `stellaris_reach_patch` | Replays the finished part of the search in the gateway reachability check | no |
| `stellaris_fleet_patch`, `stellaris_ship_patch`, `stellaris_tick_patch`, `stellaris_scan_patch` | Fleet state reset in one pass and data prefetch in micro-tick loops | no |
| `stellaris_vis_patch`, `stellaris_pair_patch` | A table instead of a scan in visibility, an early answer in the hyperlane check | no |
| `stellaris_tdisp_patch` | Short ready-made jumps instead of the object check in script conditions | no |
| `stellaris_ai_patch` | Task pool on all logical cores but two | no |
| `stellaris_alloc_patch` | Fast memory allocation (port of `hoi4_alloc_patch`) | **yes**: a different order of addresses |
| `stellaris_aiskip_patch` | Every country's AI thinks once in K days (3 by default) | **yes** |
| `stellaris_intel_patch` | Fast system visibility calculation; intel once in K days | **yes** (no with `rr=1`) |
| `stellaris_fleetscan_patch` | Exact cache in the "known fleets" task; update once in K days | **yes** (no with `rr=1`) |
| `stellaris_lazy_patch` | Visibility, sector scores, intel level, casus belli not every day | **yes** |
| `stellaris_modflush_patch` | Defers the modifier graph recalculation to the planned one | **yes** |
| `stellaris_opday_patch` | One country's opinion of another once per game day | **yes** |
| `stellaris_cmpc_patch` | Cache of "count and compare" conditions within a micro tick | **yes**, very little |

Not in the installer: measurements and closed experiments (`stellaris_focus_patch`, `stellaris_graph_patch`, `stellaris_opin_patch`, `stellaris_sched_patch`, `stellaris_sysq_patch`, `stellaris_tail_patch`, `stellaris_trig_patch`, `stellaris_wait_patch`), the test plugin `stellaris_hello` and the profiler `stellaris_sampler`. A Stellaris plugin is built with `python stellaris_common/build_plugin.py stellaris_render_patch` (`--install` copies it into the game folder from the `STELLARIS_DIR` variable).

## Building

You need [Zig 0.13.0](https://ziglang.org/download/#release-0.13.0) (it is also the C++ compiler).

1. Unpack Zig into `.tools/zig-windows-x86_64-0.13.0/` in the repository root.
2. Put a copy of the game into an `Europa Universalis IV/` folder next to it, or change the `PLUGINS` path in the scripts.
3. Build a plugin:
   ```
   python eu4_country_patch/tools/build.py
   ```
   The script builds `build/<plugin>.dll` and copies it into `Europa Universalis IV/plugins/`.

HoI4 plugins are built the same way (`python hoi4_render_patch/tools/build.py`, add `--install` to copy into `Hearts of Iron IV/plugins/` next to the repository); the loader is `python hoi4_loader/tools/build.py`.

The analysis tools (`eu4_sampler/tools`, `eu4_knowledge_patch/tools`, `eu4_port/tools`) need the Python package `capstone`: `pip install capstone`.

## Addresses in the code

Hooks are tied to exact addresses (RVAs) in the Steam `eu4.exe` 1.37.5 (SHA-256 `9ad3efe1af169f40ee577f9dae5debbc87af6fb8b5450fb345ebf110dc4d771a`). Comments name game functions by addresses such as `140a5e130` (image base `0x140000000`).

The GOG build of the same version (SHA-256 `5f4145ce5ff9a4140cc4d14440920382b698dd91e1cdae1d1dc053357d9388ee`) has the same code at other addresses. `eu4_port` matches the functions of the two builds, and `eu4_port/include/eu4_build.hpp` translates every Steam address a plugin uses at run time, comparing against GOG bytes verified offline (`eu4_gog_table.inc`). With any other game version the plugins do not turn on until the addresses are updated.

## Licences

Part of the code is ported from MIT-licensed projects, see [THIRD_PARTY.md](THIRD_PARTY.md). Our code is under the MIT licence ([LICENSE](LICENSE)); the ported parts stay under their authors' licences.

Europa Universalis IV is a trademark of Paradox Interactive. This project is not affiliated with it; the repository contains no game files.
