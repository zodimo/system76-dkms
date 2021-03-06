/*
 * system76.c
 *
 * Copyright (C) 2017 Jeremy Soller <jeremy@system76.com>
 * Copyright (C) 2014-2016 Arnoud Willemsen <mail@lynthium.com>
 * Copyright (C) 2013-2015 TUXEDO Computers GmbH <tux@tuxedocomputers.com>
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the  GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is  distributed in the hope that it  will be useful, but
 * WITHOUT  ANY   WARRANTY;  without   even  the  implied   warranty  of
 * MERCHANTABILITY  or FITNESS FOR  A PARTICULAR  PURPOSE.  See  the GNU
 * General Public License for more details.
 *
 * You should  have received  a copy of  the GNU General  Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#define S76_DRIVER_NAME KBUILD_MODNAME
#define pr_fmt(fmt) S76_DRIVER_NAME ": " fmt

#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/dmi.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/i8042.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reboot.h>
#include <linux/rfkill.h>
#include <linux/stringify.h>
#include <linux/version.h>
#include <linux/workqueue.h>

#define __S76_PR(lvl, fmt, ...) do { pr_##lvl(fmt, ##__VA_ARGS__); } \
		while (0)
#define S76_INFO(fmt, ...) __S76_PR(info, fmt, ##__VA_ARGS__)
#define S76_ERROR(fmt, ...) __S76_PR(err, fmt, ##__VA_ARGS__)
#define S76_DEBUG(fmt, ...) __S76_PR(debug, "[%s:%u] " fmt, \
		__func__, __LINE__, ##__VA_ARGS__)

#define S76_EVENT_GUID  "ABBC0F6B-8EA1-11D1-00A0-C90629100000"
#define S76_WMBB_GUID    "ABBC0F6D-8EA1-11D1-00A0-C90629100000"

#define S76_HAS_HWMON (defined(CONFIG_HWMON) || (defined(MODULE) && defined(CONFIG_HWMON_MODULE)))

/* method IDs for S76_GET */
#define GET_EVENT               0x01  /*   1 */

struct platform_device *s76_platform_device;

static int s76_wmbb(u32 method_id, u32 arg, u32 *retval) {
	struct acpi_buffer in  = { (acpi_size) sizeof(arg), &arg };
	struct acpi_buffer out = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	acpi_status status;
	u32 tmp;

	S76_DEBUG("%0#4x  IN : %0#6x\n", method_id, arg);

	status = wmi_evaluate_method(S76_WMBB_GUID, 0, method_id, &in, &out);

	if (unlikely(ACPI_FAILURE(status))) {
		return -EIO;
	}

	obj = (union acpi_object *) out.pointer;
	if (obj && obj->type == ACPI_TYPE_INTEGER) {
		tmp = (u32) obj->integer.value;
	} else {
		tmp = 0;
	}

	S76_DEBUG("%0#4x  OUT: %0#6x (IN: %0#6x)\n", method_id, tmp, arg);

	if (likely(retval)) {
		*retval = tmp;
	}

	kfree(obj);

	return 0;
}

#include "system76_ap-led.c"
#include "system76_input.c"
#include "system76_kb-led.c"
#include "system76_hwmon.c"
#include "system76_nv_hda.c"

static void s76_wmi_notify(u32 value, void *context) {
	u32 event;

	if (value != 0xD0) {
		S76_INFO("Unexpected WMI event (%0#6x)\n", value);
		return;
	}

	s76_wmbb(GET_EVENT, 0, &event);

	S76_INFO("WMI event code (%x)\n", event);

	switch (event) {
	case 0x81:
		kb_wmi_dec();
		break;
	case 0x82:
		kb_wmi_inc();
		break;
	case 0x83:
		kb_wmi_color();
		break;
	case 0x7b:
		//TODO: Fn+Backspace
		break;
	case 0x95:
		//TODO: Fn+ESC
		break;
	case 0x9F:
		kb_wmi_toggle();
		break;
	case 0xF4:
		s76_input_airplane_wmi();
		break;
	case 0xFC:
		s76_input_touchpad_wmi(false);
		break;
	case 0xFD:
		s76_input_touchpad_wmi(true);
		break;
	default:
		S76_INFO("Unknown WMI event code (%x)\n", event);
		break;
	}
}

static int s76_probe(struct platform_device *dev) {
	int err;

	err = ap_led_init(&dev->dev);
	if (unlikely(err)) {
		S76_ERROR("Could not register LED device\n");
	}

	err = kb_led_init(&dev->dev);
	if (unlikely(err)) {
		S76_ERROR("Could not register LED device\n");
	}

	err = s76_input_init(&dev->dev);
	if (unlikely(err)) {
		S76_ERROR("Could not register input device\n");
	}

#ifdef S76_HAS_HWMON
	s76_hwmon_init(&dev->dev);
#endif

	err = nv_hda_init(&dev->dev);
	if (unlikely(err)) {
		S76_ERROR("Could not register NVIDIA audio device\n");
	}

	err = wmi_install_notify_handler(S76_EVENT_GUID, s76_wmi_notify, NULL);
	if (unlikely(ACPI_FAILURE(err))) {
		S76_ERROR("Could not register WMI notify handler (%0#6x)\n", err);
		return -EIO;
	}

	// Enable hotkey support
	s76_wmbb(0x46, 0, NULL);

	// Enable touchpad lock
	i8042_lock_chip();
	i8042_command(NULL, 0x97);
	i8042_unlock_chip();

	return 0;
}

static int s76_remove(struct platform_device *dev) {
	wmi_remove_notify_handler(S76_EVENT_GUID);

	nv_hda_exit();
	#ifdef S76_HAS_HWMON
		s76_hwmon_fini(&dev->dev);
	#endif
	s76_input_exit();
	kb_led_exit();
	ap_led_exit();

	return 0;
}

static int s76_suspend(struct platform_device *dev, pm_message_t status) {
	S76_INFO("s76_suspend\n");

	kb_led_suspend();

	return 0;
}

static int s76_resume(struct platform_device *dev) {
	S76_INFO("s76_resume\n");

	msleep(2000);

	// Enable hotkey support
	s76_wmbb(0x46, 0, NULL);

	ap_led_resume();
	kb_led_resume();

	return 0;
}

static struct platform_driver s76_platform_driver = {
	.remove = s76_remove,
	.suspend = s76_suspend,
	.resume = s76_resume,
	.driver = {
		.name  = S76_DRIVER_NAME,
		.owner = THIS_MODULE,
	},
};

static int __init s76_dmi_matched(const struct dmi_system_id *id) {
	S76_INFO("Model %s found\n", id->ident);

	return 1;
}

#define DMI_TABLE(PRODUCT) { \
	.ident = "System76 " PRODUCT, \
	.matches = { \
		DMI_MATCH(DMI_SYS_VENDOR, "System76"), \
		DMI_MATCH(DMI_PRODUCT_VERSION, PRODUCT), \
	}, \
	.callback = s76_dmi_matched, \
	.driver_data = NULL, \
}

static struct dmi_system_id s76_dmi_table[] __initdata = {
	DMI_TABLE("galp3-b"),
	DMI_TABLE("gaze13"),
	DMI_TABLE("kudu5"),
	DMI_TABLE("oryp3-jeremy"),
	DMI_TABLE("oryp4"),
	DMI_TABLE("oryp4-b"),
	{}
};

MODULE_DEVICE_TABLE(dmi, s76_dmi_table);

static int __init s76_init(void) {
	dmi_check_system(s76_dmi_table);

	if (!wmi_has_guid(S76_EVENT_GUID)) {
		S76_INFO("No known WMI event notification GUID found\n");
		return -ENODEV;
	}

	if (!wmi_has_guid(S76_WMBB_GUID)) {
		S76_INFO("No known WMI control method GUID found\n");
		return -ENODEV;
	}

	s76_platform_device =
		platform_create_bundle(&s76_platform_driver, s76_probe, NULL, 0, NULL, 0);

	if (unlikely(IS_ERR(s76_platform_device))) {
		return PTR_ERR(s76_platform_device);
	}

	return 0;
}

static void __exit s76_exit(void) {
	platform_device_unregister(s76_platform_device);
	platform_driver_unregister(&s76_platform_driver);
}

module_init(s76_init);
module_exit(s76_exit);

MODULE_AUTHOR("Jeremy Soller <jeremy@system76.com>");
MODULE_DESCRIPTION("System76 laptop driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.0.3");
