#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "cpu.h"
#include "gpu.h"
#include "memory.h"
#include "registers.h"

struct registers registers;

struct instruction
{
    const char *disassembly;
    unsigned char operandLength;
    void (*execute)(unsigned short operand);
};

static int ime = 0;       /* interrupt master enable */
static int eiPending = 0; /* EI takes effect one instruction late */
static int halted = 0;
static int lastCost = 8; /* cycles used by the instruction just run */

static unsigned long cycles = 0; /* the clock */
static unsigned long divCounter = 0;
static unsigned long timerCounter = 0;
static unsigned char divReg = 0;

static const char *regNames[8] = {"B", "C", "D", "E", "H", "L", "(HL)", "A"};
static const char *aluNames[8] = {"ADD A,", "ADC A,", "SUB", "SBC A,", "AND", "XOR", "OR", "CP"};
static const char *rrNames[4] = {"BC", "DE", "HL", "SP"};
static const char *ccNames[4] = {"NZ", "Z", "NC", "C"};

/* ---- Trace: remembers the last 40 instructions ---- */
#define TRACE_LINES 40
static char traceBuf[TRACE_LINES][48];
static unsigned long traceCount = 0;

static void trace(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(traceBuf[traceCount % TRACE_LINES], sizeof traceBuf[0], fmt, args);
    va_end(args);
    traceCount++;
}

void printTrace(void)
{
    unsigned long start = (traceCount > TRACE_LINES) ? traceCount - TRACE_LINES : 0;
    for (unsigned long i = start; i < traceCount; i++)
        printf("%s\n", traceBuf[i % TRACE_LINES]);
}

/* ---- Small helpers the memory and screen code use ---- */
unsigned char getLY(void)
{
    return (unsigned char)((cycles / 456) % 154);
}

unsigned char getDIV(void) { return divReg; }
void resetDIV(void)
{
    divReg = 0;
    divCounter = 0;
}

/* Live part of the STAT register: mode (bits 0-1) and "LY equals LYC" (bit 2) */
unsigned char getStatBits(void)
{
    unsigned long line = (cycles / 456) % 154;
    unsigned long pos = cycles % 456;
    unsigned char mode;
    if (line >= 144)
        mode = 1; /* VBlank */
    else if (pos < 80)
        mode = 2;
    else if (pos < 252)
        mode = 3;
    else
        mode = 0; /* HBlank */
    unsigned char coincidence = (line == readByte(0xFF45)) ? 0x04 : 0;
    return (unsigned char)(0x80 | coincidence | mode);
}

/* ---- Interrupt requests ---- */
static void requestInterrupt(int bit)
{
    writeByte(0xFF0F, (unsigned char)(readByte(0xFF0F) | (1 << bit)));
}

/* Ask for a STAT interrupt only if the game enabled that kind (mask = its STAT bit) */
static void statInterrupt(unsigned char mask)
{
    if (readByte(0xFF41) & mask)
        requestInterrupt(1);
}

/* ---- The clock: time passes, the screen advances, the timer counts ---- */
static void advance(int c)
{
    unsigned long before = cycles;
    cycles += c;

    /* DIV counts up by 1 every 256 cycles */
    divCounter += c;
    while (divCounter >= 256)
    {
        divCounter -= 256;
        divReg++;
    }

    /* TIMA: the game's programmable stopwatch */
    unsigned char tac = readByte(0xFF07);
    if (tac & 0x04)
    { /* timer switched on */
        static const unsigned long periods[4] = {1024, 16, 64, 256};
        unsigned long period = periods[tac & 3];
        timerCounter += c;
        while (timerCounter >= period)
        {
            timerCounter -= period;
            unsigned char tima = readByte(0xFF05);
            if (tima == 0xFF)
            {
                writeByte(0xFF05, readByte(0xFF06)); /* reload from TMA */
                requestInterrupt(2);                 /* timer interrupt */
            }
            else
            {
                writeByte(0xFF05, (unsigned char)(tima + 1));
            }
        }
    }

    /* The screen: 154 lines of 456 cycles each */
    unsigned long lineBefore = (before / 456) % 154;
    unsigned long lineNow = (cycles / 456) % 154;
    unsigned long posBefore = before % 456;
    unsigned long posNow = cycles % 456;

    if (lineNow != lineBefore)
    { /* a new line has begun */
        if (lineNow == readByte(0xFF45))
            statInterrupt(0x40); /* LY == LYC */
        if (lineNow < 144)
            statInterrupt(0x20); /* start of line */
        if (lineNow == 144)
        {
            requestInterrupt(0); /* VBlank */
            statInterrupt(0x10);
        }
    }
    else if (lineNow < 144 && posBefore < 252 && posNow >= 252)
    {
        gpuRenderLine((int)lineNow); /* draw this line now */
        statInterrupt(0x08);         /* HBlank */
    }
}

/* Roughly how many cycles each kind of instruction takes */
static int cyclesFor(unsigned char op)
{
    if (op >= 0x40 && op <= 0xBF)
        return (((op & 7) == 6) || (op < 0x80 && ((op >> 3) & 7) == 6)) ? 8 : 4;

    switch (op)
    {
    case 0x00:
    case 0x76:
    case 0xE9:
        return 4;
    case 0xCD:
    case 0xC4:
    case 0xCC:
    case 0xD4:
    case 0xDC:
        return 24;
    case 0xC9:
    case 0xD9:
    case 0xC3:
    case 0xEA:
    case 0xFA:
        return 16;
    case 0x18:
    case 0x20:
    case 0x28:
    case 0x30:
    case 0x38:
        return 12;
    case 0xE0:
    case 0xF0:
        return 12;
    case 0xCB:
        return 8;
    }
    if ((op & 0xCF) == 0xC5)
        return 16; /* PUSH */
    if ((op & 0xCF) == 0xC1)
        return 12; /* POP  */
    if ((op & 0xC7) == 0xC7)
        return 16; /* RST  */
    if ((op & 0xC7) == 0x04 || (op & 0xC7) == 0x05)
        return (((op >> 3) & 7) == 6) ? 12 : 4; /* INC/DEC r */
    if ((op & 0xCF) == 0x01)
        return 12; /* LD rr, nn */
    return 8;
}

/* ---- Helpers: registers by number (B,C,D,E,H,L,(HL),A = 0..7) ---- */
static unsigned char getReg(int i)
{
    switch (i)
    {
    case 0:
        return registers.b;
    case 1:
        return registers.c;
    case 2:
        return registers.d;
    case 3:
        return registers.e;
    case 4:
        return registers.h;
    case 5:
        return registers.l;
    case 6:
        return readByte(registers.hl);
    default:
        return registers.a;
    }
}

static void setReg(int i, unsigned char v)
{
    switch (i)
    {
    case 0:
        registers.b = v;
        break;
    case 1:
        registers.c = v;
        break;
    case 2:
        registers.d = v;
        break;
    case 3:
        registers.e = v;
        break;
    case 4:
        registers.h = v;
        break;
    case 5:
        registers.l = v;
        break;
    case 6:
        writeByte(registers.hl, v);
        break;
    default:
        registers.a = v;
        break;
    }
}

static unsigned short getRR(int i)
{
    switch (i)
    {
    case 0:
        return registers.bc;
    case 1:
        return registers.de;
    case 2:
        return registers.hl;
    default:
        return registers.sp;
    }
}

static void setRR(int i, unsigned short v)
{
    switch (i)
    {
    case 0:
        registers.bc = v;
        break;
    case 1:
        registers.de = v;
        break;
    case 2:
        registers.hl = v;
        break;
    default:
        registers.sp = v;
        break;
    }
}

static void pushWord(unsigned short value)
{
    registers.sp -= 2;
    writeByte(registers.sp, value & 0xFF);
    writeByte(registers.sp + 1, value >> 8);
}

static unsigned short popWord(void)
{
    unsigned short value = readByte(registers.sp) | (readByte(registers.sp + 1) << 8);
    registers.sp += 2;
    return value;
}

static int condition(int cc)
{
    switch (cc)
    {
    case 0:
        return !(registers.f & FLAG_Z);
    case 1:
        return (registers.f & FLAG_Z) != 0;
    case 2:
        return !(registers.f & FLAG_C);
    default:
        return (registers.f & FLAG_C) != 0;
    }
}

static unsigned char inc8(unsigned char v)
{
    unsigned char r = v + 1;
    registers.f &= FLAG_C;
    if (r == 0)
        registers.f |= FLAG_Z;
    if ((v & 0x0F) == 0x0F)
        registers.f |= FLAG_H;
    return r;
}

static unsigned char dec8(unsigned char v)
{
    unsigned char r = v - 1;
    registers.f = (registers.f & FLAG_C) | FLAG_N;
    if (r == 0)
        registers.f |= FLAG_Z;
    if ((v & 0x0F) == 0)
        registers.f |= FLAG_H;
    return r;
}

/* ADD ADC SUB SBC AND XOR OR CP */
static void alu(int op, unsigned char n)
{
    unsigned char a = registers.a;
    unsigned int r;
    int c = (registers.f & FLAG_C) ? 1 : 0;

    switch (op)
    {
    case 0:
    case 1:
    {
        int carryIn = (op == 1) ? c : 0;
        r = a + n + carryIn;
        registers.f = 0;
        if ((r & 0xFF) == 0)
            registers.f |= FLAG_Z;
        if (((a & 0x0F) + (n & 0x0F) + carryIn) > 0x0F)
            registers.f |= FLAG_H;
        if (r > 0xFF)
            registers.f |= FLAG_C;
        registers.a = (unsigned char)r;
        break;
    }
    case 2:
    case 3:
    case 7:
    {
        int carryIn = (op == 3) ? c : 0;
        r = a - n - carryIn;
        registers.f = FLAG_N;
        if ((r & 0xFF) == 0)
            registers.f |= FLAG_Z;
        if ((a & 0x0F) < ((n & 0x0F) + carryIn))
            registers.f |= FLAG_H;
        if (a < (n + carryIn))
            registers.f |= FLAG_C;
        if (op != 7)
            registers.a = (unsigned char)r;
        break;
    }
    case 4:
        registers.a = a & n;
        registers.f = FLAG_H | ((registers.a == 0) ? FLAG_Z : 0);
        break;
    case 5:
        registers.a = a ^ n;
        registers.f = (registers.a == 0) ? FLAG_Z : 0;
        break;
    case 6:
        registers.a = a | n;
        registers.f = (registers.a == 0) ? FLAG_Z : 0;
        break;
    }
}

/* ---- Instructions that don't fit a pattern ---- */
static void nop(unsigned short o) { (void)o; }

static void di(unsigned short o)
{
    (void)o;
    ime = 0;
    eiPending = 0;
}
static void ei(unsigned short o)
{
    (void)o;
    eiPending = 2;
}
static void halt(unsigned short o)
{
    (void)o;
    halted = 1;
}

static void ld_bc_a(unsigned short o)
{
    (void)o;
    writeByte(registers.bc, registers.a);
}
static void ld_a_bc(unsigned short o)
{
    (void)o;
    registers.a = readByte(registers.bc);
}
static void ld_de_a(unsigned short o)
{
    (void)o;
    writeByte(registers.de, registers.a);
}
static void ld_a_de(unsigned short o)
{
    (void)o;
    registers.a = readByte(registers.de);
}

static void ld_hli_a(unsigned short o)
{
    (void)o;
    writeByte(registers.hl++, registers.a);
}
static void ld_hld_a(unsigned short o)
{
    (void)o;
    writeByte(registers.hl--, registers.a);
}
static void ld_a_hli(unsigned short o)
{
    (void)o;
    registers.a = readByte(registers.hl++);
}
static void ld_a_hld(unsigned short o)
{
    (void)o;
    registers.a = readByte(registers.hl--);
}

static void ld_nn_a(unsigned short o) { writeByte(o, registers.a); }
static void ld_a_nn(unsigned short o) { registers.a = readByte(o); }
static void ldh_n_a(unsigned short o) { writeByte(0xFF00 + o, registers.a); }
static void ldh_a_n(unsigned short o) { registers.a = readByte(0xFF00 + o); }
static void ld_c_ind_a(unsigned short o)
{
    (void)o;
    writeByte(0xFF00 + registers.c, registers.a);
}
static void ld_a_c_ind(unsigned short o)
{
    (void)o;
    registers.a = readByte(0xFF00 + registers.c);
}
static void ld_sp_hl(unsigned short o)
{
    (void)o;
    registers.sp = registers.hl;
}

static void ld_nn_sp(unsigned short o)
{
    writeByte(o, registers.sp & 0xFF);
    writeByte(o + 1, registers.sp >> 8);
}

static void add_sp_e(unsigned short o)
{
    signed char e = (signed char)o;
    unsigned short sp = registers.sp;
    registers.f = 0;
    if (((sp & 0x0F) + (e & 0x0F)) > 0x0F)
        registers.f |= FLAG_H;
    if (((sp & 0xFF) + (e & 0xFF)) > 0xFF)
        registers.f |= FLAG_C;
    registers.sp = (unsigned short)(sp + e);
}

static void ld_hl_sp_e(unsigned short o)
{
    signed char e = (signed char)o;
    unsigned short sp = registers.sp;
    registers.f = 0;
    if (((sp & 0x0F) + (e & 0x0F)) > 0x0F)
        registers.f |= FLAG_H;
    if (((sp & 0xFF) + (e & 0xFF)) > 0xFF)
        registers.f |= FLAG_C;
    registers.hl = (unsigned short)(sp + e);
}

static void rlca(unsigned short o)
{
    (void)o;
    unsigned char carry = registers.a >> 7;
    registers.a = (unsigned char)((registers.a << 1) | carry);
    registers.f = carry ? FLAG_C : 0;
}

static void rrca(unsigned short o)
{
    (void)o;
    unsigned char carry = registers.a & 1;
    registers.a = (unsigned char)((registers.a >> 1) | (carry << 7));
    registers.f = carry ? FLAG_C : 0;
}

static void rla(unsigned short o)
{
    (void)o;
    unsigned char oldCarry = (registers.f & FLAG_C) ? 1 : 0;
    unsigned char carry = registers.a >> 7;
    registers.a = (unsigned char)((registers.a << 1) | oldCarry);
    registers.f = carry ? FLAG_C : 0;
}

static void rra(unsigned short o)
{
    (void)o;
    unsigned char oldCarry = (registers.f & FLAG_C) ? 1 : 0;
    unsigned char carry = registers.a & 1;
    registers.a = (unsigned char)((registers.a >> 1) | (oldCarry << 7));
    registers.f = carry ? FLAG_C : 0;
}

static void cpl(unsigned short o)
{
    (void)o;
    registers.a = (unsigned char)~registers.a;
    registers.f |= FLAG_N | FLAG_H;
}

static void scf(unsigned short o)
{
    (void)o;
    registers.f = (registers.f & FLAG_Z) | FLAG_C;
}

static void ccf(unsigned short o)
{
    (void)o;
    registers.f = (registers.f & FLAG_Z) | ((registers.f & FLAG_C) ? 0 : FLAG_C);
}

/* DAA: fixes A after adding/subtracting decimal (BCD) numbers, e.g. game scores */
static void daa(unsigned short o)
{
    (void)o;
    int a = registers.a;
    int carry = (registers.f & FLAG_C) != 0;

    if (!(registers.f & FLAG_N))
    {
        if (carry || a > 0x99)
        {
            a += 0x60;
            carry = 1;
        }
        if ((registers.f & FLAG_H) || (a & 0x0F) > 0x09)
            a += 0x06;
    }
    else
    {
        if (carry)
            a -= 0x60;
        if (registers.f & FLAG_H)
            a -= 0x06;
    }
    registers.a = (unsigned char)a;
    registers.f = (registers.f & FLAG_N) | (registers.a == 0 ? FLAG_Z : 0) | (carry ? FLAG_C : 0);
}

static void jp_nn(unsigned short o) { registers.pc = o; }
static void jp_hl(unsigned short o)
{
    (void)o;
    registers.pc = registers.hl;
}
static void jr(unsigned short o) { registers.pc += (signed char)o; }
static void jr_nz(unsigned short o)
{
    if (condition(0))
        registers.pc += (signed char)o;
}
static void jr_z(unsigned short o)
{
    if (condition(1))
        registers.pc += (signed char)o;
}
static void jr_nc(unsigned short o)
{
    if (condition(2))
        registers.pc += (signed char)o;
}
static void jr_c(unsigned short o)
{
    if (condition(3))
        registers.pc += (signed char)o;
}

static void call_nn(unsigned short o)
{
    pushWord(registers.pc);
    registers.pc = o;
}
static void ret(unsigned short o)
{
    (void)o;
    registers.pc = popWord();
}
static void reti(unsigned short o)
{
    (void)o;
    registers.pc = popWord();
    ime = 1;
}

/* 0xCB prefix: the next byte picks one of 256 more instructions */
static void cb_prefix(unsigned short o)
{
    unsigned char op = (unsigned char)o;
    int idx = op & 7;
    int group = op >> 6;
    int bit = (op >> 3) & 7;
    unsigned char v = getReg(idx);

    if (group == 1)
    { /* BIT */
        registers.f = (registers.f & FLAG_C) | FLAG_H | (((v >> bit) & 1) ? 0 : FLAG_Z);
        return;
    }
    if (group == 2)
    { /* RES */
        setReg(idx, (unsigned char)(v & ~(1 << bit)));
        return;
    }
    if (group == 3)
    { /* SET */
        setReg(idx, (unsigned char)(v | (1 << bit)));
        return;
    }

    unsigned char oldCarry = (registers.f & FLAG_C) ? 1 : 0;
    unsigned char r = 0, carry = 0;
    switch (bit)
    {
    case 0:
        carry = v >> 7;
        r = (unsigned char)((v << 1) | carry);
        break; /* RLC  */
    case 1:
        carry = v & 1;
        r = (unsigned char)((v >> 1) | (carry << 7));
        break; /* RRC  */
    case 2:
        carry = v >> 7;
        r = (unsigned char)((v << 1) | oldCarry);
        break; /* RL   */
    case 3:
        carry = v & 1;
        r = (unsigned char)((v >> 1) | (oldCarry << 7));
        break; /* RR   */
    case 4:
        carry = v >> 7;
        r = (unsigned char)(v << 1);
        break; /* SLA  */
    case 5:
        carry = v & 1;
        r = (unsigned char)((v >> 1) | (v & 0x80));
        break; /* SRA  */
    case 6:
        carry = 0;
        r = (unsigned char)((v << 4) | (v >> 4));
        break; /* SWAP */
    case 7:
        carry = v & 1;
        r = (unsigned char)(v >> 1);
        break; /* SRL  */
    }
    registers.f = ((r == 0) ? FLAG_Z : 0) | (carry ? FLAG_C : 0);
    setReg(idx, r);
}

/* ---- Table for the irregular instructions ---- */
static const struct instruction instructions[256] = {
    [0x00] = {"NOP", 0, nop},
    [0x02] = {"LD (BC), A", 0, ld_bc_a},
    [0x07] = {"RLCA", 0, rlca},
    [0x08] = {"LD (0x%04X), SP", 2, ld_nn_sp},
    [0x0A] = {"LD A, (BC)", 0, ld_a_bc},
    [0x0F] = {"RRCA", 0, rrca},
    [0x10] = {"STOP", 1, nop},
    [0x12] = {"LD (DE), A", 0, ld_de_a},
    [0x17] = {"RLA", 0, rla},
    [0x18] = {"JR 0x%02X", 1, jr},
    [0x1A] = {"LD A, (DE)", 0, ld_a_de},
    [0x1F] = {"RRA", 0, rra},
    [0x20] = {"JR NZ, 0x%02X", 1, jr_nz},
    [0x22] = {"LD (HL+), A", 0, ld_hli_a},
    [0x27] = {"DAA", 0, daa},
    [0x28] = {"JR Z, 0x%02X", 1, jr_z},
    [0x2A] = {"LD A, (HL+)", 0, ld_a_hli},
    [0x2F] = {"CPL", 0, cpl},
    [0x30] = {"JR NC, 0x%02X", 1, jr_nc},
    [0x32] = {"LD (HL-), A", 0, ld_hld_a},
    [0x37] = {"SCF", 0, scf},
    [0x38] = {"JR C, 0x%02X", 1, jr_c},
    [0x3A] = {"LD A, (HL-)", 0, ld_a_hld},
    [0x3F] = {"CCF", 0, ccf},
    [0x76] = {"HALT", 0, halt},
    [0xC3] = {"JP 0x%04X", 2, jp_nn},
    [0xC9] = {"RET", 0, ret},
    [0xCB] = {"CB 0x%02X", 1, cb_prefix},
    [0xCD] = {"CALL 0x%04X", 2, call_nn},
    [0xD9] = {"RETI", 0, reti},
    [0xE0] = {"LDH (0x%02X), A", 1, ldh_n_a},
    [0xE2] = {"LD (C), A", 0, ld_c_ind_a},
    [0xE8] = {"ADD SP, 0x%02X", 1, add_sp_e},
    [0xE9] = {"JP HL", 0, jp_hl},
    [0xEA] = {"LD (0x%04X), A", 2, ld_nn_a},
    [0xF0] = {"LDH A, (0x%02X)", 1, ldh_a_n},
    [0xF2] = {"LD A, (C)", 0, ld_a_c_ind},
    [0xF3] = {"DI", 0, di},
    [0xF8] = {"LD HL, SP+0x%02X", 1, ld_hl_sp_e},
    [0xF9] = {"LD SP, HL", 0, ld_sp_hl},
    [0xFA] = {"LD A, (0x%04X)", 2, ld_a_nn},
    [0xFB] = {"EI", 0, ei},
};

void cpuReset(void)
{
    registers.af = 0x01B0;
    registers.bc = 0x0013;
    registers.de = 0x00D8;
    registers.hl = 0x014D;
    registers.sp = 0xFFFE;
    registers.pc = 0x0100;
}

void printRegisters(void)
{
    printf("AF=%04X BC=%04X DE=%04X HL=%04X SP=%04X PC=%04X\n",
           registers.af, registers.bc, registers.de,
           registers.hl, registers.sp, registers.pc);
}

/* Run exactly one instruction */
static void executeInstruction(void)
{
    unsigned short pc = registers.pc;
    unsigned char op = readByte(pc);
    unsigned char b1 = readByte(pc + 1);
    unsigned short nn = b1 | (readByte(pc + 2) << 8);
    int r1 = (op >> 3) & 7;
    int r2 = op & 7;
    int rr = (op >> 4) & 3;
    int cc = (op >> 3) & 3;

    lastCost = cyclesFor(op);

    if (op >= 0x40 && op <= 0x7F && op != 0x76)
    { /* LD r, r */
        trace("%04X: LD %s, %s", pc, regNames[r1], regNames[r2]);
        registers.pc += 1;
        setReg(r1, getReg(r2));
        return;
    }
    if (op >= 0x80 && op <= 0xBF)
    { /* ADD..CP with a register */
        trace("%04X: %s %s", pc, aluNames[r1], regNames[r2]);
        registers.pc += 1;
        alu(r1, getReg(r2));
        return;
    }
    if ((op & 0xC7) == 0xC6)
    { /* ADD..CP with a number */
        trace("%04X: %s 0x%02X", pc, aluNames[r1], b1);
        registers.pc += 2;
        alu(r1, b1);
        return;
    }
    if ((op & 0xC7) == 0x06)
    { /* LD r, n */
        trace("%04X: LD %s, 0x%02X", pc, regNames[r1], b1);
        registers.pc += 2;
        setReg(r1, b1);
        return;
    }
    if ((op & 0xC7) == 0x04)
    { /* INC r */
        trace("%04X: INC %s", pc, regNames[r1]);
        registers.pc += 1;
        setReg(r1, inc8(getReg(r1)));
        return;
    }
    if ((op & 0xC7) == 0x05)
    { /* DEC r */
        trace("%04X: DEC %s", pc, regNames[r1]);
        registers.pc += 1;
        setReg(r1, dec8(getReg(r1)));
        return;
    }
    if ((op & 0xCF) == 0x01)
    { /* LD rr, nn */
        trace("%04X: LD %s, 0x%04X", pc, rrNames[rr], nn);
        registers.pc += 3;
        setRR(rr, nn);
        return;
    }
    if ((op & 0xCF) == 0x03)
    { /* INC rr */
        trace("%04X: INC %s", pc, rrNames[rr]);
        registers.pc += 1;
        setRR(rr, (unsigned short)(getRR(rr) + 1));
        return;
    }
    if ((op & 0xCF) == 0x0B)
    { /* DEC rr */
        trace("%04X: DEC %s", pc, rrNames[rr]);
        registers.pc += 1;
        setRR(rr, (unsigned short)(getRR(rr) - 1));
        return;
    }
    if ((op & 0xCF) == 0x09)
    { /* ADD HL, rr */
        trace("%04X: ADD HL, %s", pc, rrNames[rr]);
        registers.pc += 1;
        unsigned int hl = registers.hl, v = getRR(rr), r = hl + v;
        registers.f = (registers.f & FLAG_Z) | ((((hl & 0x0FFF) + (v & 0x0FFF)) > 0x0FFF) ? FLAG_H : 0) | ((r > 0xFFFF) ? FLAG_C : 0);
        registers.hl = (unsigned short)r;
        return;
    }
    if ((op & 0xCF) == 0xC5)
    { /* PUSH rr */
        trace("%04X: PUSH %s", pc, (rr == 3) ? "AF" : rrNames[rr]);
        registers.pc += 1;
        pushWord((rr == 3) ? registers.af : getRR(rr));
        return;
    }
    if ((op & 0xCF) == 0xC1)
    { /* POP rr */
        trace("%04X: POP %s", pc, (rr == 3) ? "AF" : rrNames[rr]);
        registers.pc += 1;
        unsigned short v = popWord();
        if (rr == 3)
            registers.af = v & 0xFFF0;
        else
            setRR(rr, v);
        return;
    }
    if ((op & 0xC7) == 0xC7)
    { /* RST */
        trace("%04X: RST 0x%02X", pc, op & 0x38);
        registers.pc += 1;
        pushWord(registers.pc);
        registers.pc = op & 0x38;
        return;
    }
    if ((op & 0xE7) == 0xC0)
    { /* RET cc */
        trace("%04X: RET %s", pc, ccNames[cc]);
        registers.pc += 1;
        if (condition(cc))
            registers.pc = popWord();
        return;
    }
    if ((op & 0xE7) == 0xC2)
    { /* JP cc, nn */
        trace("%04X: JP %s, 0x%04X", pc, ccNames[cc], nn);
        registers.pc += 3;
        if (condition(cc))
            registers.pc = nn;
        return;
    }
    if ((op & 0xE7) == 0xC4)
    { /* CALL cc, nn */
        trace("%04X: CALL %s, 0x%04X", pc, ccNames[cc], nn);
        registers.pc += 3;
        if (condition(cc))
        {
            pushWord(registers.pc);
            registers.pc = nn;
        }
        return;
    }

    /* Everything else comes from the table */
    const struct instruction *inst = &instructions[op];

    if (inst->execute == NULL)
    {
        printTrace();
        printf("\nUnimplemented instruction 0x%02X at address 0x%04X\n", op, pc);
        printRegisters();
        exit(1);
    }

    unsigned short operand = 0;
    if (inst->operandLength == 1)
        operand = b1;
    else if (inst->operandLength == 2)
        operand = nn;

    char text[48];
    snprintf(text, sizeof text, inst->disassembly, operand);
    trace("%04X: %s", pc, text);

    registers.pc += 1 + inst->operandLength;
    inst->execute(operand);
}

/* One step of the whole machine: interrupts, sleeping, one instruction, clock */
void cpuStep(void)
{
    unsigned char pending = readByte(0xFFFF) & readByte(0xFF0F) & 0x1F;

    if (pending)
        halted = 0; /* an interrupt wakes HALT */

    if (ime && pending)
    { /* jump to the interrupt handler */
        for (int i = 0; i < 5; i++)
        {
            if (pending & (1 << i))
            {
                ime = 0;
                writeByte(0xFF0F, (unsigned char)(readByte(0xFF0F) & ~(1 << i)));
                pushWord(registers.pc);
                registers.pc = (unsigned short)(0x40 + i * 8);
                trace("--- interrupt %d ---", i);
                advance(20);
                return;
            }
        }
    }

    if (halted)
    {
        advance(4);
        return;
    }

    executeInstruction();
    advance(lastCost);

    if (eiPending)
    {
        eiPending--;
        if (eiPending == 0)
            ime = 1;
    }
}