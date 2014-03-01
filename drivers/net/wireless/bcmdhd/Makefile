# bcmdhd
SRCROOT = drivers/net/wireless/bcmdhd
DHDCFLAGS = -Wall -Wstrict-prototypes -Werror -Dlinux -DBCMDRIVER \
	-DBCMDONGLEHOST -DUNRELEASEDCHIP -DBCMDMA32 -DWLBTAMP -DBCMFILEIMAGE  \
	-DDHDTHREAD -DDHD_GPL -DDHD_SCHED -DBDC -DTOE    \
	-DDHD_BCMEVENTS -DSHOW_EVENTS -DDONGLEOVERLAYS -DBCMDBG               \
	-DMMC_SDIO_ABORT -DBCMSDIO -DBCMLXSDMMC -DBCMPLATFORM_BUS -DWLP2P     \
	-DNEW_COMPAT_WIRELESS -DWIFI_ACT_FRAME -DARP_OFFLOAD_SUPPORT          \
	-DKEEP_ALIVE -DCSCAN -DPKT_FILTER_SUPPORT -DWL_ENABLE_P2P_IF    \
	-DEMBEDDED_PLATFORM -DENABLE_INSMOD_NO_FW_LOAD -DPNO_SUPPORT          \
	-I$(SRCROOT) \
	-I$(SRCROOT)/include \

# common patch  - should be defiend!!!!
DHDCFLAGS += -DCONFIG_COMMON_PATCH

# debug
#DHDCFLAGS += -DDHD_DEBUG -DSDTEST

# for P2P
DHDCFLAGS += -DP2P_PATCH

ifeq ($(CONFIG_WIFI_CONTROL_FUNC),y)
DHDCFLAGS += -DCONFIG_WIFI_CONTROL_FUNC -DCUSTOMER_HW2
endif

#LGE_CHANGE_S, real-wifi@lge.com by kyunghoe.kim, 20120626, for ccx
# For CCX
ifeq ($(CONFIG_BRCM_CCX),y)
DHDCFLAGS += -DBCMCCX
endif
#LGE_CHANGE_E, real-wifi@lge.com by kyunghoe.kim, 20120626, for ccx

#DHDCFLAGS += -DSUPPORT_PM2_ONLY

ifeq ($(CONFIG_BCMDHD_HW_OOB),y)
DHDCFLAGS += -DHW_OOB -DOOB_INTR_ONLY
#LGE_CHANGE_S, moon-wifi@lge.com by wo0ngs 2012-03-18
ifneq ($(CONFIG_ARCH_OMAP),y)
DHDCFLAGS += -DCUSTOM_OOB_GPIO_NUM=$(CONFIG_BCMDHD_GPIO_WL_HOSTWAKEUP)
endif
#LGE_CHANGE_E, moon-wifi@lge.com by wo0ngs 2012-03-18
else
DHDCFLAGS += -DSDIO_ISR_THREAD
endif

# For Static Buffer
ifeq ($(CONFIG_BCMDHD_USE_STATIC_BUF),y)
DHDCFLAGS += -DCONFIG_DHD_USE_STATIC_BUF
endif

#LGE_CHANGE_S, moon-wifi@lge.com by wo0ngs 2012-05-01, CONFIG_VOIP_KR : Disable
# For DTIM Disable/Enable
ifeq ($(TARGET_COUNTRY), KR)
DHDCFLAGS += -DCONFIG_VOIP_KR  
endif
#LGE_CHANGE_E, moon-wifi@lge.com by wo0ngs 2012-05-01

DHDOFILES = \
		bcmsdh.o \
		bcmsdh_linux.o \
		bcmsdh_sdmmc.o \
		bcmsdh_sdmmc_linux.o \
		dhd_bta.o \
		dhd_cdc.o \
		dhd_common.o \
		dhd_custom_gpio.o \
		dhd_linux.o \
		dhd_linux_sched.o \
		dhd_sdio.o \
		aiutils.o \
		bcmutils.o   \
		bcmevent.o \
		hndpmu.o  \
		linux_osl.o	\
		sbutils.o \
		siutils.o \
		wl_android.o	\
		wldev_common.o \
		wl_linux_mon.o \
		bcmwifi_channels.o 

obj-$(CONFIG_BCMDHD_USE_STATIC_BUF) += brcm_static_buf.o

obj-$(CONFIG_BCMDHD) += bcmdhd.o
bcmdhd-objs += $(DHDOFILES)
ifneq ($(CONFIG_WIRELESS_EXT),)
bcmdhd-objs += wl_iw.o
DHDCFLAGS += -DSOFTAP
endif
ifneq ($(CONFIG_CFG80211),)
bcmdhd-objs += wl_cfg80211.o \
		wl_cfgp2p.o \
		dhd_cfg80211.o
DHDCFLAGS += -DWL_CFG80211
endif
EXTRA_CFLAGS = $(DHDCFLAGS)
ifeq ($(CONFIG_BCMDHD),m)
EXTRA_LDFLAGS += --strip-debug
endif
