obj-$(CONFIG_GOOGLE_BMS)	+= google_bms.o gbms_storage.o gvotable.o
obj-$(CONFIG_GOOGLE_BMS)	+= google_dc_pps.o
obj-$(CONFIG_GOOGLE_CPM)	+= google_cpm.o
obj-$(CONFIG_GOOGLE_CHARGER)	+= google_charger.o
obj-$(CONFIG_GOOGLE_BATTERY)	+= google_battery.o google_ttf.o
obj-$(CONFIG_GOOGLE_BEE)	+= google_eeprom.o
obj-$(CONFIG_USB_OVERHEAT_MITIGATION)	+= overheat_mitigation.o
obj-$(CONFIG_ARCH_SM8150)    	+= sm8150_bms.o
obj-$(CONFIG_GOOGLE_LOGBUFFER)	+= logbuffer.o
obj-$(CONFIG_CHARGER_P9221)	+= p9221_charger.o
obj-$(CONFIG_CHARGER_MAX77729)	+= max77729_charger.o
obj-$(CONFIG_CHARGER_MAX77759)	+= max77759_charger.o
obj-$(CONFIG_PMIC_MAX77729)	+= max77729_pmic.o
obj-$(CONFIG_MAXQ_MAX77759)	+= max77759_maxq.o
obj-$(CONFIG_UIC_MAX77729)	+= max77729_uic.o
obj-$(CONFIG_BATTERY_MAX1720X)	+= max_m5.o
obj-$(CONFIG_GOOGLE_PMIC_VOTER_COMPAT)	+= pmic-voter-compat.o
obj-$(CONFIG_PCA9468)	+= pca9468_charger.o
obj-$(CONFIG_MAX20339)		+= max20339.o
