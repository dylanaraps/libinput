/*
 * Copyright © 2013-2015 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

#include "path-seat.h"
#include "evdev.h"

static const char default_seat[] = "seat0";
static const char default_seat_name[] = "default";

static void
path_disable_device(struct libinput *libinput,
		    struct evdev_device *device)
{
	struct libinput_seat *seat = device->base.seat;
	struct evdev_device *dev, *next;

	list_for_each_safe(dev, next,
			   &seat->devices_list, base.link) {
		if (dev != device)
			continue;

		evdev_device_remove(device);
		break;
	}
}

static void
path_input_disable(struct libinput *libinput)
{
	struct path_input *input = (struct path_input*)libinput;
	struct path_seat *seat, *tmp;
	struct evdev_device *device, *next;

	list_for_each_safe(seat, tmp, &input->base.seat_list, base.link) {
		libinput_seat_ref(&seat->base);
		list_for_each_safe(device, next,
				   &seat->base.devices_list, base.link)
			path_disable_device(libinput, device);
		libinput_seat_unref(&seat->base);
	}
}

static void
path_seat_destroy(struct libinput_seat *seat)
{
	struct path_seat *pseat = (struct path_seat*)seat;
	free(pseat);
}

static struct path_seat*
path_seat_create(struct path_input *input,
		 const char *seat_name,
		 const char *seat_logical_name)
{
	struct path_seat *seat;

	seat = zalloc(sizeof(*seat));

	libinput_seat_init(&seat->base, &input->base, seat_name,
			   seat_logical_name, path_seat_destroy);

	return seat;
}

static struct path_seat*
path_seat_get_named(struct path_input *input,
		    const char *seat_name_physical,
		    const char *seat_name_logical)
{
	struct path_seat *seat;

	list_for_each(seat, &input->base.seat_list, base.link) {
		if (streq(seat->base.physical_name, seat_name_physical) &&
		    streq(seat->base.logical_name, seat_name_logical))
			return seat;
	}

	return NULL;
}

static struct libinput_device *
path_device_enable(struct path_input *input,
		   struct path_device *dev,
		   const char *seat_logical_name_override)
{
	struct path_seat *seat;
	struct evdev_device *device = NULL;
	char *seat_name = NULL, *seat_logical_name = NULL;
	const char *seat_prop;

	seat_prop = NULL;
	seat_name = safe_strdup(seat_prop ? seat_prop : default_seat);

	if (seat_logical_name_override) {
		seat_logical_name = safe_strdup(seat_logical_name_override);
	} else {
		seat_prop = NULL;
		seat_logical_name = strdup(seat_prop ? seat_prop : default_seat_name);
	}

	if (!seat_logical_name) {
		log_error(&input->base,
			  "%s: failed to create seat name for device '%s'.\n",
			  dev->sysname,
			  dev->devnode);
		goto out;
	}

	seat = path_seat_get_named(input, seat_name, seat_logical_name);

	if (seat) {
		libinput_seat_ref(&seat->base);
	} else {
		seat = path_seat_create(input, seat_name, seat_logical_name);
		if (!seat) {
			log_info(&input->base,
				 "%s: failed to create seat for device '%s'.\n",
				 dev->sysname,
				 dev->devnode);
			goto out;
		}
	}

	device = evdev_device_create(&seat->base, dev->devnode, dev->sysname);
	libinput_seat_unref(&seat->base);

	if (device == EVDEV_UNHANDLED_DEVICE) {
		device = NULL;
		log_info(&input->base,
			 "%-7s - not using input device '%s'.\n",
			 dev->sysname,
			 dev->devnode);
		goto out;
	} else if (device == NULL) {
		log_info(&input->base,
			 "%-7s - failed to create input device '%s'.\n",
			 dev->sysname,
			 dev->devnode);
		goto out;
	}

	evdev_read_calibration_prop(device);
	device->output_name = NULL;

out:
	free(seat_name);
	free(seat_logical_name);

	return device ? &device->base : NULL;
}

static int
path_input_enable(struct libinput *libinput)
{
	struct path_input *input = (struct path_input*)libinput;
	struct path_device *dev;

	list_for_each(dev, &input->path_list, link) {
		if (path_device_enable(input, dev, NULL) == NULL) {
			path_input_disable(libinput);
			return -1;
		}
	}

	return 0;
}

static void
path_input_destroy(struct libinput *input)
{
	struct path_input *path_input = (struct path_input*)input;
	struct path_device *dev, *tmp;

	list_for_each_safe(dev, tmp, &path_input->path_list, link) {
		free(dev->devnode);
		free(dev);
	}

}

static struct libinput_device *
path_create_device(struct libinput *libinput,
		   const char *devnode,
		   const char *seat_name)
{
	struct path_input *input = (struct path_input*)libinput;
	struct path_device *dev;
	struct libinput_device *device;

	dev = zalloc(sizeof *dev);
	dev->devnode = safe_strdup(devnode);
	dev->sysname = strrchr(devnode, '/');
	if (dev->sysname)
		++dev->sysname;
	else
		dev->sysname = "";

	list_insert(&input->path_list, &dev->link);

	device = path_device_enable(input, dev, seat_name);

	if (!device) {
		free(dev->devnode);
		list_remove(&dev->link);
		free(dev);
	}

	return device;
}

static int
path_device_change_seat(struct libinput_device *device,
			const char *seat_name)
{
	struct libinput *libinput = device->seat->libinput;
	struct evdev_device *evdev = evdev_device(device);
	char *devnode;

	devnode = strdup(evdev->devnode);
	if (!devnode)
		return -1;
	libinput_path_remove_device(device);

	device = path_create_device(libinput, devnode, seat_name);
	free(devnode);
	return device == NULL ? -1 : 0;
}

static const struct libinput_interface_backend interface_backend = {
	.resume = path_input_enable,
	.suspend = path_input_disable,
	.destroy = path_input_destroy,
	.device_change_seat = path_device_change_seat,
};

LIBINPUT_EXPORT struct libinput *
libinput_path_create_context(const struct libinput_interface *interface,
			     void *user_data)
{
	struct path_input *input;

	if (!interface)
		return NULL;

	input = zalloc(sizeof *input);
	if (libinput_init(&input->base, interface,
			  &interface_backend, user_data) != 0) {
		free(input);
		return NULL;
	}

	list_init(&input->path_list);

	return &input->base;
}

LIBINPUT_EXPORT struct libinput_device *
libinput_path_add_device(struct libinput *libinput,
			 const char *path)
{
	struct libinput_device *device;

	if (libinput->interface_backend != &interface_backend) {
		log_bug_client(libinput, "Mismatching backends.\n");
		return NULL;
	}

	device = path_create_device(libinput, path, NULL);
	return device;
}

LIBINPUT_EXPORT void
libinput_path_remove_device(struct libinput_device *device)
{
	struct libinput *libinput = device->seat->libinput;
	struct path_input *input = (struct path_input*)libinput;
	struct libinput_seat *seat;
	struct evdev_device *evdev = evdev_device(device);
	struct path_device *dev;

	if (libinput->interface_backend != &interface_backend) {
		log_bug_client(libinput, "Mismatching backends.\n");
		return;
	}

	list_for_each(dev, &input->path_list, link) {
		if (dev->devnode == evdev->devnode) {
			list_remove(&dev->link);
			free(dev->devnode);
			free(dev);
			break;
		}
	}

	seat = device->seat;
	libinput_seat_ref(seat);
	path_disable_device(libinput, evdev);
	libinput_seat_unref(seat);
}
