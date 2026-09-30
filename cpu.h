#ifndef CPU_H
#define CPU_H

void cpuReset(void);
void cpuStep(void);
void printRegisters(void);
void printTrace(void);

unsigned char getLY(void);
unsigned char getStatBits(void);
unsigned char getDIV(void);
void resetDIV(void);

#endif