/* Musashi configuration for the Beyond Oasis runtime (plain 68000, Genesis wiring). */
#ifndef MUSASHI_CONF_H
#define MUSASHI_CONF_H

#define M68K_OPT_OFF             0
#define M68K_OPT_ON              1
#define M68K_OPT_SPECIFY_HANDLER 2

#define M68K_COMPILE_FOR_MAME      M68K_OPT_OFF
#define M68K_EMULATE_010           M68K_OPT_OFF
#define M68K_EMULATE_EC020         M68K_OPT_OFF
#define M68K_EMULATE_020           M68K_OPT_OFF
#define M68K_EMULATE_030           M68K_OPT_OFF
#define M68K_EMULATE_040           M68K_OPT_OFF
#define M68K_SEPARATE_READS        M68K_OPT_OFF
#define M68K_SIMULATE_PD_WRITES    M68K_OPT_OFF

/* The VDP clears its pending interrupt when the CPU acknowledges it. */
#define M68K_EMULATE_INT_ACK       M68K_OPT_SPECIFY_HANDLER
#define M68K_INT_ACK_CALLBACK(A)   gen_int_ack(A)

#define M68K_EMULATE_BKPT_ACK      M68K_OPT_OFF
#define M68K_EMULATE_TRACE         M68K_OPT_OFF
#define M68K_EMULATE_RESET         M68K_OPT_OFF
#define M68K_CMPILD_HAS_CALLBACK   M68K_OPT_OFF
#define M68K_RTE_HAS_CALLBACK      M68K_OPT_OFF
#define M68K_TAS_HAS_CALLBACK      M68K_OPT_OFF
#define M68K_ILLG_HAS_CALLBACK     M68K_OPT_OFF
#define M68K_TRAP_HAS_CALLBACK     M68K_OPT_OFF
#define M68K_EMULATE_FC            M68K_OPT_OFF
#define M68K_MONITOR_PC            M68K_OPT_OFF

/* Before every interpreted instruction: hand control to recompiled/overridden code. */
#define M68K_INSTRUCTION_HOOK      M68K_OPT_SPECIFY_HANDLER
#define M68K_INSTRUCTION_CALLBACK(pc) gen_instr_hook(pc)

#define M68K_EMULATE_PREFETCH      M68K_OPT_OFF
#define M68K_EMULATE_ADDRESS_ERROR M68K_OPT_OFF
#define M68K_LOG_ENABLE            M68K_OPT_OFF
#define M68K_LOG_1010_1111         M68K_OPT_OFF
#define M68K_LOG_FILEHANDLE        stderr
#define M68K_EMULATE_PMMU          M68K_OPT_OFF
#define M68K_USE_64_BIT            M68K_OPT_ON

int gen_int_ack(int level);
void gen_instr_hook(unsigned int pc);

#endif
