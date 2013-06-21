/*
 * Copyright (C) 2011 Samsung Electronics
 *
 * Author: Adam Hampson <ahampson@sta.samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */

#ifndef _SII9244_H_
#define _SII9244_H_

#include <linux/i2c.h>

struct sii9244_platform_data {
	int prio;
	unsigned int int_gpio;
	unsigned int sel_gpio;
	unsigned int enable_gpio;
	unsigned int reset_gpio;
	unsigned int wakeup_gpio;
	void (*hpd_mux_pull_up)(bool on);
	void (*enable)(bool enable);
	void (*power)(int on);
	void (*enable_vbus)(bool enable);
	void (*vbus_present)(bool on);
	void (*gpio_config)(void);
	struct i2c_client *mhl_tx_client;
	struct i2c_client *tpi_client;
	struct i2c_client *hdmi_rx_client;
	struct i2c_client *cbus_client;
};

#endif /* _SII9244_H_ */
