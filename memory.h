#ifndef MEMORY_H
#define MEMORY_H

int loadRom(const char *filename);
unsigned char readByte(unsigned short address);
void writeByte(unsigned short address, unsigned char value);

#endif