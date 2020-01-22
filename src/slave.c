/*
 * This file is part of the KNOT Project
 *
 * Copyright (c) 2019, CESAR. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <ell/ell.h>

#include <modbus.h>
#include <string.h>

#include "dbus.h"
#include "storage.h"
#include "source.h"
#include "driver.h"
#include "slave.h"

struct slave {
	int refs;
	char *key;	/* Local random id */
	uint8_t id;	/* modbus slave id */
	char *name;	/* Local (user friendly) name */
	char *path;	/* D-Bus Object path */
	/*
	 * Hostname or serial port address. Serial may include settings.
	 * Format: "tcp://hostname:port", "serial://dev/ttyUSBx" or
	 * "serial://dev/ttyUSBx:115200,'N',8,1"
	 */
	char *url;
	modbus_t *modbus;
	struct l_io *io; /* TCP IO channel */
	struct l_queue *source_list;	/* Child sources */
	struct l_hashmap *to_list;	/* Reading/Polling timeout */
	int src_storage;		/* Source storage id */
	struct l_timeout *poll_to;	/* Connection attempt timeout */
	struct modbus_driver *drv;	/* TCP or Serial */
};

struct bond {
	struct slave *slave;
	struct source *source;
};

extern struct modbus_driver tcp;
extern struct modbus_driver rtu;

static int slaves_storage;
static int units_storage;

static bool path_cmp(const void *a, const void *b)
{
	const struct source *source = a;
	const char *b1 = b;

	return (strcmp(source_get_path(source), b1) == 0 ? true : false);
}

static bool address_cmp(const void *a, const void *b)
{
	const struct source *source = a;
	uint16_t address = L_PTR_TO_INT(b);

	return (source_get_address(source) == address ? true : false);
}

static void timeout_destroy(void *data)
{
	struct l_timeout *timeout = data;

	l_timeout_remove(timeout);
}

static void entry_destroy(void *user_data)
{
	struct source *source = user_data;

	/* Don't remove from storage */
	source_destroy(source, false);
}

static void slave_free(struct slave *slave)
{
	l_queue_destroy(slave->source_list, entry_destroy);
	l_hashmap_destroy(slave->to_list, timeout_destroy);

	if (slave->io)
		l_io_destroy(slave->io);
	if (slave->modbus)
		slave->drv->destroy(slave->modbus);

	if (slave->poll_to)
		l_timeout_remove(slave->poll_to);

	storage_close(slave->src_storage);
	l_free(slave->key);
	l_free(slave->url);
	l_free(slave->name);
	l_free(slave->path);
	l_info("slave_free(%p)", slave);
	l_free(slave);
}

static struct slave *slave_ref(struct slave *slave)
{
	if (unlikely(!slave))
		return NULL;

	__sync_fetch_and_add(&slave->refs, 1);
	l_info("slave_ref(%p): %d", slave, slave->refs);

	return slave;
}

static void slave_unref(struct slave *slave)
{
	if (unlikely(!slave))
		return;

	l_info("slave_unref(%p): %d", slave, slave->refs - 1);
	if (__sync_sub_and_fetch(&slave->refs, 1))
		return;

	slave_free(slave);
}

static void create_slave_from_storage(const char *key,
				int slave_id,
				const char *name,
				const char *url,
				void *user_data)
{
	struct l_queue *list = user_data;
	struct slave *slave;

	slave = slave_create(key, slave_id, name, url);
	if (!slave)
		return;

	l_queue_push_head(list, slave);
}

static void create_source_from_storage(const char *address,
				const char *name,
				const char *type,
				const char *unit,
				int interval,
				void *user_data)
{
	struct slave *slave = user_data;
	struct source *source;
	unsigned int uaddr;

	if (sscanf(address, "0x%04x", &uaddr) != 1)
		return;

	source = source_create(slave->path, name, type, unit, uaddr,
			       interval, slave->src_storage, false);
	if (!source)
		return;

	l_queue_push_head(slave->source_list, source);
}

static void destroy_handler(void *user_data)
{
	struct slave *slave = user_data;

	/* Try to connect again after 5 seconds: call enable_slave */
	l_timeout_modify(slave->poll_to, 5);
}

static void disconnected_cb(struct l_io *io, void *user_data)
{
	struct slave *slave = user_data;
	struct modbus_driver *driver = slave->drv;

	l_info("slave %p disconnected", slave);

	l_hashmap_destroy(slave->to_list, timeout_destroy);
	slave->to_list = l_hashmap_string_new();

	driver->destroy(slave->modbus);
	slave->modbus = NULL;

	l_io_destroy(slave->io);
	slave->io = NULL;

	l_dbus_property_changed(dbus_get_bus(), slave->path,
				SLAVE_IFACE, "Online");
}

static void polling_to_expired(struct l_timeout *timeout, void *user_data)
{
	struct bond *bond = user_data;
	struct source *source = bond->source;
	struct slave *slave = bond->slave;
	struct modbus_driver *driver = slave->drv;
	const char *sig = source_get_signature(source);
	uint16_t u16_addr = source_get_address(source);
	bool val_bool;
	uint8_t val_u8 = 0;
	uint16_t val_u16 = 0;
	uint32_t val_u32 = 0;
	uint64_t val_u64 = 0;
	int ret = 0, err;

	l_info("modbus reading source %p addr:(0x%x)", source, u16_addr);

	switch (sig[0]) {
	case 'b':
		ret = driver->read_bool(slave->modbus, u16_addr, &val_bool);
		if (ret != -1)
			source_set_value_bool(source, val_bool);
		break;
	case 'y':
		ret = driver->read_byte(slave->modbus, u16_addr, &val_u8);
		if (ret != -1)
			source_set_value_byte(source, val_u8);

		break;
	case 'q':
		ret = driver->read_u16(slave->modbus, u16_addr, &val_u16);
		if (ret != -1)
			source_set_value_u16(source, val_u16);
		break;
	case 'u':
		/* Assuming network order */
		ret = driver->read_u32(slave->modbus, u16_addr, &val_u32);
		if (ret != -1)
			source_set_value_u32(source, L_BE32_TO_CPU(val_u32));
		break;
	case 't':
		/* Assuming network order */
		ret = driver->read_u64(slave->modbus, u16_addr, &val_u64);
		if (ret != -1)
			source_set_value_u64(source, L_BE64_TO_CPU(val_u64));
		break;
	default:
		break;
	}

	if (ret == -1) {
		err = errno;
		l_error("read(%x): %s(%d)", u16_addr, strerror(err), err);
	}

	l_timeout_modify_ms(timeout, source_get_interval(source));
}

static void polling_start(void *data, void *user_data)
{
	struct slave *slave = user_data;
	struct source *source = data;
	struct l_timeout *timeout;
	struct bond *bond;

	/* timeout exists? */
	timeout = l_hashmap_lookup(slave->to_list, source_get_path(source));
	if (timeout) {
		l_timeout_modify_ms(timeout, source_get_interval(source));
		return;
	}

	/* New timeout */
	bond = l_new(struct bond, 1);
	bond->source = source;
	bond->slave = slave;

	timeout = l_timeout_create_ms(source_get_interval(source),
				      polling_to_expired, bond, l_free);

	l_hashmap_insert(slave->to_list, source_get_path(source), timeout);
}

static void enable_slave(struct l_timeout *timeout, void *user_data)
{
	struct slave *slave = user_data;
	struct modbus_driver *driver = slave->drv;
	int err;

	/* Already connected ? */
	if (slave->modbus)
		return;

	slave->modbus = driver->create(slave->url);
	if (!slave->modbus) {
		/* FIXME: URL may be invalid. How to handle this scenario? */
		l_error("Can not create modbus slave: %s", slave->url);
		goto retry;
	}

	if (modbus_set_slave(slave->modbus, slave->id) < 0) {
		l_error("Can not set slave id: %d (url: %s)",
			slave->id, slave->url);
		goto error;
	}

	if (modbus_connect(slave->modbus) != -1) {
		slave->io = l_io_new(modbus_get_socket(slave->modbus));
		if (slave->io == NULL)
			goto error;

		l_io_set_disconnect_handler(slave->io, disconnected_cb,
					    slave, destroy_handler);

		l_queue_foreach(slave->source_list,
				polling_start, slave);

		l_dbus_property_changed(dbus_get_bus(), slave->path,
					SLAVE_IFACE, "Online");

		return;
	}

error:
	/* Releasing connection */
	err = errno;

	l_info("connect(%p): %s(%d)", slave->modbus, strerror(err), err);

	driver->destroy(slave->modbus);

	slave->modbus = NULL;

retry:
	/* Try again in 5 seconds */
	l_timeout_modify(timeout, 5);
}

static struct l_dbus_message *method_source_add(struct l_dbus *dbus,
						struct l_dbus_message *msg,
						void *user_data)
{
	struct slave *slave = user_data;
	struct source *source;
	struct l_dbus_message *reply;
	struct l_dbus_message_builder *builder;
	struct l_dbus_message_iter dict;
	struct l_dbus_message_iter value;
	char signature[2];
	const char *key = NULL;
	const char *name = NULL;
	const char *type = NULL;
	const char *unit = NULL;
	char *unithex;
	uint16_t address = 0xffff;
	uint16_t interval = 1000; /* ms */
	bool ret;

	if (!l_dbus_message_get_arguments(msg, "a{sv}", &dict))
		return dbus_error_invalid_args(msg);

	while (l_dbus_message_iter_next_entry(&dict, &key, &value)) {
		/* Local assigned name: alias */
		if (strcmp(key, "Name") == 0)
			ret = l_dbus_message_iter_get_variant(&value,
							      "s", &name);
		/* Signature based on D-Bus data types */
		else if (strcmp(key, "Type") == 0)
			ret = l_dbus_message_iter_get_variant(&value,
							      "s", &type);
		/* Unit (symbol) based on IEEE 260.1 */
		else if (strcmp(key, "Unit") == 0)
			ret = l_dbus_message_iter_get_variant(&value,
							      "s", &unit);
		/* Memory address */
		else if (strcmp(key, "Address") == 0)
			ret = l_dbus_message_iter_get_variant(&value,
							      "q", &address);
		else if (strcmp(key, "PollingInterval") == 0)
			ret = l_dbus_message_iter_get_variant(&value,
							      "q", &interval);
		else
			return dbus_error_invalid_args(msg);

		if (!ret)
			return dbus_error_invalid_args(msg);
	}

	if (!name || address == 0xffff  || !type || !unit || strlen(type) != 1)
		return dbus_error_invalid_args(msg);

	unithex = l_util_hexstring_upper((const unsigned char *) unit,
					 strlen(unit));
	ret = storage_has_unit(units_storage, "SI", unithex);
	l_free(unithex);
	if (!ret)
		return dbus_error_invalid_args(msg);

	/* Restricted to basic D-Bus types: bool, byte, u16, u32, u64 */
	memset(signature, 0, sizeof(signature));
	if (sscanf(type, "%[byqut]1s", signature) != 1) {
		l_info("Limited to basic types only!");
		return dbus_error_invalid_args(msg);
	}

	source = l_queue_find(slave->source_list,
			      address_cmp, L_INT_TO_PTR(address));
	if (source) {
		l_error("source: address assigned already");
		return dbus_error_invalid_args(msg);
	}

	source = source_create(slave->path, name, type, unit, address, interval,
			       slave->src_storage, true);
	if (!source)
		return dbus_error_invalid_args(msg);

	/* Add object path to reply message */
	reply = l_dbus_message_new_method_return(msg);
	builder = l_dbus_message_builder_new(reply);
	l_dbus_message_builder_append_basic(builder, 'o',
					    source_get_path(source));
	l_dbus_message_builder_finalize(builder);
	l_dbus_message_builder_destroy(builder);

	l_queue_push_head(slave->source_list, source);

	if (slave->io)
		polling_start(source, slave);

	return reply;
}

static struct l_dbus_message *method_source_remove(struct l_dbus *dbus,
						struct l_dbus_message *msg,
						void *user_data)
{
	struct slave *slave = user_data;
	struct source *source;
	const char *opath;

	if (!l_dbus_message_get_arguments(msg, "o", &opath))
		return dbus_error_invalid_args(msg);

	source = l_queue_remove_if(slave->source_list, path_cmp, opath);
	if (unlikely(!source))
		return dbus_error_invalid_args(msg);

	/* Remove from storage */
	source_destroy(source, true);

	return l_dbus_message_new_method_return(msg);
}

static bool property_get_id(struct l_dbus *dbus,
				  struct l_dbus_message *msg,
				  struct l_dbus_message_builder *builder,
				  void *user_data)
{
	struct slave *slave = user_data;

	/* modbus specific id */
	l_dbus_message_builder_append_basic(builder, 'y', &slave->id);

	return true;
}

static bool property_get_name(struct l_dbus *dbus,
				  struct l_dbus_message *msg,
				  struct l_dbus_message_builder *builder,
				  void *user_data)
{
	struct slave *slave = user_data;

	/* Local name */
	l_dbus_message_builder_append_basic(builder, 's', slave->name);

	return true;
}

static struct l_dbus_message *property_set_name(struct l_dbus *dbus,
					 struct l_dbus_message *msg,
					 struct l_dbus_message_iter *new_value,
					 l_dbus_property_complete_cb_t complete,
					 void *user_data)
{
	struct slave *slave = user_data;
	const char *name;

	/* Local name */
	if (!l_dbus_message_iter_get_variant(new_value, "s", &name))
		return dbus_error_invalid_args(msg);

	l_free(slave->name);
	slave->name = l_strdup(name);

	complete(dbus, msg, NULL);

	storage_write_key_string(slaves_storage, slave->key, "Name", name);

	return NULL;
}

static bool property_get_url(struct l_dbus *dbus,
				  struct l_dbus_message *msg,
				  struct l_dbus_message_builder *builder,
				  void *user_data)
{
	struct slave *slave = user_data;

	/* PLC/Peer IP address or Serial port */
	l_dbus_message_builder_append_basic(builder, 's', slave->url);

	return true;
}

static bool property_get_online(struct l_dbus *dbus,
				  struct l_dbus_message *msg,
				  struct l_dbus_message_builder *builder,
				  void *user_data)
{
	struct slave *slave = user_data;
	bool online;

	online = (slave->modbus ? true : false);

	l_dbus_message_builder_append_basic(builder, 'b', &online);

	return true;
}

static void setup_interface(struct l_dbus_interface *interface)
{

	/* Add/Remove sources (a.k.a variables)  */
	l_dbus_interface_method(interface, "AddSource", 0,
				method_source_add,
				"o", "a{sv}", "path", "dict");

	l_dbus_interface_method(interface, "RemoveSource", 0,
				method_source_remove, "", "o", "path");

	if (!l_dbus_interface_property(interface, "Id", 0, "y",
				       property_get_id,
				       NULL))
		l_error("Can't add 'Id' property");

	/* Local name to identify slaves */
	if (!l_dbus_interface_property(interface, "Name", 0, "s",
				       property_get_name,
				       property_set_name))
		l_error("Can't add 'Name' property");

	/* Per/PLC IP url including port. Format: 'hostname:port' */
	if (!l_dbus_interface_property(interface, "URL", 0, "s",
				       property_get_url,
				       NULL))
		l_error("Can't add 'URL' property");

	/* Online: connected to slave */
	if (!l_dbus_interface_property(interface, "Online", 0, "b",
				       property_get_online,
				       NULL))
		l_error("Can't add 'Online' property");

}

struct slave *slave_create(const char *key, uint8_t id,
			   const char *name, const char *url)
{
	struct slave *slave;
	struct modbus_driver *drv;
	struct stat st;
	char *dpath;
	char *filename;
	int st_ret;

	/* "tcp://host:port or serial://dev/ttyUSB0, ... "*/

	if (!url)
		return NULL;

	/* URL Syntax checking happens when attempting to connect, later */

	if (strcmp("tcp://", url) < 0)
		drv = &tcp;
	else if (strcmp("serial://", url) < 0) {
		drv = &rtu;
	} else {
		l_info("Invalid url!");
		return NULL;
	}

	dpath = l_strdup_printf("/slave_%s", key);

	slave = l_new(struct slave, 1);
	slave->refs = 0;
	slave->key = l_strdup(key);
	slave->id = id;
	slave->name = l_strdup(name);
	slave->url = l_strdup(url);
	slave->modbus = NULL;
	slave->io = NULL;
	slave->source_list = l_queue_new();
	slave->to_list = l_hashmap_string_new();
	slave->drv = drv;

	filename = l_strdup_printf("%s/%s/sources.conf",
				   STORAGEDIR, slave->key);

	memset(&st, 0, sizeof(st));
	st_ret = stat(filename, &st);

	slave->src_storage = storage_open(filename);
	l_free(filename);

	if (!l_dbus_register_object(dbus_get_bus(),
				    dpath,
				    slave_ref(slave),
				    (l_dbus_destroy_func_t) slave_unref,
				    SLAVE_IFACE, slave,
				    L_DBUS_INTERFACE_PROPERTIES,
				    slave,
				    NULL)) {
		l_error("Can not register: %s", dpath);
		l_free(dpath);
		return NULL;
	}

	slave->path = dpath;

	l_info("Slave(%p): (%s) url: (%s)", slave, dpath, url);

	if (st_ret == 0) {
		/* Slave created from storage */
		storage_foreach_source(slave->src_storage,
				       create_source_from_storage, slave);
	} else {
		/* New slave */
		storage_write_key_int(slaves_storage, key, "Id", id);
		storage_write_key_string(slaves_storage, key,
					 "Name", name ? : url);
		storage_write_key_string(slaves_storage, key,
					 "URL", url); in
	}

	slave->poll_to = l_timeout_create(1, enable_slave, slave, NULL);

	return slave_ref(slave);
}

void slave_destroy(struct slave *slave, bool rm)
{
	char *filename;
	int err;

	l_info("slave_destroy(%p)", slave);

	if (unlikely(!slave))
		return;

	if (slave->io)
		l_io_set_disconnect_handler(slave->io, NULL, NULL, NULL);

	l_dbus_unregister_object(dbus_get_bus(), slave->path);

	if (!rm)
		goto done;

	/* Remove stored data: sources.conf */
	filename = l_strdup_printf("%s/%s/sources.conf",
				   STORAGEDIR, slave->key);
	if (unlink(filename) == -1) {
		err = errno;
		l_error("unlink(%s): %s(%d)", filename, strerror(err), err);
	}

	l_free(filename);

	/* Remove stored data: directory */
	filename = l_strdup_printf("%s/%s", STORAGEDIR, slave->key);
	if (rmdir(filename) == -1) {
		err = errno;
		l_error("unlink(%s): %s(%d)", filename, strerror(err), err);
	}

	l_free(filename);

	/* Remove group from slaves.conf */
	if (storage_remove_group(slaves_storage, slave->key) < 0)
		l_info("storage(): Can't delete slave!");

done:
	slave_unref(slave);
}

const char *slave_get_path(const struct slave *slave)
{
	if (unlikely(!slave))
		return NULL;

	return slave->path;
}

struct l_queue *slave_start(const char *units_filename)
{
	const char *filename = STORAGEDIR "/slaves.conf";
	struct l_queue *list;

	l_info("Starting slave ...");

	/* Slave settings file */
	slaves_storage = storage_open(filename);
	if (slaves_storage < 0) {
		l_error("Can not open/create slave files!");
		return NULL;
	}

	units_storage = storage_open(units_filename);
	if (units_storage < 0) {
		l_error("Can not open units file!");
		storage_close(slaves_storage);
		return NULL;
	}

	if (!l_dbus_register_interface(dbus_get_bus(),
				       SLAVE_IFACE,
				       setup_interface,
				       NULL, false))
		l_error("dbus: unable to register %s", SLAVE_IFACE);

	source_start();

	list = l_queue_new();
	storage_foreach_slave(slaves_storage, create_slave_from_storage, q);

	return list;
}

void slave_stop(void)
{

	storage_close(units_storage);
	storage_close(slaves_storage);

	source_stop();

	l_dbus_unregister_interface(dbus_get_bus(),
				    SLAVE_IFACE);
}
