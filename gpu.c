#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <stdio.h>
#include "gpu.h"
#include "memory.h"

#define SCALE 4

extern unsigned char joyDirs;
extern unsigned char joyButtons;

static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;
static SDL_Texture *texture = NULL;
static unsigned int pixels[160 * 144];
static unsigned char bgLine[160]; /* background colour numbers of the current line */

static const unsigned int shades[4] = {0xFFE0F8D0, 0xFF88C070, 0xFF346856, 0xFF081820};

int gpuInit(void)
{
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        printf("SDL error: %s\n", SDL_GetError());
        return 0;
    }
    window = SDL_CreateWindow("gbemu", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              160 * SCALE, 144 * SCALE, 0);
    if (!window)
    {
        printf("SDL error: %s\n", SDL_GetError());
        return 0;
    }

    renderer = SDL_CreateRenderer(window, -1, 0);
    if (!renderer)
    {
        printf("SDL error: %s\n", SDL_GetError());
        return 0;
    }

    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STREAMING, 160, 144);
    if (!texture)
    {
        printf("SDL error: %s\n", SDL_GetError());
        return 0;
    }
    return 1;
}

/* Draw ONE line (y = 0..143) using the settings the game has right now */
void gpuRenderLine(int y)
{
    if (y < 0 || y >= 144)
        return;

    unsigned char lcdc = readByte(0xFF40);
    unsigned int *row = &pixels[y * 160];

    if (!(lcdc & 0x80))
    { /* screen switched off: blank */
        for (int x = 0; x < 160; x++)
            row[x] = shades[0];
        return;
    }

    /* ---- Background and window ---- */
    if (lcdc & 0x01)
    {
        unsigned char scy = readByte(0xFF42);
        unsigned char scx = readByte(0xFF43);
        unsigned char wy = readByte(0xFF4A);
        int wx = readByte(0xFF4B) - 7;
        unsigned char bgp = readByte(0xFF47);

        unsigned short bgMap = (lcdc & 0x08) ? 0x9C00 : 0x9800;
        unsigned short winMap = (lcdc & 0x40) ? 0x9C00 : 0x9800;
        int unsignedTiles = (lcdc & 0x10) != 0;

        for (int x = 0; x < 160; x++)
        {
            int px, py;
            unsigned short base;

            if ((lcdc & 0x20) && y >= wy && x >= wx)
            { /* window (score bar) */
                px = x - wx;
                py = y - wy;
                base = winMap;
            }
            else
            { /* background */
                px = (x + scx) & 255;
                py = (y + scy) & 255;
                base = bgMap;
            }

            unsigned char tileIndex = readByte(base + (py / 8) * 32 + (px / 8));

            unsigned short tileAddr;
            if (unsignedTiles)
                tileAddr = 0x8000 + tileIndex * 16;
            else
                tileAddr = 0x9000 + (signed char)tileIndex * 16;

            int line = py % 8;
            unsigned char lo = readByte(tileAddr + line * 2);
            unsigned char hi = readByte(tileAddr + line * 2 + 1);

            int bit = 7 - (px % 8);
            int colour = (((hi >> bit) & 1) << 1) | ((lo >> bit) & 1);

            bgLine[x] = (unsigned char)colour;
            row[x] = shades[(bgp >> (colour * 2)) & 3];
        }
    }
    else
    {
        for (int x = 0; x < 160; x++)
        {
            bgLine[x] = 0;
            row[x] = shades[0];
        }
    }

    /* ---- Sprites on this line ---- */
    if (lcdc & 0x02)
    {
        int height = (lcdc & 0x04) ? 16 : 8;

        for (int i = 39; i >= 0; i--)
        { /* lower numbers end up on top */
            unsigned short oam = 0xFE00 + i * 4;
            int sy = readByte(oam) - 16;
            if (y < sy || y >= sy + height)
                continue; /* not on this line */

            int sx = readByte(oam + 1) - 8;
            unsigned char tile = readByte(oam + 2);
            unsigned char flags = readByte(oam + 3);
            if (height == 16)
                tile &= 0xFE;

            unsigned char pal = readByte((flags & 0x10) ? 0xFF49 : 0xFF48);

            int rowInSprite = y - sy;
            int line = (flags & 0x40) ? (height - 1 - rowInSprite) : rowInSprite; /* vertical flip */
            unsigned short addr = 0x8000 + tile * 16 + line * 2;
            unsigned char lo = readByte(addr);
            unsigned char hi = readByte(addr + 1);

            for (int col = 0; col < 8; col++)
            {
                int px = sx + col;
                if (px < 0 || px >= 160)
                    continue;

                int bit = (flags & 0x20) ? col : (7 - col); /* horizontal flip */
                int colour = (((hi >> bit) & 1) << 1) | ((lo >> bit) & 1);
                if (colour == 0)
                    continue; /* transparent */
                if ((flags & 0x80) && bgLine[px] != 0)
                    continue; /* behind background */

                row[px] = shades[(pal >> (colour * 2)) & 3];
            }
        }
    }
}

/* Keyboard: arrows = D-pad, Z = A, X = B, Enter = Start, Backspace = Select */
static void readKeys(void)
{
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    joyDirs = 0;
    joyButtons = 0;
    if (k[SDL_SCANCODE_RIGHT])
        joyDirs |= 0x01;
    if (k[SDL_SCANCODE_LEFT])
        joyDirs |= 0x02;
    if (k[SDL_SCANCODE_UP])
        joyDirs |= 0x04;
    if (k[SDL_SCANCODE_DOWN])
        joyDirs |= 0x08;
    if (k[SDL_SCANCODE_Z])
        joyButtons |= 0x01;
    if (k[SDL_SCANCODE_X])
        joyButtons |= 0x02;
    if (k[SDL_SCANCODE_BACKSPACE])
        joyButtons |= 0x04;
    if (k[SDL_SCANCODE_RETURN])
        joyButtons |= 0x08;
}

/* Show the finished picture (the lines were drawn one by one by gpuRenderLine) */
int gpuFrame(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e))
    {
        if (e.type == SDL_QUIT)
            return 0;
    }

    readKeys();

    SDL_UpdateTexture(texture, NULL, pixels, 160 * sizeof(unsigned int));
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);

    SDL_Delay(16);
    return 1;
}

void gpuQuit(void)
{
    if (texture)
        SDL_DestroyTexture(texture);
    if (renderer)
        SDL_DestroyRenderer(renderer);
    if (window)
        SDL_DestroyWindow(window);
    SDL_Quit();
}