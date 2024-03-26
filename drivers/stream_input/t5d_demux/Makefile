# SPDX-License-Identifier: GPL-2.0-or-later
#
#Makefile for the dvb dmx drivers
#

USE_ASM_ARM_AES := false
MODULE_NAME_DEMUX = t5d-dvb-demux
obj-$(CONFIG_AMLOGIC_DVB_DMX) = $(MODULE_NAME_DEMUX).o

ccflags-y += -I$(srctree)/include
ccflags-y += -I$(srctree)/include/media
ccflags-y += -I$(srctree)/include/uapi
ccflags-y += -I$(srctree)/common_drivers/include
ccflags-y += -I$(srctree)/common_drivers/include/linux/amlogic
ccflags-y += -I$(srctree)/$(COMMON_DRIVERS_DIR)/include/linux/amlogic
ccflags-y += -I$(srctree)/drivers/dvb

ifeq ($(USE_ASM_ARM_AES), true)
$(MODULE_NAME_DEMUX)-y += sw_demux/aes-ce-core.o
$(MODULE_NAME_DEMUX)-y += sw_demux/aes-ce-glue.o
$(MODULE_NAME_DEMUX)-y += sw_demux/swdmx_aes_cbc_arm.o
else
$(MODULE_NAME_DEMUX)-y += sw_demux/aes_cbc.o
$(MODULE_NAME_DEMUX)-y += sw_demux/cbc128.o
$(MODULE_NAME_DEMUX)-y += sw_demux/aes_core.o
$(MODULE_NAME_DEMUX)-y += sw_demux/swdmx_aes_cbc.o
endif

$(MODULE_NAME_DEMUX)-y += sw_demux/swdmx_cb_list.o
$(MODULE_NAME_DEMUX)-y += sw_demux/swdmx_demux.o
$(MODULE_NAME_DEMUX)-y += sw_demux/swdmx_pes_parser.o
$(MODULE_NAME_DEMUX)-y += sw_demux/swdmx_crc32.o
$(MODULE_NAME_DEMUX)-y += sw_demux/swdmx_descrambler.o
$(MODULE_NAME_DEMUX)-y += sw_demux/swdmx_ts_parser.o

$(MODULE_NAME_DEMUX)-y += t5d_module.o
$(MODULE_NAME_DEMUX)-y += t5d_demux.o
$(MODULE_NAME_DEMUX)-y += t5d_hw_dmx.o
$(MODULE_NAME_DEMUX)-y += t5d_dsc.o
$(MODULE_NAME_DEMUX)-y += t5d_key.o
