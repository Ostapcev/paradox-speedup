#!/bin/sh
# Rebuild data/rva_map.json and include/eu4_gog_table.inc after plugin sources change.
# Usage: sh eu4_port/tools/regen.sh STEAM_EXE GOG_EXE   (from the repository root)
set -e
python eu4_port/tools/map_rvas.py "$1" "$2" eu4_port/data/functions.json --scan . eu4_port/data/rva_map.json
python eu4_port/tools/make_table.py "$1" "$2"
python eu4_port/tools/check_table.py "$1" "$2"
