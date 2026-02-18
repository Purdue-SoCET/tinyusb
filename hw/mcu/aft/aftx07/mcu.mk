# hw/mcu/aft/aftx07/mcu.mk

CROSS_COMPILE ?= riscv64-unknown-elf-

CC      = $(CROSS_COMPILE)gcc
LD      = $(CROSS_COMPILE)gcc
AR      = $(CROSS_COMPILE)ar
AS      = $(CROSS_COMPILE)gcc
OBJCOPY = $(CROSS_COMPILE)objcopy
SIZE    = $(CROSS_COMPILE)size

# -------- CPU flags (rv32im example) --------
CFLAGS += -march=rv32im -mabi=ilp32
ASFLAGS += -march=rv32im -mabi=ilp32
LDFLAGS += -march=rv32im -mabi=ilp32

# -------- General good defaults --------
CFLAGS  += -ffunction-sections -fdata-sections
LDFLAGS += -Wl,--gc-sections
