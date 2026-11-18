#!/usr/bin/env python3
"""Adds the Goldportal button-combo menu to ps4_plugin/source/main.c.

Run from the repository root:   python3 apply_menu_patch.py
Safe to run twice. It stops without changing anything if a spot can't be found.
"""
import pathlib
import sys

path = pathlib.Path("ps4_plugin/source/main.c")
if not path.exists():
    sys.exit("ERROR: run this from the repo root (ps4_plugin/source/main.c not found)")

raw = path.read_bytes().decode("utf-8")
nl = "\r\n" if "\r\n" in raw else "\n"
text = raw

if "menu_start();" in text:
    print("main.c already patched - nothing to do")
    sys.exit(0)

edits = [
    ("include",
     '#include "audio.h"' + nl,
     '#include "audio.h"' + nl + '#include "menu.h"' + nl),
    ("start",
     '    LOG("step: waiting for the game to load libSceUsbd',
     '    menu_start();' + nl + nl + '    LOG("step: waiting for the game to load libSceUsbd'),
    ("stop",
     '    g_completer_stop = 1;' + nl,
     '    g_completer_stop = 1;' + nl + '    menu_stop();' + nl),
    ("version",
     "g_pluginVersion = 0x0000010F;",
     "g_pluginVersion = 0x00000110;"),
]

for name, old, new in edits:
    if text.count(old) != 1:
        sys.exit(f"ERROR: could not find a unique place for edit '{name}'. main.c was NOT changed. "
                 "Use the manual edits from the page.")
    text = text.replace(old, new, 1)

path.write_bytes(text.encode("utf-8"))
print("main.c patched: menu.h included, menu_start()/menu_stop() added, version 1.16")
