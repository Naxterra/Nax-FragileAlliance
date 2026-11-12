# Nax Fragile Allegiance

Nax Fragile Allegiance is meant to make *Fragile Allegiance* (Gremlin Interactive, 1996) run natively on Windows 11 without DOSBox or any other emulator. A translator turns the 32-bit x86 code of a legally owned copy into C++ ahead of time, and a native runtime will replace DOS, the DOS/4GW extender and the PC hardware with Win32 services, a Direct3D 12 presenter and XAudio2 sound.

The repository contains only the translator and the runtime. It contains no game code, graphics, sound, text or data. Every build reads the original files from a copy that the user owns, for example the GOG release, and the translated program stays on that user's machine.

## Status

The project is at its first stage. The game does not run yet.

| Part | State |
|---|---|
| Loader for the DOS/4GW bound program (`FRAGILE.EXE`) | Done. It finds the application inside the extender, places its three objects at their base addresses and applies all internal fixups. |
| Code discovery | First pass done. From the entry point, the fixup targets and the jump tables it reaches about 92 % of the 530 KB code object. |
| Translation to C++ | Not started. |
| Runtime (DOS and DPMI services, VESA graphics, timer, keyboard, mouse, sound, CD image) | Not started. |

## What the game needs from the machine

The survey of the original program shows what the runtime has to provide.

| Area | What the program uses |
|---|---|
| Memory and extender | DOS/4GW flat model, DPMI services for DOS memory, real-mode interrupts and interrupt vectors. |
| Files | DOS file services. The game data on the GOG CD image `FRAGILE.DAT` is read as drive D:. |
| Graphics | VESA SVGA with bank switching (function 4F05h) and the VGA palette and retrace ports. |
| Timing and input | The programmable interval timer, the interrupt controller, the keyboard controller and the INT 33h mouse driver. |
| Sound | The HMI sound library with its drivers `HMIDRV.386` and `HMIMDRV.386`, configured for a Sound Blaster at 220h and General MIDI at 330h. |
| Network | IPX and a serial-port handler in a small 16-bit object. Multiplayer is not planned for now. |

## Requirements

- Windows 11.
- Python 3 with the Capstone disassembler, installed with `python -m pip install capstone`.
- Your own copy of *Fragile Allegiance*, for example the GOG release.

## Trying the current stage

The survey tool prints what the loader and discovery find in your copy:

```powershell
python tools\recomp\survey.py "<your Fragile Allegiance folder>\FRAGILE.EXE"
```

The tests use small synthetic programs and need no game files:

```powershell
python -m unittest discover -s tests -v
```

## Layout

| Folder | Content |
|---|---|
| `tools/recomp` | The translator: the LE loader, code discovery and the survey tool, written in Python. |
| `tests` | Tests for the translator. |

## License

This software is under the Nax Source-Available License 1.0. You may install and use the official release builds, privately and at work, and you may read the source code. Modifying the software, redistributing it and selling it need the written permission of the owner. See [LICENSE](LICENSE).
