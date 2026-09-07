/*
 * mcs48.h - Intel MCS-48 (8035/8048 and the Mitsubishi MB8884 clone) interpreter.
 *
 * Donkey Kong's sound board is one of these running from a 4 KB ROM. It plays the music and
 * most of the effects by writing samples to an eight-bit DAC on port 1, and fetches its sample
 * data a page at a time through the BUS port, so emulating it is what gets the real tunes
 * rather than an imitation of them.
 *
 * The part is simple: an accumulator, eight registers in one of two banks, 64 bytes of RAM
 * with the stack living inside it, a timer/counter, two test pins and three ports. Instructions
 * take one or two machine cycles, and a machine cycle is 15 oscillator periods.
 *
 * Define before including:
 *   MCS48_ROM(a)          program byte at address a (12-bit)
 *   MCS48_BUS_IN(a)       read the BUS port at address a (0 for INS A,BUS)
 *   MCS48_BUS_OUT(v)      write the BUS port
 *   MCS48_P1_OUT(v)       write port 1 (the DAC)     MCS48_P2_IN() / MCS48_P2_OUT(v)
 *   MCS48_T0() MCS48_T1() the two test pins, non-zero for high
 */
#ifndef MCS48_H
#define MCS48_H
#include <stdint.h>

#define PSW_CY 0x80
#define PSW_AC 0x40
#define PSW_F0 0x20
#define PSW_BS 0x10

typedef struct {
    uint8_t a, psw;
    uint16_t pc;                 /* 12 bits, plus the A11 bank from SEL MB */
    uint8_t ram[64];
    uint8_t p1, p2;
    uint8_t timer, prescale;
    uint8_t timer_running, counter_running;
    uint8_t timer_flag, f1;
    uint8_t irq_enabled, tcnti_enabled;
    uint8_t irq_line, in_irq;
    uint16_t a11;                /* SEL MB0 / MB1: the A11 bank bit, so this must hold 0x800 */
    int32_t icount;
} mcs48_t;

#define R(n)   c->ram[((c->psw & PSW_BS) ? 24 : 0) + (n)]
#define SP()   (c->psw & 7)

static void mcs48_reset(mcs48_t *c)
{
    uint8_t i;
    for (i = 0; i < 64; i++) c->ram[i] = 0;
    c->a = 0; c->psw = 0x08; c->pc = 0; c->a11 = 0;
    c->p1 = 0xff; c->p2 = 0xff;
    c->timer = 0; c->prescale = 0;
    c->timer_running = c->counter_running = c->timer_flag = c->f1 = 0;
    c->irq_enabled = c->tcnti_enabled = c->irq_line = c->in_irq = 0;
    c->icount = 0;
}

static inline void mcs48_push(mcs48_t *c)
{
    uint8_t sp = (uint8_t)SP();
    c->ram[8 + sp * 2] = (uint8_t)(c->pc & 0xff);
    c->ram[9 + sp * 2] = (uint8_t)(((c->pc >> 8) & 0x0f) | (c->psw & 0xf0));
    c->psw = (uint8_t)((c->psw & 0xf8) | ((sp + 1) & 7));
}

static inline void mcs48_pop(mcs48_t *c, int restore_psw)
{
    uint8_t sp = (uint8_t)((SP() - 1) & 7);
    c->psw = (uint8_t)((c->psw & 0xf8) | sp);
    c->pc = (uint16_t)(c->ram[8 + sp * 2] | ((c->ram[9 + sp * 2] & 0x0f) << 8));
    if (restore_psw) c->psw = (uint8_t)((c->ram[9 + sp * 2] & 0xf0) | (c->psw & 0x0f));
}

static inline void mcs48_add(mcs48_t *c, uint8_t v, uint8_t carry)
{
    uint16_t t = (uint16_t)(c->a + v + carry);
    c->psw &= (uint8_t)~(PSW_CY | PSW_AC);
    if (t > 0xff) c->psw |= PSW_CY;
    if (((c->a & 0x0f) + (v & 0x0f) + carry) > 0x0f) c->psw |= PSW_AC;
    c->a = (uint8_t)t;
}

/* one instruction; returns the machine cycles it took */
static int mcs48_step(mcs48_t *c)
{
    /* interrupts are recognised between instructions */
    if (!c->in_irq) {
        if (c->irq_line && c->irq_enabled) {
            mcs48_push(c); c->in_irq = 1; c->pc = 3; c->a11 = 0;
            return 2;
        }
        if (c->timer_flag && c->tcnti_enabled) {
            mcs48_push(c); c->in_irq = 1; c->timer_flag = 0; c->pc = 7; c->a11 = 0;
            return 2;
        }
    }

    uint8_t op = MCS48_ROM(c->pc); c->pc = (uint16_t)((c->pc + 1) & 0xfff);
    int cyc = 1;
    #define IMM()  ({ uint8_t v_ = MCS48_ROM(c->pc); c->pc = (uint16_t)((c->pc + 1) & 0xfff); v_; })
    #define JMPA()  do { uint8_t lo_ = IMM(); c->pc = (uint16_t)(lo_ | ((op & 0xe0) << 3) | c->a11); cyc = 2; } while (0)
    #define CALLA() do { uint8_t lo_ = IMM(); mcs48_push(c); \
                         c->pc = (uint16_t)(lo_ | ((op & 0xe0) << 3) | c->a11); cyc = 2; } while (0)
    #define COND(x) do { uint8_t lo_ = IMM(); cyc = 2; \
                         if (x) c->pc = (uint16_t)((c->pc & 0xf00) | lo_); } while (0)

    switch (op) {
    case 0x00: break;                                             /* NOP */
    case 0x02: MCS48_BUS_OUT(c->a); cyc = 2; break;               /* OUTL BUS,A */
    case 0x03: mcs48_add(c, IMM(), 0); cyc = 2; break;            /* ADD A,#d */
    case 0x04: case 0x24: case 0x44: case 0x64:
    case 0x84: case 0xa4: case 0xc4: case 0xe4: JMPA(); break;    /* JMP page */
    case 0x05: c->irq_enabled = 1; break;                         /* EN I */
    case 0x07: c->a--; break;                                     /* DEC A */
    case 0x08: c->a = MCS48_BUS_IN(0); cyc = 2; break;             /* INS A,BUS */
    case 0x09: c->a = c->p1; cyc = 2; break;                      /* IN A,P1 */
    case 0x0a: c->a = MCS48_P2_IN(); cyc = 2; break;              /* IN A,P2 */
    case 0x10: case 0x11: c->ram[R(op & 1) & 0x3f]++; break;      /* INC @Rr: the byte pointed at */
    case 0x12: COND(c->a & 0x01); break;                          /* JB0 */
    case 0x13: mcs48_add(c, IMM(), (uint8_t)((c->psw & PSW_CY) ? 1 : 0)); cyc = 2; break;
    case 0x14: case 0x34: case 0x54: case 0x74:
    case 0x94: case 0xb4: case 0xd4: case 0xf4: CALLA(); break;   /* CALL page */
    case 0x15: c->irq_enabled = 0; break;                         /* DIS I */
    case 0x16: COND(c->timer_flag); c->timer_flag = 0; break;     /* JTF */
    case 0x17: c->a++; break;                                     /* INC A */
    case 0x18: case 0x19: case 0x1a: case 0x1b:
    case 0x1c: case 0x1d: case 0x1e: case 0x1f: R(op & 7)++; break;
    case 0x20: case 0x21: { uint8_t addr = (uint8_t)(R(op & 1) & 0x3f); uint8_t t = c->ram[addr];
                            c->ram[addr] = c->a; c->a = t; } break;          /* XCH A,@Rr */
    case 0x23: c->a = IMM(); cyc = 2; break;                      /* MOV A,#d */
    case 0x25: c->tcnti_enabled = 1; break;                       /* EN TCNTI */
    case 0x26: COND(!MCS48_T0()); break;                          /* JNT0 */
    case 0x27: c->a = 0; break;                                   /* CLR A */
    case 0x28: case 0x29: case 0x2a: case 0x2b:
    case 0x2c: case 0x2d: case 0x2e: case 0x2f: { uint8_t t = R(op & 7); R(op & 7) = c->a; c->a = t; } break;
    case 0x30: case 0x31: { uint8_t addr = (uint8_t)(R(op & 1) & 0x3f);
                            uint8_t t = c->ram[addr];
                            c->ram[addr] = (uint8_t)((t & 0xf0) | (c->a & 0x0f));
                            c->a = (uint8_t)((c->a & 0xf0) | (t & 0x0f)); } break;   /* XCHD */
    case 0x32: COND(c->a & 0x02); break;
    case 0x35: c->tcnti_enabled = 0; break;                       /* DIS TCNTI */
    case 0x36: COND(MCS48_T0()); break;                           /* JT0 */
    case 0x37: c->a = (uint8_t)~c->a; break;                      /* CPL A */
    case 0x39: c->p1 = c->a; MCS48_P1_OUT(c->p1); cyc = 2; break; /* OUTL P1,A - the DAC */
    case 0x3a: c->p2 = c->a; MCS48_P2_OUT(c->p2); cyc = 2; break; /* OUTL P2,A */
    case 0x40: case 0x41: c->a |= c->ram[R(op & 1) & 0x3f]; break;
    case 0x42: c->a = c->timer; break;                            /* MOV A,T */
    case 0x43: c->a |= IMM(); cyc = 2; break;
    case 0x45: c->counter_running = 1; c->timer_running = 0; break;   /* STRT CNT */
    case 0x46: COND(!MCS48_T1()); break;                          /* JNT1 */
    case 0x47: c->a = (uint8_t)((c->a >> 4) | (c->a << 4)); break;    /* SWAP A */
    case 0x48: case 0x49: case 0x4a: case 0x4b:
    case 0x4c: case 0x4d: case 0x4e: case 0x4f: c->a |= R(op & 7); break;
    case 0x50: case 0x51: c->a &= c->ram[R(op & 1) & 0x3f]; break;
    case 0x52: COND(c->a & 0x04); break;
    case 0x53: c->a &= IMM(); cyc = 2; break;
    case 0x55: c->timer_running = 1; c->counter_running = 0; break;  /* STRT T */
    case 0x56: COND(MCS48_T1()); break;                           /* JT1 */
    case 0x57: {                                                  /* DA A */
        if ((c->a & 0x0f) > 9 || (c->psw & PSW_AC)) {
            if ((uint16_t)c->a + 6 > 0xff) c->psw |= PSW_CY;
            c->a = (uint8_t)(c->a + 6);
        }
        if ((c->a >> 4) > 9 || (c->psw & PSW_CY)) { c->a = (uint8_t)(c->a + 0x60); c->psw |= PSW_CY; }
        } break;
    case 0x58: case 0x59: case 0x5a: case 0x5b:
    case 0x5c: case 0x5d: case 0x5e: case 0x5f: c->a &= R(op & 7); break;
    case 0x60: case 0x61: mcs48_add(c, c->ram[R(op & 1) & 0x3f], 0); break;
    case 0x62: c->timer = c->a; break;                            /* MOV T,A */
    case 0x65: c->timer_running = c->counter_running = 0; break;  /* STOP TCNT */
    case 0x67: { uint8_t cy = (uint8_t)((c->psw & PSW_CY) ? 0x80 : 0);   /* RRC A */
                 if (c->a & 1) c->psw |= PSW_CY; else c->psw &= (uint8_t)~PSW_CY;
                 c->a = (uint8_t)((c->a >> 1) | cy); } break;
    case 0x68: case 0x69: case 0x6a: case 0x6b:
    case 0x6c: case 0x6d: case 0x6e: case 0x6f: mcs48_add(c, R(op & 7), 0); break;
    case 0x70: case 0x71: mcs48_add(c, c->ram[R(op & 1) & 0x3f], (uint8_t)((c->psw & PSW_CY) ? 1 : 0)); break;
    case 0x72: COND(c->a & 0x08); break;
    case 0x75: break;                                             /* ENT0 CLK */
    case 0x76: COND(c->f1); break;                                /* JF1 */
    case 0x77: c->a = (uint8_t)((c->a >> 1) | (c->a << 7)); break; /* RR A */
    case 0x78: case 0x79: case 0x7a: case 0x7b:
    case 0x7c: case 0x7d: case 0x7e: case 0x7f:
        mcs48_add(c, R(op & 7), (uint8_t)((c->psw & PSW_CY) ? 1 : 0)); break;
    case 0x80: case 0x81: c->a = MCS48_BUS_IN(R(op & 1)); cyc = 2; break;   /* MOVX A,@Rr */
    case 0x83: mcs48_pop(c, 0); cyc = 2; break;                   /* RET */
    case 0x85: c->psw &= (uint8_t)~PSW_F0; break;                 /* CLR F0 */
    /* JNI jumps when the interrupt input is LOW, and that pin is active low - so it jumps
     * when an interrupt is actually pending, not when it is absent. */
    case 0x86: COND(c->irq_line); break;                          /* JNI */
    case 0x88: MCS48_BUS_OUT((uint8_t)(MCS48_BUS_IN(0) | IMM())); cyc = 2; break;
    case 0x89: c->p1 |= IMM(); MCS48_P1_OUT(c->p1); cyc = 2; break;
    case 0x8a: c->p2 |= IMM(); MCS48_P2_OUT(c->p2); cyc = 2; break;
    case 0x90: case 0x91: MCS48_BUS_OUT(c->a); cyc = 2; break;    /* MOVX @Rr,A */
    case 0x92: COND(c->a & 0x10); break;
    case 0x93: mcs48_pop(c, 1); c->in_irq = 0; cyc = 2; break;    /* RETR */
    case 0x95: c->psw ^= PSW_F0; break;                           /* CPL F0 */
    case 0x96: COND(c->a != 0); break;                            /* JNZ */
    case 0x97: c->psw &= (uint8_t)~PSW_CY; break;                 /* CLR C */
    case 0x98: MCS48_BUS_OUT((uint8_t)(MCS48_BUS_IN(0) & IMM())); cyc = 2; break;
    case 0x99: c->p1 &= IMM(); MCS48_P1_OUT(c->p1); cyc = 2; break;
    case 0x9a: c->p2 &= IMM(); MCS48_P2_OUT(c->p2); cyc = 2; break;
    case 0xa0: case 0xa1: c->ram[R(op & 1) & 0x3f] = c->a; break; /* MOV @Rr,A */
    case 0xa3: c->a = MCS48_ROM((c->pc & 0xf00) | c->a); cyc = 2; break;    /* MOVP A,@A */
    case 0xa5: c->f1 = 0; break;
    case 0xa7: c->psw ^= PSW_CY; break;                           /* CPL C */
    case 0xa8: case 0xa9: case 0xaa: case 0xab:
    case 0xac: case 0xad: case 0xae: case 0xaf: R(op & 7) = c->a; break;
    case 0xb0: case 0xb1: c->ram[R(op & 1) & 0x3f] = IMM(); cyc = 2; break;
    case 0xb2: COND(c->a & 0x20); break;
    case 0xb3: c->pc = (uint16_t)((c->pc & 0xf00) | MCS48_ROM((c->pc & 0xf00) | c->a)); cyc = 2; break;  /* JMPP */
    case 0xb5: c->f1 = (uint8_t)!c->f1; break;
    case 0xb6: COND(c->psw & PSW_F0); break;                      /* JF0 */
    case 0xb8: case 0xb9: case 0xba: case 0xbb:
    case 0xbc: case 0xbd: case 0xbe: case 0xbf: R(op & 7) = IMM(); cyc = 2; break;
    case 0xc5: c->psw &= (uint8_t)~PSW_BS; break;                 /* SEL RB0 */
    case 0xc6: COND(c->a == 0); break;                            /* JZ */
    case 0xc7: c->a = (uint8_t)(c->psw | 0x08); break;            /* MOV A,PSW */
    case 0xc8: case 0xc9: case 0xca: case 0xcb:
    case 0xcc: case 0xcd: case 0xce: case 0xcf: R(op & 7)--; break;
    case 0xd0: case 0xd1: c->a ^= c->ram[R(op & 1) & 0x3f]; break;
    case 0xd2: COND(c->a & 0x40); break;
    case 0xd3: c->a ^= IMM(); cyc = 2; break;
    case 0xd5: c->psw |= PSW_BS; break;                           /* SEL RB1 */
    case 0xd7: c->psw = (uint8_t)(c->a | 0x08); break;            /* MOV PSW,A */
    case 0xd8: case 0xd9: case 0xda: case 0xdb:
    case 0xdc: case 0xdd: case 0xde: case 0xdf: c->a ^= R(op & 7); break;
    case 0xe3: c->a = MCS48_ROM(0x300 | c->a); cyc = 2; break;    /* MOVP3 A,@A */
    case 0xe5: c->a11 = 0; break;                                 /* SEL MB0 */
    case 0xe6: COND(!(c->psw & PSW_CY)); break;                   /* JNC */
    case 0xe7: c->a = (uint8_t)((c->a << 1) | (c->a >> 7)); break; /* RL A */
    case 0xe8: case 0xe9: case 0xea: case 0xeb:
    case 0xec: case 0xed: case 0xee: case 0xef: { uint8_t lo = IMM(); cyc = 2;
                 if (--R(op & 7) != 0) c->pc = (uint16_t)((c->pc & 0xf00) | lo); } break;   /* DJNZ */
    case 0xf0: case 0xf1: c->a = c->ram[R(op & 1) & 0x3f]; break; /* MOV A,@Rr */
    case 0xf2: COND(c->a & 0x80); break;
    case 0xf5: c->a11 = 0x800; break;                             /* SEL MB1 */
    case 0xf6: COND(c->psw & PSW_CY); break;                      /* JC */
    case 0xf7: { uint8_t cy = (uint8_t)((c->psw & PSW_CY) ? 1 : 0);    /* RLC A */
                 if (c->a & 0x80) c->psw |= PSW_CY; else c->psw &= (uint8_t)~PSW_CY;
                 c->a = (uint8_t)((c->a << 1) | cy); } break;
    case 0xf8: case 0xf9: case 0xfa: case 0xfb:
    case 0xfc: case 0xfd: case 0xfe: case 0xff: c->a = R(op & 7); break;
    default: break;                                               /* undefined: treated as NOP */
    }
    #undef IMM
    #undef JMPA
    #undef CALLA
    #undef COND

    /* the timer is clocked once every 32 machine cycles */
    if (c->timer_running) {
        c->prescale = (uint8_t)(c->prescale + cyc);
        while (c->prescale >= 32) {
            c->prescale -= 32;
            if (++c->timer == 0) c->timer_flag = 1;
        }
    }
    return cyc;
}

#undef R
#undef SP
#endif
