TARGET  = thermo_monitor
BUILD   = build

TOOLS   = C:/ST/STM32CubeIDE_1.15.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.12.3.rel1.win32_1.0.100.202403111256/tools/bin
CC      = $(TOOLS)/arm-none-eabi-gcc
OBJCOPY = $(TOOLS)/arm-none-eabi-objcopy
SIZE    = $(TOOLS)/arm-none-eabi-size

C_SOURCES = \
  Core/Src/main.c \
  Core/Src/control.c \
  Core/Src/stm32f1xx_hal_msp.c \
  Core/Src/stm32f1xx_it.c \
  Core/Src/syscalls.c \
  Core/Src/sysmem.c \
  Core/Src/system_stm32f1xx.c \
  $(wildcard Drivers/STM32F1xx_HAL_Driver/Src/*.c)

ASM_SOURCES = Core/Startup/startup_stm32f103c8tx.s

INCLUDES = \
  -ICore/Inc \
  -IDrivers/STM32F1xx_HAL_Driver/Inc \
  -IDrivers/STM32F1xx_HAL_Driver/Inc/Legacy \
  -IDrivers/CMSIS/Device/ST/STM32F1xx/Include \
  -IDrivers/CMSIS/Include

DEFS  = -DUSE_HAL_DRIVER -DSTM32F103xB
# make SCK_TEST=1 -> clock-line injection build, see main.c
ifdef SCK_TEST
DEFS += -DSCK_TEST_MODE=1
endif
# make MODE=pi   -> closed-loop PI heater control   (control.c)
# make MODE=step -> open-loop step test, for tuning (control.c)
# no MODE        -> plain thermocouple monitor      (main.c)
ifeq ($(MODE),pi)
DEFS += -DCONTROL_MODE=1
endif
ifeq ($(MODE),step)
DEFS += -DCONTROL_MODE=2
endif
# make MODE=hold STEP_DUTY=100 -> park the output so the driver can be probed
ifeq ($(MODE),hold)
DEFS += -DCONTROL_MODE=3
endif
# make MODE=server -> steered from the web app in webapp/
ifeq ($(MODE),server)
DEFS += -DCONTROL_MODE=4
endif
# make MODE=hmi -> first-contact probe for the Nextion display
ifeq ($(MODE),hmi)
DEFS += -DCONTROL_MODE=5
endif
# make MODE=tx -> continuous 0x55 on PA9, for tracing the wire with a meter
ifeq ($(MODE),tx)
DEFS += -DCONTROL_MODE=6
endif
# make MODE=rx -> listen only, timestamp edges, read the baud off the wire
ifeq ($(MODE),rx)
DEFS += -DCONTROL_MODE=7
endif
# make MODE=nextion -> drive Devin's HMI, log its touch codes, no heating
ifeq ($(MODE),nextion)
DEFS += -DCONTROL_MODE=8
endif
# Duty ceiling for the panel-driven controller; it cannot be raised from the
# display. A backstop against a bad gain, not a working limit - default 30 %.
ifdef NX_MAX_DUTY
DEFS += -DNX_MAX_DUTY=$(NX_MAX_DUTY)
endif
# Setpoint ramp, C/s. 0 restores the old step behaviour.
ifdef NX_RAMP
DEFS += -DNX_RAMP=$(NX_RAMP)
endif
# Power-on setpoint and gains for the panel controller, in the panel's own
# units: NX_SP is x10 C, NX_KP and NX_KI are x1000. Lets a bench run be set
# up at build time instead of through the panel's 0.01-per-press steppers.
#   make MODE=nextion NX_SP=900 NX_KP=1000 NX_KI=10
ifdef NX_SP
DEFS += -DNX_SP=$(NX_SP)
endif
ifdef NX_KP
DEFS += -DNX_KP=$(NX_KP)
endif
ifdef NX_KI
DEFS += -DNX_KI=$(NX_KI)
endif
# Silence the semihosting log so a debugger can attach mid-run without the
# next bkpt halting the core. Required for unattended runs read via g_trace.
ifdef NX_QUIET
DEFS += -DNX_QUIET=1
endif
# Retune without touching the source:  make MODE=pi KP=8.0 KI=0.4
ifdef KP
DEFS += -DKP_RAW=$(KP)
endif
ifdef KI
DEFS += -DKI_RAW=$(KI)
endif
ifdef STEP_DUTY
DEFS += -DSTEP_DUTY_RAW=$(STEP_DUTY)
endif
ifdef MAX_DUTY
DEFS += -DMAX_DUTY_RAW=$(MAX_DUTY)
endif
# OVERTEMP=150 raises the hard cutoff (default 120 C)
ifdef OVERTEMP
DEFS += -DOVERTEMP_RAW=$(OVERTEMP)
endif
# PWM_HZ=1 for an opto-isolated SSR; 200 (default) suits a direct MOSFET
ifdef PWM_HZ
DEFS += -DPWM_HZ=$(PWM_HZ)u
endif
# Step-test segment lengths and PI dwell, in seconds
ifdef BASE_S
DEFS += -DSTEP_BASE_S=$(BASE_S)
endif
ifdef HEAT_S
DEFS += -DSTEP_HEAT_S=$(HEAT_S)
endif
ifdef COOL_S
DEFS += -DSTEP_COOL_S=$(COOL_S)
endif
ifdef DWELL_S
DEFS += -DDWELL_S=$(DWELL_S)
endif
ARCH  = -mcpu=cortex-m3 -mthumb -mfloat-abi=soft

CFLAGS  = $(ARCH) $(DEFS) $(INCLUDES) -Og -g3 -std=gnu11 \
          -ffunction-sections -fdata-sections -Wall -MMD -MP
LDFLAGS = $(ARCH) -TSTM32F103C8TX_FLASH.ld --specs=nano.specs --specs=nosys.specs \
          -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map

# A -D flag change must force a rebuild. Without this, `make MODE=server
# MAX_DUTY=10` after a MAX_DUTY=30 build silently keeps the old objects and
# ships a binary that does not do what was asked - which is exactly the sort
# of thing you only discover when a heater overshoots by 30 C.
FLAGS_STAMP := $(BUILD)/flags.stamp
ALL_FLAGS   := $(CFLAGS) $(LDFLAGS)
OLD_FLAGS   := $(if $(wildcard $(FLAGS_STAMP)),$(file <$(FLAGS_STAMP)),)
ifneq ($(strip $(OLD_FLAGS)),$(strip $(ALL_FLAGS)))
$(shell mkdir -p $(BUILD))
$(file >$(FLAGS_STAMP),$(ALL_FLAGS))
endif

OBJECTS = $(addprefix $(BUILD)/,$(C_SOURCES:.c=.o)) \
          $(addprefix $(BUILD)/,$(ASM_SOURCES:.s=.o))

all: $(BUILD)/$(TARGET).bin

$(BUILD)/%.o: %.c $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD)/%.o: %.s $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) -c $(ARCH) -g3 -x assembler-with-cpp $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	$(SIZE) $@

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
