# TQ2 Clone

Duplicate a **Titan Quest II** character under a new name — full level, inventory, and progression intact.

## Downloads

Grab the latest release from the [Releases](../../releases/latest) page.

| File | Description |
|---|---|
| `tq2clone_gui.exe` | Windows GUI — recommended |
| `tq2clone.exe` | Command-line tool |

## Usage

### GUI (`tq2clone_gui.exe`)

1. Run `tq2clone_gui.exe`
2. Select the source character from the dropdown
3. Type a new character name
4. Click **Clone Character**
5. Start TQ2 — the new character appears in the selection screen

### CLI (`tq2clone.exe`)

```
tq2clone.exe <SourceName> <NewName>
```

Example:

```
tq2clone.exe Andreas AndreasBackup
```

## How it works

TQ2 stores each character as a set of `.sav` files in:

```
%LOCALAPPDATA%\TQ2\Saved\SaveGames\
```

The tool copies every `.sav` file belonging to the source character, renames them to the new character name, and patches the name string inside the binary GVAS save format. It also updates the three internal GrimSave size fields that must stay consistent with the name length, then removes `Saving.sav` so TQ2 regenerates its checksum manifest on next launch.

Both tools abort safely if TQ2 is already running.

## Building from source

Requires [Zig](https://ziglang.org/) (or MSVC).

```bat
rem GUI
zig cc tq2clone_gui.c -o tq2clone_gui.exe -O2 ^
    -lkernel32 -luser32 -lgdi32 -lcomctl32 -lshell32 -ladvapi32 ^
    -Wl,--subsystem,windows

rem CLI
zig cc tq2clone.c -o tq2clone.exe -O2 -lkernel32
```
