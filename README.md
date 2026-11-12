# Nax Fragile Allegiance

Nax Fragile Allegiance makes *Fragile Allegiance* (Gremlin Interactive, 1996) run natively on Windows 11 without DOSBox or any other emulator. A translator turns the 32-bit x86 code of a legally owned copy into C++ ahead of time. Every place where the original program talks to DOS, the DOS/4GW extender, the BIOS or the PC hardware is replaced by native Windows code.

The repository contains only the translator, the runtime and the native replacements. It contains no game code, graphics, sound, text or data. Every build reads the original files from a copy that the user owns, for example the GOG release, and the translated program stays on that user's machine.

## Status

The game does not show a picture yet. Its own start-up code runs natively on Windows: the C library starts, the game prints its start-up messages, reads its configuration, sets up its memory and starts its timer. It stops when it reaches the graphics module, which is the next part to replace.

| Part | State |
|---|---|
| Loader for the DOS/4GW bound program (`FRAGILE.EXE`) | Done. |
| Code discovery | Done. Proven control flow comes first. A fixup target inside code is accepted only when it decodes as plausible code, and instructions that the original code hides inside other instructions are kept. |
| Translation to C++ | Done for the whole program. Original functions, or single instructions, can be replaced by native C++ code. |
| CPU core (registers, flags, x87, the 4 GB guest address space, the dispatcher) | Done. |
| Start-up, memory, files, clock, country settings, timer | Native Windows replacements. |
| Graphics, keyboard and mouse input, sound | Not replaced yet. |

## How the game runs on Windows

There is no DOS, no DOS extender and no emulated PC hardware. Every place where the original program would call DOS, the extender, the BIOS or a hardware port is replaced by a native function, listed in `games\fragile\overrides.txt`. Most replacements cover a whole function of the C library or of the game, such as `open`, `read` or the timer set-up. Where such a call sits inside the game's own logic, only that one instruction is replaced. If the program reaches a DOS call, an extender call or a port access that has no replacement yet, it stops with a message that names the original function.

| Area | Native replacement |
|---|---|
| Start-up | The command line, the environment and the library's start-up variables come from Windows. |
| Memory | Heap blocks come from the 4 GB guest address space. |
| Files | Drive C: is the game folder and drive D: is the CD image `FRAGILE.DAT`, which is read directly. Every write goes to `%LOCALAPPDATA%\Nax Fragile Allegiance\C`, first into a temporary file that replaces the previous version only when the game closes the file. The original files are never changed. Save games that GOG keeps in `cloud_saves` are read as well. |
| Clock and timer | The game's timer interrupt handler is called at its own rate from the Windows performance counter. |

## What the game still needs

| Area | What the original program uses |
|---|---|
| Graphics | VESA SVGA with bank switching (function 4F05h) and the VGA palette and retrace ports. |
| Input | The game's keyboard handler (its key table is already filled natively once a window exists) and the INT 33h mouse driver. |
| Sound | The HMI sound library with its drivers `HMIDRV.386` and `HMIMDRV.386`, configured for a Sound Blaster at 220h and General MIDI at 330h. |
| Network | IPX and a serial-port handler in a small 16-bit object. Multiplayer is not planned for now. |

## Requirements

- Windows 11.
- Python 3 with the Capstone disassembler, installed with `python -m pip install capstone`.
- Visual Studio Build Tools with the C++ workload, installed with `winget install Microsoft.VisualStudio.BuildTools`.
- Your own copy of *Fragile Allegiance*, for example the GOG release.

## Building and running

`build.ps1` translates `FRAGILE.EXE` from the folder that the GOG installer recorded, or from `-GameFolder <folder>`, and builds `build\fragile\FragileAllegiance.exe`. Only sources that changed are compiled again; `-Clean` compiles everything.

```powershell
.\build.ps1
.\build\fragile\FragileAllegiance.exe
```

The program finds the game folder the same way, or takes `--data <folder>`. It writes a log to `%LOCALAPPDATA%\Nax Fragile Allegiance\runtime.log`.

## Looking at the original program

The survey tool prints what the loader and discovery find in your copy, and the listing tool prints the original instructions of a function:

```powershell
python tools\recomp\survey.py "<your Fragile Allegiance folder>\FRAGILE.EXE"
python tools\recomp\listing.py "<your Fragile Allegiance folder>\FRAGILE.EXE" 70564
```

## Tests

The tests use small synthetic programs and a small built-in CD image, so they need no game files. The tests in `tests\test_emit32.py` and `tests\test_runtime.py` compile C++ and need the C++ build tools:

```powershell
python -m unittest discover -s tests -v
```

## Layout

| Folder | Content |
|---|---|
| `tools/recomp` | The translator: the LE loader, code discovery, the C++ generator, the survey and the listing tool, written in Python. |
| `runtime` | The CPU core that translated code runs on, the Windows host, the file layer and the CD image reader. |
| `games/fragile` | The native replacements for Fragile Allegiance and the list of replaced functions. |
| `tests` | Tests for the translator, the CPU core and the file layer. |

## License

This software is under the Nax Source-Available License 1.0. You may install and use the official release builds, privately and at work, and you may read the source code. Modifying the software, redistributing it and selling it need the written permission of the owner. See [LICENSE](LICENSE).
