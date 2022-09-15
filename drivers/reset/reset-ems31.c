// SPDX-License-Identifier: GPL-2.0
/*
 * GPIO-based Reset Driver for Tales EMS31
 *
 * Copyright (c) 2021 Atmark Techno, Inc. All Rights Reserved.
 */
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/reset-controller.h>

struct ems31_reset_data {
	struct reset_controller_dev rcdev;

	struct gpio_desc *on;
	struct gpio_desc *pwr;
	struct gpio_desc *gpio25;
};

#define EMS31_ON_ASSERT_WAIT_TIME_MS	(4)	/* wait batt++ high */
#define EMS31_ON_ASSERT_TIME_US		(500)	/* ignition, 5x margin */
#define EMS31_BATTPLUS_OFF_WAIT_TIME_MS	(230)	/* wait batt+ under 0.3V */
#define EMS31_RESET_WAIT_TIME_MS	(1000)	/* interval off to on. */

#define to_ems31_reset_data(_rcdev) \
	container_of(_rcdev, struct ems31_reset_data, rcdev)

static void ems31_reset_ignition(struct gpio_desc *gpiod,
				 unsigned long delay_us)
{
	gpiod_set_value_cansleep(gpiod, 1);
	udelay(delay_us);
	gpiod_set_value_cansleep(gpiod, 0);
}

static void ems31_reset_power_on(struct ems31_reset_data *drvdata)
{
	int ret;

	ret = gpiod_get_value_cansleep(drvdata->pwr);
	if (ret)
		/* already powered on */
		return;

	gpiod_set_value_cansleep(drvdata->pwr, 1);
	msleep(EMS31_ON_ASSERT_WAIT_TIME_MS);
	ems31_reset_ignition(drvdata->on, EMS31_ON_ASSERT_TIME_US);
}

static void ems31_reset_power_off(struct ems31_reset_data *drvdata)
{
	int ret;

	ret = gpiod_get_value_cansleep(drvdata->pwr);
	if (!ret)
		/* already powered off */
		return;

	gpiod_set_value_cansleep(drvdata->pwr, 0);
	/* wait batt+ under 0.3V */
	msleep(EMS31_BATTPLUS_OFF_WAIT_TIME_MS);
}

static int ems31_reset_reset(struct reset_controller_dev *rcdev,
							unsigned long id)
{
	struct ems31_reset_data *drvdata = to_ems31_reset_data(rcdev);

	ems31_reset_power_off(drvdata);
	msleep(EMS31_RESET_WAIT_TIME_MS);
	ems31_reset_power_on(drvdata);

	return 0;
}

static struct reset_control_ops ems31_reset_ops = {
	.reset = ems31_reset_reset,
};

static ssize_t ems31_reset_reset_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct platform_device *pdev= to_platform_device(dev);
	struct ems31_reset_data *drvdata = platform_get_drvdata(pdev);
	int val, ret;

	ret = kstrtoint(buf, 0, &val);
	if (ret)
		return ret;

	if (val)
		ems31_reset_reset(&drvdata->rcdev, 0);

	return count;
}
static DEVICE_ATTR(ems31_reset, S_IWUSR, NULL, ems31_reset_reset_store);

static ssize_t ems31_power_ctrl_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct platform_device *pdev= to_platform_device(dev);
	struct ems31_reset_data *drvdata = platform_get_drvdata(pdev);
	int val, ret;

	ret = kstrtoint(buf, 0, &val);
	if (ret)
		return ret;

	if (val)
		ems31_reset_power_on(drvdata);
	else
		ems31_reset_power_off(drvdata);

	return count;
}
static DEVICE_ATTR(ems31_power_ctrl, S_IWUSR, NULL, ems31_power_ctrl_store);

static struct attribute *ems31_reset_attrs[] = {
	&dev_attr_ems31_reset.attr,
	&dev_attr_ems31_power_ctrl.attr,
	NULL
};

static struct attribute_group ems31_reset_attr_group = {
	.name = NULL, /* put in device directory */
	.attrs = ems31_reset_attrs,
};

static int of_ems31_reset_xlate(struct reset_controller_dev *rcdev,
				const struct of_phandle_args *reset_spec)
{
	if (WARN_ON(reset_spec->args_count != 0))
		return -EINVAL;

	return 0;
}

static struct gpio_desc *ems31_reset_gpiod_get(struct device *dev,
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

static int ems31_reset_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct ems31_reset_data *drvdata;
	int ret;

	drvdata = devm_kzalloc(&pdev->dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	drvdata->pwr = ems31_reset_gpiod_get(&pdev->dev, "pwr");
	if (IS_ERR(drvdata->pwr))
		return PTR_ERR(drvdata->pwr);

	drvdata->on = ems31_reset_gpiod_get(&pdev->dev, "on");
	if (IS_ERR(drvdata->on))
		return PTR_ERR(drvdata->on);

	drvdata->gpio25 = ems31_reset_gpiod_get(&pdev->dev,
						"lte-gpio25");
	if (IS_ERR(drvdata->gpio25))
		return PTR_ERR(drvdata->gpio25);

	platform_set_drvdata(pdev, drvdata);

	/* export EMS31 GPIO25 */
	gpiod_export(drvdata->gpio25, false);
	gpiod_export_link(&pdev->dev, "lte_gpio25", drvdata->gpio25);

	ems31_reset_power_on(drvdata);

	drvdata->rcdev.of_node = np;
	drvdata->rcdev.owner = THIS_MODULE;
	drvdata->rcdev.nr_resets = 1;
	drvdata->rcdev.ops = &ems31_reset_ops;
	drvdata->rcdev.of_xlate = of_ems31_reset_xlate;

	ret = devm_reset_controller_register(&pdev->dev, &drvdata->rcdev);
	if (ret)
		goto err;

	ret = sysfs_create_group(&pdev->dev.kobj, &ems31_reset_attr_group);
	if (ret)
		goto err;

	return 0;

err:
	gpiod_unexport(drvdata->gpio25);

	return ret;
}

static void ems31_reset_remove(struct platform_device *pdev)
{
	struct ems31_reset_data *drvdata = platform_get_drvdata(pdev);

	gpiod_unexport(drvdata->gpio25);

	sysfs_remove_group(&pdev->dev.kobj, &ems31_reset_attr_group);
}

static void ems31_reset_shutdown(struct platform_device *pdev)
{
	struct ems31_reset_data *data = platform_get_drvdata(pdev);

	ems31_reset_power_off(data);
}

static struct of_device_id ems31_reset_dt_ids[] = {
	{ .compatible = "ems31-reset" },
	{ }
};

static struct platform_driver ems31_reset_driver = {
	.probe = ems31_reset_probe,
	.remove = ems31_reset_remove,
	.shutdown = ems31_reset_shutdown,
	.driver = {
		.name = "ems31-reset",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(ems31_reset_dt_ids),
	},
};

static int __init ems31_reset_init(void)
{
	return platform_driver_register(&ems31_reset_driver);
}
arch_initcall(ems31_reset_init);

static void __exit ems31_reset_exit(void)
{
	platform_driver_unregister(&ems31_reset_driver);
}
module_exit(ems31_reset_exit);

MODULE_AUTHOR("Mitsuhiro Yoshida <mitsuhiro.yoshida@atmark-techno.com>");
MODULE_DESCRIPTION("Tales EMS31 Reset Controller");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:ems31-reset");
MODULE_DEVICE_TABLE(of, ems31_reset_dt_ids);
