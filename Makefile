SDK_DEMO_PATH ?= .
BL_SDK_BASE ?= ../bouffalo_sdk
TANG_BOARD ?= console60k
USB_CDC_CONSOLE ?= 0
USB_CDC_VID ?= 0xffff
USB_CDC_PID ?= 0x6160

export BL_SDK_BASE

CHIP ?= bl616
BOARD ?= bl616dk
CROSS_COMPILE ?= riscv64-unknown-elf-

cmake_definition+=-DTANG_BOARD=$(TANG_BOARD)
cmake_definition+=-DTANG_USB_CDC_CONSOLE=$(USB_CDC_CONSOLE)
cmake_definition+=-DTANG_USB_CDC_VID=$(USB_CDC_VID)
cmake_definition+=-DTANG_USB_CDC_PID=$(USB_CDC_PID)

include $(BL_SDK_BASE)/project.build
