// SPDX-License-Identifier: GPL-2.0
/*
 * GPIO-based Reset Driver for Tales ELS31
 *
 * Copyright (c) 2022-2023 Atmark Techno, Inc. All Rights Reserved.
 */
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/reset-controller.h>

struct els31_reset_data {
	struct reset_controller_dev rcdev;

	struct gpio_desc	*on;
	struct gpio_desc	*pwr;
	struct gpio_desc	*vusb;

	unsigned int poweroff_interval;
};

#define ELS31_ON_ASSERT_WAIT_TIME_MS	(4)	/* wait batt++ high */
#define ELS31_ON_ASSERT_TIME_US		(500)	/* ignition, 5x margin */
#define ELS31_RESET_WAIT_TIME_MS	(1000)	/* interval off to on. */

#define to_els31_reset_data(_rcdev) \
	container_of(_rcdev, struct els31_reset_data, rcdev)

static void els31_reset_ignition(struct gpio_desc *gpio,
				 unsigned long delay_us)
{
	gpiod_set_value_cansleep(gpio, 1);
	udelay(delay_us);
	gpiod_set_value_cansleep(gpio, 0);
}

static void els31_reset_power_on(struct els31_reset_data *drvdata)
{
	int ret;

	ret = gpiod_get_value_cansleep(drvdata->pwr);
	if (ret)
		/* already powered on */
		return;

	gpiod_set_value_cansleep(drvdata->pwr, 1);
	msleep(ELS31_ON_ASSERT_WAIT_TIME_MS);
	els31_reset_ignition(drvdata->on, ELS31_ON_ASSERT_TIME_US);
}

static void els31_reset_power_off(struct els31_reset_data *drvdata)
{
	int ret;

	ret = gpiod_get_value_cansleep(drvdata->pwr);
	if (!ret)
		/* already powered off */
		return;

	gpiod_set_value_cansleep(drvdata->pwr, 0);
	/* wait batt+ under 0.5V */
	msleep(drvdata->poweroff_interval);
}

static int els31_reset_reset(struct reset_controller_dev *rcdev,
							unsigned long id)
{
	struct els31_reset_data *drvdata = to_els31_reset_data(rcdev);

	els31_reset_power_off(drvdata);
	msleep(ELS31_RESET_WAIT_TIME_MS);
	els31_reset_power_on(drvdata);

	return 0;
}

static struct reset_control_ops els31_reset_ops = {
	.reset = els31_reset_reset,
};

static ssize_t els31_reset_reset_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct platform_device *pdev= to_platform_device(dev);
	struct els31_reset_data *drvdata = platform_get_drvdata(pdev);
	int val, ret;

	ret = kstrtoint(buf, 0, &val);
	if (ret)
		return ret;

	if (val)
		els31_reset_reset(&drvdata->rcdev, 0);

	return count;
}
static DEVICE_ATTR(els31_reset, S_IWUSR, NULL, els31_reset_reset_store);

static ssize_t els31_power_ctrl_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct platform_device *pdev= to_platform_device(dev);
	struct els31_reset_data *drvdata = platform_get_drvdata(pdev);
	int val, ret;

	ret = kstrtoint(buf, 0, &val);
	if (ret)
		return ret;

	if (val)
		els31_reset_power_on(drvdata);
	else
		els31_reset_power_off(drvdata);

	return count;
}
static DEVICE_ATTR(els31_power_ctrl, S_IWUSR, NULL, els31_power_ctrl_store);

static struct attribute *els31_reset_attrs[] = {
	&dev_attr_els31_reset.attr,
	&dev_attr_els31_power_ctrl.attr,
	NULL
};

static struct attribute_group els31_reset_attr_group = {
	.name = NULL, /* put in device directory */
	.attrs = els31_reset_attrs,
};

static int of_els31_reset_xlate(struct reset_controller_dev *rcdev,
				const struct of_phandle_args *reset_spec)
{
	if (WARN_ON(reset_spec->args_count != 0))
		return -EINVAL;

	return 0;
}

static struct gpio_desc *els31_reset_gpiod_get(struct device *dev,
					       const char *name)
{
	struct gpio_desc *gpiod;

	if (gpiod_count(dev, name) != 1) {
		dev_err(dev, "%s property missing, or not a single gpio\n",
			name);
		return ERR_PTR(-EINVAL);
	}

	gpiod = devm_gpiod_get(dev, name, GPIOD_OUT_LOW);
	if (IS_ERR(gpiod))
		dev_err_probe(dev, PTR_ERR(gpiod), "invalid %s gpio\n", name);

	return gpiod;
}

static int els31_reset_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct els31_reset_data *drvdata;
	int ret;

	drvdata = devm_kzalloc(&pdev->dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	drvdata->pwr = els31_reset_gpiod_get(&pdev->dev, "pwr");
	if (IS_ERR(drvdata->pwr))
		return PTR_ERR(drvdata->pwr);

	drvdata->on = els31_reset_gpiod_get(&pdev->dev, "on");
	if (IS_ERR(drvdata->on))
		return PTR_ERR(drvdata->on);

	drvdata->vusb = els31_reset_gpiod_get(&pdev->dev, "vusb");
	if (IS_ERR(drvdata->vusb))
		return PTR_ERR(drvdata->vusb);

	ret = of_property_read_u32(np, "poweroff-interval",
				   &drvdata->poweroff_interval);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, drvdata);

	/* VUSB always set high */
	gpiod_set_value_cansleep(drvdata->vusb, 1);

	els31_reset_power_on(drvdata);

	drvdata->rcdev.of_node = np;
	drvdata->rcdev.owner = THIS_MODULE;
	drvdata->rcdev.nr_resets = 1;
	drvdata->rcdev.ops = &els31_reset_ops;
	drvdata->rcdev.of_xlate = of_els31_reset_xlate;

	ret = devm_reset_controller_register(&pdev->dev, &drvdata->rcdev);
	if (ret)
		goto err;

	ret = sysfs_create_group(&pdev->dev.kobj, &els31_reset_attr_group);
	if (ret)
		goto err;

	return 0;

err:
	return ret;
}

static void els31_reset_remove(struct platform_device *pdev)
{
	sysfs_remove_group(&pdev->dev.kobj, &els31_reset_attr_group);
}

static void els31_reset_shutdown(struct platform_device *pdev)
{
	struct els31_reset_data *data = platform_get_drvdata(pdev);

	els31_reset_power_off(data);
}

static struct of_device_id els31_reset_dt_ids[] = {
	{ .compatible = "els31-reset" },
	{ }
};

static struct platform_driver els31_reset_driver = {
	.probe = els31_reset_probe,
	.remove = els31_reset_remove,
	.shutdown = els31_reset_shutdown,
	.driver = {
		.name = "els31-reset",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(els31_reset_dt_ids),
	},
};

static int __init els31_reset_init(void)
{
	return platform_driver_register(&els31_reset_driver);
}
arch_initcall(els31_reset_init);

static void __exit els31_reset_exit(void)
{
	platform_driver_unregister(&els31_reset_driver);
}
module_exit(els31_reset_exit);

MODULE_AUTHOR("Mitsuhiro Yoshida <mitsuhiro.yoshida@atmark-techno.com>");
MODULE_DESCRIPTION("Tales ELS31 Reset Controller");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:els31-reset");
MODULE_DEVICE_TABLE(of, els31_reset_dt_ids);
