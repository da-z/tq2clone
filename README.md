# TQ2 Clone

Duplicate a **Titan Quest II** character under a new name — full level, inventory, and progression intact.

## Download

Grab `tq2clone.exe` from the [Releases](../../releases/latest) page.

## Usage

1. Run `tq2clone.exe`
2. Select the source character from the dropdown
3. Type a new character name
4. Click **Clone Character**
5. Start TQ2 — the new character appears in the selection screen

The tool aborts safely if TQ2 is already running.

## How it works

TQ2 stores each character as a set of `.sav` files in `%LOCALAPPDATA%\TQ2\Saved\SaveGames\`. The tool copies every `.sav` file belonging to the source character, renames them to the new character name, and patches the name string inside the binary GVAS save format. It also updates the three internal GrimSave size fields that must stay consistent with the name length, then removes `Saving.sav` so TQ2 regenerates its checksum manifest on next launch.

## Building from source

Requires [Zig](https://ziglang.org/).

```bat
zig rc tq2clone.rc
zig cc tq2clone.c tq2clone.res -o tq2clone.exe -O2 ^
    -lkernel32 -luser32 -lgdi32 -lcomctl32 -lshell32 ^
    "-Wl,--subsystem,windows"
```
