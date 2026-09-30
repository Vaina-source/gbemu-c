#include <stdio.h>
#include "cpu.h"
#include "gpu.h"
#include "memory.h"
#include "registers.h"

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        printf("Usage: gbemu <rom file>\n");
        return 1;
    }
    if (!loadRom(argv[1]))
        return 1;
    if (!gpuInit())
        return 1;

    cpuReset();

    unsigned char lastLY = 0;
    int running = 1;

    while (running)
    {
        cpuStep();

        /* If the CPU wanders outside ROM / high RAM, something is wrong */
        if (registers.pc >= 0x8000 && registers.pc < 0xFF80)
        {
            printf("\n*** PC left the ROM ***\n");
            printTrace();
            printRegisters();
            gpuQuit();
            return 1;
        }

        /* Draw one frame each time the screen line reaches 144 (start of VBlank) */
        unsigned char ly = getLY();
        if (ly == 144 && lastLY != 144)
        {
            running = gpuFrame();
        }
        lastLY = ly;
    }

    gpuQuit();
    return 0;
}