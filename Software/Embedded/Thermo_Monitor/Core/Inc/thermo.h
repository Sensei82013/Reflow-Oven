/* Shared thermocouple plumbing, so the control modes in control.c can reuse
 * the sensor and console code that lives in main.c. */
#ifndef THERMO_H
#define THERMO_H

#include "main.h"

#define FAULT_NONE         0u
#define FAULT_OPEN_TC      1u   /* D2 set: thermocouple input is open */
#define FAULT_FRAMING      2u   /* D15/D1 not as spec'd: no/bad MAX6675 reply */

/* Writes one line to USART1 and, when a debugger is attached, to semihosting. */
void emit(const char *s);

/* Debug output that goes ONLY to semihosting, never to USART1. The Nextion
 * lives on that UART, and squirting log text at it produces garbage
 * commands. */
void emit_dbg(const char *s);

/* USART1 on PA9/PA10 (TP6/TP5). The HMI modes take this over to talk to the
 * Nextion, which is why emit_dbg() exists. */
extern UART_HandleTypeDef huart1;

/* Reads one 16-bit MAX6675 frame. Returns a FAULT_* code; *word_out is the
 * raw frame either way. Never call faster than every 250 ms - that is the
 * part's conversion time. */
uint32_t max6675_read(uint16_t *word_out);

/* Raw frame -> milli-degrees C. Only meaningful when the fault code is
 * FAULT_NONE. */
static inline int32_t max6675_milli_c(uint16_t raw)
{
	return (int32_t)((raw >> 3) & 0x0FFFu) * 250;
}

/* Entry point for the heater control modes; built only when CONTROL_MODE != 0.
 * Never returns. */
void run_control(void);

#endif /* THERMO_H */
