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
#include <stdlib.h>

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
/* Time-proportioned, 1 Hz. The load is an SSR-25DD: it needs roughly a
 * millisecond to switch, so at 200 Hz a 1 % duty is a 48 us pulse and the
 * relay simply never turns on. Resolution stays 0.05 % because PWM_PERIOD is
 * always 2000 counts. The old 200 Hz default belonged to the IRF520N. */
#define PWM_HZ             1u
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

/* Put PA8 back under the timer. heater_off() deliberately tears that down so
 * the pin is held low by the GPIO block even if the timer misbehaves, which
 * means arming again has to be explicit. */
static void heater_arm(void)
{
	GPIO_InitTypeDef gpio = {0};
	gpio.Pin = GPIO_PIN_8;
	gpio.Mode = GPIO_MODE_AF_PP;
	gpio.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(GPIOA, &gpio);
	HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
}

static void heater_set(float pct)
{
	if (latched_off || pct < 0.0f) {
		pct = 0.0f;
	}
	if (pct > DUTY_MAX_PCT) {
		pct = DUTY_MAX_PCT;
	}
	uint32_t cmp = (uint32_t)((pct / 100.0f) * (float)PWM_PERIOD);
	__HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, cmp);
	/* Writing CCR1 does nothing while the timer is stopped and PA8 is a plain
	 * GPIO. Commanding real output has to restore both. */
	if (cmp != 0u && (TIM1->CR1 & TIM_CR1_CEN) == 0u) {
		heater_arm();
	}
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
__attribute__((unused)) static const uint32_t probe_bauds[] = {
	/* A new HMI can change the stored rate via `bauds=` in its Program.s,
	 * so do not trust the last known value. Most likely first. */
	9600, 115200, 38400, 57600, 19200, 230400, 250000, 921600
};  /* the panel's
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
__attribute__((unused)) static void nx_tx_open_drain(void)
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


/* What is the 3.3 V rail actually at?
 *
 * Both UART pins reading 2.6 V with nothing plugged in is the giveaway. R6
 * and R9 are 10k pull-ups to +3.3 V, so an idle pin should sit at the rail.
 * If it sits at 2.6 V, the rail is at 2.6 V - and an AMS1117 with ~1.1 V of
 * dropout produces exactly that from a VBUS sagging to ~3.7 V under the
 * panel's 430 mA.
 *
 * The STM32 can measure its own supply without any external help: ADC
 * channel 17 is an internal 1.20 V bandgap reference, and the ADC measures
 * it as a fraction of VDDA. So VDD = 1.20 * 4095 / raw.
 *
 * Done with raw registers because the ADC HAL files are not in this project.
 */
static void report_vdd(void)
{
	char line[180];

	__HAL_RCC_ADC1_CLK_ENABLE();
	/* ADC clock must be <= 14 MHz and PCLK2 is 64 MHz, so divide by 6. */
	RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_ADCPRE) | RCC_CFGR_ADCPRE_DIV6;

	ADC1->CR2 = ADC_CR2_ADON;                 /* wake the ADC */
	HAL_Delay(2);
	ADC1->CR2 |= ADC_CR2_TSVREFE;             /* connect VREFINT / temp sensor */
	HAL_Delay(2);

	ADC1->CR2 |= ADC_CR2_RSTCAL;
	while (ADC1->CR2 & ADC_CR2_RSTCAL) {
	}
	ADC1->CR2 |= ADC_CR2_CAL;
	while (ADC1->CR2 & ADC_CR2_CAL) {
	}

	/* channel 17, longest sample time - the bandgap needs >= 17.1 us */
	ADC1->SMPR1 = (ADC1->SMPR1 & ~(7u << 21)) | (7u << 21);
	ADC1->SQR1 = 0;
	ADC1->SQR3 = 17u;
	ADC1->CR2 |= ADC_CR2_EXTSEL | ADC_CR2_EXTTRIG;   /* SWSTART */

	uint32_t sum = 0;
	const int N = 16;
	for (int i = 0; i < N; i++) {
		ADC1->SR = 0;
		ADC1->CR2 |= ADC_CR2_SWSTART;
		uint32_t guard = 0;
		while (!(ADC1->SR & ADC_SR_EOC) && ++guard < 1000000u) {
		}
		sum += ADC1->DR & 0x0FFFu;
	}
	uint32_t raw = sum / N;

	if (raw == 0u) {
		emit_dbg("   ADC gave nothing - cannot read VDD\r\n");
		return;
	}

	uint32_t vdd_mv = (1200u * 4095u) / raw;
	snprintf(line, sizeof(line), "   VREFINT raw %lu  ->  VDD = %lu.%03lu V\r\n",
	         (unsigned long)raw,
	         (unsigned long)(vdd_mv / 1000u), (unsigned long)(vdd_mv % 1000u));
	emit_dbg(line);

	if (vdd_mv < 3000u) {
		emit_dbg("   -> THE RAIL IS LOW. The AMS1117 is dropping out, which means\r\n"
		         "      VBUS is sagging - almost certainly the panel's 430 mA\r\n"
		         "      pulled through the micro-USB and the PCB. Every logic high\r\n"
		         "      we drive is only this high, which is why the panel cannot\r\n"
		         "      read us. Give the display its own 5 V supply.\r\n");
	} else if (vdd_mv > 3600u) {
		emit_dbg("   -> rail is high; check the regulator.\r\n");
	} else {
		emit_dbg("   -> rail is healthy, so the 2.6 V on the UART pins is not a\r\n"
		         "      supply problem and something else is loading them.\r\n");
	}
}


void run_control(void)
{
	char line[200];
	uint8_t buf[192];

	heater_init();
	heater_off();
	dwt_init();
	uart_set_baud(9600);

	emit_dbg("\r\n=== Nextion probe ===\r\n"
	         "PA9 = TX (TP6), PA10 = RX (TP5)\r\n");

	/* --- is our own TX pin actually moving? ----------------------------
	 * Written straight to the data register rather than through
	 * HAL_UART_Transmit_IT: this firmware's MSP never enables the USART1
	 * NVIC line, so the interrupt-driven call queues a transfer that can
	 * never run, leaves gState stuck at BUSY_TX, and silently turns every
	 * later HAL_UART_Transmit into a no-op. Polling TXE avoids all that. */
	emit_dbg("\r\n[0] what is the 3.3 V rail actually at?\r\n");
	report_vdd();

	emit_dbg("\r\n[1] is PA9 toggling while USART1 transmits?\r\n");
	{
		uint32_t hi = 0, lo = 0;
		for (int c = 0; c < 40; c++) {
			while (!(huart1.Instance->SR & USART_SR_TXE)) {
			}
			huart1.Instance->DR = 0x55;
			for (int k = 0; k < 400; k++) {
				if (GPIOA->IDR & GPIO_PIN_9) {
					hi++;
				} else {
					lo++;
				}
			}
		}
		while (!(huart1.Instance->SR & USART_SR_TC)) {
		}
		snprintf(line, sizeof(line), "   PA9 high %lu, low %lu -> %s\r\n",
		         (unsigned long)hi, (unsigned long)lo,
		         lo ? "driving both ways, MCU is sending" : "NEVER LOW, not transmitting");
		emit_dbg(line);
	}

	/* --- is the display's TX present on PA10? --------------------------- */
	emit_dbg("\r\n[2] is anything driving PA10?\r\n");
	pa10_report();
	pa10_pulldown_test();
	uart_set_baud(9600);

	/* --- sweep every plausible rate asking for a reply -------------------
	 * A freshly uploaded HMI can change the stored rate through `bauds=` in
	 * its Program.s, so the last known value proves nothing. 'sendme'
	 * returns 66 <page> FF FF FF on any HMI regardless of bkcmd, which is
	 * why it is the probe of choice. */
	emit_dbg("\r\n[3] sweeping baud rates, asking each for a reply:\r\n");
	uint32_t found = 0;
	for (unsigned pass = 0; pass < 3u && !found; pass++) {
		for (unsigned i = 0; i < N_BAUDS; i++) {
			uart_set_baud(probe_bauds[i]);
			HAL_Delay(30);
			nx_collect(buf, sizeof(buf), 60);

			nx_send("");            /* terminator: close any partial command */
			nx_send("bkcmd=3");
			nx_send("sendme");
			int n = nx_collect(buf, sizeof(buf), 350);

			snprintf(line, sizeof(line), "   %7lu baud -> %d bytes%s\r\n",
			         (unsigned long)probe_bauds[i], n, n ? "   <-- REPLY" : "");
			emit_dbg(line);
			if (n) {
				dump(buf, n);
				found = probe_bauds[i];
				break;
			}
		}
		if (!found && pass == 0u) {
			emit_dbg("   (nothing yet - repeating, power-cycle the panel now\r\n"
			         "    if you want to catch its startup message)\r\n");
		}
	}

	if (found) {
		snprintf(line, sizeof(line),
		         "\r\n*** PANEL ANSWERED AT %lu BAUD ***\r\n", (unsigned long)found);
		emit_dbg(line);
		uart_set_baud(found);
		nx_send("bkcmd=3");
		nx_send("page 0");
	} else {
		emit_dbg("\r\nNo reply at any rate.\r\n");
		uart_set_baud(9600);
	}

	/* --- keep listening, and keep something visible happening ----------- */
	emit_dbg("\r\n[4] listening. Touch the panel; codes appear below.\r\n"
	         "    Backlight pulses so our TX direction stays testable.\r\n\r\n");

	uint32_t iter = 0;
	int ever_rx = 0;
	for (;;) {
		int n = nx_collect(buf, sizeof(buf), 400);
		if (n) {
			ever_rx = 1;
			emit_dbg("   <-- BYTES:\r\n");
			dump(buf, n);
		}

		if ((iter % 6u) == 0u) {
			nx_send("dim=20");
		} else if ((iter % 6u) == 3u) {
			nx_send("dim=100");
		}
		if ((iter % 4u) == 1u) {
			nx_send("sendme");
		}

		if ((iter % 12u) == 11u) {
			snprintf(line, sizeof(line), "   ... %lu polls, bytes seen: %s\r\n",
			         (unsigned long)iter + 1u, ever_rx ? "YES" : "no");
			emit_dbg(line);
		}
		iter++;
		HAL_Delay(250);
	}
}


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
__attribute__((unused)) static void nx_tx_open_drain(void)
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


/* ---- Mode 8: Nextion-driven controller ----------------------------------
 * Implements the contract in Software/Display/README.txt.
 *
 * Wire the panel straight to PA9/PA10 - J4's level shifters are miswired and
 * have no DC path to the MCU.
 *
 * panel -> MCU, ASCII lines ending CR LF:
 *   p0b20 START      p0b21 STOP
 *   p0b10/11/12      monitor / setup / trend
 *   SP=<x10>,KP=<x1000>,KI=<x1000>     from APPLY
 *
 * MCU -> panel, ASCII ending FF FF FF:
 *   xPV xSP xDuty xErr   all x100, vscope global, writable from any page
 *   tState tLed tMode tPI tAlarm s0   page-local, only while their page is up
 *
 * Bandwidth matters here: 9600 baud is ~960 byte/s, so the four numbers go
 * out every cycle and the decorations only when they change.
 *
 * Safety: sensor fault forces duty to 0; over-temperature latches off until
 * reset; duty is capped by NX_MAX_DUTY, compiled in and not raisable from the
 * panel; and the heater disarms if the panel goes quiet, because a controller
 * whose user interface has vanished must not keep heating.
 */
#elif CONTROL_MODE == 8

#define NX_BAUD          9600u
#define NX_SILENCE_MS    6000u
#define NX_POLL_MS       2000u

/* ~160 W into a plant that holds setpoint on under 1 % duty. The ceiling is
 * the real safety device, not the gains: make MODE=nextion NX_MAX_DUTY=5 */
#ifndef NX_MAX_DUTY
#define NX_MAX_DUTY      2.0
#endif

/* tLed.bco colours, RGB565 decimal, from the HMI contract */
#define LED_GRAY    33808u
#define LED_GREEN    2016u
#define LED_RED     63488u
#define LED_ORANGE  64800u

/* which page the panel is showing, from the last nav code */
#define PG_MONITOR  0
#define PG_SETUP    1
#define PG_TREND    2

static uint8_t frame[64];
static uint32_t frame_n;
static uint32_t nx_last_byte;

/* Panel diagnostics. Deliberately non-static so `nm` can find the symbol:
 * OpenOCD reads this through the DAP while the core keeps running, which means
 * "did that button actually reach the MCU?" can be answered after the fact
 * instead of by pressing a key inside a log capture window. */
volatile struct {
	uint32_t bytes;      /* every byte the ISR has seen */
	uint32_t frames;     /* completed frames of any kind */
	uint32_t ascii;      /* CR LF terminated frames, i.e. touch codes */
	uint32_t starts;     /* p0b20 decoded */
	uint32_t stops;      /* p0b21 decoded */
	uint32_t applies;    /* SP= / KP= / KI= decoded */
	uint32_t unknown;    /* ASCII frame we did not recognise */
	uint32_t overflows;  /* frame buffer reset because it filled */
	char     last[32];   /* most recent decoded code */
} g_nx;
static int nx_page = PG_MONITOR;        /* monitor is the boot page */
static int nx_enable = 0;
static volatile uint32_t nx_last_rx;
static int32_t nx_sp_x10 = 300;
static int32_t nx_kp_milli = 200;
static int32_t nx_ki_milli = 2;

static void nx_tx(const char *cmd)
{
	static const uint8_t term[3] = { 0xFF, 0xFF, 0xFF };
	HAL_UART_Transmit(&huart1, (uint8_t *)cmd, strlen(cmd), 200);
	HAL_UART_Transmit(&huart1, (uint8_t *)term, 3, 100);
}

static void nx_set(const char *obj, int32_t value)
{
	char buf[48];
	snprintf(buf, sizeof(buf), "%s.val=%ld", obj, (long)value);
	nx_tx(buf);
}

static void nx_txt(const char *obj, const char *value)
{
	char buf[80];
	snprintf(buf, sizeof(buf), "%s.txt=\"%s\"", obj, value);
	nx_tx(buf);
}

static void nx_report(const uint8_t *f, uint32_t n)
{
	char line[200];

	if (n >= 3u && f[n - 2] == 0x0D && f[n - 1] == 0x0A) {
		char code[80] = {0};
		uint32_t c = 0;
		for (uint32_t i = 0; i < n - 2u && c < sizeof(code) - 1u; i++) {
			if (f[i] >= 32 && f[i] < 127) {
				code[c++] = (char)f[i];
			}
		}

		g_nx.ascii++;
		snprintf((char *)g_nx.last, sizeof(g_nx.last), "%.31s", code);

		const char *sp = strstr(code, "SP=");
		const char *kp = strstr(code, "KP=");
		const char *ki = strstr(code, "KI=");
		if (sp || kp || ki) {
			if (sp) { nx_sp_x10   = atoi(sp + 3); }
			if (kp) { nx_kp_milli = atoi(kp + 3); }
			if (ki) { nx_ki_milli = atoi(ki + 3); }
			snprintf(line, sizeof(line),
			         "  APPLY  sp %ld.%ld C  Kp %ld.%03ld  Ki %ld.%03ld\r\n",
			         (long)(nx_sp_x10 / 10), (long)(nx_sp_x10 % 10),
			         (long)(nx_kp_milli / 1000), (long)(nx_kp_milli % 1000),
			         (long)(nx_ki_milli / 1000), (long)(nx_ki_milli % 1000));
			emit_dbg(line);
			g_nx.applies++;
			return;
		}
		if (strstr(code, "p0b20")) {
			nx_enable = 1;
			g_nx.starts++;
			emit_dbg("  START\r\n");
		} else if (strstr(code, "p0b21")) {
			nx_enable = 0;
			g_nx.stops++;
			emit_dbg("  STOP\r\n");
		} else if (strstr(code, "p0b10")) {
			nx_page = PG_MONITOR;
		} else if (strstr(code, "p0b11")) {
			nx_page = PG_SETUP;
		} else if (strstr(code, "p0b12")) {
			nx_page = PG_TREND;
			nx_tx("cle 1,255");          /* fresh trace on entry */
		} else {
			/* Worth a line: a button that arrives but is not understood looks
			 * exactly like one that was never sent. */
			g_nx.unknown++;
			snprintf(line, sizeof(line), "  rx \"%s\"\r\n", code);
			emit_dbg(line);
		}
		return;
	}

	/* 66 <page> FF FF FF, the reply to `sendme`. Trust it over the nav codes:
	 * it also reports the keyboard page, which no nav button announces. */
	if (n >= 2u && f[0] == 0x66) {
		nx_page = (int)f[1];
	}
}

/* RX runs on an interrupt, not on polling. The control cycle cannot be relied
 * on to visit the UART often enough: the MAX6675 conversion alone is 220 ms of
 * a 250 ms period, and the telemetry writes are blocking on top of that, so a
 * polled receive loses bytes and the dead-man trips at random. The USART1 NVIC
 * line is unused by this project's MSP, so claim it here. */
static volatile uint8_t nx_ring[256];
static volatile uint8_t nx_head, nx_tail;

void USART1_IRQHandler(void)
{
	if (USART1->SR & (USART_SR_RXNE | USART_SR_ORE)) {
		uint8_t b = (uint8_t)(USART1->DR & 0xFF);   /* the read clears ORE */
		uint8_t h = (uint8_t)(nx_head + 1u);
		if (h != nx_tail) {                        /* drop, never overwrite */
			nx_ring[nx_head] = b;
			nx_head = h;
		}
		nx_last_rx = HAL_GetTick();
		g_nx.bytes++;
	}
}

/* Drain the ring for up to `ms`, assembling frames. ms = 0 takes whatever is
 * already there and returns. Any byte at all counts as the panel being alive. */
static void nx_pump(uint32_t ms)
{
	uint32_t until = HAL_GetTick() + ms;
	for (;;) {
		if (nx_tail == nx_head) {
			/* Stray status bytes carry no terminator. Drop a stalled partial
			 * so it cannot prefix the next real frame. */
			if (frame_n && (HAL_GetTick() - nx_last_byte) > 100u) {
				frame_n = 0;
			}
			if ((int32_t)(HAL_GetTick() - until) >= 0) {
				return;
			}
			continue;
		}
		uint8_t b = nx_ring[nx_tail];
		nx_tail = (uint8_t)(nx_tail + 1u);
		if (frame_n >= sizeof(frame)) {
			frame_n = 0;        /* junk: start over rather than jam forever */
			g_nx.overflows++;
		}
		frame[frame_n++] = b;
		nx_last_byte = HAL_GetTick();
		int done = 0;
		if (frame_n >= 3u && frame[frame_n - 1] == 0xFF &&
		    frame[frame_n - 2] == 0xFF && frame[frame_n - 3] == 0xFF) {
			frame_n -= 3u;
			done = 1;
		} else if (frame_n >= 2u && frame[frame_n - 1] == 0x0A &&
		           frame[frame_n - 2] == 0x0D) {
			done = 1;
		}
		if (done) {
			g_nx.frames++;
			if (frame_n) {
				nx_report(frame, frame_n);
			}
			frame_n = 0;
		}
	}
}

void run_control(void)
{
	char line[200];
	const float duty_max = (float)(NX_MAX_DUTY);

	heater_init();
	heater_off();

	HAL_UART_DeInit(&huart1);
	huart1.Init.BaudRate = NX_BAUD;
	if (HAL_UART_Init(&huart1) != HAL_OK) {
		Error_Handler();
	}

	snprintf(line, sizeof(line),
	         "\r\n=== Nextion controller ===\r\n"
	         "  duty ceiling %d.%01d %%, over-temp latch %d C\r\n"
	         "  p0b20 START / p0b21 STOP / SP=,KP=,KI= APPLY\r\n"
	         "  disarms if the panel is silent for %lu ms\r\n\r\n",
	         (int)duty_max, (int)(duty_max * 10) % 10, (int)OVERTEMP_C,
	         (unsigned long)NX_SILENCE_MS);
	emit_dbg(line);

	nx_head = nx_tail = 0;
	USART1->CR1 |= USART_CR1_RXNEIE;
	HAL_NVIC_SetPriority(USART1_IRQn, 1, 0);
	HAL_NVIC_EnableIRQ(USART1_IRQn);

	nx_pump(150);
	/* bkcmd=0, emphatically. At bkcmd=1 the panel returns a bare 0x01 after
	 * every command, and those are unterminated single bytes: four per cycle
	 * saturate the frame buffer in about four seconds, after which no touch
	 * code is ever parsed again. We never read the acks, so do not ask. */
	nx_tx("bkcmd=0");
	nx_pump(150);
	nx_txt("tMode", "CONSTANT");
	nx_pump(100);

	const float dt = (float)CTRL_PERIOD_MS / 1000.0f;
	float integ_err = 0.0f;
	uint32_t next = HAL_GetTick();
	uint32_t last_poll = 0, last_pi = 0;
	uint32_t iter = 0;
	const char *shown_state = "";
	uint32_t shown_led = 0;
	int shown_alarm = -1;
	nx_last_rx = HAL_GetTick();

	for (;;) {
		uint32_t now = HAL_GetTick();

		uint16_t raw;
		int ok = (max6675_read(&raw) == FAULT_NONE);
		float t = ok ? (float)max6675_milli_c(raw) / 1000.0f : 0.0f;

		if (ok && t >= OVERTEMP_C && !latched_off) {
			latched_off = 1;
			heater_off();
			emit_dbg("  SAFETY: over temperature, latched off\r\n");
		}

		int alive = (now - nx_last_rx) < NX_SILENCE_MS;
		float kp = (float)nx_kp_milli / 1000.0f;
		float ki = (float)nx_ki_milli / 1000.0f;
		float sp = (float)nx_sp_x10 / 10.0f;
		float duty = 0.0f, p_term = 0.0f, i_term = 0.0f;
		const char *state;
		uint32_t led;

		if (latched_off) {
			state = "OVER TEMP";  led = LED_RED;
		} else if (!ok) {
			state = "TC FAULT";   led = LED_RED;
			integ_err = 0.0f;
		} else if (!alive) {
			state = "WARNING";    led = LED_ORANGE;
			nx_enable = 0;
			integ_err = 0.0f;
		} else if (!nx_enable) {
			state = "IDLE";       led = LED_GRAY;
			integ_err = 0.0f;
		} else {
			float err = sp - t;
			if (ki > 0.0f) {
				float cand = integ_err + err * dt;
				float u = kp * err + ki * cand;
				/* Conditional integration, blocked only in the direction that
				 * would push further into a rail. The previous test blocked
				 * BOTH directions while saturated, which let the integral
				 * freeze at a value whose I term alone met the ceiling - after
				 * which duty sat at the cap permanently and no setpoint or gain
				 * change could retrieve it. Integration that relieves the rail
				 * must always be allowed. */
				int relieving = (err < 0.0f && u >= duty_max) ||
				                (err > 0.0f && u <= 0.0f);
				if ((u > 0.0f && u < duty_max) || relieving) {
					integ_err = cand;
				}
				/* The element only heats, so a negative integral is just lag,
				 * and I alone must never be able to exceed the ceiling or P
				 * loses all authority over the output. */
				if (integ_err < 0.0f) {
					integ_err = 0.0f;
				}
				if (integ_err > duty_max / ki) {
					integ_err = duty_max / ki;
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
			if (duty > duty_max) {
				duty = duty_max;
			}
			state = "HEATING";    led = LED_GREEN;
		}
		heater_set(duty);

		/* --- global values: safe to write from any page ------------- */
		int32_t pv_x100 = ok ? (max6675_milli_c(raw) / 10) : 0;
		int32_t sp_x100 = nx_sp_x10 * 10;
		nx_set("xPV", pv_x100);
		nx_set("xSP", sp_x100);
		nx_set("xDuty", (int32_t)(duty * 100.0f));
		nx_set("xErr", sp_x100 - pv_x100);

		/* --- decorations: only on change, and only where they live --- */
		if (state != shown_state) {
			shown_state = state;
			if (nx_page == PG_MONITOR) {
				nx_txt("tState", state);
			}
		}
		if (led != shown_led) {
			shown_led = led;
			if (nx_page == PG_MONITOR) {
				nx_set("tLed.bco", (int32_t)led);
			}
		}
		int alarm = latched_off ? 1 : 0;
		if (alarm != shown_alarm && nx_page == PG_MONITOR) {
			shown_alarm = alarm;
			if (alarm) {
				nx_txt("tAlarm", "OVER-TEMPERATURE - OUTPUT LATCHED OFF");
			}
			nx_tx(alarm ? "vis tAlarm,1" : "vis tAlarm,0");
		}
		if (nx_page == PG_MONITOR && (now - last_pi) >= 1000u) {
			last_pi = now;
			char pi[40];
			snprintf(pi, sizeof(pi), "%ld.%01ld / %ld.%01ld %%",
			         (long)p_term, (long)((int32_t)(p_term * 10.0f) % 10),
			         (long)i_term, (long)((int32_t)(i_term * 10.0f) % 10));
			nx_txt("tPI", pi);
		}

		/* --- trend: one sample pair while that page is up ------------
		 * `add` takes a single byte, so rows above 255 are unreachable;
		 * the contract places the 100 C label at row 255 accordingly. */
		if (nx_page == PG_TREND) {
			int32_t y = (int32_t)(t * 2.8f + 0.5f);
			if (y < 0)   { y = 0; }
			if (y > 255) { y = 255; }
			snprintf(line, sizeof(line), "add 1,0,%ld", (long)y);
			nx_tx(line);
			int32_t ys = (int32_t)(sp * 2.8f + 0.5f);
			if (ys < 0)   { ys = 0; }
			if (ys > 255) { ys = 255; }
			snprintf(line, sizeof(line), "add 1,1,%ld", (long)ys);
			nx_tx(line);
		}

		if ((now - last_poll) >= NX_POLL_MS) {
			last_poll = now;
			nx_tx("sendme");             /* doubles as the liveness probe */
		}

		if ((iter % 8u) == 0u) {
			/* P and I are reported separately on purpose. A duty sitting on the
			 * ceiling says nothing about why: an over-large Kp and a wound-up
			 * integral look identical in the total. Milli-units so the printing
			 * stays integer and signs survive. */
			snprintf(line, sizeof(line),
			         "  %-10s PV %ld.%02ld  SP %ld.%ld  Kp %ld.%03ld Ki %ld.%03ld"
			         "  P %ld I %ld milli  duty %ld.%02ld %%  page %d\r\n",
			         state,
			         (long)t, (long)((int32_t)(t * 100.0f) % 100),
			         (long)(nx_sp_x10 / 10), (long)(nx_sp_x10 % 10),
			         (long)(nx_kp_milli / 1000), (long)(nx_kp_milli % 1000),
			         (long)(nx_ki_milli / 1000), (long)(nx_ki_milli % 1000),
			         (long)(p_term * 1000.0f), (long)(i_term * 1000.0f),
			         (long)duty, (long)((int32_t)(duty * 100.0f) % 100), nx_page);
			emit_dbg(line);
		}

		iter++;
		next += CTRL_PERIOD_MS;
		if ((int32_t)(HAL_GetTick() - next) > (int32_t)CTRL_PERIOD_MS) {
			next = HAL_GetTick();      /* overran: resync, do not chase */
		}
		nx_pump(0);                    /* always drain, even with no slack */
		while ((int32_t)(HAL_GetTick() - next) < 0) {
			nx_pump(5);
		}
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
