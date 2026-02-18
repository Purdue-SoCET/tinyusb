# ------------------------------------------
# Board: de2_115_aft (AFTX07 running on FPGA)
# Location: hw/bsp/aft/boards/de2_115_aft/board.mk
# ------------------------------------------

VENDOR      = aft
CHIP_FAMILY = aftx07

BOARD       = de2_115_aft

CROSS_COMPILE ?= riscv32-unknown-elf-

CC = $(CROSS_COMPILE)gcc
AS = $(CROSS_COMPILE)gcc
AR = $(CROSS_COMPILE)ar
LD = $(CROSS_COMPILE)gcc
OBJCOPY = $(CROSS_COMPILE)objcopy
SIZE = $(CROSS_COMPILE)size

# -------- TinyUSB config: host mode --------
CFLAGS += \
  -DCFG_TUSB_MCU=OPT_MCU_AFTX07 \
  -DCFG_TUH_ENABLED=1 \
  -DCFG_TUD_ENABLED=0 \
  -DCFG_TUSB_RHPORT0_MODE="(OPT_MODE_HOST | OPT_MODE_FULL_SPEED)" \
  -DCFG_TUSB_OS=OPT_OS_NONE

# -------- Includes --------
INC += \
  $(TOP)/hw/bsp/aft/boards/de2_115_aft \
  $(TOP)/hw/mcu/aft/aftx07

# -------- Sources (board + HCD) --------
SRC_C += \
  hw/bsp/aft/boards/de2_115_aft/board_de2_115_aft.c \
  src/portable/aft/aftx07/hcd_aftx07.c

# startup .S? 

# -------- Linker script --------
LD_FILE = hw/bsp/aft/boards/de2_115_aft/de2_115_aft.ld

LDFLAGS += \
  -T$(TOP)/$(LD_FILE) \
  -Wl,--gc-sections \
  -Wl,-Map=$(@:.elf=.map)

# may want:
# LDFLAGS += -nostartfiles -nostdlib

# -------- Basic compile flags (safe defaults) --------
CFLAGS += \
  -ffunction-sections -fdata-sections \
  -Wall -Wextra -Werror-implicit-function-declaration

# CPU-specific flags?
# RISC-V:
# CFLAGS += -march=rv32im -mabi=ilp32
# ARM:
# CFLAGS += -mcpu=cortex-m3 -mthumb
