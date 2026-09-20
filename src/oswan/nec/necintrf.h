/* ASG 971222 -- rewrote this interface */
#ifndef __NEC_H_
#define __NEC_H_



enum {
	NEC_IP=1, NEC_AW, NEC_CW, NEC_DW, NEC_BW, NEC_SP, NEC_BP, NEC_IX, NEC_IY,
	NEC_FLAGS, NEC_ES, NEC_CS, NEC_SS, NEC_DS,
	NEC_VECTOR, NEC_PENDING, NEC_NMI_STATE, NEC_IRQ_STATE };

/* Public variables */
extern int nec_ICount;

/* Public functions */

/*
#define v20_ICount nec_ICount
extern void v20_init(void);
extern void v20_reset(void *param);
extern void v20_exit(void);
extern int v20_execute(int cycles);
extern unsigned v20_get_context(void *dst);
extern void v20_set_context(void *src);
extern unsigned v20_get_reg(int regnum);
extern void v20_set_reg(int regnum, unsigned val);
extern void v20_set_irq_line(int irqline, int state);
extern void v20_set_irq_callback(int (*callback)(int irqline));
extern const char *v20_info(void *context, int regnum);
extern unsigned v20_dasm(char *buffer, unsigned pc);

#define v30_ICount nec_ICount
extern void v30_init(void);
extern void v30_reset(void *param);
extern void v30_exit(void);
extern int v30_execute(int cycles);
extern unsigned v30_get_context(void *dst);
extern void v30_set_context(void *src);
extern unsigned v30_get_reg(int regnum);
extern void v30_set_reg(int regnum, unsigned val);
extern void v30_set_irq_line(int irqline, int state);
extern void v30_set_irq_callback(int (*callback)(int irqline));
extern const char *v30_info(void *context, int regnum);
extern unsigned v30_dasm(char *buffer, unsigned pc);

#define v33_ICount nec_ICount
extern void v33_init(void);
extern void v33_reset(void *param);
extern void v33_exit(void);
extern int v33_execute(int cycles);
extern unsigned v33_get_context(void *dst);
extern void v33_set_context(void *src);
extern unsigned v33_get_reg(int regnum);
extern void v33_set_reg(int regnum, unsigned val);
extern void v33_set_irq_line(int irqline, int state);
extern void v33_set_irq_callback(int (*callback)(int irqline));
extern const char *v33_info(void *context, int regnum);
extern unsigned v33_dasm(char *buffer, unsigned pc);
*/

void nec_set_reg(int,unsigned);
int nec_execute(int cycles);
uint32_t nec_get_clock(void);
unsigned nec_get_reg(int regnum);
void nec_reset (void *param);
int nec_int(uint32_t wektor);

typedef struct {
    uint16_t regs[8];
    uint16_t sregs[4];
    uint16_t ip;
    int32_t sign_val;
    uint32_t aux_val, over_val, zero_val, carry_val, parity_val;
    uint8_t tf, iff, df, mf;
    uint32_t int_vector, pending_irq, nmi_state, irq_state;
    uint32_t cpu_type, prefix_base, total_clock;
    int32_t icount, no_interrupt;
    uint8_t seg_prefix;
} nec_snapshot_t;

void nec_snapshot_get(nec_snapshot_t *state);
void nec_snapshot_set(const nec_snapshot_t *state);

#endif
