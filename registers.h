#ifndef REGISTERS_H
#define REGISTERS_H

struct registers {
    union { struct { unsigned char f; unsigned char a; }; unsigned short af; };
    union { struct { unsigned char c; unsigned char b; }; unsigned short bc; };
    union { struct { unsigned char e; unsigned char d; }; unsigned short de; };
    union { struct { unsigned char l; unsigned char h; }; unsigned short hl; };
    unsigned short sp;
    unsigned short pc;
};

extern struct registers registers;

/* The F register holds four flags in its top 4 bits */
#define FLAG_Z 0x80  /* result was zero      */
#define FLAG_N 0x40  /* last op was subtract */
#define FLAG_H 0x20  /* half carry           */
#define FLAG_C 0x10  /* carry                */

#endif