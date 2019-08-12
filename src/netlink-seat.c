/*
 * Copyright © 2013 Intel Corporation
 * Copyright © 2013-2015 Red Hat, Inc.
 * Copyright © 2017 Michael Forney
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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>

#include <sys/socket.h>
#include <linux/netlink.h>

#include "evdev.h"
#include "netlink-seat.h"

#define INPUT_MAJOR 13

static const char default_seat[] = "seat0";
static const char default_seat_name[] = "default";

static struct netlink_seat *
netlink_seat_create(struct netlink_input *input,
		    const char *device_seat,
		    const char *seat_name);
static struct netlink_seat *
netlink_seat_get_named(struct netlink_input *input, const char *seat_name);

static int
device_added(struct netlink_input *input,
	     const char *devnode)
{
	struct evdev_device *device;
	const char *device_seat, *seat_name, *sysname;
	struct netlink_seat *seat;

	device_seat = default_seat;
	seat_name = default_seat_name;
	seat = netlink_seat_get_named(input, seat_name);

	if (seat)
		libinput_seat_ref(&seat->base);
	else {
		seat = netlink_seat_create(input, device_seat, seat_name);
		if (!seat)
			return -1;
	}

	sysname = strrchr(devnode, '/');
	if (sysname)
		++sysname;
	else
		sysname = "";

	device = evdev_device_create(&seat->base, NULL, devnode, sysname);
	libinput_seat_unref(&seat->base);

	if (device == EVDEV_UNHANDLED_DEVICE) {
		log_info(&input->base,
			 "%-7s - not using input device '%s'\n",
			 sysname,
			 devnode);
		return 0;
	} else if (device == NULL) {
		log_info(&input->base,
			 "%-7s - failed to create input device '%s'\n",
			 sysname,
			 devnode);
		return 0;
	}

	evdev_read_calibration_prop(device);

	return 0;
}

static void
device_removed(struct netlink_input *input, const char *devnode)
{
	struct evdev_device *device, *next;
	struct netlink_seat *seat;

	list_for_each(seat, &input->base.seat_list, base.link) {
		list_for_each_safe(device, next,
				   &seat->base.devices_list, base.link) {
			if (streq(devnode, device->devnode)) {
				evdev_device_remove(device);
				break;
			}
		}
	}
}

static int
select_device(const struct dirent *entry)
{
	const char *p;

	if (strncmp(entry->d_name, "event", 5) != 0)
		return 0;
	for (p = entry->d_name + 5; '0' <= *p && *p <= '9'; ++p)
		;
	return *p == '\0';
}

static int
netlink_input_add_devices(struct netlink_input *input)
{
	struct dirent **devices;
	char path[PATH_MAX];
	int i, n, len;

	n = scandir("/dev/input", &devices, &select_device, &alphasort);
	if (n == -1)
		return -1;
	for (i = 0; i < n; ++i) {
		len = snprintf(path, sizeof(path), "/dev/input/%s", devices[i]->d_name);
		free(devices[i]);
		if (len < 0 || len >= sizeof(path)) {
			free(devices);
			return -1;
		}
		device_added(input, path);
	}
	free(devices);

	return 0;
}

static void
netlink_input_remove_devices(struct netlink_input *input)
{
	struct evdev_device *device, *next;
	struct netlink_seat *seat, *tmp;

	list_for_each_safe(seat, tmp, &input->base.seat_list, base.link) {
		libinput_seat_ref(&seat->base);
		list_for_each_safe(device, next,
				   &seat->base.devices_list, base.link) {
			evdev_device_remove(device);
		}
		libinput_seat_unref(&seat->base);
	}
}

static void
netlink_input_disable(struct libinput *libinput)
{
	struct netlink_input *input = (struct netlink_input*)libinput;

	if (input->sock == -1)
		return;

	close(input->sock);
	input->sock = -1;
	libinput_remove_source(&input->base, input->source);
	input->source = NULL;

	netlink_input_remove_devices(input);
}

static void
netlink_handler(void *data)
{
	struct netlink_input *input = data;
	char buf[BUFSIZ], *key, *val;
	ssize_t n;
	size_t len;
	char *action = NULL, *devname = NULL, *devnode, *sysname;

	n = read(input->sock, buf, sizeof(buf));
	if (n <= 0)
		return;
	for (key = buf; key < buf + n; key += len + 1) {
		len = strlen(key);
		val = strchr(key, '=');
		if (!val)
			continue;
		*val++ = '\0';
		if (strcmp(key, "ACTION") == 0) {
			action = val;
		} else if (strcmp(key, "SUBSYSTEM") == 0) {
			if (strcmp(val, "input") != 0)
				return;
		} else if (strcmp(key, "DEVNAME") == 0) {
			devname = val;
		}
	}
	if (!action || !devname)
		return;
	sysname = strrchr(devname, '/');
	if (sysname)
		++sysname;
	else
		sysname = devname;
	if (strncmp(sysname, "event", 5) != 0)
		return;
	devnode = devname - 5;
	memcpy(devnode, "/dev/", 5);
	if (strcmp(action, "add") == 0)
		device_added(input, devnode);
	else if (strcmp(action, "remove") == 0)
		device_removed(input, devnode);
}

static int
netlink_input_enable(struct libinput *libinput)
{
	int s;
	struct sockaddr_nl addr = {
		.nl_family = AF_NETLINK,
		.nl_groups = 1,
	};
	struct netlink_input *input = (struct netlink_input*)libinput;

	if (input->sock != -1)
		return 0;
	s = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
	if (s == -1)
		return -1;
	if (bind(s, (void *)&addr, sizeof(addr)) < 0) {
		close(s);
		return -1;
	}
	input->source = libinput_add_fd(&input->base, s, netlink_handler, input);
	if (!input->source) {
		close(s);
		return -1;
	}
	input->sock = s;
	if (netlink_input_add_devices(input) < 0) {
		netlink_input_disable(libinput);
		return -1;
	}

	return 0;
}

static void
netlink_input_destroy(struct libinput *input)
{
}

static void
netlink_seat_destroy(struct libinput_seat *seat)
{
	struct netlink_seat *nseat = (struct netlink_seat*)seat;
	free(nseat);
}

static struct netlink_seat *
netlink_seat_create(struct netlink_input *input,
		    const char *device_seat,
		    const char *seat_name)
{
	struct netlink_seat *seat;

	seat = zalloc(sizeof(*seat));

	libinput_seat_init(&seat->base, &input->base,
			   device_seat, seat_name,
			   netlink_seat_destroy);

	return seat;
}

static struct netlink_seat *
netlink_seat_get_named(struct netlink_input *input, const char *seat_name)
{
	struct netlink_seat *seat;

	list_for_each(seat, &input->base.seat_list, base.link) {
		if (streq(seat->base.logical_name, seat_name))
			return seat;
	}

	return NULL;
}

static int
netlink_device_change_seat(struct libinput_device *device,
			   const char *seat_name)
{
	struct libinput *libinput = device->seat->libinput;
	struct netlink_input *input = (struct netlink_input *)libinput;
	struct evdev_device *evdev = evdev_device(device);
	char *devnode;
	int rc;

	devnode = safe_strdup(evdev->devnode);
	device_removed(input, devnode);
	rc = device_added(input, devnode);
	free(devnode);

	return rc;
}

static const struct libinput_interface_backend interface_backend = {
	.resume = netlink_input_enable,
	.suspend = netlink_input_disable,
	.destroy = netlink_input_destroy,
	.device_change_seat = netlink_device_change_seat,
};

LIBINPUT_EXPORT struct libinput *
libinput_netlink_create_context(const struct libinput_interface *interface,
				void *user_data)
{
	struct netlink_input *input;

	if (!interface)
		return NULL;

	input = zalloc(sizeof(*input));
	input->sock = -1;

	if (libinput_init(&input->base, interface,
			  &interface_backend, user_data) != 0) {
		libinput_unref(&input->base);
		free(input);
		return NULL;
	}

	return &input->base;
}

LIBINPUT_EXPORT int
libinput_netlink_assign_seat(struct libinput *libinput,
			     const char *seat_id)
{
	struct netlink_input *input = (struct netlink_input*)libinput;

	if (libinput->interface_backend != &interface_backend) {
		log_bug_client(libinput, "Mismatching backends.\n");
		return -1;
	}

	/* We cannot do this during netlink_create_context because the log
	 * handler isn't set up there but we really want to log to the right
	 * place if the quirks run into parser errors. So we have to do it
	 * here since we can expect the log handler to be set up by now.
	 */
	libinput_init_quirks(libinput);

	if (netlink_input_enable(&input->base) < 0)
		return -1;

	return 0;
}
