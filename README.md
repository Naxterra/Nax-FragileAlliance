# Nax Fragile Allegiance

Nax Fragile Allegiance makes *Fragile Allegiance* (Gremlin Interactive, 1996) run natively on Windows 11 without DOSBox or any other emulator. A translator turns the 32-bit x86 code of a legally owned copy into C++ ahead of time. Every place where the original program talks to DOS, the DOS/4GW extender, the BIOS or the PC hardware is replaced by native Windows code.

The repository contains only the translator, the runtime and the native replacements. It contains no game code, graphics, sound, text or data. Every build reads the original files from a copy that the user owns, for example the GOG release, and the translated program stays on that user's machine.

## Status

The game starts natively on Windows 11, shows its title screen and menus in a Direct3D 12 window, plays its music, speech and sound effects through XAudio2 and can be played: a preset game such as "Beginner's Luck" starts and runs on the strategic map. The two intro movies are not replaced yet, and only the start of a game has been tried so far.

| Part | State |
|---|---|
| Loader, code discovery and translation to C++ | Done for the whole program. Original functions, or single instructions, can be replaced by native C++ code. |
| CPU core (registers, flags, x87, the 4 GB guest address space, the dispatcher) | Done. |
| Start-up, memory, files, clock, country settings, timer | Native Windows replacements. |
| Graphics | Native. The picture is shown through Direct3D 12, in a window or borderless fullscreen (Alt+Enter), and F12 saves a screenshot. F11 switches between AI upscaling (the default), sharp pixels, smooth edges and a CRT look. |
| AI upscaling | Every frame is enlarged four times by a Real-ESRGAN neural network that runs live on the graphics card, so the terrain, buildings, ships and text are all redrawn by it. The network is computed again only where the picture changed. It needs Upscayl, whose compact model the game reads (see Requirements). |
| Keyboard and mouse | Native. Windows messages feed the game's own key table and mouse handler. |
| Network | Reported as absent, so the game runs as a single-player game. |
| Sound | Native. Music, speech and sound effects are mixed in software and played through XAudio2. |
| Enhanced textures | Panels, buttons, icons and other sprites can be replaced by enlarged versions from a texture pack that you make from your own copy (see below). F10 switches between enhanced and original textures. |
| Intro movies (GDV) | Not replaced yet. They are skipped. |

## How the game runs on Windows

There is no DOS, no DOS extender and no emulated PC hardware. Every place where the original program would call DOS, the extender, the BIOS or a hardware port is replaced by a native function, listed in `games\fragile\overrides.txt`. Most replacements cover a whole function of the C library or of the game, such as `open`, `read` or the timer set-up. Where such a call sits inside the game's own logic, only that one instruction is replaced. If the program reaches a DOS call, an extender call or a port access that has no replacement yet, it stops with a message that names the original function.

| Area | Native replacement |
|---|---|
| Start-up | The command line, the environment and the library's start-up variables come from Windows. |
| Memory | Heap blocks come from the 4 GB guest address space. |
| Files | Drive C: is the game folder and drive D: is the CD image `FRAGILE.DAT`, which is read directly. Every write goes to `%LOCALAPPDATA%\Nax Fragile Allegiance\C`, first into a temporary file that replaces the previous version only when the game closes the file. The original files are never changed. Save games that GOG keeps in `cloud_saves` are read as well. |
| Clock and timer | The game's timer interrupt handler is called at its own rate from the Windows performance counter. |
| Graphics | Video memory is a Windows memory section. The game's read and write windows are 4 MB views of it, and a bank switch maps a different part of the section into a window, as a VESA card's memory mapping did, so nothing is copied. The window thread shows the section through Direct3D 12. Palette writes go to the display directly. |
| Keyboard and mouse | Windows key messages are fed into the game's key table the way its keyboard interrupt handler did, and the game's mouse handler is called for every mouse message, with the game pointer kept exactly under the Windows pointer. |
| Sound | The game's calls to the HMI sound library start, continue, stop and fade samples in a software mixer, which reads the sample data straight from the game's memory and resamples it to 44.1 kHz with interpolation. The game streams music and speech from its WAV files in parts and hands over each next part from a callback, which is called at the end of every part as the Sound Blaster driver did. The mixer runs on the game's own 120 Hz sound tick and keeps about 60 ms of sound queued at XAudio2. |
| AI upscaling | The compact Real-ESRGAN network (SRVGGNetCompact: 34 convolutions of 3x3 with 64 channels, PReLU, a 4x pixel shuffle and the input enlarged by nearest neighbour added on top) runs as Direct3D 12 compute shaders, with its activations kept as 16-bit floats. The weights come from Upscayl's `upscayl-lite-4x` model and are read from the Upscayl installation at start. The picture edges are mirrored as Upscayl does, so the result matches Upscayl's own output. Each frame is compared with the last one in 16x16 tiles, and only the changed areas, widened by the 34 pixels a change can reach through the network, are computed again. |
| Enhanced textures | The game keeps everything it shows on top of its backgrounds as objects in one display list, which its sprite engine redraws into a picture buffer and copies to the screen. After each copy, native code reads that list and identifies every sprite frame by a hash of its content. The window thread draws the enhanced version of each frame over the picture in the list's order, and on each pixel only while the screen still shows that frame's original colour there, so text, boxes and anything else drawn over a sprite stay visible. Each pixel follows the change of its palette entry since the enhanced picture was made, which keeps fades and colour cycling working. |
| Memory layout | The program is loaded 4 MB above its link addresses, as DOS/4GW loaded it, so the first 640 KB stay free for the game's fixed data. |

## What the game still needs

| Area | What the original program uses |
|---|---|
| Movies | The GDV movie player draws in VGA mode X, one plane at a time. |
| Network | IPX, NetBIOS and a serial-port handler. Multiplayer is not planned for now. |

## Requirements

- Windows 11.
- Python 3 with the Capstone disassembler, installed with `python -m pip install capstone`.
- Visual Studio Build Tools with the C++ workload, installed with `winget install Microsoft.VisualStudio.BuildTools`.
- Your own copy of *Fragile Allegiance*, for example the GOG release.
- For AI upscaling and texture packs: Upscayl, installed with `winget install Upscayl.Upscayl`. Without it the game runs with the other filters.

## Building and running

`build.ps1` translates `FRAGILE.EXE` from the folder that the GOG installer recorded, or from `-GameFolder <folder>`, and builds `build\fragile\FragileAllegiance.exe`. Only sources that changed are compiled again; `-Clean` compiles everything.

```powershell
.\build.ps1
.\build\fragile\FragileAllegiance.exe
```

The program finds the game folder the same way, or takes `--data <folder>`. It writes a log to `%LOCALAPPDATA%\Nax Fragile Allegiance\runtime.log`.

## Making a texture pack

AI upscaling needs no preparation. A texture pack is an optional extra on top of it: panels, buttons and other unscaled sprites are then drawn from pictures enlarged ahead of time with a larger and slower Real-ESRGAN model, which keeps more of the original art's grain than the live network. The enhanced textures are made on your computer from your own copy of the game, so they are not part of this repository. Start the game once with `--dump-textures` and visit the screens you want enhanced: every sprite frame that appears is saved as a PNG file in `%LOCALAPPDATA%\Nax Fragile Allegiance\textures\dump`. Then enlarge the new pictures four times with an AI upscaler and add them to the pack:

```powershell
winget install Upscayl.Upscayl
.\build\fragile\FragileAllegiance.exe --dump-textures
.\tools\make-texture-pack.ps1
```

The script uses the Real-ESRGAN models that Upscayl installs and runs on the graphics card. `-Model` chooses another model (`high-fidelity-4x` is the default and keeps the grain of the original art, while `digital-art-4x` paints smoother surfaces), and `-Rebuild` enlarges every picture again. The pack lives in `textures\pack` next to the dump. A running game looks for new or changed pack pictures every few seconds, so there is no need to restart it. Any picture in the pack can also be edited or replaced by hand at any size, as long as it keeps the transparent margin of the dumped picture, enlarged in proportion.

Enlarging keeps the art as it is. `tools\redraw-textures.py` goes further and has Stable Diffusion XL with the tile ControlNet repaint every dumped picture at four times its size, which adds detail such as rock texture and machined metal while the broad colours and shapes of the original stay. Large pictures are repainted in overlapping tiles. The asteroid surface is a new picture for every asteroid, so the tool can keep running with `--watch` while you play: it repaints each new dump within a minute or so, and the game swaps the picture in when it is ready. It needs a Python with PyTorch (CUDA), diffusers, transformers, accelerate, numpy and Pillow, and the models stabilityai/stable-diffusion-xl-base-1.0 (fp16), xinsir/controlnet-tile-sdxl-1.0 and madebyollin/sdxl-vae-fp16-fix in the Hugging Face cache that `HF_HOME` names:

```powershell
python -I tools\redraw-textures.py --watch
```

`--strength` sets how much SDXL may change (higher adds more new detail), `--control` how closely it follows the original, and `--rebuild` repaints pictures that were repainted before.

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
| `runtime` | The CPU core that translated code runs on, the Windows host, the file layer, the CD image reader, the sound mixer and the texture packs. |
| `games/fragile` | The native replacements for Fragile Allegiance and the list of replaced functions. |
| `tests` | Tests for the translator, the CPU core, the file layer, the sound mixer, the texture packs and the AI upscaler. |

## License

This software is under the Nax Source-Available License 1.0. You may install and use the official release builds, privately and at work, and you may read the source code. Modifying the software, redistributing it and selling it need the written permission of the owner. See [LICENSE](LICENSE).
