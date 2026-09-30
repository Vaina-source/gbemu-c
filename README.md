# gbemu

A simple Game Boy emulator written in C, built step by step as a learning project by following the
[Cinoop write-up by CTurt](https://cturt.github.io/cinoop.html).

It runs Super Mario Land (level 1-1 is playable) and other small games that don't need special cartridge hardware.

## What works

- Game Boy CPU: all common instructions plus the `0xCB` prefix group
- Cartridge banking (MBC1, ROM only)
- Interrupts (VBlank, STAT/LYC, timer) and the DIV/TIMA timer
- Line-by-line graphics: background, window layer and sprites, drawn with SDL2
- Keyboard input

## What doesn't work yet

- Sound
- Other cartridge types (MBC3, MBC5) and battery saves
- Exact timing (speed is approximate)

## Build

You need a C compiler (gcc) and SDL2.

**Windows (MSYS2 UCRT64):**

```
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-SDL2
```

**Linux:**

```
sudo apt install build-essential libsdl2-dev
```

Then compile:

```
gcc -std=c11 -Wall main.c cpu.c memory.c gpu.c -o gbemu -lSDL2
```

## Run

You must supply your own ROM file (none is included):

```
./gbemu yourgame.gb
```

On Windows PowerShell use `.\gbemu.exe yourgame.gb`.

## Controls

| Key | Game Boy button |
|---|---|
| Arrow keys | D-pad |
| Z | A |
| X | B |
| Enter | Start |
| Backspace | Select |

## Files

| File | Purpose |
|---|---|
| `main.c` | Starts everything and runs the main loop |
| `cpu.c` / `cpu.h` | Instructions, interrupts, clock and timer |
| `memory.c` / `memory.h` | Memory, ROM loading, bank switching, joypad |
| `gpu.c` / `gpu.h` | SDL2 window and drawing |
| `registers.h` | CPU registers and flags |

## Legal note

This project contains no game code. Only use ROMs you own or free homebrew games.

## Credits

Based on the ideas in [Cinoop](https://cturt.github.io/cinoop.html) by CTurt.