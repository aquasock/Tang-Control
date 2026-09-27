cmake_minimum_required(VERSION 3.15)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CONFIG_FREERTOS 1)
set(CONFIG_CHERRYUSB 1)
set(CONFIG_CHERRYUSB_HOST 1)
set(CONFIG_BSP_SDH_SDCARD 1)
set(CONFIG_FATFS 1)
set(CONFIG_FATFS_USBH 1)
set(CONFIG_FATFS_SDH_SDCARD 1)

option(TANG_USB_CDC_CONSOLE
       "Use the Console 138K DEBUG/OTG port as a CDC console" OFF)
set(TANG_USB_CDC_VID "0xffff" CACHE STRING "USB CDC vendor ID")
set(TANG_USB_CDC_PID "0x6160" CACHE STRING "USB CDC product ID")

if(TANG_USB_CDC_CONSOLE)
    if(NOT TANG_BOARD STREQUAL "console138k")
        message(FATAL_ERROR
                "TANG_USB_CDC_CONSOLE is currently supported only on console138k")
    endif()
    set(CONFIG_CHERRYUSB_DEVICE 1)
    set(CONFIG_CHERRYUSB_DEVICE_CDC 1)
    add_definitions(
        -DTANG_USB_CDC_CONSOLE=1
        -DTANG_USB_CDC_VID=${TANG_USB_CDC_VID}
        -DTANG_USB_CDC_PID=${TANG_USB_CDC_PID}
    )
endif()

if(TANG_BOARD STREQUAL "mega60k")
    add_definitions(-DTANG_MEGA60K)
elseif(TANG_BOARD STREQUAL "mega138k")
    add_definitions(-DTANG_MEGA138K)
elseif(TANG_BOARD STREQUAL "console60k") 
    add_definitions(-DTANG_CONSOLE60K)
elseif(TANG_BOARD STREQUAL "console138k")
    add_definitions(-DTANG_CONSOLE138K)
elseif(TANG_BOARD STREQUAL "primer25k")
    add_definitions(-DTANG_PRIMER25K)
elseif(TANG_BOARD STREQUAL "nano20k")
    add_definitions(-DTANG_NANO20K)
endif()

find_package(bouffalo_sdk REQUIRED HINTS $ENV{BL_SDK_BASE})

# Add include directories
sdk_add_include_directories(
    . usb fpga ui core utils
)

# Add source files
file(GLOB_RECURSE MY_SOURCES
    "usb/*.cpp"
    "fpga/*.cpp"
    "ui/*.cpp"
    "core/*.cpp"
    "utils/*.cpp"
)

if(NOT TANG_USB_CDC_CONSOLE)
    list(FILTER MY_SOURCES EXCLUDE REGEX ".*/usb_cdc_console\\.cpp$")
endif()

target_sources(app PRIVATE ${MY_SOURCES})
sdk_set_main_file(main.cpp)

project(tangcore CXX)
