/*
 * Copyright (C) 2010 NXP Semiconductors
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/i2c.h>
#include <linux/irq.h>
#include <linux/jiffies.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/miscdevice.h>
#include <linux/spinlock.h>
#include <linux/nfc/pn544.h>
//#include <mach-omap2/mux.h>

/* LGE_SJIT 2011-12-23 [dojip.kim@lge.com] message too noisy */
//#define DEBUG_MESSAGE
#ifdef DEBUG_MESSAGE
#define dprintk(fmt, args...) printk(fmt, ##args)
#else
#define dprintk(fmt, args...) do{ } while(0)
#endif

#define CONFIG_LGE_NFC_DRIVER_TEST	
#define CONFIG_LGE_NXP_NFC

#define MAX_BUFFER_SIZE	512
#define PN544_RESET_CMD 	0
#define PN544_DOWNLOAD_CMD	1
#if defined(CONFIG_LGE_NFC_DRIVER_TEST)
#define PN544_INTERRUPT_CMD	2	//seokmin added for debugging
#define PN544_READ_POLLING_CMD	3	//seokmin added for debugging
#endif


struct pn544_dev	{
	wait_queue_head_t	read_wq;
	struct mutex		read_mutex;
	struct i2c_client	*client;
	struct miscdevice	pn544_device;
	unsigned int 		ven_gpio;
	unsigned int 		firm_gpio;
	unsigned int		irq_gpio;
	bool			irq_enabled;
	spinlock_t		irq_enabled_lock;
};

#ifdef CONFIG_LGE_NFC_DRIVER_TEST
static int	stReadIntFlag = 0;	//seokmin added for debugging
#endif
#ifdef CONFIG_LGE_NXP_NFC
static struct i2c_client *pn544_client = NULL;	//seokmin
#endif

static void pn544_disable_irq(struct pn544_dev *pn544_dev)
{
	unsigned long flags;

	spin_lock_irqsave(&pn544_dev->irq_enabled_lock, flags);
	if (pn544_dev->irq_enabled) {
//		disable_irq_nosync(pn544_dev->client->irq);
		disable_irq_nosync(OMAP_GPIO_IRQ(pn544_dev->irq_gpio));
//		disable_irq_nosync(pn544_dev->irq_gpio);
		pn544_dev->irq_enabled = false;
	}
	spin_unlock_irqrestore(&pn544_dev->irq_enabled_lock, flags);
}

static irqreturn_t pn544_dev_irq_handler(int irq, void *dev_id)
{
	struct pn544_dev *pn544_dev = dev_id;
	dprintk("[%s] in!\n", __func__);
	pn544_disable_irq(pn544_dev);

	/* Wake up waiting readers */
	wake_up(&pn544_dev->read_wq);

	return IRQ_HANDLED;
}

static ssize_t pn544_dev_read(struct file *filp, char __user *buf,
		size_t count, loff_t *offset)
{
	struct pn544_dev *pn544_dev = filp->private_data;
	char tmp[MAX_BUFFER_SIZE];
	int ret = 0;

	if (count > MAX_BUFFER_SIZE)
		count = MAX_BUFFER_SIZE;

	pr_debug("%s : reading %zu bytes.\n", __func__, count);

	dprintk("%s in : readin size %zu bytes \n", __func__, count);
	mutex_lock(&pn544_dev->read_mutex);
#ifdef CONFIG_LGE_NFC_DRIVER_TEST
	if(!stReadIntFlag) { 	 //seokmin added for debugging
#endif
		if (!gpio_get_value(pn544_dev->irq_gpio)) {
			if (filp->f_flags & O_NONBLOCK) {
				dprintk("[seokmin] interrupt nonblock \n");
				ret = -EAGAIN;
				goto fail;
			}
			dprintk("[seokmin]Waiting IRQ[%d, %d]Starts\n", ret, gpio_get_value(pn544_dev->irq_gpio));
			pn544_dev->irq_enabled = true;
			//enable_irq(pn544_dev->client->irq);
			enable_irq(OMAP_GPIO_IRQ(pn544_dev->irq_gpio));
			//enable_irq(pn544_dev->irq_gpio);
			dprintk("[%s]irq num[%d][%d][%d]\n", __func__, pn544_dev->irq_gpio, OMAP_GPIO_IRQ(pn544_dev->irq_gpio), pn544_dev->client->irq);
			ret = wait_event_interruptible(pn544_dev->read_wq,
					gpio_get_value(pn544_dev->irq_gpio));

			pn544_disable_irq(pn544_dev);
			dprintk("[seokmin]Waiting IRQ[%d, %d]Ends\n", ret,gpio_get_value(pn544_dev->irq_gpio));

			if (ret)
				goto fail;

		}
#ifdef CONFIG_LGE_NFC_DRIVER_TEST
	}	//seokmin
#endif
	/* Read data */
	ret = i2c_master_recv(pn544_dev->client, tmp, count);
	mutex_unlock(&pn544_dev->read_mutex);

	if (ret < 0) {
		pr_err("%s: i2c_master_recv returned %d\n", __func__, ret);
		return ret;
	}
	if (ret > count) {
		pr_err("%s: received too many bytes from i2c (%d)\n",
			__func__, ret);
		return -EIO;
	}
	if (copy_to_user(buf, tmp, ret)) {
		pr_warning("%s : failed to copy to user space\n", __func__);
		return -EFAULT;
	}
	return ret;

fail:
	mutex_unlock(&pn544_dev->read_mutex);
	return ret;
}


static ssize_t pn544_dev_write(struct file *filp, const char __user *buf,
		size_t count, loff_t *offset)
{
	struct pn544_dev  *pn544_dev;
	char tmp[MAX_BUFFER_SIZE];
	int ret;

	pn544_dev = filp->private_data;

        /* [SJIT] - probably need locking */
	if (count > MAX_BUFFER_SIZE)
		count = MAX_BUFFER_SIZE;

	if (copy_from_user(tmp, buf, count)) {
		pr_err("%s : failed to copy from user space\n", __func__);
		return -EFAULT;
	}

	pr_debug("%s : writing %zu bytes.\n", __func__, count);
	/* Write data */
	dprintk("write: pn544_write len=:%d\n", count);


	ret = i2c_master_send(pn544_dev->client, tmp, count);
	if (ret != count) {
		pr_err("%s : i2c_master_send returned %d\n", __func__, ret);
		ret = -EIO;
	}

	return ret;
}

static int pn544_dev_open(struct inode *inode, struct file *filp)
{
#ifdef CONFIG_LGE_NXP_NFC
	filp->private_data = i2c_get_clientdata(pn544_client);
#else
	struct pn544_dev *pn544_dev = container_of(filp->private_data,
					struct pn544_dev,
					pn544_device);

	filp->private_data = pn544_dev;

#endif
	pr_debug("%s : %d,%d\n", __func__, imajor(inode), iminor(inode));

	return 0;
}

static long pn544_dev_ioctl(struct file *filp,
		unsigned int cmd, unsigned long arg)
{
	struct pn544_dev *pn544_dev = filp->private_data;
	int ret;
        /* [SJIT] probably need locking */
	dprintk("%s\n",__func__);
	switch (cmd) {
	case PN544_SET_PWR:
		{
			if (arg == 2) {
				/* power on with firmware download (requires hw reset)
				 */
				dprintk("%s power on with firmware\n", __func__);
				gpio_set_value(pn544_dev->ven_gpio, 1);
				gpio_set_value(pn544_dev->firm_gpio, 1);
				msleep(10);
				gpio_set_value(pn544_dev->ven_gpio, 0);
				msleep(10);
				gpio_set_value(pn544_dev->ven_gpio, 1);
				msleep(10);
			} else if (arg == 1) {
				/* power on */
				dprintk("%s power on\n", __func__);
				gpio_set_value(pn544_dev->firm_gpio, 0);
				gpio_set_value(pn544_dev->ven_gpio, 1);

//				gpio_direction_output(pn544_dev->irq_gpio, 0);
//				gpio_set_value(pn544_dev->irq_gpio, 0);
//				gpio_direction_input(pn544_dev->irq_gpio);

				msleep(10);
				ret = gpio_get_value(pn544_dev->ven_gpio);
				dprintk("ioctl: pn544_set_pwr %d\n", ret);
			} else  if (arg == 0) {
				/* power off */
				dprintk("%s power off\n", __func__);
				gpio_set_value(pn544_dev->firm_gpio, 0);
				gpio_set_value(pn544_dev->ven_gpio, 0);
				msleep(10);
				ret = gpio_get_value(pn544_dev->ven_gpio);
				dprintk("ioctl: ven_gpio %d\n", ret);
				ret = gpio_get_value(pn544_dev->firm_gpio);
				dprintk("ioctl: firm_gpio %d\n", ret);
				//gpio_tlmm_config(GPIO_CFG(130, 0, GPIO_CFG_OUTPUT, GPIO_CFG_NO_PULL, GPIO_CFG_2MA), GPIO_CFG_ENABLE);
				//gpio_set_value(130, 0);
				//ret = gpio_get_value(130);
				//printk("ioctl : gpio_130 : %d\n", ret);
				//gpio_set_value(130, 1);
				//ret = gpio_get_value(130);
				//printk("ioctl : gpio_130 : %d\n", ret);
				//gpio_tlmm_config(GPIO_CFG(pn544_dev->firm_gpio, 0, GPIO_CFG_INPUT, GPIO_CFG_NO_PULL, GPIO_CFG_2MA), GPIO_CFG_ENABLE);
			} else {
				pr_err("%s bad arg %ld\n", __func__, arg);
				return -EINVAL;
			}
			break;
		}
#ifdef CONFIG_LGE_NFC_DRIVER_TEST
//seokmin added for debugging
	case PN544_INTERRUPT_CMD:
		{
//			pn544_disable_irq = level;
			dprintk("ioctl: pn544_interrupt enable level:%ld\n", arg);
			break;
		}
	case PN544_READ_POLLING_CMD:
		{
			stReadIntFlag = arg;
			dprintk("ioctl: pn544_polling flag set:%ld\n", arg);
			break;
		}
#endif
	default:
		pr_err("%s bad ioctl %d\n", __func__, cmd);
		return -EINVAL;
	}

	return 0;
}

static const struct file_operations pn544_dev_fops = {
	.owner	= THIS_MODULE,
	.llseek	= no_llseek,
	.read	= pn544_dev_read,
	.write	= pn544_dev_write,
	.open	= pn544_dev_open,
	.unlocked_ioctl  = pn544_dev_ioctl,
};

static int pn544_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
{
	int ret;
	struct pn544_i2c_platform_data *platform_data;
	struct pn544_dev *pn544_dev = NULL;

#ifdef CONFIG_LGE_NXP_NFC
	dprintk("[seokmin]pn544_probe in!!\n");
	pn544_client = client;//seokmin 
#endif

	platform_data = client->dev.platform_data;

	if (platform_data == NULL) {
		pr_err("%s : nfc probe fail\n", __func__);
		return  -ENODEV;
	}

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		pr_err("%s : need I2C_FUNC_I2C\n", __func__);
		return  -ENODEV;
	}
	dprintk("[%s]platform_data irq set [%d]\n", __func__, platform_data->irq_gpio);
	ret = gpio_request(platform_data->irq_gpio, "nfc_int");
	if (ret)
		return  -ENODEV;
	ret = gpio_request(platform_data->ven_gpio, "nfc_ven");
	if (ret)
		goto err_ven;
	ret = gpio_request(platform_data->firm_gpio, "nfc_firm");
	if (ret)
		goto err_firm;

	pn544_dev = kzalloc(sizeof(*pn544_dev), GFP_KERNEL);
	if (pn544_dev == NULL) {
		dev_err(&client->dev,
				"failed to allocate memory for module data\n");
		ret = -ENOMEM;
		goto err_exit;
	}
	ret = gpio_direction_output(platform_data->ven_gpio,1);
	ret = gpio_direction_output(platform_data->firm_gpio,0);
	ret = gpio_direction_input(platform_data->irq_gpio);
	dprintk("[%s]direction input return[%d]\n", __func__, ret);
	//ret = gpio_direction_input(gpio_to_irq(platform_data->irq_gpio));

//	pn544_dev->irq_gpio = gpio_to_irq(platform_data->irq_gpio);
	pn544_dev->irq_gpio = platform_data->irq_gpio;
	pn544_dev->ven_gpio  = platform_data->ven_gpio;
	pn544_dev->firm_gpio  = platform_data->firm_gpio;
	pn544_dev->client   = client;
	dprintk("[%s]pn544_dev irq set [%d]\n", __func__ , pn544_dev->irq_gpio);

	/* init mutex and queues */
	init_waitqueue_head(&pn544_dev->read_wq);
	mutex_init(&pn544_dev->read_mutex);
	spin_lock_init(&pn544_dev->irq_enabled_lock);

	pn544_dev->pn544_device.minor = MISC_DYNAMIC_MINOR;
	pn544_dev->pn544_device.name = "pn544";
	pn544_dev->pn544_device.fops = &pn544_dev_fops;

	ret = misc_register(&pn544_dev->pn544_device);
	if (ret) {
		pr_err("%s : misc_register failed\n", __FILE__);
		goto err_misc_register;
	}

	/* request irq.  the irq is set whenever the chip has data available
	 * for reading.  it is cleared when all data has been read.
	 */
	pr_info("%s : requesting IRQ %d\n", __func__, client->irq);
	pn544_dev->irq_enabled = true;
//	ret = request_irq(client->irq, pn544_dev_irq_handler, IRQF_TRIGGER_HIGH, client->name, pn544_dev);
	ret = request_irq(gpio_to_irq(pn544_dev->irq_gpio), pn544_dev_irq_handler, IRQF_TRIGGER_HIGH, "pn544", pn544_dev);
//	ret = request_irq(pn544_dev->irq_gpio, pn544_dev_irq_handler, IRQF_TRIGGER_HIGH, "pn544", pn544_dev);
	if (ret) {
		dev_err(&client->dev, "request_irq failed\n");
		goto err_request_irq_failed;
	}
	pn544_disable_irq(pn544_dev);
	i2c_set_clientdata(client, pn544_dev);	
	
	return 0;

err_request_irq_failed:
	misc_deregister(&pn544_dev->pn544_device);
err_misc_register:
	mutex_destroy(&pn544_dev->read_mutex);
	kfree(pn544_dev);
err_exit:
	gpio_free(pn544_dev->irq_gpio);
err_firm:
	gpio_free(pn544_dev->ven_gpio);
err_ven:
	gpio_free(pn544_dev->firm_gpio);
	return ret;
}

static int pn544_remove(struct i2c_client *client)
{
	struct pn544_dev *pn544_dev;

	pn544_dev = i2c_get_clientdata(client);
//	free_irq(client->irq, pn544_dev);
	free_irq(gpio_to_irq(pn544_dev->irq_gpio), pn544_dev);
//	free_irq(pn544_dev->irq_gpio, pn544_dev);
	misc_deregister(&pn544_dev->pn544_device);
	mutex_destroy(&pn544_dev->read_mutex);
	gpio_free(pn544_dev->irq_gpio);
	gpio_free(pn544_dev->ven_gpio);
	gpio_free(pn544_dev->firm_gpio);
	kfree(pn544_dev);

	return 0;
}

static const struct i2c_device_id pn544_id[] = {
	{ "pn544", 0 },
	{ }
};

static struct i2c_driver pn544_driver = {
	.id_table	= pn544_id,
	.probe		= pn544_probe,
	.remove		= pn544_remove,
	.driver		= {
		.owner	= THIS_MODULE,
		.name	= "pn544",
	},
};

/*
 * module load/unload record keeping
 */

static int __init pn544_dev_init(void)
{
	dprintk("[seokmin]pn544_dev_init in\n");
	
	pr_info("Loading pn544 driver\n");
	return i2c_add_driver(&pn544_driver);
}

static void __exit pn544_dev_exit(void)
{
	pr_info("Unloading pn544 driver\n");
	i2c_del_driver(&pn544_driver);
}

module_init(pn544_dev_init);
module_exit(pn544_dev_exit);
MODULE_AUTHOR("Sylvain Fonteneau");
MODULE_DESCRIPTION("NFC PN544 driver");
MODULE_LICENSE("GPL");
