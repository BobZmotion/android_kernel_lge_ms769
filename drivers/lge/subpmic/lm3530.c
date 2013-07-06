#include <linux/lge/lm3530.h>
#include <linux/power_supply.h>

#if defined(CONFIG_MAX8971_CHARGER)&&  defined(CONFIG_MACH_LGE_P2_DCM)

static int bl_on_off=1;

int lm3530_get_lcd_on_off()
{
	        return bl_on_off;
}
#endif

#ifdef CONFIG_MACH_LGE_CX2
	int su870_56k_detect = 0;
#endif

static int	lm3530_read_byte(struct lm3530_private_data* pdata, int reg)
{
	int		ret;

	mutex_lock(&pdata->update_lock);
	ret	=	i2c_smbus_read_byte_data(pdata->client, reg);
	mutex_unlock(&pdata->update_lock);

	return	ret;
}

static int	lm3530_write_byte(struct lm3530_private_data* pdata, int reg, int value)
{
	int		ret;

	mutex_lock(&pdata->update_lock);
	ret	=	i2c_smbus_write_byte_data(pdata->client, reg, value);
	mutex_unlock(&pdata->update_lock);

	return	ret;
}
void	lm3530_brr_write(struct lm3530_private_data* pdata)
{

	lm3530_write_byte(pdata, LM3530_REG_BRR, pdata->reg_brr);

}
static void	lm3530_store(struct lm3530_private_data* pdata)
{
	lm3530_write_byte(pdata, LM3530_REG_GP, pdata->reg_gp);
	lm3530_write_byte(pdata, LM3530_REG_BRR, pdata->reg_brr);
#ifdef CONFIG_MACH_LGE_CX2 //nthyunjin.yang 120619 reduce backlight at muic 56k detect
    if(su870_56k_detect)
		lm3530_write_byte(pdata, LM3530_REG_BRT, 40);
	else
	lm3530_write_byte(pdata, LM3530_REG_BRT, pdata->reg_brt);
#else
	lm3530_write_byte(pdata, LM3530_REG_BRT, pdata->reg_brt);
#endif
}

static void	lm3530_load(struct lm3530_private_data* pdata)
{
// LGE_CHANGE [bk.shin@lge.com] 2012-02-01, LGE_P940, add from P940 GB
#if defined(CONFIG_MACH_LGE_P2) || defined(CONFIG_MACH_LGE_U2) || defined(CONFIG_MACH_LGE_CX2)
	pdata->reg_gp   =	0x15;
#else
	pdata->reg_gp	=	lm3530_read_byte(pdata, LM3530_REG_GP);
#endif
	pdata->reg_brr   =       lm3530_read_byte(pdata, LM3530_REG_BRR);
	pdata->reg_brt	=	lm3530_read_byte(pdata, LM3530_REG_BRT);
}

int	lm3530_set_hwen(struct lm3530_private_data* pdata, int gpio, int status)
{
	if (status == 0) {
		lm3530_load(pdata);
		gpio_set_value(gpio, 0);
#if defined(CONFIG_MAX8971_CHARGER)&&  defined(CONFIG_MACH_LGE_P2_DCM)
		bl_on_off=0;
#endif
		return	0;
	}

	gpio_set_value(gpio, 1);
//20120709 mo2mk.kim@lge.com delete because this code is not available in GB =>
//	lm3530_write_byte(pdata, LM3530_REG_GP, 0x14);
//	lm3530_write_byte(pdata, LM3530_REG_BRT, 0x01);
//20120709 mo2mk.kim@lge.com delete because this code is not available in GB <=	
	lm3530_store(pdata);
#if defined(CONFIG_MAX8971_CHARGER)&&  defined(CONFIG_MACH_LGE_P2_DCM)
	bl_on_off=1;
#endif
	return	1;
}

int	lm3530_get_hwen(struct lm3530_private_data* pdata, int gpio)
{
	return	gpio_get_value(gpio);
}

#if defined(CONFIG_LG_FW_MAX17043_FUEL_GAUGE_I2C) || defined(CONFIG_LG_FW_MAX17048_FUEL_GAUGE_I2C)
extern int get_bat_present(void);
extern enum power_supply_type get_charging_ic_status(void);
#endif

int	lm3530_set_brightness_control(struct lm3530_private_data* pdata, int val)
{
	if ((val < 0) || (val > 255))
		return	-EINVAL;

	/* LGE_CHANGE [wonhui.lee@lge.com] 2011-11-23, To prevent Backlight control in Factory*/
#if defined(CONFIG_LG_FW_MAX17043_FUEL_GAUGE_I2C) || defined(CONFIG_LG_FW_MAX17048_FUEL_GAUGE_I2C)
	if (get_bat_present() == 0 &&
	    get_charging_ic_status() == POWER_SUPPLY_TYPE_FACTORY)
		return -1;
#endif

#ifdef CONFIG_MACH_LGE_CX2 //nthyunjin.yang 120619 factory mode lcd brightness issue
		if(su870_56k_detect)
			return lm3530_write_byte(pdata, LM3530_REG_BRT, 40);
		else
#endif
	return	lm3530_write_byte(pdata, LM3530_REG_BRT, val);
}

int	lm3530_get_brightness_control(struct lm3530_private_data* pdata)
{
	int		val;

	val	=	lm3530_read_byte(pdata, LM3530_REG_BRT);
	if (val < 0)
		return	val;

	return	(val & LM3530_BMASK);
}

int	lm3530_init(struct lm3530_private_data* pdata, struct i2c_client* client)
{
	mutex_init(&pdata->update_lock);
	pdata->client	=	client;

//20120709 mo2mk.kim@leg.com bring initialize values from CX2 GB =>
    // 22.5mA ->  real current 19.560
    lm3530_write_byte(pdata, LM3530_REG_GP,  0x15);

    #ifdef CONFIG_MACH_LGE_CX2 //ntdeaewan.choi@lge.com reduce backlight at muic 56k detect
    if(su870_56k_detect)
    {
        lm3530_write_byte(pdata, LM3530_REG_BRT, 40);
    }
    else
    {
        // Booting current adjust 0x7D -> 107 (7.3mA) 40%
        lm3530_write_byte(pdata, LM3530_REG_BRT, 104);
    }
    #endif
//20120709 mo2mk.kim@leg.com bring initialize values from CX2 GB <=
	
	lm3530_load(pdata);
// LGE_CHANGE [bk.shin@lge.com] 2012-02-01, LGE_P940, add from P940 GB
	lm3530_store(pdata);

	return 0;
}

#if defined(CONFIG_PANEL_LH430WV5_SD01)	//##hwcho_20120514
void lm3530_set_GP_control(struct lm3530_private_data* pdata, int val)
{
    int re = 0;
    re = lm3530_write_byte(pdata, LM3530_REG_GP, val);
}
EXPORT_SYMBOL(lm3530_set_GP_control);
#endif //##

EXPORT_SYMBOL(lm3530_brr_write);
EXPORT_SYMBOL(lm3530_init);
EXPORT_SYMBOL(lm3530_set_hwen);
EXPORT_SYMBOL(lm3530_get_hwen);
EXPORT_SYMBOL(lm3530_set_brightness_control);
EXPORT_SYMBOL(lm3530_get_brightness_control);

MODULE_AUTHOR("LG Electronics (dongjin73.kim@lge.com)");
MODULE_DESCRIPTION("Multi Display LED driver");
MODULE_LICENSE("GPL");
