BOARD_TYPE          := 0xD7
BOARD_REVISION      := 0x01
BOOTLOADER_VERSION  := 0x87
HW_TYPE             := 0x00

CHIP                := STM32F303VCT
BOARD               := STM32F30x_DTFc
MODEL               := HD
MODEL_SUFFIX        := 

USB_VEND            := "DTF Air"
USB_PROD            := "DTFc"

include $(MAKE_INC_DIR)/firmware-arches.mk

# Note: These must match the values in link_$(BOARD)_memory.ld
BL_BANK_BASE        := 0x08000000  # Start of bootloader flash
BL_BANK_SIZE        := 0x00004000  # Should include BD_INFO region (16kB)

EE_BANK_BASE        := 0x08004000  # @16kB
EE_BANK_SIZE        := 0x00008000  # (32kb)

# Leave the remaining 16KB and 64KB sectors for other uses
FW_BANK_BASE        := 0x0800C000  # Start of firmware flash @48kB
FW_BANK_SIZE        := 0x00034000  # Should include FW_DESC_SIZE (208kB)

FW_DESC_SIZE        := 0x00000064

EF_BANK_BASE        := 0x08000000  # Start of entire flash image (usually start of bootloader as well)
EF_BANK_SIZE        := 0x00040000  # Size of the entire flash image (from bootloader until end of firmware)

OSCILLATOR_FREQ     :=   8000000
SYSCLK_FREQ         :=  72000000

