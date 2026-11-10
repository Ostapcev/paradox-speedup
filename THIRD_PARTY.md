# Сторонний код

| Откуда | Лицензия | Где используется |
|---|---|---|
| **Bellows** (Happy Dwarf Team) — открытый плагин ускорения EU4 | MIT, текст в `LICENSE.bellows.txt` | `eu4_load_patch/src/bellows/` и отдельные функции в `eu4_load_patch`, `eu4_ai_serial`, `eu4_modifier_patch` (помечены в комментариях) |
| **Hamweard** (Happy Dwarf Team) и **EU4 Menu Patch** (VulonLok) | MIT, текст в `eu4_menu_patch/LICENSE.hamweard.txt` | `eu4_menu_patch` — порт: последовательность очистки, сброс мира и адреса взяты оттуда |

Копии лицензий лежат в папках этих плагинов: `eu4_load_patch`, `eu4_ai_serial`, `eu4_modifier_patch`, `eu4_menu_patch`.

Не входят в репозиторий, но нужны готовой установке. Их раздаёт установщик вместе с их лицензиями:
- **EU4 Unicode Patch** — загрузчик `VERSION.dll`;
- **DXVK** — `d3d9.dll`, лицензия zlib/libpng.
