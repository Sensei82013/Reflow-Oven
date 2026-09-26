/**
 * Reflow Oven - MAX6675 thermocouple live monitor
 *
 * Board: STM32F103C8T6 (Reflow Project Kicad rev.)
 *   MAX6675 (U4)  ~CS  -> PA4   (GPIO, software chip select)
 *                  SCK -> PA5   (GPIO, bit-banged clock)
 *                  SO  -> PA6   (GPIO input, pulled up)
 *   USART1 TX     -> PA9  (test point TP6)
 *   USART1 RX     -> PA10 (test point TP5)
 *   Relay         -> PA8  (J6, TIM1_CH1) - heater PWM in the CONTROL_MODE
 *                   builds, held low otherwise. See control.c.
 *
 * Temperature is reported three ways so it can be observed with or without
 * extra hardware:
 *   1. USART1 @ 115200 8N1 on PA9/TP6, plain ASCII lines.
 *   2. ARM semihosting over SWD, so an attached ST-Link prints the same lines
 *      without needing a USB-serial adapter.
 *   3. A telemetry struct at a fixed RAM address that a debugger can poll
 *      while the core keeps running.
 */

#include "main.h"
#include "thermo.h"
#include <stdio.h>
#include <string.h>

/* ---- MAX6675 ------------------------------------------------------------ */
/* The startup probe exists only to tell a wiring fault from a protocol fault,
 * and it costs a second or two. Left off now that the sensor reads correctly;
 * set to 1 if the MAX6675 ever goes quiet again. */
#define RUN_BUS_PROBE      0

/* Built with `make SCK_TEST=1`: instead of monitoring, hold CS low and drive
 * SCK as a 1 kHz square wave forever, so the clock line can be chased with a
 * meter. A 50% square wave reads about half of 3.3V on a DC multimeter, so
 * TP3 and U4 pin 5 should both sit near 1.6V. TP3 at 1.6V with pin 5 stuck at
 * 0V or 3.3V means the joint or trace at the chip is open. */
#ifndef SCK_TEST_MODE
#define SCK_TEST_MODE      0
#endif

/* One conversion takes up to 220 ms, so never sample faster than that. */
#define SAMPLE_PERIOD_MS   500u

#define CS_GPIO_Port       GPIOA
#define CS_Pin             GPIO_PIN_4
#define SCK_GPIO_Port      GPIOA
#define SCK_Pin            GPIO_PIN_5
#define SO_GPIO_Port       GPIOA
#define SO_Pin             GPIO_PIN_6

/* ---- Telemetry block read by the debugger while the core runs ----------- */
#define TELEM_MAGIC        0x544D5031u   /* "TMP1" */

typedef struct {
	uint32_t magic;
	uint32_t seq;        /* increments once per completed sample */
	uint32_t raw;        /* raw 16-bit MAX6675 word */
	int32_t  milli_c;    /* temperature in milli-degrees C */
	uint32_t fault;      /* FAULT_* */
	uint32_t uptime_ms;
	uint32_t sysclk_hz;  /* which clock actually came up, as a frequency */
	uint32_t on_hse;     /* 1 = crystal, 0 = internal oscillator */
} telem_t;

volatile telem_t g_telem __attribute__((used)) = { TELEM_MAGIC, 0, 0, 0, 0, 0, 0, 0 };

UART_HandleTypeDef huart1;

static void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART1_UART_Init(void);
void Error_Handler(void);

/* ---- Semihosting -------------------------------------------------------- */
/* SYS_WRITE0 (0x04) writes a NUL-terminated string to the debugger console.
 * Guarded on C_DEBUGEN: with no debugger attached the BKPT would fault, so
 * the board still runs standalone. */
static int debugger_attached(void)
{
	return (CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk) != 0;
}

static void semihost_write0(const char *s)
{
	if (!debugger_attached()) {
		return;
	}
	register int r0 __asm__("r0") = 0x04;
	register const char *r1 __asm__("r1") = s;
	__asm__ volatile("bkpt #0xAB" : "+r"(r0) : "r"(r1) : "memory");
}

/* Semihosting only - for when USART1 belongs to something else. */
void emit_dbg(const char *s)
{
	semihost_write0(s);
}

/* Send one line out every available channel. */
void emit(const char *s)
{
	HAL_UART_Transmit(&huart1, (uint8_t *)s, strlen(s), 100);
	semihost_write0(s);
}

/* ---- MAX6675 read (bit-banged) -----------------------------------------
 *
 * Frame layout (MSB first):
 *   D15      dummy sign bit, always 0
 *   D14..D3  12-bit temperature, 0.25 C per LSB
 *   D2       1 = thermocouple input open
 *   D1       device ID, always 0
 *   D0       tri-state
 *
 * Clocked by hand rather than through SPI1: the part only needs 16 clocks and
 * this removes any doubt about CPOL/CPHA or the receive-only clock gating.
 * SO is sampled while SCK is low, then SCK is pulsed to shift the next bit.
 */
static void bb_delay(void)
{
	for (volatile int i = 0; i < 40; i++) {
		__NOP();
	}
}

static void sck_write(int level)
{
	HAL_GPIO_WritePin(SCK_GPIO_Port, SCK_Pin, level ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static int so_read(void)
{
	return HAL_GPIO_ReadPin(SO_GPIO_Port, SO_Pin) == GPIO_PIN_SET;
}

uint32_t max6675_read(uint16_t *word_out)
{
	uint16_t word = 0;

	sck_write(0);
	HAL_GPIO_WritePin(CS_GPIO_Port, CS_Pin, GPIO_PIN_RESET);
	bb_delay();

	for (int bit = 15; bit >= 0; bit--) {
		sck_write(0);
		bb_delay();
		if (so_read()) {
			word |= (uint16_t)(1u << bit);
		}
		sck_write(1);
		bb_delay();
	}

	sck_write(0);
	HAL_GPIO_WritePin(CS_GPIO_Port, CS_Pin, GPIO_PIN_SET);

	*word_out = word;

	/* A real reply always has D15 = 0 and D1 = 0. All-ones means nothing is
	 * driving SO (the pull-up wins); all-zeros means SO is stuck low. Neither
	 * is a temperature, so don't report one. */
	if (word == 0x0000u || word == 0xFFFFu || (word & 0x8002u) != 0u) {
		return FAULT_FRAMING;
	}
	if ((word & 0x0004u) != 0u) {
		return FAULT_OPEN_TC;
	}
	return FAULT_NONE;
}

/* Pin-to-pin short test.
 *
 * On the SOIC-8 the MAX6675's signal pins are adjacent: 5 = SCK, 6 = ~CS,
 * 7 = SO. A solder bridge between 6 and 7 reproduces the all-zeros symptom
 * exactly - SO would simply mirror whatever CS is doing, reading high while
 * deselected and low throughout a transfer - so it has to be ruled out before
 * blaming an open SCK.
 *
 * The trick is the pull direction. Whenever CS is high the MAX6675 releases
 * SO, so a pulled-down SO must read 0. If it reads 1, something other than
 * the part is driving it, and CS is the only candidate next door.
 */
static void cfg_pin(GPIO_TypeDef *port, uint16_t pin, uint32_t mode, uint32_t pull)
{
	GPIO_InitTypeDef gpio = {0};
	gpio.Pin = pin;
	gpio.Mode = mode;
	gpio.Pull = pull;
	gpio.Speed = GPIO_SPEED_FREQ_HIGH;
	HAL_GPIO_Init(port, &gpio);
}

static void probe_shorts(void)
{
	char line[192];
	int cs_so, sck_so_hi, sck_so_lo, sck_cs_hi, sck_cs_lo;

	cfg_pin(CS_GPIO_Port, CS_Pin, GPIO_MODE_OUTPUT_PP, GPIO_NOPULL);
	cfg_pin(SCK_GPIO_Port, SCK_Pin, GPIO_MODE_OUTPUT_PP, GPIO_NOPULL);

	/* CS <-> SO (pins 6-7). CS high, SCK low, SO pulled down. */
	HAL_GPIO_WritePin(SCK_GPIO_Port, SCK_Pin, GPIO_PIN_RESET);
	HAL_GPIO_WritePin(CS_GPIO_Port, CS_Pin, GPIO_PIN_SET);
	cfg_pin(SO_GPIO_Port, SO_Pin, GPIO_MODE_INPUT, GPIO_PULLDOWN);
	bb_delay();
	cs_so = so_read();

	/* SCK <-> SO (pins 5-7). CS stays high so SO is still released. */
	HAL_GPIO_WritePin(SCK_GPIO_Port, SCK_Pin, GPIO_PIN_SET);
	cfg_pin(SO_GPIO_Port, SO_Pin, GPIO_MODE_INPUT, GPIO_PULLDOWN);
	bb_delay();
	sck_so_hi = so_read();
	HAL_GPIO_WritePin(SCK_GPIO_Port, SCK_Pin, GPIO_PIN_RESET);
	cfg_pin(SO_GPIO_Port, SO_Pin, GPIO_MODE_INPUT, GPIO_PULLUP);
	bb_delay();
	sck_so_lo = so_read();

	/* SCK <-> CS (pins 5-6). Let CS float on its own pull resistor and see
	 * whether driving SCK drags it along. */
	cfg_pin(CS_GPIO_Port, CS_Pin, GPIO_MODE_INPUT, GPIO_PULLDOWN);
	HAL_GPIO_WritePin(SCK_GPIO_Port, SCK_Pin, GPIO_PIN_SET);
	bb_delay();
	sck_cs_hi = HAL_GPIO_ReadPin(CS_GPIO_Port, CS_Pin) == GPIO_PIN_SET;
	cfg_pin(CS_GPIO_Port, CS_Pin, GPIO_MODE_INPUT, GPIO_PULLUP);
	HAL_GPIO_WritePin(SCK_GPIO_Port, SCK_Pin, GPIO_PIN_RESET);
	bb_delay();
	sck_cs_lo = HAL_GPIO_ReadPin(CS_GPIO_Port, CS_Pin) == GPIO_PIN_SET;

	/* Back to normal: CS an output idling high, SCK an output idling low,
	 * SO an input pulled up. */
	cfg_pin(CS_GPIO_Port, CS_Pin, GPIO_MODE_OUTPUT_PP, GPIO_NOPULL);
	HAL_GPIO_WritePin(CS_GPIO_Port, CS_Pin, GPIO_PIN_SET);
	HAL_GPIO_WritePin(SCK_GPIO_Port, SCK_Pin, GPIO_PIN_RESET);
	cfg_pin(SO_GPIO_Port, SO_Pin, GPIO_MODE_INPUT, GPIO_PULLUP);

	snprintf(line, sizeof(line),
	         "short: CS->SO=%d  SCK->SO=%d/%d  SCK->CS=%d/%d\r\n",
	         cs_so, sck_so_hi, sck_so_lo, sck_cs_hi, sck_cs_lo);
	emit(line);

	const char *verdict = "no pin-to-pin short found";
	if (cs_so) {
		verdict = "CS and SO look SHORTED (U4 pins 6-7)";
	} else if (sck_so_hi && !sck_so_lo) {
		verdict = "SCK and SO look SHORTED (U4 pins 5-7)";
	} else if (sck_cs_hi && !sck_cs_lo) {
		verdict = "SCK and CS look SHORTED (U4 pins 5-6)";
	}
	snprintf(line, sizeof(line), "short: %s\r\n", verdict);
	emit(line);
}

/* One-shot bus diagnostics, printed once at startup.
 *
 * SO with CS high vs low separates a wiring fault from a protocol fault: the
 * MAX6675 releases SO when deselected (the pull-up shows a 1) and drives the
 * dummy bit when selected (a 0). SCK is read back through IDR while being
 * driven both ways, which catches a pin that isn't physically moving. The
 * per-clock SO capture shows whether the part shifts at all.
 */
static void report_bus_state(void)
{
	char line[192];
	char bits[20];
	int so_idle, so_selected, sck_hi, sck_lo;

	HAL_GPIO_WritePin(CS_GPIO_Port, CS_Pin, GPIO_PIN_SET);
	bb_delay();
	so_idle = so_read();

	HAL_GPIO_WritePin(CS_GPIO_Port, CS_Pin, GPIO_PIN_RESET);
	bb_delay();
	so_selected = so_read();
	HAL_GPIO_WritePin(CS_GPIO_Port, CS_Pin, GPIO_PIN_SET);

	/* Drive SCK both ways and read the pin back. A push-pull output that
	 * won't follow its own ODR is shorted or not connected to the die. */
	sck_write(1);
	bb_delay();
	sck_hi = HAL_GPIO_ReadPin(SCK_GPIO_Port, SCK_Pin) == GPIO_PIN_SET;
	sck_write(0);
	bb_delay();
	sck_lo = HAL_GPIO_ReadPin(SCK_GPIO_Port, SCK_Pin) == GPIO_PIN_SET;

	snprintf(line, sizeof(line),
	         "bus: SO(CS=1)=%d SO(CS=0)=%d  SCK readback hi=%d lo=%d\r\n",
	         so_idle, so_selected, sck_hi, sck_lo);
	emit(line);

	/* Capture SO on each of 16 clocks, MSB first. */
	sck_write(0);
	HAL_GPIO_WritePin(CS_GPIO_Port, CS_Pin, GPIO_PIN_RESET);
	bb_delay();
	for (int i = 0; i < 16; i++) {
		sck_write(0);
		bb_delay();
		bits[i] = so_read() ? '1' : '0';
		sck_write(1);
		bb_delay();
	}
	bits[16] = '\0';
	sck_write(0);
	HAL_GPIO_WritePin(CS_GPIO_Port, CS_Pin, GPIO_PIN_SET);

	snprintf(line, sizeof(line), "bus: SO per clock = %s\r\n", bits);
	emit(line);
}

/* Clock the part several different ways to tell a protocol problem from a
 * hardware one. If SO never once goes high - whatever the clock phase or
 * speed - then the shift register is not advancing and the fault is physical
 * rather than in the bit-banging above. */
#if RUN_BUS_PROBE
static void probe_variants(void)
{
	char line[192];
	char bits[40];
	static const int delays[] = { 1, 20, 400 };

	for (unsigned d = 0; d < sizeof(delays) / sizeof(delays[0]); d++) {
		for (int phase = 0; phase < 2; phase++) {
			sck_write(0);
			HAL_GPIO_WritePin(CS_GPIO_Port, CS_Pin, GPIO_PIN_RESET);
			for (volatile int i = 0; i < delays[d]; i++) { __NOP(); }

			int ones = 0;
			for (int i = 0; i < 32; i++) {
				int sample;
				sck_write(0);
				for (volatile int k = 0; k < delays[d]; k++) { __NOP(); }
				sample = so_read();          /* phase 0: sample while low */
				sck_write(1);
				for (volatile int k = 0; k < delays[d]; k++) { __NOP(); }
				if (phase) {
					sample = so_read();      /* phase 1: sample while high */
				}
				if (i < 32) {
					bits[i] = sample ? '1' : '0';
				}
				ones += sample;
			}
			bits[32] = '\0';
			sck_write(0);
			HAL_GPIO_WritePin(CS_GPIO_Port, CS_Pin, GPIO_PIN_SET);
			HAL_Delay(250);

			snprintf(line, sizeof(line),
			         "probe: delay=%3d phase=%d ones=%2d  %s\r\n",
			         delays[d], phase, ones, bits);
			emit(line);
		}
	}
}
#endif /* RUN_BUS_PROBE */

int main(void)
{
	HAL_Init();
	SystemClock_Config();
	MX_GPIO_Init();
	MX_USART1_UART_Init();

	g_telem.sysclk_hz = HAL_RCC_GetSysClockFreq();

	char line[192];
	snprintf(line, sizeof(line),
	         "\r\n=== Reflow Oven MAX6675 monitor ===\r\n"
	         "SYSCLK %lu Hz (%s), sampling every %u ms\r\n"
	         "seq,raw,tempC,status\r\n",
	         (unsigned long)g_telem.sysclk_hz,
	         g_telem.on_hse ? "HSE crystal" : "internal HSI, Y1 not populated",
	         (unsigned)SAMPLE_PERIOD_MS);
	emit(line);

#if SCK_TEST_MODE
	emit("SCK test mode: CS held low, SCK toggling at ~1 kHz.\r\n"
	     "Expect ~1.6 V DC on TP3 and on U4 pin 5.\r\n");
	HAL_GPIO_WritePin(CS_GPIO_Port, CS_Pin, GPIO_PIN_RESET);
	while (1) {
		sck_write(1);
		for (volatile int i = 0; i < 1300; i++) { __NOP(); }
		sck_write(0);
		for (volatile int i = 0; i < 1300; i++) { __NOP(); }
	}
#endif

#if CONTROL_MODE
	run_control();   /* never returns */
#endif

	report_bus_state();
	probe_shorts();
#if RUN_BUS_PROBE
	probe_variants();
#endif

	/* First conversion after power-up isn't ready yet. */
	HAL_Delay(SAMPLE_PERIOD_MS);

	uint32_t seq = 0;

	while (1) {
		uint16_t raw = 0;
		uint32_t fault = max6675_read(&raw);

		/* 12 bits at 0.25 C -> milli-degrees without floating point. */
		int32_t milli_c = (int32_t)((raw >> 3) & 0x0FFFu) * 250;

		seq++;
		g_telem.raw       = raw;
		g_telem.milli_c   = milli_c;
		g_telem.fault     = fault;
		g_telem.uptime_ms = HAL_GetTick();
		g_telem.seq       = seq;   /* written last: seq change means all valid */

		const char *status = (fault == FAULT_OPEN_TC) ? "OPEN_THERMOCOUPLE"
		                   : (fault == FAULT_FRAMING) ? "NO_SENSOR_REPLY"
		                   : "OK";

		if (fault == FAULT_NONE) {
			snprintf(line, sizeof(line), "%lu,0x%04X,%ld.%02ld,%s\r\n",
			         (unsigned long)seq, (unsigned)raw,
			         (long)(milli_c / 1000), (long)((milli_c % 1000) / 10),
			         status);
		} else {
			snprintf(line, sizeof(line), "%lu,0x%04X,-,%s\r\n",
			         (unsigned long)seq, (unsigned)raw, status);
		}
		emit(line);

		HAL_Delay(SAMPLE_PERIOD_MS);
	}
}

/* ---- Clocks -------------------------------------------------------------
 * Y1 (the 8 MHz crystal) is not populated on this board, so there is no HSE
 * to run from. Everything runs off the internal oscillator instead: HSI/2 via
 * PLL x16 = 64 MHz. HSI is internal to the MCU, so nothing has to be routed
 * for this to work.
 *
 * The tradeoff is accuracy: HSI is factory-trimmed to about +/-1% at room
 * temperature and drifts a few percent across the full range, against tens of
 * ppm for a crystal. That is irrelevant for reading a thermocouple (the
 * MAX6675 does its own conversion and SCK has no minimum rate) and fine for
 * the 115200 UART. It is not enough for USB device mode, which needs 48 MHz
 * from an HSE PLL - so J1 stays power-only unless Y1 gets populated.
 *
 * Set USE_HSE to 1 if the crystal is ever fitted; the code then tries it
 * first and still falls back to HSI rather than dying silently. */
#ifndef USE_HSE
#define USE_HSE 0
#endif

#if USE_HSE
static int clock_from_hse(void)
{
	RCC_OscInitTypeDef osc = {0};

	osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
	osc.HSEState = RCC_HSE_ON;
	osc.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
	osc.PLL.PLLState = RCC_PLL_ON;
	osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
	osc.PLL.PLLMUL = RCC_PLL_MUL9;
	return HAL_RCC_OscConfig(&osc) == HAL_OK;
}
#endif /* USE_HSE */

static int clock_from_hsi(void)
{
	RCC_OscInitTypeDef osc = {0};

	osc.OscillatorType = RCC_OSCILLATORTYPE_HSI;
	osc.HSIState = RCC_HSI_ON;
	osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
	osc.PLL.PLLState = RCC_PLL_ON;
	osc.PLL.PLLSource = RCC_PLLSOURCE_HSI_DIV2;
	osc.PLL.PLLMUL = RCC_PLL_MUL16;
	return HAL_RCC_OscConfig(&osc) == HAL_OK;
}

static void SystemClock_Config(void)
{
	RCC_ClkInitTypeDef clk = {0};

	int on_hse = 0;
#if USE_HSE
	/* HAL allows 100 ms per attempt; three passes is far longer than any
	 * 8 MHz crystal needs to start. */
	for (int attempt = 0; attempt < 3 && !on_hse; attempt++) {
		on_hse = clock_from_hse();
	}
#endif
	if (!on_hse && !clock_from_hsi()) {
		Error_Handler();
	}
	g_telem.on_hse = (uint32_t)on_hse;

	clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
	                RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
	clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
	clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
	clk.APB1CLKDivider = RCC_HCLK_DIV2;   /* <= 36 MHz */
	clk.APB2CLKDivider = RCC_HCLK_DIV1;

	if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2) != HAL_OK) {
		Error_Handler();
	}
}

static void MX_USART1_UART_Init(void)
{
	huart1.Instance = USART1;
	huart1.Init.BaudRate = 115200;
	huart1.Init.WordLength = UART_WORDLENGTH_8B;
	huart1.Init.StopBits = UART_STOPBITS_1;
	huart1.Init.Parity = UART_PARITY_NONE;
	huart1.Init.Mode = UART_MODE_TX_RX;
	huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
	huart1.Init.OverSampling = UART_OVERSAMPLING_16;
	if (HAL_UART_Init(&huart1) != HAL_OK) {
		Error_Handler();
	}
}

static void MX_GPIO_Init(void)
{
	GPIO_InitTypeDef gpio = {0};

	__HAL_RCC_GPIOA_CLK_ENABLE();
	__HAL_RCC_GPIOB_CLK_ENABLE();
	__HAL_RCC_AFIO_CLK_ENABLE();

	/* Free PB3/PB4 but keep SWD alive - this is how we talk to the board. */
	__HAL_AFIO_REMAP_SWJ_NOJTAG();

	/* Relay off. control.c takes this pin over as TIM1_CH1 PWM in the
	 * control builds; in every other build it stays a driven low. */
	HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_RESET);
	gpio.Pin = GPIO_PIN_8;
	gpio.Mode = GPIO_MODE_OUTPUT_PP;
	gpio.Pull = GPIO_NOPULL;
	gpio.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(GPIOA, &gpio);

	/* MAX6675 chip select and clock, both idle low/high as the part expects. */
	HAL_GPIO_WritePin(CS_GPIO_Port, CS_Pin, GPIO_PIN_SET);
	HAL_GPIO_WritePin(SCK_GPIO_Port, SCK_Pin, GPIO_PIN_RESET);
	gpio.Pin = CS_Pin | SCK_Pin;
	gpio.Mode = GPIO_MODE_OUTPUT_PP;
	gpio.Pull = GPIO_NOPULL;
	gpio.Speed = GPIO_SPEED_FREQ_HIGH;
	HAL_GPIO_Init(GPIOA, &gpio);

	/* MAX6675 SO. Pulled up so an absent or unpowered sensor reads back as
	 * all-ones and is reported as a fault instead of a plausible temperature. */
	gpio.Pin = SO_Pin;
	gpio.Mode = GPIO_MODE_INPUT;
	gpio.Pull = GPIO_PULLUP;
	HAL_GPIO_Init(SO_GPIO_Port, &gpio);
}

void Error_Handler(void)
{
	__disable_irq();
	while (1) {
	}
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
	(void)file;
	(void)line;
}
#endif
