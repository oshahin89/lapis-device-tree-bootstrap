# Research scaffold only — NOT a build-ready BoardConfig.mk.
# Values below are grounded in the supplied stock images; remaining board
# configuration must be derived from the matching Android build branch.

TARGET_ARCH := arm64
TARGET_ARCH_VARIANT := armv8-a
TARGET_CPU_VARIANT := TODO_FROM_MATCHING_KERNEL_AND_DEVICE_CONFIG

BOARD_PAGE_SIZE := 4096
BOARD_BOOT_HEADER_VERSION := 4
BOARD_INIT_BOOT_HEADER_VERSION := 4
BOARD_VENDOR_BOOT_HEADER_VERSION := 4

# TODO: determine exact boot image header cmdline, offsets, base, dtb/DTBO layout.
# TODO: determine kernel source branch, defconfig, image type, modules, and ABI.
# TODO: determine partition sizes, dynamic partition group, super layout, AVB keys.
# TODO: determine vendor/product/odm/mi_ext proprietary blob lists and VINTF.
# TODO: add exact SELinux policy and file contexts from matching firmware.
# Do not use this file to build/flash until all TODOs are resolved and tested.
