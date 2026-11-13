# Paradox games speed-up — plugins

[Русская версия](README.ru.md)

Source code of DLL plugins that speed up **Europa Universalis IV 1.37.5 (Steam and GOG, Windows x64)**, including large mods such as Anbennar. Plugins for other Paradox games will come later.

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

## Building

You need [Zig 0.13.0](https://ziglang.org/download/#release-0.13.0) (it is also the C++ compiler).

1. Unpack Zig into `.tools/zig-windows-x86_64-0.13.0/` in the repository root.
2. Put a copy of the game into an `Europa Universalis IV/` folder next to it, or change the `PLUGINS` path in the scripts.
3. Build a plugin:
   ```
   python eu4_country_patch/tools/build.py
   ```
   The script builds `build/<plugin>.dll` and copies it into `Europa Universalis IV/plugins/`.

The analysis tools (`eu4_sampler/tools`, `eu4_knowledge_patch/tools`, `eu4_port/tools`) need the Python package `capstone`: `pip install capstone`.

## Addresses in the code

Hooks are tied to exact addresses (RVAs) in the Steam `eu4.exe` 1.37.5 (SHA-256 `9ad3efe1af169f40ee577f9dae5debbc87af6fb8b5450fb345ebf110dc4d771a`). Comments name game functions by addresses such as `140a5e130` (image base `0x140000000`).

The GOG build of the same version (SHA-256 `5f4145ce5ff9a4140cc4d14440920382b698dd91e1cdae1d1dc053357d9388ee`) has the same code at other addresses. `eu4_port` matches the functions of the two builds, and `eu4_port/include/eu4_build.hpp` translates every Steam address a plugin uses at run time, comparing against GOG bytes verified offline (`eu4_gog_table.inc`). With any other game version the plugins do not turn on until the addresses are updated.

## Licences

Part of the code is ported from MIT-licensed projects, see [THIRD_PARTY.md](THIRD_PARTY.md). Our code is under the MIT licence ([LICENSE](LICENSE)); the ported parts stay under their authors' licences.

Europa Universalis IV is a trademark of Paradox Interactive. This project is not affiliated with it; the repository contains no game files.
