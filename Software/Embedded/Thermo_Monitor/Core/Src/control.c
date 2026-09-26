/**
 * Heater control for the Reflow Oven board.
 *
 * The heating element is switched by an IRF520N on PA8 (the "Relay" net, J6),
 * which is TIM1_CH1, so the duty cycle is real hardware PWM rather than
 * bit-banged. D3 sits on the same net and dims with duty, which makes the
 * output visible from across the bench.
 *
 * Two builds, selected by CONTROL_MODE:
 *   1 = closed loop. PI control through a setpoint schedule.
 *   2 = open loop step. Holds a fixed duty so the plant can be characterised
 *       before anyone tries to pick gains for it.
 *   3 = hold a fixed duty, for probing the driver stage with a meter.
 *   4 = setpoint server, steered from a host over SWD. See webapp/.
 *   5 = Nextion HMI probe: find the display, then watch its touch codes.
 *
 * Safety, in order of how much it is relied on:
 *   - Any sensor fault forces duty to 0. A controller that cannot see the
 *     temperature must not be driving a heater.
 *   - Above OVERTEMP_C the output latches off for good and needs a reset.
 *   - Every stage has a timeout, so a heater too weak to reach setpoint ends
 *     the run instead of sitting at full power forever.
 *   - The profile switches off and reports DONE when it finishes.
 *   - The host script separately forces PA8 low after every run, so a halted
 *     core cannot leave the element energised. See run_profile.sh.
 */

#include "thermo.h"
#include <stdio.h>
#include <string.h>

#if CONTROL_MODE

/* ---- PWM ---------------------------------------------------------------- */
/* Switching frequency, in Hz. 200 Hz suits a MOSFET driven directly from the
 * pin. An opto-isolated DC solid state relay does NOT switch that fast - turn
 * on and off take on the order of a millisecond, which is a large slice of a
 * 5 ms period, so it would never fully switch and would dissipate heat in the
 * transition. Heaters driven by an SSR use time-proportioned control instead:
 *   make MODE=pi PWM_HZ=1
 * A thermal plant with a minutes-long time constant does not care.
 *
 * The period is always 2000 counts, so duty resolution is 0.05% whatever the
 * frequency; only the prescaler changes. */
#ifndef PWM_HZ
#define PWM_HZ             200u
#endif
#define PWM_PERIOD         2000u

/* ---- Loop and limits ---------------------------------------------------- */
#define CTRL_PERIOD_MS     250u      /* MAX6675 conversion time is 220 ms max */
#ifndef OVERTEMP_RAW
#define OVERTEMP_RAW       120.0
#endif
#define OVERTEMP_C         ((float)(OVERTEMP_RAW))  /* latch off above this */

/* Absolute ceiling on commanded duty, baked into the build. This is a hard
 * guard, not the working limit - in server mode the host sets the working cap
 * at runtime and can change it from the UI, so leaving this at 100 keeps the
 * control honest. Lower it only to make a build that physically cannot exceed
 * some duty:
 *   make MODE=pi MAX_DUTY=25
 * The over-temperature latch is the real backstop, not this. */
#ifndef MAX_DUTY_RAW
#define MAX_DUTY_RAW       100.0
#endif

/* Tunables come in as bare numbers from the Makefile and get cast here, so
 * that both `KP=8.5` and `KP=100` are valid on the command line. */
#ifndef STEP_DUTY_RAW
#define STEP_DUTY_RAW      40.0
#endif

/* Step-test segment lengths, in seconds. This plant is slow - about a degree
 * a minute at full power - so the heating window has to be minutes, not the
 * tens of seconds that would characterise a fast heater. */
#ifndef STEP_BASE_S
#define STEP_BASE_S        4
#endif
#ifndef STEP_HEAT_S
#define STEP_HEAT_S        30
#endif
#ifndef STEP_COOL_S
#define STEP_COOL_S        30
#endif
#ifndef KP_RAW
#define KP_RAW             12.0
#endif
#ifndef KI_RAW
#define KI_RAW             0.6
#endif

static const float DUTY_MAX_PCT = (float)(MAX_DUTY_RAW);
__attribute__((unused)) static const float STEP_DUTY_PCT  = (float)(STEP_DUTY_RAW);
__attribute__((unused)) static const float KP_PCT_PER_C   = (float)(KP_RAW);
__attribute__((unused)) static const float KI_PCT_PER_C_S = (float)(KI_RAW);

#ifndef DWELL_S
#define DWELL_S            10
#endif
#define DWELL_MS           ((uint32_t)DWELL_S * 1000u)  /* hold each setpoint */
#define DWELL_TOL_C        1.0f      /* within this of setpoint counts as there */
#define STAGE_TIMEOUT_MS   150000u   /* give up on a stage after this */
#define COOLDOWN_MS        20000u    /* keep logging after the last stage */

static TIM_HandleTypeDef htim1;
static uint8_t latched_off;

/* ---- Heater ------------------------------------------------------------- */

static void heater_set(float pct)
{
	if (latched_off || pct < 0.0f) {
		pct = 0.0f;
	}
	if (pct > DUTY_MAX_PCT) {
		pct = DUTY_MAX_PCT;
	}
	__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1,
	                      (uint32_t)((pct / 100.0f) * (float)PWM_PERIOD));
}

/* Belt and braces: stop the timer driving the pin and hold PA8 low as a plain
 * output, so the element is off even if the timer is left in a strange state. */
static void heater_off(void)
{
	heater_set(0.0f);
	HAL_TIM_PWM_Stop(&htim1, TIM_CHANNEL_1);

	GPIO_InitTypeDef gpio = {0};
	HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_RESET);
	gpio.Pin = GPIO_PIN_8;
	gpio.Mode = GPIO_MODE_OUTPUT_PP;
	gpio.Pull = GPIO_NOPULL;
	gpio.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(GPIOA, &gpio);
}

static void heater_init(void)
{
	GPIO_InitTypeDef gpio = {0};
	TIM_OC_InitTypeDef oc = {0};

	__HAL_RCC_TIM1_CLK_ENABLE();
	__HAL_RCC_GPIOA_CLK_ENABLE();

	htim1.Instance = TIM1;
	uint32_t tick_hz = (uint32_t)PWM_HZ * PWM_PERIOD;
	uint32_t presc = (HAL_RCC_GetPCLK2Freq() + tick_hz / 2u) / tick_hz;
	if (presc < 1u) {
		presc = 1u;
	}
	htim1.Init.Prescaler = presc - 1u;
	htim1.Init.CounterMode = TIM_COUNTERMODE_UP;
	htim1.Init.Period = PWM_PERIOD - 1u;
	htim1.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
	htim1.Init.RepetitionCounter = 0;
	htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
	if (HAL_TIM_PWM_Init(&htim1) != HAL_OK) {
		Error_Handler();
	}

	oc.OCMode = TIM_OCMODE_PWM1;
	oc.Pulse = 0;
	oc.OCPolarity = TIM_OCPOLARITY_HIGH;
	oc.OCNPolarity = TIM_OCNPOLARITY_HIGH;
	oc.OCFastMode = TIM_OCFAST_DISABLE;
	/* TIM1 is an advanced timer: say explicitly that the idle state is low,
	 * so dropping MOE cannot park the gate high. */
	oc.OCIdleState = TIM_OCIDLESTATE_RESET;
	oc.OCNIdleState = TIM_OCNIDLESTATE_RESET;
	if (HAL_TIM_PWM_ConfigChannel(&htim1, &oc, TIM_CHANNEL_1) != HAL_OK) {
		Error_Handler();
	}

	gpio.Pin = GPIO_PIN_8;
	gpio.Mode = GPIO_MODE_AF_PP;
	gpio.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(GPIOA, &gpio);

	heater_set(0.0f);
	HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
}

/* Measures what PA8 is actually doing by sampling its input register for
 * 60 ms - about a dozen PWM periods - and counting how much of that time it
 * spent high. Reading the pin back separates "the MCU is not switching" from
 * "the MCU is switching but the gate is not following", which are otherwise
 * indistinguishable from the temperature alone. */
static float pwm_measured_duty(float commanded_pct)
{
	heater_set(commanded_pct);
	HAL_Delay(5);

	uint32_t high = 0, total = 0;
	uint32_t until = HAL_GetTick() + 60u;
	while ((int32_t)(HAL_GetTick() - until) < 0) {
		if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_8) == GPIO_PIN_SET) {
			high++;
		}
		total++;
	}
	return total ? (100.0f * (float)high / (float)total) : -1.0f;
}

__attribute__((unused)) static void pwm_selftest(void)
{
	static const float steps[] = { 0.0f, 25.0f, 50.0f, 75.0f, 100.0f };
	char line[128];

	emit("pwm selftest (commanded -> measured at the pin):\r\n");
	for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
		float m = pwm_measured_duty(steps[i]);
		snprintf(line, sizeof(line), "  %3d%% -> %3d.%01d%%\r\n",
		         (int)steps[i], (int)m, (int)(m * 10) % 10);
		emit(line);
	}
	heater_set(0.0f);
}

/* ---- Sampling ----------------------------------------------------------- */

/* Reads the thermocouple and enforces the two hard cutoffs. Returns 0 if the
 * reading cannot be trusted, in which case the heater has already been shut
 * off and *temp_c is untouched. */
__attribute__((unused)) static int sample(float *temp_c)
{
	uint16_t raw;
	uint32_t fault = max6675_read(&raw);

	if (fault != FAULT_NONE) {
		heater_set(0.0f);
		return 0;
	}

	float t = (float)max6675_milli_c(raw) / 1000.0f;

	if (t >= OVERTEMP_C) {
		latched_off = 1;
		heater_off();
		emit("SAFETY: over temperature, heater latched off\r\n");
		return 0;
	}

	*temp_c = t;
	return 1;
}

/* Wait until `when` on the HAL tick, so logging cost does not accumulate into
 * the loop period. */
__attribute__((unused)) static void wait_until(uint32_t when)
{
	while ((int32_t)(HAL_GetTick() - when) < 0) {
	}
}

/* ---- Mode 2: open-loop step -------------------------------------------- */
#if CONTROL_MODE == 2

void run_control(void)
{
	char line[160];

	heater_init();

	snprintf(line, sizeof(line),
	         "\r\n=== open-loop step test ===\r\n"
	         "step duty %d%%, PWM %lu Hz on PA8/TIM1_CH1\r\n"
	         "t_ms,duty_pct,tempC\r\n",
	         (int)STEP_DUTY_PCT, (unsigned long)PWM_HZ);
	emit(line);

	pwm_selftest();

	/* baseline, heat, cool */
	const uint32_t t_baseline = (uint32_t)STEP_BASE_S * 1000u;
	const uint32_t t_heat     = (uint32_t)STEP_HEAT_S * 1000u;
	const uint32_t t_cool     = (uint32_t)STEP_COOL_S * 1000u;

	uint32_t t0 = HAL_GetTick();
	uint32_t next = t0;

	for (;;) {
		uint32_t elapsed = HAL_GetTick() - t0;
		float duty;

		if (elapsed < t_baseline) {
			duty = 0.0f;
		} else if (elapsed < t_baseline + t_heat) {
			duty = STEP_DUTY_PCT;
		} else if (elapsed < t_baseline + t_heat + t_cool) {
			duty = 0.0f;
		} else {
			break;
		}

		heater_set(duty);

		float t;
		if (sample(&t)) {
			snprintf(line, sizeof(line), "%lu,%d,%ld.%02ld\r\n",
			         (unsigned long)elapsed, (int)duty,
			         (long)t, (long)((int32_t)(t * 100.0f) % 100));
			emit(line);
		} else {
			snprintf(line, sizeof(line), "%lu,%d,FAULT\r\n",
			         (unsigned long)elapsed, (int)duty);
			emit(line);
		}

		next += CTRL_PERIOD_MS;
		wait_until(next);
	}

	heater_off();
	emit("DONE\r\n");
	for (;;) {
	}
}

/* ---- Mode 3: hold a fixed duty ----------------------------------------- */
/* For probing the driver stage with a meter: parks the output at a steady
 * duty and just logs, so drain-source voltage can be measured under a known
 * gate condition instead of during a 30-second window. */
#elif CONTROL_MODE == 3

void run_control(void)
{
	char line[160];

	heater_init();
	snprintf(line, sizeof(line),
	         "\r\n=== hold %d%% ===\r\n"
	         "Probe with a meter now:\r\n"
	         "  J6 pin 2 to GND   -> gate drive the board is supplying (3.3 V max)\r\n"
	         "  MOSFET D to S     -> ~24 V means it is NOT turning on, ~0 V means it is\r\n"
	         "  across the heater -> ~24 V when the element is actually being driven\r\n"
	         "t_ms,duty_pct,tempC\r\n",
	         (int)STEP_DUTY_PCT);
	emit(line);

	pwm_selftest();
	heater_set(STEP_DUTY_PCT);

	uint32_t t0 = HAL_GetTick();
	uint32_t next = t0;
	for (;;) {
		float t;
		if (sample(&t)) {
			snprintf(line, sizeof(line), "%lu,%d,%ld.%02ld\r\n",
			         (unsigned long)(HAL_GetTick() - t0), (int)STEP_DUTY_PCT,
			         (long)t, (long)((int32_t)(t * 100.0f) % 100));
			emit(line);
		}
		next += CTRL_PERIOD_MS;
		wait_until(next);
	}
}

/* ---- Mode 4: setpoint server ------------------------------------------- */
/* Takes its setpoint from a command block in RAM rather than a fixed
 * schedule, and publishes state to a telemetry block, so a host can steer it
 * over SWD while the core keeps running. Neither side ever halts the other.
 *
 * Both blocks use the same discipline: the writer fills the fields first and
 * bumps `seq` last, and the reader only acts on a block whose seq has changed.
 * That makes a half-written update impossible to act on.
 *
 * The host must also keep bumping `heartbeat`. If it stops - browser closed,
 * bridge crashed, cable pulled - the heater shuts off after HOST_TIMEOUT_MS.
 * A heater commanded over a network needs a dead-man switch.
 *
 * The host sends a setpoint and the two gains, and nothing else. Duty is
 * computed here and only reported back. */
#elif CONTROL_MODE == 4

#define CMD_MAGIC          0x434D4431u   /* "CMD1" */
#define SRV_MAGIC          0x53525631u   /* "SRV1" */
#define HOST_TIMEOUT_MS    4000u

/* state values published to the host */
#define ST_IDLE            0u
#define ST_RUNNING         1u
#define ST_SENSOR_FAULT    2u
#define ST_OVERTEMP        3u
#define ST_HOST_TIMEOUT    4u

/* The host sends only what it should: where to go and how hard to chase it.
 * Duty is the loop's business, not the browser's. Gains arrive scaled by
 * 1000 so the whole interface stays integer. */
typedef struct {
	uint32_t magic;
	uint32_t seq;            /* host bumps this last */
	int32_t  setpoint_mc;    /* milli-degrees C */
	uint32_t enable;
	int32_t  kp_micro;       /* %/C   * 1e6 */
	int32_t  ki_micro;       /* %/C/s * 1e6 */
	uint32_t heartbeat;      /* host bumps at least once a second */
} cmd_block_t;

/* P and I are published separately from the duty they sum to. Seeing which
 * term is doing the work is the whole game when tuning by hand. */
typedef struct {
	uint32_t magic;
	uint32_t seq;            /* firmware bumps this last */
	int32_t  temp_mc;
	int32_t  setpoint_mc;
	uint32_t duty_x10;       /* what actually reached the SSR, % * 10 */
	uint32_t fault;
	uint32_t state;
	uint32_t uptime_ms;
	int32_t  kp_micro;       /* echoed back, so the UI can show what is live */
	int32_t  ki_micro;
	int32_t  p_term_x10;     /* % * 10, before clamping */
	int32_t  i_term_x10;
} srv_block_t;

volatile cmd_block_t g_cmd __attribute__((used)) =
	{ CMD_MAGIC, 0, 30000, 0, (int32_t)(KP_RAW * 1000000), (int32_t)(KI_RAW * 1000000), 0 };
volatile srv_block_t g_srv __attribute__((used)) =
	{ SRV_MAGIC, 0, 0, 30000, 0, 0, ST_IDLE, 0,
	  (int32_t)(KP_RAW * 1000000), (int32_t)(KI_RAW * 1000000), 0, 0 };

void run_control(void)
{
	heater_init();
	emit("\r\n=== PI setpoint server ===\r\n"
	     "setpoint and gains come from the host; duty is ours.\r\n");

	const float dt = (float)CTRL_PERIOD_MS / 1000.0f;

	float setpoint = (float)g_cmd.setpoint_mc / 1000.0f;
	/* Micro-units, not milli. This plant wants Ki around 0.002, and a
	 * thousandth is barely two counts of resolution - not enough to tune with. */
	float kp = (float)g_cmd.kp_micro / 1000000.0f;
	float ki = (float)g_cmd.ki_micro / 1000000.0f;

	/* The integral is kept as accumulated *error* (C.s), not as an output
	 * contribution. That way the I term is always ki * integ_err, and the
	 * gain can be retuned without the history meaning something different
	 * than it did a moment ago. */
	float integ_err = 0.0f;

	uint32_t enable = 0;
	uint32_t last_cmd_seq = g_cmd.seq;
	uint32_t last_beat = g_cmd.heartbeat;
	uint32_t last_beat_ms = HAL_GetTick();
	uint32_t seq = 0;

	uint32_t next = HAL_GetTick();

	for (;;) {
		uint32_t now = HAL_GetTick();

		/* latch a complete command update */
		if (g_cmd.magic == CMD_MAGIC && g_cmd.seq != last_cmd_seq) {
			last_cmd_seq = g_cmd.seq;
			setpoint = (float)g_cmd.setpoint_mc / 1000.0f;

			float new_kp = (float)g_cmd.kp_micro / 1000000.0f;
			float new_ki = (float)g_cmd.ki_micro / 1000000.0f;
			if (new_kp < 0.0f) {
				new_kp = 0.0f;
			}
			if (new_ki < 0.0f) {
				new_ki = 0.0f;
			}

			/* Bumpless Ki change: rescale the stored error so the I term
			 * comes out where it already was. Without this, nudging Ki
			 * while hot steps the output - and on a 160 W element that is
			 * a real temperature excursion, not a cosmetic glitch. */
			if (new_ki > 0.0f && ki > 0.0f && new_ki != ki) {
				integ_err = integ_err * (ki / new_ki);
			} else if (new_ki <= 0.0f) {
				integ_err = 0.0f;
			}
			kp = new_kp;
			ki = new_ki;

			if (!enable && g_cmd.enable) {
				integ_err = 0.0f;   /* fresh start, no stale effort */
			}
			enable = g_cmd.enable;
		}

		/* dead-man switch */
		if (g_cmd.heartbeat != last_beat) {
			last_beat = g_cmd.heartbeat;
			last_beat_ms = now;
		}
		int host_alive = (now - last_beat_ms) < HOST_TIMEOUT_MS;

		/* Read the sensor here rather than through sample(), so that a
		 * latched or faulted controller still publishes a temperature. The
		 * moment the output trips is exactly when you most want to watch
		 * the temperature, and zeroing it then is worse than useless. */
		uint16_t raw;
		int ok = (max6675_read(&raw) == FAULT_NONE);
		float t = ok ? (float)max6675_milli_c(raw) / 1000.0f : 0.0f;

		if (ok && t >= OVERTEMP_C && !latched_off) {
			latched_off = 1;
			heater_off();
			emit("SAFETY: over temperature, heater latched off\r\n");
		}

		float duty = 0.0f, p_term = 0.0f, i_term = 0.0f;
		uint32_t state;

		if (latched_off) {
			state = ST_OVERTEMP;
		} else if (!ok) {
			state = ST_SENSOR_FAULT;
			integ_err = 0.0f;
		} else if (!host_alive) {
			state = ST_HOST_TIMEOUT;
			enable = 0;
			integ_err = 0.0f;
		} else if (!enable) {
			state = ST_IDLE;
			integ_err = 0.0f;
		} else {
			float err = setpoint - t;

			/* Conditional integration: only accumulate when the result
			 * would not be against the stop. This is what keeps the
			 * integral from winding up during a long climb. */
			if (ki > 0.0f) {
				float cand = integ_err + err * dt;
				float u_cand = kp * err + ki * cand;
				if (u_cand > 0.0f && u_cand < DUTY_MAX_PCT) {
					integ_err = cand;
				}
			} else {
				integ_err = 0.0f;
			}

			p_term = kp * err;
			i_term = ki * integ_err;
			duty = p_term + i_term;
			if (duty < 0.0f) {
				duty = 0.0f;
			}
			if (duty > DUTY_MAX_PCT) {
				duty = DUTY_MAX_PCT;
			}
			state = ST_RUNNING;
		}

		heater_set(duty);

		g_srv.temp_mc     = ok ? (int32_t)(t * 1000.0f) : 0;  /* 0 only when unreadable */
		g_srv.setpoint_mc = (int32_t)(setpoint * 1000.0f);
		g_srv.duty_x10    = (uint32_t)(duty * 10.0f);
		g_srv.fault       = ok ? 0u : 1u;
		g_srv.state       = state;
		g_srv.uptime_ms   = now;
		g_srv.kp_micro    = (int32_t)(kp * 1000000.0f);
		g_srv.ki_micro    = (int32_t)(ki * 1000000.0f);
		g_srv.p_term_x10  = (int32_t)(p_term * 10.0f);
		g_srv.i_term_x10  = (int32_t)(i_term * 10.0f);
		g_srv.seq         = ++seq;   /* last, so the rest is consistent */

		next += CTRL_PERIOD_MS;
		wait_until(next);
	}
}

/* ---- Mode 5: Nextion HMI probe ------------------------------------------
 * First contact with the display, covering the three ways it usually fails.
 *
 *  A. Baud sweep with `get 1`, which returns 0x71 <int32> FF FF FF whatever
 *     bkcmd is set to. The shipped Reflow.HMI sets bkcmd=0, so ordinary
 *     commands are never acked - asking for a value is the only way to tell
 *     a wrong baud from a display that is wired up but silent.
 *  B. Backlight blink at each baud. `dim=` works on any HMI whatever objects
 *     the pages contain, and needs no reply, so it tests TX alone. Watch the
 *     screen: whichever baud makes it flash is the right one.
 *  C. The same blink bit-banged on PA10, for when TX and RX are swapped.
 *     Open drain, so if the display's TX really is on PA10 the two drivers
 *     cannot fight - R6's 10k does the pulling up.
 *
 * Debug output is semihosting only; USART1 belongs to the display.
 */
#elif CONTROL_MODE == 5

#define NX_TX_Pin          GPIO_PIN_9
#define NX_RX_Pin          GPIO_PIN_10

/* The shipped Reflow.HMI contains `baud=38400` but also `bauds=115200`
 * and `bauds=250000` - and `bauds` writes the rate permanently, so the
 * panel may well not be at 38400 any more. Most likely first. */
__attribute__((unused)) static const uint32_t probe_bauds[] = { 9600 };  /* the panel's
	* own settings page reports bauds: 9600, so stop guessing */
#define N_BAUDS (sizeof(probe_bauds) / sizeof(probe_bauds[0]))

/* The Nextion's RX is 5 V logic with its own pull-up to 5 V, and PA9's
 * push-pull high only reaches 3.3 V - measured 3.2 V at the panel, which is
 * under the ~3.5 V a 5 V CMOS input needs. The display therefore never sees
 * a valid high and never decodes a frame.
 *
 * Open drain fixes it with no extra hardware: the USART still sinks for a
 * low, and for a high it simply lets go and the panel's own pull-up takes
 * the line to a clean 5 V. Rise time is a microsecond or so against that
 * pull-up, which is nothing next to a 104 us bit at 9600. */
static void nx_tx_open_drain(void)
{
	GPIO_InitTypeDef g = {0};
	g.Pin = GPIO_PIN_9;
	g.Mode = GPIO_MODE_AF_OD;
	g.Pull = GPIO_NOPULL;
	g.Speed = GPIO_SPEED_FREQ_HIGH;
	HAL_GPIO_Init(GPIOA, &g);
}

static void uart_set_baud(uint32_t baud)
{
	HAL_UART_DeInit(&huart1);
	huart1.Init.BaudRate = baud;
	if (HAL_UART_Init(&huart1) != HAL_OK) {
		Error_Handler();
	}
	nx_tx_open_drain();   /* HAL_UART_Init resets PA9 to push-pull */
}

static void nx_send(const char *cmd)
{
	static const uint8_t term[3] = { 0xFF, 0xFF, 0xFF };
	HAL_UART_Transmit(&huart1, (uint8_t *)cmd, strlen(cmd), 200);
	HAL_UART_Transmit(&huart1, (uint8_t *)term, 3, 100);
}

static int nx_collect(uint8_t *buf, int max, uint32_t ms)
{
	int n = 0;
	uint32_t until = HAL_GetTick() + ms;
	while ((int32_t)(HAL_GetTick() - until) < 0) {
		if (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_RXNE)) {
			uint8_t b = (uint8_t)(huart1.Instance->DR & 0xFF);
			if (n < max) {
				buf[n++] = b;
			}
		}
		if (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_ORE)) {
			__HAL_UART_CLEAR_OREFLAG(&huart1);
		}
	}
	return n;
}

static void dump(const uint8_t *b, int n)
{
	char line[128];
	int i = 0;
	while (i < n) {
		int m = snprintf(line, sizeof(line), "   ");
		for (int k = 0; k < 12 && i + k < n; k++) {
			m += snprintf(line + m, sizeof(line) - m, "%02X ", b[i + k]);
		}
		m += snprintf(line + m, sizeof(line) - m, " |");
		for (int k = 0; k < 12 && i + k < n; k++) {
			uint8_t c = b[i + k];
			m += snprintf(line + m, sizeof(line) - m, "%c",
			              (c >= 32 && c < 127) ? c : '.');
		}
		snprintf(line + m, sizeof(line) - m, "|\r\n");
		emit_dbg(line);
		i += 12;
	}
}

/* ---- bit-banged TX on PA10, for the swapped-wire case ------------------- */

static void dwt_init(void)
{
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static void bb_wait(uint32_t cycles)
{
	uint32_t t0 = DWT->CYCCNT;
	while ((DWT->CYCCNT - t0) < cycles) {
	}
}

__attribute__((unused)) static void bb_pin(int high)
{
	HAL_GPIO_WritePin(GPIOA, NX_RX_Pin, high ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

__attribute__((unused)) static void bb_byte(uint8_t b, uint32_t bit_cycles)
{
	bb_pin(0);                       /* start bit */
	bb_wait(bit_cycles);
	for (int i = 0; i < 8; i++) {
		bb_pin((b >> i) & 1);        /* LSB first */
		bb_wait(bit_cycles);
	}
	bb_pin(1);                       /* stop bit */
	bb_wait(bit_cycles * 2);
}

__attribute__((unused)) static void bb_send(const char *cmd, uint32_t baud)
{
	uint32_t bit_cycles = HAL_RCC_GetHCLKFreq() / baud;
	while (*cmd) {
		bb_byte((uint8_t)*cmd++, bit_cycles);
	}
	for (int i = 0; i < 3; i++) {
		bb_byte(0xFF, bit_cycles);
	}
}

/* Open drain, so if the display's TX really is on PA10 the two drivers
 * cannot fight. R6's 10k pull-up handles the rising edges, which is fine at
 * these baud rates. */
__attribute__((unused)) static void bb_claim_pa10(void)
{
	GPIO_InitTypeDef g = {0};
	HAL_GPIO_WritePin(GPIOA, NX_RX_Pin, GPIO_PIN_SET);
	g.Pin = NX_RX_Pin;
	g.Mode = GPIO_MODE_OUTPUT_OD;
	g.Pull = GPIO_NOPULL;
	g.Speed = GPIO_SPEED_FREQ_HIGH;
	HAL_GPIO_Init(GPIOA, &g);
}

__attribute__((unused)) static void bb_release_pa10(uint32_t baud)
{
	GPIO_InitTypeDef g = {0};
	g.Pin = NX_RX_Pin;
	g.Mode = GPIO_MODE_INPUT;
	g.Pull = GPIO_NOPULL;
	HAL_GPIO_Init(GPIOA, &g);
	uart_set_baud(baud);             /* hand the pin back to USART1 */
}


/* Rise-time measurement, done properly.
 *
 * The first version used HAL_GPIO_WritePin/ReadPin inside the timing loop.
 * Those are function calls at -Og, so each iteration cost tens of cycles and
 * the result sat at a constant 40 whatever the pin did - it was measuring the
 * measurement, not the signal. Raw register access brings the loop down to a
 * few cycles, and the floor is measured explicitly and subtracted so what
 * comes back is the pin's own rise and nothing else. */

#define PA10_TIMEOUT_CYCLES  64000u   /* 1 ms at 64 MHz */

/* Same loop, but the pin is already high: pure overhead. */
static uint32_t pa10_floor_cycles(void)
{
	uint32_t best = 0xFFFFFFFFu;
	for (int i = 0; i < 8; i++) {
		__disable_irq();
		GPIOA->BSRR = NX_RX_Pin;
		uint32_t t0 = DWT->CYCCNT;
		while (!(GPIOA->IDR & NX_RX_Pin)) {
			if ((DWT->CYCCNT - t0) > PA10_TIMEOUT_CYCLES) {
				break;
			}
		}
		uint32_t e = DWT->CYCCNT - t0;
		__enable_irq();
		if (e < best) {
			best = e;
		}
	}
	return best;
}

/* Pull low open-drain, release, and time the rise. Returns cycles with the
 * loop overhead already taken out, or 0xFFFFFFFF if the pin never came up. */
static uint32_t pa10_rise_cycles(void)
{
	GPIO_InitTypeDef g = {0};
	uint32_t best = 0xFFFFFFFFu;
	int stuck = 0;

	g.Pin = NX_RX_Pin;
	g.Mode = GPIO_MODE_OUTPUT_OD;
	g.Pull = GPIO_NOPULL;
	g.Speed = GPIO_SPEED_FREQ_HIGH;
	HAL_GPIO_Init(GPIOA, &g);

	uint32_t floor_cyc = pa10_floor_cycles();

	for (int i = 0; i < 6; i++) {
		GPIOA->BSRR = (uint32_t)NX_RX_Pin << 16;    /* pull low */
		bb_wait(HAL_RCC_GetHCLKFreq() / 100000u);   /* ~10 us */

		__disable_irq();
		GPIOA->BSRR = NX_RX_Pin;                    /* release */
		uint32_t t0 = DWT->CYCCNT;
		while (!(GPIOA->IDR & NX_RX_Pin)) {
			if ((DWT->CYCCNT - t0) > PA10_TIMEOUT_CYCLES) {
				stuck = 1;
				break;
			}
		}
		uint32_t e = DWT->CYCCNT - t0;
		__enable_irq();

		if (stuck) {
			break;
		}
		if (e < best) {
			best = e;
		}
		HAL_Delay(1);
	}

	uart_set_baud(9600);            /* hand PA10 back to USART1 */

	if (stuck) {
		return 0xFFFFFFFFu;
	}
	return (best > floor_cyc) ? (best - floor_cyc) : 0u;
}

/* Reports the floor alongside the result so the number can be sanity-checked
 * rather than taken on faith. */
static void pa10_report(void)
{
	char line[180];
	GPIO_InitTypeDef g = {0};

	g.Pin = NX_RX_Pin;
	g.Mode = GPIO_MODE_OUTPUT_OD;
	g.Pull = GPIO_NOPULL;
	g.Speed = GPIO_SPEED_FREQ_HIGH;
	HAL_GPIO_Init(GPIOA, &g);
	uint32_t floor_cyc = pa10_floor_cycles();
	uart_set_baud(9600);

	uint32_t net = pa10_rise_cycles();

	if (net == 0xFFFFFFFFu) {
		emit_dbg("   PA10 never comes up - held low. Short to ground, or a\r\n"
		         "   powered-off driver clamping the line.\r\n");
		return;
	}

	uint32_t mhz = HAL_RCC_GetHCLKFreq() / 1000000u;
	snprintf(line, sizeof(line),
	         "   loop floor %lu cyc; PA10 net rise %lu cyc (~%lu ns)\r\n",
	         (unsigned long)floor_cyc, (unsigned long)net,
	         (unsigned long)((net * 1000u) / mhz));
	emit_dbg(line);

	if (net <= 3u) {
		emit_dbg("   -> instant. A live driver is holding PA10 high: the\r\n"
		         "      display's TX is connected and powered.\r\n");
	} else if (net < 15u) {
		emit_dbg("   -> quick. Light capacitance only - bare pin and trace,\r\n"
		         "      so probably nothing attached to TP5.\r\n");
	} else {
		snprintf(line, sizeof(line),
		         "   -> RC. About %lu pF hanging off a 10k pull-up: a cable is\r\n"
		         "      attached but nothing on it is driving.\r\n",
		         (unsigned long)((net * 1000u) / mhz) / 7u);
		emit_dbg(line);
	}
}


/* Can PA10 be pulled down at all, and does it ever go low on its own?
 *
 * "Rises instantly" on its own is ambiguous: an idle UART TX and a 5 V supply
 * rail both look like that. What separates them is what happens when we sink
 * the pin, and whether it ever falls by itself.
 *
 *   idle UART TX  - a 20 mA push-pull driver; our open-drain sink fights it
 *                   to somewhere mid-rail, and it drops properly whenever the
 *                   panel actually sends something
 *   supply rail   - immovable, and never falls
 *   floating/R6   - our sink wins easily, reads a clean low
 *
 * The pull is open-drain and only ~10 us, so even against a rail the current
 * is brief. PA10 is 5 V tolerant, so a mis-wire to 5 V does no damage.
 */
static void pa10_pulldown_test(void)
{
	GPIO_InitTypeDef g = {0};
	char line[180];

	g.Pin = NX_RX_Pin;
	g.Mode = GPIO_MODE_OUTPUT_OD;
	g.Pull = GPIO_NOPULL;
	g.Speed = GPIO_SPEED_FREQ_HIGH;
	HAL_GPIO_Init(GPIOA, &g);

	int read_low = 0;
	for (int i = 0; i < 4; i++) {
		GPIOA->BSRR = (uint32_t)NX_RX_Pin << 16;      /* sink it */
		bb_wait(HAL_RCC_GetHCLKFreq() / 100000u);     /* ~10 us */
		if (!(GPIOA->IDR & NX_RX_Pin)) {
			read_low = 1;
		}
		GPIOA->BSRR = NX_RX_Pin;
		HAL_Delay(2);
	}

	g.Mode = GPIO_MODE_INPUT;
	g.Pull = GPIO_NOPULL;
	HAL_GPIO_Init(GPIOA, &g);

	/* Now just watch it for a second without touching it. */
	uint32_t lows = 0, total = 0;
	uint32_t until = HAL_GetTick() + 1000u;
	while ((int32_t)(HAL_GetTick() - until) < 0) {
		if (!(GPIOA->IDR & NX_RX_Pin)) {
			lows++;
		}
		total++;
	}

	uart_set_baud(9600);

	snprintf(line, sizeof(line),
	         "   sink test: %s   |   idle watch: %lu low of %lu samples\r\n",
	         read_low ? "pulls low OK" : "CANNOT pull low",
	         (unsigned long)lows, (unsigned long)total);
	emit_dbg(line);

	if (!read_low) {
		emit_dbg("   -> immovable. PA10 is tied to something low-impedance and\r\n"
		         "      high. A UART TX can be fought down to mid-rail; a 5 V\r\n"
		         "      supply cannot. Check TP5 is not on the display's 5 V.\r\n");
	} else if (lows == 0u) {
		emit_dbg("   -> pulls low fine, but never falls on its own. The line is\r\n"
		         "      idle-high and nothing is transmitting on it.\r\n");
	} else {
		emit_dbg("   -> the line is moving by itself: data is present.\r\n");
	}
}


void run_control(void)
{
	char line[180];
	uint8_t buf[192];

	heater_init();
	heater_off();
	dwt_init();
	uart_set_baud(9600);

	emit_dbg("\r\n=== Nextion probe at 9600 ===\r\n"
	         "PA9 = TX (TP6), PA10 = RX (TP5)\r\n");

	/* --- is our own TX pin actually moving? ----------------------------
	 * Written straight to the data register rather than through
	 * HAL_UART_Transmit_IT: this firmware's MSP never enables the USART1
	 * NVIC line, so the interrupt-driven call queues a transfer that can
	 * never run, leaves gState stuck at BUSY_TX, and silently turns every
	 * later HAL_UART_Transmit into a no-op. Polling TXE avoids all that. */
	emit_dbg("\r\n[1] is PA9 toggling while USART1 transmits?\r\n");
	{
		uint32_t hi = 0, lo = 0;
		for (int c = 0; c < 40; c++) {
			while (!(huart1.Instance->SR & USART_SR_TXE)) {
			}
			huart1.Instance->DR = 0x55;      /* alternating bits: most edges */
			for (int k = 0; k < 400; k++) {  /* sample inside the character */
				if (GPIOA->IDR & GPIO_PIN_9) {
					hi++;
				} else {
					lo++;
				}
			}
		}
		while (!(huart1.Instance->SR & USART_SR_TC)) {
		}
		snprintf(line, sizeof(line), "   PA9 high %lu, low %lu samples\r\n",
		         (unsigned long)hi, (unsigned long)lo);
		emit_dbg(line);
		if (lo == 0u) {
			emit_dbg("   -> PA9 never goes low: the pin really is not driving.\r\n");
		} else {
			emit_dbg("   -> PA9 drives both ways, so the MCU is sending. If the\r\n"
			         "      panel still never answers, the break is between TP6\r\n"
			         "      and the display's RX pin.\r\n");
		}
	}

	/* --- is the display's TX present on PA10? --------------------------- */
	emit_dbg("\r\n[2] is anything driving PA10?\r\n");
	pa10_report();
	pa10_pulldown_test();
	uart_set_baud(9600);

	/* --- ask it to identify itself ------------------------------------- */
	emit_dbg("\r\n[3] asking the panel to reply.\r\n"
	         "    'sendme' returns 66 <page> FF FF FF on any HMI;\r\n"
	         "    bkcmd=3 makes every command ack with 01 FF FF FF.\r\n");
	nx_collect(buf, sizeof(buf), 100);
	nx_send("bkcmd=3");
	int n = nx_collect(buf, sizeof(buf), 300);
	snprintf(line, sizeof(line), "   bkcmd=3 -> %d bytes\r\n", n);
	emit_dbg(line);
	if (n) {
		dump(buf, n);
	}
	nx_send("sendme");
	n = nx_collect(buf, sizeof(buf), 400);
	snprintf(line, sizeof(line), "   sendme  -> %d bytes\r\n", n);
	emit_dbg(line);
	if (n) {
		dump(buf, n);
	}

	/* --- keep trying, and keep the backlight pulsing -------------------- */
	emit_dbg("\r\n[4] looping. Watch the screen for a slow pulse - that means\r\n"
	         "    our TX is landing. Any bytes received appear below.\r\n"
	         "    Power-cycling the panel now should emit 00 00 00 FF FF FF.\r\n\r\n");

	uint32_t iter = 0;
	int ever_rx = 0;
	for (;;) {
		nx_send("sendme");
		n = nx_collect(buf, sizeof(buf), 400);
		if (n) {
			ever_rx = 1;
			emit_dbg("   <-- BYTES:\r\n");
			dump(buf, n);
		}

		if ((iter % 4u) == 0u) {
			nx_send("dim=15");
		} else if ((iter % 4u) == 2u) {
			nx_send("dim=100");
		}

		if ((iter % 8u) == 7u) {
			snprintf(line, sizeof(line), "   ... %lu tries, bytes seen: %s\r\n",
			         (unsigned long)iter + 1u, ever_rx ? "YES" : "no");
			emit_dbg(line);
		}
		iter++;
		HAL_Delay(500);
	}
}


/* ---- Mode 6: continuous TX, for tracing with a meter --------------------
 * Hammers 0x55 out of PA9 at 9600 with no gaps between characters. In 8N1
 * that frame is start,1,0,1,0,1,0,1,0,stop - five low bits and five high -
 * so the line sits at a 50% duty square wave and an ordinary DC multimeter
 * reads about half of 3.3 V on it.
 *
 * That gives a signal you can chase with nothing but a meter:
 *
 *     idle (not sending)   ~3.3 V
 *     sending 0x55         ~1.6 V
 *
 * It alternates between the two every three seconds and prints which state
 * it is in, so a reading that tracks the printout is definitely our signal
 * and not something else on the bench.
 *
 * Probe TP6 first to confirm the board end, then the display's RX pin. The
 * same ~1.6 V at both ends means the wire is good and the problem is at the
 * display; 3.3 V at the far end while TP6 swings means the wire is not
 * carrying it.
 */
#elif CONTROL_MODE == 6

#ifndef TX_BAUD
#define TX_BAUD 9600
#endif

/* The Nextion's RX is 5 V logic with its own pull-up to 5 V, and PA9's
 * push-pull high only reaches 3.3 V - measured 3.2 V at the panel, which is
 * under the ~3.5 V a 5 V CMOS input needs. The display therefore never sees
 * a valid high and never decodes a frame.
 *
 * Open drain fixes it with no extra hardware: the USART still sinks for a
 * low, and for a high it simply lets go and the panel's own pull-up takes
 * the line to a clean 5 V. Rise time is a microsecond or so against that
 * pull-up, which is nothing next to a 104 us bit at 9600. */
static void nx_tx_open_drain(void)
{
	GPIO_InitTypeDef g = {0};
	g.Pin = GPIO_PIN_9;
	g.Mode = GPIO_MODE_AF_OD;
	g.Pull = GPIO_NOPULL;
	g.Speed = GPIO_SPEED_FREQ_HIGH;
	HAL_GPIO_Init(GPIOA, &g);
}

void run_control(void)
{
	char line[320];

	/* Nothing to do with heating - make sure the element stays off. */
	heater_init();
	heater_off();

	HAL_UART_DeInit(&huart1);
	huart1.Init.BaudRate = TX_BAUD;
	if (HAL_UART_Init(&huart1) != HAL_OK) {
		Error_Handler();
	}
	nx_tx_open_drain();

	snprintf(line, sizeof(line),
	         "\r\n=== continuous TX on PA9 (TP6) at %lu baud ===\r\n"
	         "  sending 0x55 back to back: line sits near half rail\r\n"
	         "  expect ~1.6 V on a DC meter while SENDING\r\n"
	         "  expect  ~3.3 V while IDLE\r\n"
	         "  probe TP6 first, then the display's RX pin\r\n\r\n",
	         (unsigned long)TX_BAUD);
	emit_dbg(line);

	for (;;) {
		emit_dbg("  SENDING  (meter should read about half rail)\r\n");
		uint32_t until = HAL_GetTick() + 3000u;
		while ((int32_t)(HAL_GetTick() - until) < 0) {
			/* Straight to the data register: back-to-back characters with
			 * no idle gaps, and no dependence on the USART1 NVIC line that
			 * this firmware's MSP never enables. */
			while (!(huart1.Instance->SR & USART_SR_TXE)) {
			}
			huart1.Instance->DR = 0x55;
		}
		while (!(huart1.Instance->SR & USART_SR_TC)) {
		}

		emit_dbg("  IDLE     (meter should read full rail)\r\n");
		HAL_Delay(3000);
	}
}


/* ---- Mode 7: raw RX capture, for the power-cycle test -------------------
 * Watches PA10 as a plain input and timestamps every transition, instead of
 * handing it to the USART. That matters because a UART only yields sensible
 * bytes at the right baud, whereas edge timing reads the baud straight off
 * the wire - the narrowest pulse in a frame is one bit.
 *
 * The panel emits 00 00 00 FF FF FF when it powers up, and that happens
 * whether or not it can hear us. So power-cycling the display tests the
 * receive path end to end without depending on our TX at all, which is the
 * one direction we still have no evidence about.
 *
 * Nothing is transmitted here; PA9 is left idle so it cannot muddy the
 * result.
 */
#elif CONTROL_MODE == 7

#define MAX_EDGES  400u
#define QUIET_MS   40u      /* end of burst once the line rests this long */

static uint32_t edge_at[MAX_EDGES];

/* dwt_init lives in the mode 5 branch; mode 7 needs its own copy. */
static void dwt_init(void)
{
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

void run_control(void)
{
	char line[200];
	GPIO_InitTypeDef g = {0};

	heater_init();
	heater_off();
	dwt_init();

	/* PA10 as a plain input - no UART on it at all. */
	HAL_UART_DeInit(&huart1);
	g.Pin = GPIO_PIN_10;
	g.Mode = GPIO_MODE_INPUT;
	g.Pull = GPIO_NOPULL;
	HAL_GPIO_Init(GPIOA, &g);

	uint32_t hclk = HAL_RCC_GetHCLKFreq();

	emit_dbg("\r\n=== raw RX capture on PA10 (TP5) ===\r\n"
	         "  nothing is being transmitted - this only listens\r\n"
	         "  >>> POWER-CYCLE THE DISPLAY NOW <<<\r\n"
	         "  it should emit 00 00 00 FF FF FF as it boots\r\n\r\n");

	uint32_t idle_report = HAL_GetTick();

	for (;;) {
		/* wait for the line to leave idle */
		while (GPIOA->IDR & GPIO_PIN_10) {
			if ((int32_t)(HAL_GetTick() - idle_report) >= 0) {
				idle_report = HAL_GetTick() + 5000u;
				emit_dbg("  ... line still idle high, nothing received\r\n");
			}
		}

		/* capture transitions until the line goes quiet */
		uint32_t n = 0;
		uint32_t last = GPIOA->IDR & GPIO_PIN_10;
		uint32_t t0 = DWT->CYCCNT;
		edge_at[n++] = t0;
		uint32_t quiet_cycles = (hclk / 1000u) * QUIET_MS;
		uint32_t last_change = t0;

		while (n < MAX_EDGES) {
			uint32_t now_lvl = GPIOA->IDR & GPIO_PIN_10;
			uint32_t now = DWT->CYCCNT;
			if (now_lvl != last) {
				last = now_lvl;
				edge_at[n++] = now;
				last_change = now;
			} else if ((now - last_change) > quiet_cycles) {
				break;
			}
		}

		/* narrowest gap between transitions is one bit time */
		uint32_t shortest = 0xFFFFFFFFu;
		for (uint32_t i = 1; i < n; i++) {
			uint32_t w = edge_at[i] - edge_at[i - 1];
			if (w < shortest) {
				shortest = w;
			}
		}

		snprintf(line, sizeof(line), "\r\n  ACTIVITY: %lu transitions\r\n",
		         (unsigned long)n);
		emit_dbg(line);

		if (n > 2 && shortest > 0u) {
			uint32_t baud = hclk / shortest;
			snprintf(line, sizeof(line),
			         "  shortest pulse %lu cycles (~%lu ns) -> about %lu baud\r\n",
			         (unsigned long)shortest,
			         (unsigned long)((shortest * 1000u) / (hclk / 1000000u)),
			         (unsigned long)baud);
			emit_dbg(line);

			/* widths in bit-times, so a frame can be read by eye */
			int m = snprintf(line, sizeof(line), "  widths (bits):");
			for (uint32_t i = 1; i < n && i < 30u; i++) {
				uint32_t w = (edge_at[i] - edge_at[i - 1] + shortest / 2u) / shortest;
				m += snprintf(line + m, sizeof(line) - m, " %lu", (unsigned long)w);
			}
			snprintf(line + m, sizeof(line) - m, "\r\n");
			emit_dbg(line);

			/* Real UART frames are small integer multiples of one bit. A
			 * handful of sub-microsecond spikes separated by milliseconds is
			 * coupled noise - exactly what a 5 V rail switching nearby looks
			 * like. Do not call that a working link. */
			uint32_t wide = 0;
			for (uint32_t k = 1; k < n; k++) {
				uint32_t w = (edge_at[k] - edge_at[k - 1] + shortest / 2u) / shortest;
				if (w > 12u) {
					wide++;
				}
			}
			if (baud > 500000u || wide * 3u > n) {
				emit_dbg("  -> coupled noise, not UART: pulses too short and too\r\n"
				         "     far apart to be frames. This is not a link.\r\n");
			} else {
				emit_dbg("  -> plausible UART framing. The receive path works, so\r\n"
				         "     any failure to talk is our TX direction only.\r\n");
			}
		}
		idle_report = HAL_GetTick() + 5000u;
	}
}


#else

static const float setpoints_c[] = { 30.0f, 40.0f, 50.0f };
#define N_STAGES (sizeof(setpoints_c) / sizeof(setpoints_c[0]))

void run_control(void)
{
	char line[192];

	heater_init();

	snprintf(line, sizeof(line),
	         "\r\n=== PI heater control ===\r\n"
	         "Kp=%d.%02d %%/C  Ki=%d.%03d %%/C/s  loop %lu ms  PWM %lu Hz\r\n"
	         "setpoints 30/40/50 C, %lu ms dwell within +/-%d.%01d C\r\n"
	         "t_ms,stage,setpoint,tempC,duty_pct,phase\r\n",
	         (int)KP_PCT_PER_C, (int)(KP_PCT_PER_C * 100) % 100,
	         (int)KI_PCT_PER_C_S, (int)(KI_PCT_PER_C_S * 1000) % 1000,
	         (unsigned long)CTRL_PERIOD_MS, (unsigned long)PWM_HZ,
	         (unsigned long)DWELL_MS,
	         (int)DWELL_TOL_C, (int)(DWELL_TOL_C * 10) % 10);
	emit(line);

	const float dt = (float)CTRL_PERIOD_MS / 1000.0f;
	float integral = 0.0f;
	uint32_t t0 = HAL_GetTick();
	uint32_t next = t0;

	for (unsigned stage = 0; stage < N_STAGES; stage++) {
		const float sp = setpoints_c[stage];
		uint32_t stage_start = HAL_GetTick();
		uint32_t dwell_start = 0;
		int dwelling = 0;

		/* Carrying the integral between stages would dump the previous
		 * stage's accumulated effort into the next step. Each setpoint
		 * starts fresh. */
		integral = 0.0f;

		for (;;) {
			float t = 0.0f;
			int ok = sample(&t);
			uint32_t now = HAL_GetTick();
			float duty = 0.0f;

			if (ok && !latched_off) {
				float err = sp - t;

				/* Integrate first, then clamp the total. Holding the
				 * integral back whenever the output is saturated is what
				 * stops it winding up during the long climb to setpoint. */
				float unsat = KP_PCT_PER_C * err + integral + KI_PCT_PER_C_S * err * dt;
				if (unsat > 0.0f && unsat < DUTY_MAX_PCT) {
					integral += KI_PCT_PER_C_S * err * dt;
				}

				duty = KP_PCT_PER_C * err + integral;
				if (duty < 0.0f) {
					duty = 0.0f;
				}
				if (duty > DUTY_MAX_PCT) {
					duty = DUTY_MAX_PCT;
				}
				heater_set(duty);

				if (!dwelling && err <= DWELL_TOL_C && err >= -DWELL_TOL_C) {
					dwelling = 1;
					dwell_start = now;
				}
			}

			snprintf(line, sizeof(line), "%lu,%u,%d.%01d,%s%ld.%02ld,%d.%01d,%s\r\n",
			         (unsigned long)(now - t0), stage + 1u,
			         (int)sp, (int)(sp * 10) % 10,
			         ok ? "" : "FAULT:",
			         (long)t, (long)((int32_t)(t * 100.0f) % 100),
			         (int)duty, (int)(duty * 10) % 10,
			         dwelling ? "DWELL" : "APPROACH");
			emit(line);

			if (latched_off) {
				goto finished;
			}
			if (dwelling && (now - dwell_start) >= DWELL_MS) {
				break;
			}
			if ((now - stage_start) >= STAGE_TIMEOUT_MS) {
				snprintf(line, sizeof(line),
				         "STAGE %u TIMED OUT short of %d C - heater too weak, "
				         "or gate drive too low\r\n",
				         stage + 1u, (int)sp);
				emit(line);
				goto finished;
			}

			next += CTRL_PERIOD_MS;
			wait_until(next);
		}

		snprintf(line, sizeof(line), "STAGE %u COMPLETE at %d C\r\n",
		         stage + 1u, (int)sp);
		emit(line);
	}

finished:
	heater_off();
	emit("heater off, logging cooldown\r\n");

	uint32_t cool_start = HAL_GetTick();
	while ((HAL_GetTick() - cool_start) < COOLDOWN_MS) {
		float t;
		if (sample(&t)) {
			snprintf(line, sizeof(line), "%lu,0,0.0,%ld.%02ld,0.0,COOLDOWN\r\n",
			         (unsigned long)(HAL_GetTick() - t0),
			         (long)t, (long)((int32_t)(t * 100.0f) % 100));
			emit(line);
		}
		next += CTRL_PERIOD_MS;
		wait_until(next);
	}

	emit("DONE\r\n");
	for (;;) {
	}
}


#endif /* CONTROL_MODE == 2 */

#endif /* CONTROL_MODE */
