/*
 * hx8389_panel  DSI Video/Command Mode Panel Driver
 *
 * modified from panel-hx8389.c
 *jeonghoon.cho@lge.com
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

//#define DEBUG

#if defined(CONFIG_LUT_FILE_TUNING)
#include <linux/syscalls.h>
#include <linux/fcntl.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#endif
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linux/backlight.h>
#include <linux/fb.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/mutex.h>

#include <video/omapdss.h>
#include <video/lge-dsi-panel.h>

#ifdef CONFIG_FB_OMAP_BOOTLOADER_INIT
#include "../dss/dss.h"
#endif

/* DSI Virtual channel. Hardcoded for now. */
#define TCH 0

#define DCS_READ_NUM_ERRORS	0x05
#define DCS_READ_POWER_MODE	0x0a
#define DCS_READ_MADCTL		0x0b
#define DCS_READ_PIXEL_FORMAT	0x0c
#define DCS_RDDSDR		0x0f
#define DCS_SLEEP_IN		0x10
#define DCS_SLEEP_OUT		0x11
#define DCS_DISPLAY_OFF		0x28
#define DCS_DISPLAY_ON		0x29
#define DCS_COLUMN_ADDR		0x2a
#define DCS_PAGE_ADDR		0x2b
#define DCS_MEMORY_WRITE	0x2c
#define DCS_TEAR_OFF		0x34
#define DCS_TEAR_ON		0x35
#define DCS_MEM_ACC_CTRL	0x36
#define DCS_PIXEL_FORMAT	0x3a
#define DCS_BRIGHTNESS		0x51
#define DCS_CTRL_DISPLAY	0x53
#define DCS_WRITE_CABC		0x55
#define DCS_READ_CABC		0x56
#define DCS_DEEP_STANDBY_IN		0xC1
#define DCS_GET_ID			0xf8

#define DSI_DT_DCS_SHORT_WRITE_0	0x05
#define DSI_DT_DCS_SHORT_WRITE_1	0x15
#define DSI_DT_DCS_READ			0x06
#define DSI_DT_SET_MAX_RET_PKG_SIZE	0x37
#define DSI_DT_NULL_PACKET		0x09
#define DSI_DT_DCS_LONG_WRITE		0x39
extern int ssc_enable;
static irqreturn_t hx8389_panel_te_isr(int irq, void *data);
static void hx8389_panel_te_timeout_work_callback(struct work_struct *work);
static int _hx8389_panel_enable_te(struct omap_dss_device *dssdev, bool enable);
//                                                                                                                   
extern int dispc_enable_gamma(enum omap_channel ch, u8 gamma);
//                                                                                                                   
#define DSI_GEN_SHORTWRITE_NOPARAM 0x3
#define DSI_GEN_SHORTWRITE_1PARAM 0x13
#define DSI_GEN_SHORTWRITE_2PARAM 0x23
#define DSI_GEN_LONGWRITE 	  0x29
#define DSI_DCS_LONGWRITE    0x39

#define LONG_CMD_MIPI	0
#define SHORT_CMD_MIPI	1
#define END_OF_COMMAND	2


/**
 * struct panel_config - panel configuration
 * @name: panel name
 * @type: panel type
 * @timings: panel resolution
 * @sleep: various panel specific delays, passed to msleep() if non-zero
 * @reset_sequence: reset sequence timings, passed to udelay() if non-zero
 * @regulators: array of panel regulators
 * @num_regulators: number of regulators in the array
 */
struct panel_config {
	const char *name;
	int type;

	struct omap_video_timings timings;

	struct {
		unsigned int sleep_in;
		unsigned int sleep_out;
		unsigned int hw_reset;
		unsigned int enable_te;
	} sleep;

	struct {
		unsigned int high;
		unsigned int low;
	} reset_sequence;

	struct panel_regulator *regulators;
	int num_regulators;
};

enum {
	PANEL_HX8389,
};

static struct panel_config panel_configs[] = {
	{
		.name		= "hx8389_panel",
		.type		= PANEL_HX8389,
		.timings	= {
			.x_res		= 540,
			.y_res		= 960,
			.vfp = 12,
			.vsw = 4,
			.vbp = 8,
			.hfp = 20, //20
			.hsw =10,
			.hbp = 47, // 47
		},
		.sleep		= {
			.sleep_in	= 20,
			.sleep_out	= 5,
			.hw_reset	= 10,
			.enable_te	= 5,
		},
		.reset_sequence	= {
			.high		= 10000,
			.low		= 10000,
		},
	},
};

struct hx8389_panel_data {
	struct mutex lock;

	struct backlight_device *bldev;

	unsigned long	hw_guard_end;	/* next value of jiffies when we can
					 * issue the next sleep in/out command
					 */
	unsigned long	hw_guard_wait;	/* max guard time in jiffies */

	struct omap_dss_device *dssdev;

	bool enabled;
	u8 rotate;
	bool mirror;

	bool te_enabled;

	atomic_t do_update;
	struct {
		u16 x;
		u16 y;
		u16 w;
		u16 h;
	} update_region;
	int channel;

	struct delayed_work te_timeout_work;

	bool use_dsi_bl;

	bool cabc_broken;
	unsigned cabc_mode;

	bool intro_printed;

	struct workqueue_struct *workqueue;

	struct delayed_work esd_work;
	unsigned esd_interval;

	bool ulps_enabled;
	unsigned ulps_timeout;
	struct delayed_work ulps_work;

	struct panel_config *panel_config;

	bool display_on;
	struct work_struct display_on_work;
};

int row_of_init_code = 0;
#ifdef CONFIG_DSI_CMD_MODE
u8 hitachi_lcd_command_for_mipi[][30] = {
	{END_OF_COMMAND,},
};
#else //CONFIG_DSI_VIDEO_MODE
u8 u760_hitachi_lcd_command_for_mipi[20][0x84] = {
	{LONG_CMD_MIPI, DSI_DCS_LONGWRITE, 0x04,
		0xB9, 0xFF, 0x83, 0x89,},
	{LONG_CMD_MIPI, DSI_DCS_LONGWRITE, 0x13,
		0xBA, 0x01, 0x92, 0x00, 0x16, 0xC4, 0x00, 0x18, 0xFF, 0x02,
		0x21, 0x03, 0x21, 0x23, 0x25, 0x20, 0x00, 0x35, 0x40,},
	{LONG_CMD_MIPI, DSI_DCS_LONGWRITE, 0x03,
		0xDE, 0x05, 0x58,},
	{LONG_CMD_MIPI, DSI_DCS_LONGWRITE, 0x14,
		0xB1, 0x00, 0x00, 0x07, 0xE3, 0x91, 0x10, 0x11, 0x6F, 0x0C,
		0x1D, 0x25, 0x1E, 0x1E, 0x41, 0x01, 0x58, 0xF7, 0x00, 0xC0,},
	{LONG_CMD_MIPI, DSI_DCS_LONGWRITE, 0x18,
		0xB4, 0x80, 0x14, 0x00, 0x32, 0x10, 0x07, 0x32, 0x10, 0x00,
		0x00, 0x00, 0x00, 0x17, 0x0A, 0x40, 0x0B, 0x13, 0x00, 0x4B,
		0x14, 0x53, 0x53, 0x0A,},
	{LONG_CMD_MIPI, DSI_DCS_LONGWRITE, 0x08,
		0xB2, 0x00, 0x00, 0x78, 0x09, 0x0A, 0x00, 0x60,},
	{LONG_CMD_MIPI, DSI_DCS_LONGWRITE, 0x31,
		0xD5, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x20,
		0x00, 0x99, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,
		0x23, 0x88, 0x01, 0x01, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,
		0x88, 0x99, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x10,
		0x88, 0x32, 0x10, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88,},
	{LONG_CMD_MIPI, DSI_DCS_LONGWRITE, 0x23,
		0xE0, 0x05, 0x11, 0x14, 0x37, 0x3F, 0x3F, 0x20, 0x4F, 0x08,
		0x0E, 0x0D, 0x12, 0x14, 0x12, 0x14, 0x1D, 0x1C, 0x05, 0x11,
		0x14, 0x37, 0x3F, 0x3F, 0x20, 0x4F, 0x08, 0x0E, 0x0D, 0x12,
		0x14, 0x12, 0x14, 0x1D, 0x1C,},
	{LONG_CMD_MIPI, DSI_DCS_LONGWRITE, 0x41,
		0xC1, 0x01, 0x00, 0x04, 0x07, 0x0D, 0x1A, 0x1E, 0x25, 0x2C,
		0x35, 0x3D, 0x46, 0x4F, 0x58, 0x60, 0x6A, 0x74, 0x7D, 0x85,
		0x8E, 0x98, 0xA0, 0xAA, 0xB2, 0xBF, 0xC8, 0xCB, 0xD4, 0xE0,
		0xE5, 0xF1, 0xF5, 0xFB, 0xFF, 0x80, 0x2F, 0x37, 0x2F, 0x35,
		0x91, 0x24, 0x6A, 0xC0, 0x00, 0x04, 0x07, 0x0D, 0x1A, 0x1E,
		0x25, 0x2C, 0x35, 0x3D, 0x46, 0x4F, 0x58, 0x60, 0x6A, 0x74,
		0x7D, 0x85, 0x8E, 0x98, 0xA0,},
	{LONG_CMD_MIPI, DSI_GEN_LONGWRITE, 0x40,
		0xc1, 0xAA, 0xB2, 0xBF, 0xC8, 0xCB, 0xD4, 0xE0, 0xE5, 0xF1,
		0xF5, 0xFB, 0xFF, 0x80, 0x2F, 0x37, 0x2F, 0x35, 0x91, 0x24,
		0x6A, 0xC0, 0x00, 0x04, 0x07, 0x0D, 0x19, 0x1D, 0x25, 0x2C,
		0x35, 0x3D, 0x46, 0x4F, 0x58, 0x60, 0x6A, 0x74, 0x7D, 0x85,
		0x8E, 0x98, 0xA0, 0xAA, 0xB2, 0xBF, 0xC8, 0xCB, 0xD4, 0xE0,
		0xE5, 0xF1, 0xF5, 0xFB, 0xFF, 0x80, 0x2F, 0x37, 0x2F, 0x35,
		0x91, 0x24, 0x6A, 0xC0,},
	{LONG_CMD_MIPI, DSI_DCS_LONGWRITE, 0x41,
		0xC1, 0x01, 0x00, 0x04, 0x07, 0x0D, 0x1A, 0x1E, 0x25, 0x2C,
		0x35, 0x3D, 0x46, 0x4F, 0x58, 0x60, 0x6A, 0x74, 0x7D, 0x85,
		0x8E, 0x98, 0xA0, 0xAA, 0xB2, 0xBF, 0xC8, 0xCB, 0xD4, 0xE0,
		0xE5, 0xF1, 0xF5, 0xFB, 0xFF, 0x80, 0x2F, 0x37, 0x2F, 0x35,
		0x91, 0x24, 0x6A, 0xC0, 0x00, 0x04, 0x07, 0x0D, 0x1A, 0x1E,
		0x25, 0x2C, 0x35, 0x3D, 0x46, 0x4F, 0x58, 0x60, 0x6A, 0x74,
		0x7D, 0x85, 0x8E, 0x98, 0xA0,},
	{LONG_CMD_MIPI, DSI_DCS_LONGWRITE, 0x05,
		0xB6, 0x00, 0x86, 0x00, 0x86,},
	{SHORT_CMD_MIPI, DSI_GEN_SHORTWRITE_2PARAM, 0x02,
		0xCC, 0x0E,},
	{LONG_CMD_MIPI, DSI_DCS_LONGWRITE, 0x03,
		0xCB, 0x07, 0x07,},
	{END_OF_COMMAND,},
};
#endif

u8 u760_hitachi_lcd_command_for_mipi_set_power[][100] = {
	{LONG_CMD_MIPI, DSI_DCS_LONGWRITE, 0x14,
		0xB1, 0x00, 0x00, 0x07, 0xE3, 0x91, 0x10, 0x11, 0x6F, 0x0C,
		0x1D, 0x25, 0x1E, 0x1E, 0x41, 0x01, 0x58, 0xF7, 0x00, 0x00,},

};


static inline struct lge_dsi_panel_data
*get_panel_data(const struct omap_dss_device *dssdev)
{
	return (struct lge_dsi_panel_data *) dssdev->data;
}

static void hx8389_panel_esd_work(struct work_struct *work);
static void hx8389_panel_ulps_work(struct work_struct *work);
static void hx8389_panel_display_on_work(struct work_struct *work);

static void hw_guard_start(struct hx8389_panel_data *td, int guard_msec)
{
	td->hw_guard_wait = msecs_to_jiffies(guard_msec);
	td->hw_guard_end = jiffies + td->hw_guard_wait;
}

static void hw_guard_wait(struct hx8389_panel_data *td)
{
	unsigned long wait = td->hw_guard_end - jiffies;

	if ((long)wait > 0 && wait <= td->hw_guard_wait) {
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(wait);
	}
}

static int hx8389_panel_dcs_read_1(struct hx8389_panel_data *td, u8 dcs_cmd, u8 *data)
{
	int r;
	u8 buf[1];

	r = dsi_vc_dcs_read(td->dssdev, td->channel, dcs_cmd, buf, 1);

	if (r < 0)
		return r;

	*data = buf[0];

	return 0;
}

static int hx8389_panel_dcs_write_0(struct hx8389_panel_data *td, u8 dcs_cmd)
{
	return dsi_vc_dcs_write(td->dssdev, td->channel, &dcs_cmd, 1);
}

static int hx8389_panel_dcs_write_1(struct hx8389_panel_data *td, u8 dcs_cmd, u8 param)
{
	u8 buf[2];
	buf[0] = dcs_cmd;
	buf[1] = param;
	return dsi_vc_dcs_write(td->dssdev, td->channel, buf, 2);
}

static int hx8389_panel_sleep_in(struct hx8389_panel_data *td)
{
	int r;


	r = hx8389_panel_dcs_write_0(td, DCS_SLEEP_IN);
	if (r)
		return r;
#if 0
	r = hx8389_panel_dcs_write_1(td, DCS_DEEP_STANDBY_IN, 0x1);
	if (r)
		return r;
#endif
	hw_guard_start(td, 120);
	hw_guard_wait(td);

	if (td->panel_config->sleep.sleep_in)
		msleep(td->panel_config->sleep.sleep_in);

	return 0;
}

static int hx8389_panel_sleep_out(struct hx8389_panel_data *td)
{
	int r;

	r = hx8389_panel_dcs_write_0(td, DCS_SLEEP_OUT);
	if (r)
		return r;

	hw_guard_start(td, 150);

	//                                          
	hw_guard_wait(td);

	return 0;
}

static int hx8389_panel_set_addr_mode(struct hx8389_panel_data *td, u8 rotate, bool mirror)
{
	int r;
	u8 mode=0;
	int b5, b6, b7;

	r = hx8389_panel_dcs_read_1(td, DCS_READ_MADCTL, &mode);
	if (r)
		return r;

	switch (rotate) {
	default:
	case 0:
		b7 = 0;
		b6 = 0;
		b5 = 0;
		break;
	case 1:
		b7 = 0;
		b6 = 1;
		b5 = 1;
		break;
	case 2:
		b7 = 1;
		b6 = 1;
		b5 = 0;
		break;
	case 3:
		b7 = 1;
		b6 = 0;
		b5 = 1;
		break;
	}

	if (mirror)
		b6 = !b6;

	mode &= ~((1<<7) | (1<<6) | (1<<5));
	mode |= (b7 << 7) | (b6 << 6) | (b5 << 5);

	return hx8389_panel_dcs_write_1(td, DCS_MEM_ACC_CTRL, mode);
}
static int hx8389_panel_set_update_window(struct hx8389_panel_data *td,
		u16 x, u16 y, u16 w, u16 h)
{
	int r;
	u16 x1 = x;
	u16 x2 = x + w - 1;
	u16 y1 = y;
	u16 y2 = y + h - 1;

	u8 buf[5];
	buf[0] = DCS_COLUMN_ADDR;
	buf[1] = (x1 >> 8) & 0xff;
	buf[2] = (x1 >> 0) & 0xff;
	buf[3] = (x2 >> 8) & 0xff;
	buf[4] = (x2 >> 0) & 0xff;

	r = dsi_vc_dcs_write_nosync(td->dssdev, td->channel, buf, sizeof(buf));
	if (r)
		return r;

	buf[0] = DCS_PAGE_ADDR;
	buf[1] = (y1 >> 8) & 0xff;
	buf[2] = (y1 >> 0) & 0xff;
	buf[3] = (y2 >> 8) & 0xff;
	buf[4] = (y2 >> 0) & 0xff;

	r = dsi_vc_dcs_write_nosync(td->dssdev, td->channel, buf, sizeof(buf));
	if (r)
		return r;

#if !defined (CONFIG_OMAP_USE_CMOS_TE_TRIGGER)
	dsi_vc_send_bta_sync(td->dssdev, td->channel);
#endif

	return r;
}

static void hx8389_panel_queue_esd_work(struct omap_dss_device *dssdev)
{
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);

	if (td->esd_interval > 0)
		queue_delayed_work(td->workqueue, &td->esd_work,
				msecs_to_jiffies(td->esd_interval));
}

static void hx8389_panel_cancel_esd_work(struct omap_dss_device *dssdev)
{
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);

	if (td->esd_interval > 0)
		cancel_delayed_work(&td->esd_work);
}

static void hx8389_panel_queue_ulps_work(struct omap_dss_device *dssdev)
{
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);

	if (td->ulps_timeout > 0)
		queue_delayed_work(td->workqueue, &td->ulps_work,
				msecs_to_jiffies(td->ulps_timeout));
}

static void hx8389_panel_cancel_ulps_work(struct omap_dss_device *dssdev)
{
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);

	if (td->ulps_timeout > 0)
		cancel_delayed_work(&td->ulps_work);
}

static int hx8389_panel_enter_ulps(struct omap_dss_device *dssdev)
{
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);
	struct lge_dsi_panel_data *panel_data = get_panel_data(dssdev);
	int r;

	if (td->ulps_enabled)
		return 0;

	hx8389_panel_cancel_ulps_work(dssdev);

	r = _hx8389_panel_enable_te(dssdev, false);
	if (r)
		goto err;

	disable_irq(gpio_to_irq(panel_data->ext_te_gpio));

	omapdss_dsi_display_disable(dssdev, false, true);

	td->ulps_enabled = true;

	return 0;
err:
	dev_err(&dssdev->dev, "enter ULPS failed");

	td->ulps_enabled = false;

	hx8389_panel_queue_ulps_work(dssdev);

	return r;
}

static int hx8389_panel_exit_ulps(struct omap_dss_device *dssdev)
{
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);
	struct lge_dsi_panel_data *panel_data = get_panel_data(dssdev);
	int r;

	if (!td->ulps_enabled)
		return 0;

	r = omapdss_dsi_display_enable(dssdev);
	if (r) {
		dev_err(&dssdev->dev, "failed to enable DSI\n");
		goto err1;
	}

	omapdss_dsi_vc_enable_hs(dssdev, td->channel, true);

	r = _hx8389_panel_enable_te(dssdev, true);
	if (r) {
		dev_err(&dssdev->dev, "failed to re-enable TE");
		goto err1;
	}

	enable_irq(gpio_to_irq(panel_data->ext_te_gpio));

	hx8389_panel_queue_ulps_work(dssdev);

	td->ulps_enabled = false;

	return 0;

err1:
	hx8389_panel_queue_ulps_work(dssdev);

	return r;
}

static int hx8389_panel_wake_up(struct omap_dss_device *dssdev)
{
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);

	if (td->ulps_enabled)
		return hx8389_panel_exit_ulps(dssdev);

	hx8389_panel_cancel_ulps_work(dssdev);
	hx8389_panel_queue_ulps_work(dssdev);
	return 0;
}

static int hx8389_panel_bl_update_status(struct backlight_device *dev)
{
	struct omap_dss_device *dssdev = dev_get_drvdata(&dev->dev);
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);
	int r=0;
	int level;

	if (dev->props.fb_blank == FB_BLANK_UNBLANK &&
			dev->props.power == FB_BLANK_UNBLANK)
		level = dev->props.brightness;
	else
		level = 0;

	dev_dbg(&dssdev->dev, "update brightness to %d\n", level);

	mutex_lock(&td->lock);

	if (td->use_dsi_bl) {
		if (td->enabled) {
			dsi_bus_lock(dssdev);

			r = hx8389_panel_wake_up(dssdev);
			if (!r)
				r = hx8389_panel_dcs_write_1(td, DCS_BRIGHTNESS, level);

			dsi_bus_unlock(dssdev);
		} else {
			r = 0;
		}
	}

	mutex_unlock(&td->lock);

	return r;
}

static int hx8389_panel_bl_get_intensity(struct backlight_device *dev)
{
	if (dev->props.fb_blank == FB_BLANK_UNBLANK &&
			dev->props.power == FB_BLANK_UNBLANK)
		return dev->props.brightness;

	return 0;
}

static const struct backlight_ops hx8389_panel_bl_ops = {
	.get_brightness = hx8389_panel_bl_get_intensity,
	.update_status  = hx8389_panel_bl_update_status,
};

static void hx8389_panel_get_timings(struct omap_dss_device *dssdev,
			struct omap_video_timings *timings)
{
	*timings = dssdev->panel.timings;
}

static void hx8389_panel_get_resolution(struct omap_dss_device *dssdev,
		u16 *xres, u16 *yres)
{
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);

	if (td->rotate == 0 || td->rotate == 2) {
		*xres = dssdev->panel.timings.x_res;
		*yres = dssdev->panel.timings.y_res;
	} else {
		*yres = dssdev->panel.timings.x_res;
		*xres = dssdev->panel.timings.y_res;
	}
}

static const char *cabc_modes[] = {
	"off",		/* used also always when CABC is not supported */
	"ui",
	"still-image",
	"moving-image",
};

static ssize_t show_cabc_mode(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	struct omap_dss_device *dssdev = to_dss_device(dev);
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);
	const char *mode_str;
	int mode;
	int len;

	mode = td->cabc_mode;

	mode_str = "unknown";
	if (mode >= 0 && mode < ARRAY_SIZE(cabc_modes))
		mode_str = cabc_modes[mode];
	len = snprintf(buf, PAGE_SIZE, "%s\n", mode_str);

	return len < PAGE_SIZE - 1 ? len : PAGE_SIZE - 1;
}

static ssize_t store_cabc_mode(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct omap_dss_device *dssdev = to_dss_device(dev);
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);
	int i;
	int r;

	for (i = 0; i < ARRAY_SIZE(cabc_modes); i++) {
		if (sysfs_streq(cabc_modes[i], buf))
			break;
	}

	if (i == ARRAY_SIZE(cabc_modes))
		return -EINVAL;

	mutex_lock(&td->lock);

	if (td->enabled) {
		dsi_bus_lock(dssdev);

		if (!td->cabc_broken) {
			r = hx8389_panel_wake_up(dssdev);
			if (r)
				goto err;

			r = hx8389_panel_dcs_write_1(td, DCS_WRITE_CABC, i);
			if (r)
				goto err;
		}

		dsi_bus_unlock(dssdev);
	}

	td->cabc_mode = i;

	mutex_unlock(&td->lock);

	return count;
err:
	dsi_bus_unlock(dssdev);
	mutex_unlock(&td->lock);
	return r;
}
#if defined(CONFIG_LUT_FILE_TUNING)
	extern long tuning_table[256];
static ssize_t display_file_tuning_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t size)
{
	    int fd;
           char tmp_buf[256][6]={0x00};
           char tmp_buf2[13]={0x00};
	    char tmp_buf22[6]={0x00};
	    //char tuning_table[256];
           int input,i,j;
           //u32      temp[256];
           sscanf(buf, "%d",&input);
           set_fs(KERNEL_DS);
           fd = sys_open((const char __user *) "/mnt/sdcard/file_tuning.txt", O_RDONLY, 0);
           if(fd >= 0)
           {
                                memset(tmp_buf, 0x00, sizeof(tmp_buf));
                                for(i=0;i<256;i++)
                                {
                                          sys_read(fd, (const char __user *) tmp_buf2, 13);
					for(j=0;j<6;j++)
						{
							tmp_buf22[j] = tmp_buf2[j+4];
						}
						tuning_table[i] = simple_strtol(tmp_buf22, NULL, 16);
                                }
					dispc_enable_gamma(OMAP_DSS_CHANNEL_LCD2, 1);
					sys_close(fd);
           }
	return size;
}
static DEVICE_ATTR(file_tuning, 0660, NULL, display_file_tuning_store);
#endif
//                                                                                                                   
extern int dispc_set_gamma_rgb(enum omap_channel ch, u8 gamma,int red,int green,int blue);
static ssize_t display_gamma_tuning_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return 0;
}
static ssize_t display_gamma_tuning_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t size)
{
	int red,green,blue;
	sscanf(buf, "%d,%d,%d",&red,&green,&blue);
	printk("SJ	:	RED	:	%d	GREEN :		%d	BLUE :		%d\n",red,green,blue);
	dispc_set_gamma_rgb(OMAP_DSS_CHANNEL_LCD, 0,red,green,blue);
	dispc_set_gamma_rgb(OMAP_DSS_CHANNEL_LCD2, 0,red,green,blue);
	return size;
}

static ssize_t display_init_code_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int i = row_of_init_code;
	int r;

	for(i=0; i < u760_hitachi_lcd_command_for_mipi[row_of_init_code][2] + 3; i++)
	{
		sprintf(buf, "%x,", u760_hitachi_lcd_command_for_mipi[row_of_init_code][i]);
		buf += 3;
	}
	sprintf(buf, "\n");

	return i*3;
}
static ssize_t display_init_code_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t size)
{
	int cmd, type, len, adr, val;
	int row,col,num;


	num = sscanf(buf, "%x,%x,%x,%x",&cmd,&type,&len,&adr);
	printk(KERN_ERR "buf_adr=%x, buf=%c%c%c \n",buf,buf[0],buf[1],buf[2]);
	printk("num=%x",num);

	if(cmd == END_OF_COMMAND)
	{
		row_of_init_code = type;
		return size;
	}



#if 0
	for(row=0; u760_hitachi_lcd_command_for_mipi[row][0] != END_OF_COMMAND; row++)
	{
		if(u760_hitachi_lcd_command_for_mipi[row][3] == adr) break;
	}

	if(u760_hitachi_lcd_command_for_mipi[row][0] == END_OF_COMMAND)
	{
		if(row == 19) {
			printk("Can't add row to init code array \n");
			return size;
		}
		u760_hitachi_lcd_command_for_mipi[row+1][0] = END_OF_COMMAND;
	}
#else
	row = row_of_init_code;
#endif
	if(row > 19) {
		printk("Can't add row to init code array \n");
		return size;
	}

	for(col=0; col < len + 3; col++)
	{
		while( !( (buf[0]>='0' && buf[0]<='9') || (buf[0]>='a' && buf[0]<='f') \
			|| (buf[0]>='A' && buf[0]<='F') ) ){
			buf++;
		}

		num = sscanf(buf,"%x",&val);

		printk("num=%x",num);
		printk("buf_adr=%x, buf=%c%c%c, val=%x\n",buf,buf[0],buf[1],buf[2],val);

		while( ( (buf[0]>='0' && buf[0]<='9') || (buf[0]>='a' && buf[0]<='f') \
			|| (buf[0]>='A' && buf[0]<='F') ) ){
			buf++;
		}


		u760_hitachi_lcd_command_for_mipi[row][col] = (u8)val;
	}


	return size;
}

static DEVICE_ATTR(init_code, 0660, display_init_code_show, display_init_code_store);

static DEVICE_ATTR(gamma_tuning, 0660, display_gamma_tuning_show, display_gamma_tuning_store);
//                                                                                                                   
static ssize_t display_porch_value_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
       struct omap_dss_device *dssdev = to_dss_device(dev);
       int vbp,vsw,vfp,hbp,hsw,hfp;
       sscanf(buf, "%d,%d,%d,%d,%d,%d",&vbp,&vsw,&vfp,&hbp,&hsw,&hfp);
       dssdev->panel.timings.vbp = vbp;
       dssdev->panel.timings.vsw = vsw;
       dssdev->panel.timings.vfp = vfp;
       dssdev->panel.timings.hbp = hbp;
       dssdev->panel.timings.hsw = hsw;
       dssdev->panel.timings.hfp = hfp;
       return;
}
static ssize_t display_porch_value_show(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
       struct omap_dss_device *dssdev = to_dss_device(dev);
	 int vbp,vsw,vfp,hbp,hsw,hfp;
       vbp = dssdev->panel.timings.vbp;
       vsw = dssdev->panel.timings.vsw;
       vfp = dssdev->panel.timings.vfp;
       hbp = dssdev->panel.timings.hbp;
       hsw = dssdev->panel.timings.hsw;
       hfp = dssdev->panel.timings.hfp;
	return snprintf(buf, PAGE_SIZE, "vbp=%d, vsw=%d, vfp=%d, hbp=%d, hsw=%d, hfp=%d\n", vbp,vsw,vfp,hbp,hsw,hfp);
}
static DEVICE_ATTR(porch_value, 0660, display_porch_value_show, display_porch_value_store);

static ssize_t display_clock_value_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
       struct omap_dss_device *dssdev = to_dss_device(dev);
       int regn,regm;//,regm_dispc,regm_dsi;
       sscanf(buf, "%d,%d",&regn,&regm);//,&regm_dispc,&regm_dsi);
       dssdev->clocks.dsi.regn = regn;
       dssdev->clocks.dsi.regm = regm;
       //dssdev->clocks.dsi.regm_dispc = regm_dispc;
       //dssdev->clocks.dsi.regm_dsi = regm_dsi;
       //dssdev->clocks.dsi.lp_clk_div = lp_clk_div;
       return;
}
static ssize_t display_clock_value_show(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
       struct omap_dss_device *dssdev = to_dss_device(dev);
	   int clock_value;
	   clock_value = 19200*(dssdev->clocks.dsi.regm)/(1+(dssdev->clocks.dsi.regn));
	return snprintf(buf, PAGE_SIZE, "regn=%d, regm=%d, clock_value(KHz)= %d\n", dssdev->clocks.dsi.regn,dssdev->clocks.dsi.regm,clock_value);
}
static DEVICE_ATTR(clock_value, 0660, display_clock_value_show, display_clock_value_store);
static ssize_t display_ssc_enable_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
       struct omap_dss_device *dssdev = to_dss_device(dev);
       sscanf(buf, "%d",&ssc_enable);
       printk("[dyotest]ssc_enable=%d\n",ssc_enable);
       return;
}
static ssize_t display_ssc_enable_show(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	return snprintf(buf, PAGE_SIZE, "ssc_enable=%d\n",ssc_enable);
}
static DEVICE_ATTR(ssc_enable, 0660, display_ssc_enable_show, display_ssc_enable_store);

static ssize_t show_cabc_available_modes(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	int len;
	int i;

	for (i = 0, len = 0;
	     len < PAGE_SIZE && i < ARRAY_SIZE(cabc_modes); i++)
		len += snprintf(&buf[len], PAGE_SIZE - len, "%s%s%s",
			i ? " " : "", cabc_modes[i],
			i == ARRAY_SIZE(cabc_modes) - 1 ? "\n" : "");

	return len < PAGE_SIZE ? len : PAGE_SIZE - 1;
}

static ssize_t hx8389_panel_store_esd_interval(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct omap_dss_device *dssdev = to_dss_device(dev);
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);

	unsigned long t;
	int r;

	r = strict_strtoul(buf, 10, &t);
	if (r)
		return r;

	mutex_lock(&td->lock);
	hx8389_panel_cancel_esd_work(dssdev);
	td->esd_interval = t;
	if (td->enabled)
		hx8389_panel_queue_esd_work(dssdev);
	mutex_unlock(&td->lock);

	return count;
}
//                                                                                                                   
extern int dispc_enable_gamma(enum omap_channel ch, u8 gamma);
//                                                                                                                   

static ssize_t hx8389_panel_show_esd_interval(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	struct omap_dss_device *dssdev = to_dss_device(dev);
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);
	unsigned t;

	mutex_lock(&td->lock);
	t = td->esd_interval;
	mutex_unlock(&td->lock);

	return snprintf(buf, PAGE_SIZE, "%u\n", t);
}

static ssize_t hx8389_panel_store_ulps(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct omap_dss_device *dssdev = to_dss_device(dev);
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);
	unsigned long t;
	int r;

	r = strict_strtoul(buf, 10, &t);
	if (r)
		return r;

	mutex_lock(&td->lock);

	if (td->enabled) {
		dsi_bus_lock(dssdev);

		if (t)
			r = hx8389_panel_enter_ulps(dssdev);
		else
			r = hx8389_panel_wake_up(dssdev);

		dsi_bus_unlock(dssdev);
	}

	mutex_unlock(&td->lock);

	if (r)
		return r;

	return count;
}

static ssize_t hx8389_panel_show_ulps(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	struct omap_dss_device *dssdev = to_dss_device(dev);
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);
	unsigned t;

	mutex_lock(&td->lock);
	t = td->ulps_enabled;
	mutex_unlock(&td->lock);

	return snprintf(buf, PAGE_SIZE, "%u\n", t);
}

static ssize_t hx8389_panel_store_ulps_timeout(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct omap_dss_device *dssdev = to_dss_device(dev);
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);
	unsigned long t;
	int r;

	r = strict_strtoul(buf, 10, &t);
	if (r)
		return r;

	mutex_lock(&td->lock);
	td->ulps_timeout = t;

	if (td->enabled) {
		/* hx8389_panel_wake_up will restart the timer */
		dsi_bus_lock(dssdev);
		r = hx8389_panel_wake_up(dssdev);
		dsi_bus_unlock(dssdev);
	}

	mutex_unlock(&td->lock);

	if (r)
		return r;

	return count;
}

static ssize_t hx8389_panel_show_ulps_timeout(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	struct omap_dss_device *dssdev = to_dss_device(dev);
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);
	unsigned t;

	mutex_lock(&td->lock);
	t = td->ulps_timeout;
	mutex_unlock(&td->lock);

	return snprintf(buf, PAGE_SIZE, "%u\n", t);
}

static DEVICE_ATTR(cabc_mode, S_IRUGO | S_IWUSR,
		show_cabc_mode, store_cabc_mode);
static DEVICE_ATTR(cabc_available_modes, S_IRUGO,
		show_cabc_available_modes, NULL);
static DEVICE_ATTR(esd_interval, S_IRUGO | S_IWUSR,
		hx8389_panel_show_esd_interval, hx8389_panel_store_esd_interval);
static DEVICE_ATTR(ulps, S_IRUGO | S_IWUSR,
		hx8389_panel_show_ulps, hx8389_panel_store_ulps);
static DEVICE_ATTR(ulps_timeout, S_IRUGO | S_IWUSR,
		hx8389_panel_show_ulps_timeout, hx8389_panel_store_ulps_timeout);

static struct attribute *hx8389_panel_attrs[] = {
	&dev_attr_cabc_mode.attr,
	&dev_attr_cabc_available_modes.attr,
	&dev_attr_esd_interval.attr,
	&dev_attr_ulps.attr,
	&dev_attr_ulps_timeout.attr,
//                                                                                                                   
    &dev_attr_gamma_tuning.attr,
#if defined(CONFIG_LUT_FILE_TUNING)
    &dev_attr_file_tuning.attr,
#endif
    &dev_attr_porch_value.attr,
    &dev_attr_clock_value.attr,
    &dev_attr_ssc_enable.attr,
    &dev_attr_init_code.attr,

//                                                                                                                   
	NULL,
};

static struct attribute_group hx8389_panel_attr_group = {
	.attrs = hx8389_panel_attrs,
};

static void hx8389_panel_hw_reset(struct omap_dss_device *dssdev)
{
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);
	struct lge_dsi_panel_data *panel_data = get_panel_data(dssdev);

	if (panel_data->reset_gpio == -1)
		return;

	gpio_request(panel_data->reset_gpio, "lcd_reset");
	gpio_direction_output(panel_data->reset_gpio, 1);

	gpio_set_value(panel_data->reset_gpio, 1);
	if (td->panel_config->reset_sequence.high)
		udelay(td->panel_config->reset_sequence.high);
	/* reset the panel */
	gpio_set_value(panel_data->reset_gpio, 0);
	/* assert reset */
	if (td->panel_config->reset_sequence.low)
		udelay(td->panel_config->reset_sequence.low);
	gpio_set_value(panel_data->reset_gpio, 1);
	/* wait after releasing reset */
	if (td->panel_config->sleep.hw_reset)
		msleep(td->panel_config->sleep.hw_reset);
}

static int hx8389_panel_probe(struct omap_dss_device *dssdev)
{
	struct backlight_properties props;
	struct hx8389_panel_data *td;
	struct backlight_device *bldev;
	struct lge_dsi_panel_data *panel_data = get_panel_data(dssdev);
	struct panel_config *panel_config = NULL;
	int r, i;

	dev_dbg(&dssdev->dev, "probe\n");

	if (!panel_data || !panel_data->name) {
		r = -EINVAL;
		goto err;
	}

	for (i = 0; i < ARRAY_SIZE(panel_configs); i++) {
		if (strcmp(panel_data->name, panel_configs[i].name) == 0) {
			panel_config = &panel_configs[i];
			break;
		}
	}

	if (!panel_config) {
		r = -EINVAL;
		goto err;
	}

	dssdev->panel.config = OMAP_DSS_LCD_TFT | OMAP_DSS_LCD_ONOFF | OMAP_DSS_LCD_RF ;
	dssdev->panel.timings = panel_config->timings;
	dssdev->ctrl.pixel_size = 24;

	/* Since some android application use physical dimension, that information should be set here */
	dssdev->panel.width_in_um = 57000; /* physical dimension in um */
	dssdev->panel.height_in_um = 94000; /* physical dimension in um */

	td = kzalloc(sizeof(*td), GFP_KERNEL);
	if (!td) {
		r = -ENOMEM;
		goto err;
	}
	td->dssdev = dssdev;
	td->panel_config = panel_config;
	td->esd_interval = panel_data->esd_interval;
	td->ulps_enabled = false;
	td->ulps_timeout = panel_data->ulps_timeout;

	mutex_init(&td->lock);

	atomic_set(&td->do_update, 0);

	td->workqueue = create_singlethread_workqueue("hx8389_panel_panel_esd");
	if (td->workqueue == NULL) {
		dev_err(&dssdev->dev, "can't create ESD workqueue\n");
		r = -ENOMEM;
		goto err_wq;
	}
	INIT_DELAYED_WORK_DEFERRABLE(&td->esd_work, hx8389_panel_esd_work);
	INIT_DELAYED_WORK(&td->ulps_work, hx8389_panel_ulps_work);
	INIT_WORK(&td->display_on_work, hx8389_panel_display_on_work);

	dev_set_drvdata(&dssdev->dev, td);

	/* if no platform set_backlight() defined, presume DSI backlight
	 * control */
	memset(&props, 0, sizeof(struct backlight_properties));

	/* P940 dose not use dsi blacklight control */
	td->use_dsi_bl = false;

	if (td->use_dsi_bl)
		props.max_brightness = 255;
	else
		props.max_brightness = 127;

	props.type = BACKLIGHT_RAW;
	bldev = backlight_device_register(dev_name(&dssdev->dev), &dssdev->dev,
					dssdev, &hx8389_panel_bl_ops, &props);
	if (IS_ERR(bldev)) {
		r = PTR_ERR(bldev);
		goto err_bl;
	}

	td->bldev = bldev;

	bldev->props.fb_blank = FB_BLANK_UNBLANK;
	bldev->props.power = FB_BLANK_UNBLANK;
	if (td->use_dsi_bl)
		bldev->props.brightness = 255;
	else
		bldev->props.brightness = 127;

	hx8389_panel_bl_update_status(bldev);

	if (panel_data->use_ext_te) {
		int gpio = panel_data->ext_te_gpio;

		r = gpio_request(gpio, "hx8389_panel irq");
		if (r) {
			dev_err(&dssdev->dev, "GPIO request failed\n");
			goto err_gpio;
		}

		gpio_direction_input(gpio);

		r = request_threaded_irq(gpio_to_irq(gpio), NULL, hx8389_panel_te_isr,
				IRQF_DISABLED | IRQF_TRIGGER_RISING,
				"hx8389_panel vsync", dssdev);

		if (r) {
			dev_err(&dssdev->dev, "IRQ request failed\n");
			gpio_free(gpio);
			goto err_irq;
		}

		INIT_DELAYED_WORK_DEFERRABLE(&td->te_timeout_work,
					hx8389_panel_te_timeout_work_callback);

		dev_dbg(&dssdev->dev, "Using GPIO TE\n");
	}

	r = omap_dsi_request_vc(dssdev, &td->channel);
	if (r) {
		dev_err(&dssdev->dev, "failed to get virtual channel\n");
		goto err_req_vc;
	}

	r = omap_dsi_set_vc_id(dssdev, td->channel, TCH);
	if (r) {
		dev_err(&dssdev->dev, "failed to set VC_ID\n");
		goto err_vc_id;
	}

	r = sysfs_create_group(&dssdev->dev.kobj, &hx8389_panel_attr_group);
	if (r) {
		dev_err(&dssdev->dev, "failed to create sysfs files\n");
		goto err_vc_id;
	}

#ifdef CONFIG_FB_OMAP_BOOTLOADER_INIT
	dss_runtime_get ();
#endif

	return 0;

err_vc_id:
	omap_dsi_release_vc(dssdev, td->channel);
err_req_vc:
	if (panel_data->use_ext_te)
		free_irq(gpio_to_irq(panel_data->ext_te_gpio), dssdev);
err_irq:
	if (panel_data->use_ext_te)
		gpio_free(panel_data->ext_te_gpio);
err_gpio:
err_bl:
	destroy_workqueue(td->workqueue);
err_wq:
err:
	kfree(td);
	return r;
}

static void __exit hx8389_panel_remove(struct omap_dss_device *dssdev)
{
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);
	struct lge_dsi_panel_data *panel_data = get_panel_data(dssdev);
	struct backlight_device *bldev;

	dev_dbg(&dssdev->dev, "remove\n");

	sysfs_remove_group(&dssdev->dev.kobj, &hx8389_panel_attr_group);
	omap_dsi_release_vc(dssdev, td->channel);

	if (panel_data->use_ext_te) {
		int gpio = panel_data->ext_te_gpio;
		free_irq(gpio_to_irq(gpio), dssdev);
		gpio_free(gpio);
	}

	bldev = td->bldev;
	bldev->props.power = FB_BLANK_POWERDOWN;
	hx8389_panel_bl_update_status(bldev);
	backlight_device_unregister(bldev);

	hx8389_panel_cancel_ulps_work(dssdev);
	hx8389_panel_cancel_esd_work(dssdev);
	destroy_workqueue(td->workqueue);

	/* reset, to be sure that the panel is in a valid state */
	hx8389_panel_hw_reset(dssdev);

	kfree(td);
}

static int hx8389_panel_power_on(struct omap_dss_device *dssdev)
{
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);
	int i, r;

	printk("[LCD]	:	hx8389_panel_power_on\n");

	/* At power on the first vsync has not been received yet */
        dssdev->first_vsync = false;

	if (dssdev->platform_enable) {
		r = dssdev->platform_enable(dssdev);
		if (r)
			return r;
	}
	r = omapdss_dsi_display_enable(dssdev);
	if (r) {
		dev_err(&dssdev->dev, "failed to enable DSI\n");
		goto err0;
	}

	if(!dssdev->skip_init)
	{
		hx8389_panel_hw_reset(dssdev);

		omapdss_dsi_vc_enable_hs(dssdev, td->channel, false);

		mdelay(5);

		for (i = 0; u760_hitachi_lcd_command_for_mipi[i][0] != END_OF_COMMAND; i++) {
			if(u760_hitachi_lcd_command_for_mipi[i][1] == DSI_GEN_LONGWRITE) {
				dsi_vc_gen_write_nosync(dssdev, td->channel, &u760_hitachi_lcd_command_for_mipi[i][3], u760_hitachi_lcd_command_for_mipi[i][2]);
			}
			else {
				dsi_vc_dcs_write_nosync(dssdev, td->channel, &u760_hitachi_lcd_command_for_mipi[i][3], u760_hitachi_lcd_command_for_mipi[i][2]);
			}
			mdelay(2);
		}

		r = hx8389_panel_sleep_out(td);
		if (r)
			goto err;

		dsi_vc_dcs_write_nosync(dssdev, td->channel, &u760_hitachi_lcd_command_for_mipi_set_power[0][3], u760_hitachi_lcd_command_for_mipi_set_power[0][2]);



		if(dssdev->phy.dsi.type == OMAP_DSS_DSI_TYPE_CMD_MODE){
			r = hx8389_panel_set_addr_mode(td, td->rotate, td->mirror);
			if (r)
				goto err;
		}
//                                                                                                                   
#if defined(CONFIG_U2_GAMMA)
	dispc_enable_gamma(OMAP_DSS_CHANNEL_LCD, 0);
	dispc_enable_gamma(OMAP_DSS_CHANNEL_LCD2, 0);
#endif
//                                                                                                                   
	if(dssdev->phy.dsi.type == OMAP_DSS_DSI_TYPE_VIDEO_MODE){
			r = hx8389_panel_dcs_write_0(td,DCS_DISPLAY_ON);
			if (r)
				goto err;
			td->display_on = true;
		}
		else
			td->display_on = false;
		mdelay(10);

		if(dssdev->phy.dsi.type == OMAP_DSS_DSI_TYPE_CMD_MODE){
			r = _hx8389_panel_enable_te(dssdev, false);
			if (r)
				goto err;
		}

		omapdss_dsi_vc_enable_hs(dssdev, td->channel, true);

		/*                                              
                                                                    
                                                                                  
   */
#ifdef CONFIG_OMAP4_DSS_HDMI
		omap_dispc_set_first_vsync(OMAP_DSS_CHANNEL_DIGIT, false);
#endif

		if(dssdev->phy.dsi.type == OMAP_DSS_DSI_TYPE_VIDEO_MODE){
			dsi_video_mode_enable(dssdev, 0x3e);
			//mdelay(30);
		}
	}
	else
	{
		omapdss_dsi_update_vc_mode(dssdev, 0, 0); //DSI_VC_MODE_VP set
		dssdev->skip_init = false;
	#ifdef CONFIG_FB_OMAP_BOOTLOADER_INIT
		dss_runtime_put();
	#endif
	}

	td->enabled = 1;

	return 0;
err:
	dev_err(&dssdev->dev, "error while enabling panel, issuing HW reset\n");

	hx8389_panel_hw_reset(dssdev);

	omapdss_dsi_display_disable(dssdev, true, false);
err0:
	return r;
}

static void hx8389_panel_power_off(struct omap_dss_device *dssdev)
{
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);
	struct lge_dsi_panel_data *panel_data = get_panel_data(dssdev);
	int r;
        printk("[LCD]	:	hx8389_panel_power_off\n");

	if(dssdev->phy.dsi.type == OMAP_DSS_DSI_TYPE_VIDEO_MODE)
		dsi_video_mode_disable(dssdev);

	r = hx8389_panel_dcs_write_0(td, DCS_DISPLAY_OFF);

	hw_guard_start(td, 50);
	hw_guard_wait(td);

	if (!r)
		r = hx8389_panel_sleep_in(td);

	if (r) {
		dev_err(&dssdev->dev,
				"error disabling panel, issuing HW reset\n");
	}

	/* reset  the panel */
	if (panel_data->reset_gpio)
		gpio_set_value(panel_data->reset_gpio, 0);

	/* disable lcd ldo */
	if (dssdev->platform_disable)
		dssdev->platform_disable(dssdev);

	/* if we try to turn off dsi regulator (VCXIO), system will be halt  */
	/* So below funtion's sencod args should set as false  */
	omapdss_dsi_display_disable(dssdev, false, false);
	td->enabled = 0;
}

static int hx8389_panel_reset(struct omap_dss_device *dssdev)
{
	dev_err(&dssdev->dev, "performing LCD reset\n");

	hx8389_panel_power_off(dssdev);
	return hx8389_panel_power_on(dssdev);
}

static int hx8389_panel_enable(struct omap_dss_device *dssdev)
{
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);
	int r;

	dev_dbg(&dssdev->dev, "enable\n");

	mutex_lock(&td->lock);

	if (dssdev->state != OMAP_DSS_DISPLAY_DISABLED) {
		r = -EINVAL;
		goto err;
	}

	dsi_bus_lock(dssdev);

	r = hx8389_panel_power_on(dssdev);

	dsi_bus_unlock(dssdev);

	if (r)
		goto err;

	hx8389_panel_queue_esd_work(dssdev);

	dssdev->state = OMAP_DSS_DISPLAY_ACTIVE;

	mutex_unlock(&td->lock);

	return 0;
err:
	dev_dbg(&dssdev->dev, "enable failed\n");
	mutex_unlock(&td->lock);
	return r;
}

static void hx8389_panel_disable(struct omap_dss_device *dssdev)
{
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);

	dev_dbg(&dssdev->dev, "disable\n");

	mutex_lock(&td->lock);

	hx8389_panel_cancel_ulps_work(dssdev);
	hx8389_panel_cancel_esd_work(dssdev);

	dsi_bus_lock(dssdev);

	if (dssdev->state == OMAP_DSS_DISPLAY_ACTIVE) {
		int r;

		r = hx8389_panel_wake_up(dssdev);
		if (!r)
			hx8389_panel_power_off(dssdev);
	}

	dsi_bus_unlock(dssdev);

	dssdev->state = OMAP_DSS_DISPLAY_DISABLED;

	mutex_unlock(&td->lock);
}

static int hx8389_panel_suspend(struct omap_dss_device *dssdev)
{
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);
	int r;

	dev_dbg(&dssdev->dev, "suspend\n");

	mutex_lock(&td->lock);

	if (dssdev->state == OMAP_DSS_DISPLAY_DISABLED) {
		r = -EINVAL;
		goto err;
	}else if (dssdev->state == OMAP_DSS_DISPLAY_SUSPENDED) {
		r = 0;
		goto err;
	}

	hx8389_panel_cancel_ulps_work(dssdev);
	hx8389_panel_cancel_esd_work(dssdev);

	dsi_bus_lock(dssdev);

	r = hx8389_panel_wake_up(dssdev);
	if (!r)
		hx8389_panel_power_off(dssdev);

	dsi_bus_unlock(dssdev);

	dssdev->state = OMAP_DSS_DISPLAY_SUSPENDED;

	mutex_unlock(&td->lock);

	return 0;
err:
	mutex_unlock(&td->lock);
	return r;
}

static int hx8389_panel_resume(struct omap_dss_device *dssdev)
{
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);
	int r;

	dev_dbg(&dssdev->dev, "resume\n");

	mutex_lock(&td->lock);

	if (dssdev->state != OMAP_DSS_DISPLAY_SUSPENDED) {
		r = -EINVAL;
		goto err;
	}

	dsi_bus_lock(dssdev);

	r = hx8389_panel_power_on(dssdev);

	dsi_bus_unlock(dssdev);

	if (r) {
		dssdev->state = OMAP_DSS_DISPLAY_DISABLED;
	} else {
		dssdev->state = OMAP_DSS_DISPLAY_ACTIVE;
		hx8389_panel_queue_esd_work(dssdev);
	}

	mutex_unlock(&td->lock);

	return r;
err:
	mutex_unlock(&td->lock);
	return r;
}

/*                                              
                                                                              
                                                                            
                                                
                                                                               
  */
static void hx8389_panel_display_on_work(struct work_struct *work)
{
	struct hx8389_panel_data *td = container_of(work, struct hx8389_panel_data,
			display_on_work);
	struct omap_dss_device *dssdev = td->dssdev;
	int r;

	dev_dbg(&dssdev->dev, "display_on_worker\n");

	hw_guard_start(td, 80);
	hw_guard_wait(td);

	dsi_bus_lock(dssdev);
	r = hx8389_panel_dcs_write_0(td, DCS_DISPLAY_ON); // display ON
	if(r)
		dev_err(&dssdev->dev, "display on fail\n");
	r = _hx8389_panel_enable_te(dssdev, true); // enable TE
	if(r)
		dev_err(&dssdev->dev, "TE enable fail\n");
	td->display_on = true;
	dsi_bus_unlock(dssdev);
}

static void hx8389_panel_framedone_cb(int err, void *data)
{
	struct omap_dss_device *dssdev = data;
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);

	dev_dbg(&dssdev->dev, "framedone, err %d\n", err);
	dsi_bus_unlock(dssdev);
	if(!td->display_on)
		schedule_work(&td->display_on_work);
}

static irqreturn_t hx8389_panel_te_isr(int irq, void *data)
{
	struct omap_dss_device *dssdev = data;
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);
	int old;
	int r;

	old = atomic_cmpxchg(&td->do_update, 1, 0);

	if (old) {
		cancel_delayed_work(&td->te_timeout_work);

		r = omap_dsi_update(dssdev, td->channel,
				td->update_region.x,
				td->update_region.y,
				td->update_region.w,
				td->update_region.h,
				hx8389_panel_framedone_cb, dssdev);
		if (r)
			goto err;
	}

	return IRQ_HANDLED;
err:
	dev_err(&dssdev->dev, "start update failed\n");
	dsi_bus_unlock(dssdev);
	return IRQ_HANDLED;
}

static void hx8389_panel_te_timeout_work_callback(struct work_struct *work)
{
	struct hx8389_panel_data *td = container_of(work, struct hx8389_panel_data,
					te_timeout_work.work);
	struct omap_dss_device *dssdev = td->dssdev;

	dev_err(&dssdev->dev, "TE not received for 250ms!\n");

	atomic_set(&td->do_update, 0);
	dsi_bus_unlock(dssdev);
}

static int hx8389_panel_update(struct omap_dss_device *dssdev,
				    u16 x, u16 y, u16 w, u16 h)
{
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);
	struct lge_dsi_panel_data *panel_data = get_panel_data(dssdev);
	int r;

	dev_dbg(&dssdev->dev, "update %d, %d, %d x %d\n", x, y, w, h);

	mutex_lock(&td->lock);
	dsi_bus_lock(dssdev);

	r = hx8389_panel_wake_up(dssdev);
	if (r)
		goto err;

	if (!td->enabled) {
		r = 0;
		goto err;
	}

	r = omap_dsi_prepare_update(dssdev, &x, &y, &w, &h, true);
	if (r)
		goto err;

	r = hx8389_panel_set_update_window(td, x, y, w, h);
	if (r)
		goto err;

	if (td->te_enabled && panel_data->use_ext_te) {
		td->update_region.x = x;
		td->update_region.y = y;
		td->update_region.w = w;
		td->update_region.h = h;
		barrier();
		schedule_delayed_work(&td->te_timeout_work,
				msecs_to_jiffies(250));
		atomic_set(&td->do_update, 1);
	} else {
		r = omap_dsi_update(dssdev, td->channel, x, y, w, h,
				hx8389_panel_framedone_cb, dssdev);
		if (r)
			goto err;
	}

	/* note: no bus_unlock here. unlock is in framedone_cb */
	mutex_unlock(&td->lock);
	return 0;
err:
	dsi_bus_unlock(dssdev);
	mutex_unlock(&td->lock);
	return r;
}

static int hx8389_panel_sync(struct omap_dss_device *dssdev)
{
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);

	dev_dbg(&dssdev->dev, "sync\n");

	mutex_lock(&td->lock);
	dsi_bus_lock(dssdev);
	dsi_bus_unlock(dssdev);
	mutex_unlock(&td->lock);

	dev_dbg(&dssdev->dev, "sync done\n");

	return 0;
}

static int _hx8389_panel_enable_te(struct omap_dss_device *dssdev, bool enable)
{
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);
	struct lge_dsi_panel_data *panel_data = get_panel_data(dssdev);
	int r;

	if (enable)
		r = hx8389_panel_dcs_write_1(td, DCS_TEAR_ON, 0);
	else
		r = hx8389_panel_dcs_write_0(td, DCS_TEAR_OFF);

	if (!panel_data->use_ext_te)
		omapdss_dsi_enable_te(dssdev, enable);

	if (td->panel_config->sleep.enable_te)
		msleep(td->panel_config->sleep.enable_te);

	td->te_enabled = enable;

	return r;
}

static int hx8389_panel_enable_te(struct omap_dss_device *dssdev, bool enable)
{
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);
	int r;

	mutex_lock(&td->lock);

	if (td->te_enabled == enable)
		goto end;

	dsi_bus_lock(dssdev);

	if (td->enabled) {
		r = hx8389_panel_wake_up(dssdev);
		if (r)
			goto err;

		r = _hx8389_panel_enable_te(dssdev, enable);
		if (r)
			goto err;
	}

	dsi_bus_unlock(dssdev);
end:
	mutex_unlock(&td->lock);

	return 0;
err:
	dsi_bus_unlock(dssdev);
	mutex_unlock(&td->lock);

	return r;
}

static int hx8389_panel_get_te(struct omap_dss_device *dssdev)
{
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);
	int r;

	mutex_lock(&td->lock);
	r = td->te_enabled;
	mutex_unlock(&td->lock);

	return r;
}

static int hx8389_panel_rotate(struct omap_dss_device *dssdev, u8 rotate)
{
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);
	int r;

	dev_dbg(&dssdev->dev, "rotate %d\n", rotate);

	mutex_lock(&td->lock);

	if (td->rotate == rotate)
		goto end;

	dsi_bus_lock(dssdev);

	if (td->enabled) {
		r = hx8389_panel_wake_up(dssdev);
		if (r)
			goto err;

		r = hx8389_panel_set_addr_mode(td, rotate, td->mirror);
		if (r)
			goto err;
	}

	td->rotate = rotate;

	dsi_bus_unlock(dssdev);
end:
	mutex_unlock(&td->lock);
	return 0;
err:
	dsi_bus_unlock(dssdev);
	mutex_unlock(&td->lock);
	return r;
}

static u8 hx8389_panel_get_rotate(struct omap_dss_device *dssdev)
{
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);
	int r;

	mutex_lock(&td->lock);
	r = td->rotate;
	mutex_unlock(&td->lock);

	return r;
}

static int hx8389_panel_mirror(struct omap_dss_device *dssdev, bool enable)
{
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);
	int r;

	dev_dbg(&dssdev->dev, "mirror %d\n", enable);

	mutex_lock(&td->lock);

	if (td->mirror == enable)
		goto end;

	dsi_bus_lock(dssdev);
	if (td->enabled) {
		r = hx8389_panel_wake_up(dssdev);
		if (r)
			goto err;

		r = hx8389_panel_set_addr_mode(td, td->rotate, enable);
		if (r)
			goto err;
	}

	td->mirror = enable;

	dsi_bus_unlock(dssdev);
end:
	mutex_unlock(&td->lock);
	return 0;
err:
	dsi_bus_unlock(dssdev);
	mutex_unlock(&td->lock);
	return r;
}

static bool hx8389_panel_get_mirror(struct omap_dss_device *dssdev)
{
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);
	int r;

	mutex_lock(&td->lock);
	r = td->mirror;
	mutex_unlock(&td->lock);

	return r;
}

static int hx8389_panel_memory_read(struct omap_dss_device *dssdev,
		void *buf, size_t size,
		u16 x, u16 y, u16 w, u16 h)
{
	int r;
	int first = 1;
	int plen;
	unsigned buf_used = 0;
	struct hx8389_panel_data *td = dev_get_drvdata(&dssdev->dev);

	if (size < w * h * 3)
		return -ENOMEM;

	mutex_lock(&td->lock);

	if (!td->enabled) {
		r = -ENODEV;
		goto err1;
	}

	size = min(w * h * 3,
			dssdev->panel.timings.x_res *
			dssdev->panel.timings.y_res * 3);

	dsi_bus_lock(dssdev);

	r = hx8389_panel_wake_up(dssdev);
	if (r)
		goto err2;

	/* plen 1 or 2 goes into short packet. until checksum error is fixed,
	 * use short packets. plen 32 works, but bigger packets seem to cause
	 * an error. */
	if (size % 2)
		plen = 1;
	else
		plen = 2;

	hx8389_panel_set_update_window(td, x, y, w, h);

	r = dsi_vc_set_max_rx_packet_size(dssdev, td->channel, plen);
	if (r)
		goto err2;

	while (buf_used < size) {
		u8 dcs_cmd = first ? 0x2e : 0x3e;
		first = 0;

		r = dsi_vc_dcs_read(dssdev, td->channel, dcs_cmd,
				buf + buf_used, size - buf_used);

		if (r < 0) {
			dev_err(&dssdev->dev, "read error\n");
			goto err3;
		}

		buf_used += r;

		if (r < plen) {
			dev_err(&dssdev->dev, "short read\n");
			break;
		}

		if (signal_pending(current)) {
			dev_err(&dssdev->dev, "signal pending, "
					"aborting memory read\n");
			r = -ERESTARTSYS;
			goto err3;
		}
	}

	r = buf_used;

err3:
	dsi_vc_set_max_rx_packet_size(dssdev, td->channel, 1);
err2:
	dsi_bus_unlock(dssdev);
err1:
	mutex_unlock(&td->lock);
	return r;
}

static void hx8389_panel_ulps_work(struct work_struct *work)
{
	struct hx8389_panel_data *td = container_of(work, struct hx8389_panel_data,
			ulps_work.work);
	struct omap_dss_device *dssdev = td->dssdev;

	mutex_lock(&td->lock);

	if (dssdev->state != OMAP_DSS_DISPLAY_ACTIVE || !td->enabled) {
		mutex_unlock(&td->lock);
		return;
	}

	dsi_bus_lock(dssdev);

	hx8389_panel_enter_ulps(dssdev);

	dsi_bus_unlock(dssdev);
	mutex_unlock(&td->lock);
}

static void hx8389_panel_esd_work(struct work_struct *work)
{
	struct hx8389_panel_data *td = container_of(work, struct hx8389_panel_data,
			esd_work.work);
	struct omap_dss_device *dssdev = td->dssdev;
	u8 state1, state2;
	int r;

	mutex_lock(&td->lock);

	if (!td->enabled) {
		mutex_unlock(&td->lock);
		return;
	}

	dsi_bus_lock(dssdev);

	r = hx8389_panel_wake_up(dssdev);
	if (r) {
		dev_err(&dssdev->dev, "failed to exit ULPS\n");
		goto err;
	}

	r = hx8389_panel_dcs_read_1(td, DCS_RDDSDR, &state1);
	if (r) {
		dev_err(&dssdev->dev, "failed to read hx8389_panel status\n");
		goto err;
	}

	/* Run self diagnostics */
	r = hx8389_panel_sleep_out(td);
	if (r) {
		dev_err(&dssdev->dev, "failed to run hx8389_panel self-diagnostics\n");
		goto err;
	}

	r = hx8389_panel_dcs_read_1(td, DCS_RDDSDR, &state2);
	if (r) {
		dev_err(&dssdev->dev, "failed to read hx8389_panel status\n");
		goto err;
	}

	/* Each sleep out command will trigger a self diagnostic and flip
	 * Bit6 if the test passes.
	 */
	if (!((state1 ^ state2) & (1 << 6))) {
		dev_err(&dssdev->dev, "LCD self diagnostics failed\n");
		goto err;
	}

	dsi_bus_unlock(dssdev);

	hx8389_panel_queue_esd_work(dssdev);

	mutex_unlock(&td->lock);
	return;
err:
	dev_err(&dssdev->dev, "performing LCD reset\n");

	hx8389_panel_reset(dssdev);

	dsi_bus_unlock(dssdev);

	hx8389_panel_queue_esd_work(dssdev);

	mutex_unlock(&td->lock);
}

static int hx8389_panel_set_update_mode(struct omap_dss_device *dssdev,
		enum omap_dss_update_mode mode)
{
	int update_mode;

	if(dssdev->phy.dsi.type == OMAP_DSS_DSI_TYPE_CMD_MODE)
		update_mode = OMAP_DSS_UPDATE_MANUAL;
	else
		update_mode = OMAP_DSS_UPDATE_AUTO;

	if (mode != update_mode)
		return -EINVAL;
	return 0;
}

static enum omap_dss_update_mode hx8389_panel_get_update_mode(
		struct omap_dss_device *dssdev)
{
	int update_mode;

	if(dssdev->phy.dsi.type == OMAP_DSS_DSI_TYPE_CMD_MODE)
		update_mode = OMAP_DSS_UPDATE_MANUAL;
	else
		update_mode = OMAP_DSS_UPDATE_AUTO;

	return update_mode;
}

static struct omap_dss_driver hx8389_panel_driver = {
	.probe		= hx8389_panel_probe,
	.remove		= __exit_p(hx8389_panel_remove),

	.enable		= hx8389_panel_enable,
	.disable	= hx8389_panel_disable,
	.suspend	= hx8389_panel_suspend,
	.resume		= hx8389_panel_resume,

	.set_update_mode = hx8389_panel_set_update_mode,
	.get_update_mode = hx8389_panel_get_update_mode,

	.update		= hx8389_panel_update,
	.sync		= hx8389_panel_sync,

	.get_resolution	= hx8389_panel_get_resolution,
	.get_recommended_bpp = omapdss_default_get_recommended_bpp,

	.enable_te	= hx8389_panel_enable_te,
	.get_te		= hx8389_panel_get_te,

	.set_rotate	= hx8389_panel_rotate,
	.get_rotate	= hx8389_panel_get_rotate,
	.set_mirror	= hx8389_panel_mirror,
	.get_mirror	= hx8389_panel_get_mirror,
	.memory_read	= hx8389_panel_memory_read,

	.get_timings	= hx8389_panel_get_timings,

	.driver         = {
		.name 	= "hx8389_panel",
		.owner  = THIS_MODULE,
	},
};

static int __init hx8389_panel_init(void)
{
	omap_dss_register_driver(&hx8389_panel_driver);

	return 0;
}

static void __exit hx8389_panel_exit(void)
{
	omap_dss_unregister_driver(&hx8389_panel_driver);
}

module_init(hx8389_panel_init);
module_exit(hx8389_panel_exit);

MODULE_AUTHOR("choongryeol.lee  <choongryeol.lee@lge.com>");
MODULE_DESCRIPTION("hx8389_panel Driver");
MODULE_LICENSE("GPL");
