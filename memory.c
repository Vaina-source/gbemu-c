#include <stdio.h>
#include "memory.h"
#include "cpu.h"

#define ROM_MAX 0x200000

static unsigned char rom[ROM_MAX];
static unsigned char memory[0x10000];
static unsigned long romSize = 0;
static int romBank = 1;

/* Filled in by gpu.c from the keyboard. 1 = pressed.
   joyDirs bits:    0=Right 1=Left 2=Up 3=Down
   joyButtons bits: 0=A 1=B 2=Select 3=Start */
unsigned char joyDirs = 0;
unsigned char joyButtons = 0;

int loadRom(const char *filename)
{
    FILE *f = fopen(filename, "rb");
    if (!f)
    {
        printf("Could not open ROM: %s\n", filename);
        return 0;
    }
    romSize = fread(rom, 1, ROM_MAX, f);
    fclose(f);

    char title[17] = {0};
    for (int i = 0; i < 16; i++)
        title[i] = rom[0x134 + i];
    printf("Loaded ROM: %s (%lu bytes, cartridge type 0x%02X)\n",
           title, romSize, rom[0x147]);

    romBank = 1;
    return 1;
}

unsigned char readByte(unsigned short address)
{
    if (address < 0x4000)
        return rom[address];

    if (address < 0x8000)
        return rom[romBank * 0x4000 + (address - 0x4000)];

    if (address == 0xFF00)
    { /* joypad */
        unsigned char sel = memory[0xFF00];
        unsigned char r = 0xCF | (sel & 0x30);
        if (!(sel & 0x10))
            r &= (unsigned char)(0xF0 | (~joyDirs & 0x0F));
        if (!(sel & 0x20))
            r &= (unsigned char)(0xF0 | (~joyButtons & 0x0F));
        return r;
    }

    if (address == 0xFF04) /* DIV: the free-running counter */
        return getDIV();

    if (address == 0xFF41) /* STAT: enable bits + live screen status */
        return (unsigned char)((memory[0xFF41] & 0x78) | getStatBits());

    if (address == 0xFF44) /* LY: current screen line */
        return getLY();

    return memory[address];
}

void writeByte(unsigned short address, unsigned char value)
{
    if (address < 0x8000)
    {
        if (address >= 0x2000 && address < 0x4000)
        {
            romBank = value & 0x1F;
            if (romBank == 0)
                romBank = 1;

            unsigned long numBanks = romSize / 0x4000;
            if (numBanks >= 2)
                romBank &= (int)(numBanks - 1);
        }
        return;
    }

    if (address == 0xFF04)
    { /* writing anything resets DIV */
        resetDIV();
        return;
    }

    if (address == 0xFF44) /* LY is read-only */
        return;

    if (address == 0xFF46)
    { /* DMA: copy 160 bytes into sprite table */
        for (int i = 0; i < 160; i++)
            memory[0xFE00 + i] = readByte((unsigned short)((value << 8) + i));
    }

    memory[address] = value;
}