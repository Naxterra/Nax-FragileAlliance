# Nax Fragile Allegiance

Nax Fragile Allegiance makes *Fragile Allegiance* (Gremlin Interactive, 1996) run natively on Windows 11 without DOSBox or any other emulator. A translator turns the 32-bit x86 code of a legally owned copy into C++ ahead of time. Every place where the original program talks to DOS, the DOS/4GW extender, the BIOS or the PC hardware is replaced by native Windows code.

The repository contains only the translator, the runtime and the native replacements. It contains no game code, graphics, sound, text or data. Every build reads the original files from a copy that the user owns, for example the GOG release, and the translated program stays on that user's machine.

## Status

The game starts natively on Windows 11 and shows its title screen and main menu in a Direct3D 12 window, and mouse clicks reach the menu. Choosing a menu entry can still stop the program, and sound and the two intro movies are not replaced yet, so the game is not playable yet.

| Part | State |
|---|---|
| Loader, code discovery and translation to C++ | Done for the whole program. Original functions, or single instructions, can be replaced by native C++ code. |
| CPU core (registers, flags, x87, the 4 GB guest address space, the dispatcher) | Done. |
| Start-up, memory, files, clock, country settings, timer | Native Windows replacements. |
| Graphics | Native. The picture is shown through Direct3D 12 with three filters (F11), in a window or borderless fullscreen (Alt+Enter), and F12 saves a screenshot. |
| Keyboard and mouse | Native. Windows messages feed the game's own key table and mouse handler. |
| Network | Reported as absent, so the game runs as a single-player game. |
| Sound | Not replaced yet. The game runs without sound. |
| Intro movies (GDV) | Not replaced yet. They are skipped. |

## How the game runs on Windows

There is no DOS, no DOS extender and no emulated PC hardware. Every place where the original program would call DOS, the extender, the BIOS or a hardware port is replaced by a native function, listed in `games\fragile\overrides.txt`. Most replacements cover a whole function of the C library or of the game, such as `open`, `read` or the timer set-up. Where such a call sits inside the game's own logic, only that one instruction is replaced. If the program reaches a DOS call, an extender call or a port access that has no replacement yet, it stops with a message that names the original function.

| Area | Native replacement |
|---|---|
| Start-up | The command line, the environment and the library's start-up variables come from Windows. |
| Memory | Heap blocks come from the 4 GB guest address space. |
| Files | Drive C: is the game folder and drive D: is the CD image `FRAGILE.DAT`, which is read directly. Every write goes to `%LOCALAPPDATA%\Nax Fragile Allegiance\C`, first into a temporary file that replaces the previous version only when the game closes the file. The original files are never changed. Save games that GOG keeps in `cloud_saves` are read as well. |
| Clock and timer | The game's timer interrupt handler is called at its own rate from the Windows performance counter. |
| Graphics | The game's 64 KB VESA bank window at A0000h is backed by a framebuffer. A bank switch swaps the window with the framebuffer, as the video card's memory mapping did, and the window thread shows the framebuffer through Direct3D 12. Palette writes go to the display directly. |
| Keyboard and mouse | Windows key messages are fed into the game's key table the way its keyboard interrupt handler did, and the game's mouse handler is called for every mouse message, with the game pointer kept exactly under the Windows pointer. |
| Memory layout | The program is loaded 4 MB above its link addresses, as DOS/4GW loaded it, so the first 640 KB stay free for the game's fixed data. |

## What the game still needs

| Area | What the original program uses |
|---|---|
| Sound | The HMI sound library with its driver `HMIDRV.386`, configured for a Sound Blaster at 220h. |
| Movies | The GDV movie player draws in VGA mode X, one plane at a time. |
| Network | IPX, NetBIOS and a serial-port handler. Multiplayer is not planned for now. |

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
