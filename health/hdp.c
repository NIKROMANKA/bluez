/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2010 GSyC/LibreSoft, Universidad Rey Juan Carlos.
 *  Authors:
 *  Santiago Carot Nemesio <sancane at gmail.com>
 *  Jose Antonio Santos-Cadenas <santoscadenas at gmail.com>
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

#include <gdbus.h>

#include "log.h"
#include "error.h"
#include <stdlib.h>
#include <stdint.h>
#include <hdp_types.h>
#include <hdp_util.h>
#include <adapter.h>
#include <device.h>
#include <hdp.h>
#include <mcap.h>
#include <btio.h>
#include <mcap_lib.h>
#include <l2cap.h>
#include <sdpd.h>
#include "../src/dbus-common.h"
#include <unistd.h>

#ifndef DBUS_TYPE_UNIX_FD
	#define DBUS_TYPE_UNIX_FD -1
#endif

#define ECHO_TIMEOUT	1 /* second */
#define HDP_ECHO_LEN	15

static DBusConnection *connection = NULL;

static GSList *applications = NULL;
static GSList *devices = NULL;
static uint8_t next_app_id = HDP_MDEP_INITIAL;

static GSList *adapters;

static gboolean update_adapter(struct hdp_adapter *adapter);
static struct hdp_device *create_health_device(DBusConnection *conn,
						struct btd_device *device);
static void free_echo_data(struct hdp_echo_data *edata);

struct hdp_create_dc {
	DBusConnection			*conn;
	DBusMessage			*msg;
	struct hdp_application		*app;
	struct hdp_device		*dev;
	uint8_t				config;
	uint8_t				mdep;
	guint				ref;
	mcap_mdl_operation_cb		cb;
};

struct hdp_tmp_dc_data {
	DBusConnection			*conn;
	DBusMessage			*msg;
	struct hdp_channel		*hdp_chann;
	guint				ref;
	mcap_mdl_operation_cb		cb;
};

struct hdp_echo_data {
	gboolean		echo_done;	/* Is a echo was already done */
	gpointer		buf;		/* echo packet sent */
	uint			tid;		/* echo timeout */
};

static struct hdp_channel *hdp_channel_ref(struct hdp_channel *chan)
{
	if (!chan)
		return NULL;

	chan->ref++;

	DBG("health_channel_ref(%p): ref=%d", chan, chan->ref);
	return chan;
}

static void free_health_channel(struct hdp_channel *chan)
{
	if (chan->mdep == HDP_MDEP_ECHO) {
		free_echo_data(chan->edata);
		chan->edata = NULL;
	}

	mcap_mdl_unref(chan->mdl);
	hdp_application_unref(chan->app);
	health_device_unref(chan->dev);
	g_free(chan->path);
	g_free(chan);
}

static void hdp_channel_unref(struct hdp_channel *chan)
{
	if (!chan)
		return;

	chan->ref --;
	DBG("health_channel_unref(%p): ref=%d", chan, chan->ref);

	if (chan->ref > 0)
		return;

	free_health_channel(chan);
}

static void free_hdp_create_dc(struct hdp_create_dc *dc_data)
{
	dbus_message_unref(dc_data->msg);
	dbus_connection_unref(dc_data->conn);
	hdp_application_unref(dc_data->app);
	health_device_unref(dc_data->dev);

	g_free(dc_data);
}

static struct hdp_create_dc *hdp_create_data_ref(struct hdp_create_dc *dc_data)
{
	dc_data->ref++;

	DBG("hdp_create_data_ref(%p): ref=%d", dc_data, dc_data->ref);

	return dc_data;
}

static void hdp_create_data_unref(struct hdp_create_dc *dc_data)
{
	dc_data->ref--;

	DBG("hdp_create_data_unref(%p): ref=%d", dc_data, dc_data->ref);

	if (dc_data->ref > 0)
		return;

	free_hdp_create_dc(dc_data);
}

static void free_hdp_conn_dc(struct hdp_tmp_dc_data *data)
{
	dbus_message_unref(data->msg);
	dbus_connection_unref(data->conn);
	hdp_channel_unref(data->hdp_chann);

	g_free(data);
}

static struct hdp_tmp_dc_data *hdp_tmp_dc_data_ref(struct hdp_tmp_dc_data *data)
{
	data->ref++;

	DBG("hdp_conn_data_ref(%p): ref=%d", data, data->ref);

	return data;
}

static void hdp_tmp_dc_data_unref(struct hdp_tmp_dc_data *data)
{
	data->ref--;

	DBG("hdp_conn_data_unref(%p): ref=%d", data, data->ref);

	if (data->ref > 0)
		return;

	free_hdp_conn_dc(data);
}

static int cmp_app_id(gconstpointer a, gconstpointer b)
{
	const struct hdp_application *app = a;
	const uint8_t *id = b;

	return app->id - *id;
}

static int cmp_adapter(gconstpointer a, gconstpointer b)
{
	const struct hdp_adapter *hdp_adapter = a;
	const struct btd_adapter *adapter = b;

	if (hdp_adapter->btd_adapter == adapter)
		return 0;

	return -1;
}

static int cmp_device(gconstpointer a, gconstpointer b)
{
	const struct hdp_device *hdp_device = a;
	const struct btd_device *device = b;

	if (hdp_device->dev == device)
		return 0;

	return -1;
}

static gint cmp_dev_addr(gconstpointer a, gconstpointer dst)
{
	const struct hdp_device *device = a;
	bdaddr_t addr;

	device_get_address(device->dev, &addr);
	return bacmp(&addr, dst);
}

static gint cmp_dev_mcl(gconstpointer a, gconstpointer mcl)
{
	const struct hdp_device *device = a;

	if (mcl == device->mcl)
		return 0;
	return -1;
}

static gint cmp_chan_mdlid(gconstpointer a, gconstpointer b)
{
	const struct hdp_channel *chan = a;
	const uint16_t *mdlid = b;

	return chan->mdlid - *mdlid;
}

static gint cmp_chan_path(gconstpointer a, gconstpointer b)
{
	const struct hdp_channel *chan = a;
	const char *path = b;

	return g_ascii_strcasecmp(chan->path, path);
}

static gint cmp_chan_mdl(gconstpointer a, gconstpointer mdl)
{
	const struct hdp_channel *chan = a;

	if (chan->mdl == mdl)
		return 0;
	return -1;
}

static uint8_t get_app_id()
{
	uint8_t id = next_app_id;

	do {
		GSList *l = g_slist_find_custom(applications, &id, cmp_app_id);

		if (!l) {
			next_app_id = (id % HDP_MDEP_FINAL) + 1;
			return id;
		} else
			id = (id % HDP_MDEP_FINAL) + 1;
	} while (id != next_app_id);

	/* No more ids available */
	return 0;
}

static int cmp_app(gconstpointer a, gconstpointer b)
{
	const struct hdp_application *app = a;

	return g_strcmp0(app->path, b);
}

static gboolean set_app_path(struct hdp_application *app)
{
	app->id = get_app_id();
	if (!app->id)
		return FALSE;
	app->path = g_strdup_printf(MANAGER_PATH "/health_app_%d", app->id);

	return TRUE;
};

static void device_unref_mcl(struct hdp_device *hdp_device)
{
	if (!hdp_device->mcl)
		return;

	mcap_close_mcl(hdp_device->mcl, FALSE);
	mcap_mcl_unref(hdp_device->mcl);
	hdp_device->mcl = NULL;
	hdp_device->mcl_conn = FALSE;
}

static void free_health_device(struct hdp_device *device)
{
	if (device->conn) {
		dbus_connection_unref(device->conn);
		device->conn = NULL;
	}

	if (device->dev) {
		btd_device_unref(device->dev);
		device->dev = NULL;
	}

	device_unref_mcl(device);

	g_free(device);
}

static void remove_application(struct hdp_application *app)
{
	DBG("Application %s deleted", app->path);
	hdp_application_unref(app);

	g_slist_foreach(adapters, (GFunc) update_adapter, NULL);
}

static void client_disconnected(DBusConnection *conn, void *user_data)
{
	struct hdp_application *app = user_data;

	DBG("Client disconnected from the bus, deleting hdp application");
	applications = g_slist_remove(applications, app);

	app->dbus_watcher = 0; /* Watcher shouldn't be freed in this case */
	remove_application(app);
}

static DBusMessage *manager_create_application(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	struct hdp_application *app;
	const char *name;
	DBusMessageIter iter;
	GError *err = NULL;

	dbus_message_iter_init(msg, &iter);
	app = hdp_get_app_config(&iter, &err);
	if (err) {
		g_error_free(err);
		return btd_error_invalid_args(msg);
	}

	name = dbus_message_get_sender(msg);
	if (!name) {
		hdp_application_unref(app);
		return g_dbus_create_error(msg,
					ERROR_INTERFACE ".HealthError",
					"Can't get sender name");
	}

	if (!set_app_path(app)) {
		hdp_application_unref(app);
		return g_dbus_create_error(msg,
				ERROR_INTERFACE ".HealthError",
				"Can't get a valid id for the application");
	}

	app->oname = g_strdup(name);
	app->conn = dbus_connection_ref(conn);

	applications = g_slist_prepend(applications, app);

	app->dbus_watcher = g_dbus_add_disconnect_watch(conn, name,
						client_disconnected, app, NULL);
	g_slist_foreach(adapters, (GFunc) update_adapter, NULL);

	DBG("Health application created with id %s", app->path);

	return g_dbus_create_reply(msg, DBUS_TYPE_OBJECT_PATH, &app->path,
							DBUS_TYPE_INVALID);
}

static DBusMessage *manager_destroy_application(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	const char *path;
	struct hdp_application *app;
	GSList *l;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
						DBUS_TYPE_INVALID))
		return btd_error_invalid_args(msg);

	l = g_slist_find_custom(applications, path, cmp_app);

	if (!l)
		return g_dbus_create_error(msg,
					ERROR_INTERFACE ".InvalidArguments",
					"Invalid arguments in method call, "
					"no such application");

	app = l->data;
	applications = g_slist_remove(applications, app);

	remove_application(app);

	return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
}

static void manager_path_unregister(gpointer data)
{
	g_slist_foreach(applications, (GFunc) hdp_application_unref, NULL);

	g_slist_free(applications);
	applications = NULL;

	g_slist_foreach(adapters, (GFunc) update_adapter, NULL);
}

static GDBusMethodTable health_manager_methods[] = {
	{"CreateApplication", "a{sv}", "o", manager_create_application},
	{"DestroyApplication", "o", "", manager_destroy_application},
	{ NULL }
};

static DBusMessage *channel_get_properties(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	struct hdp_channel *chan = user_data;
	DBusMessageIter iter, dict;
	DBusMessage *reply;
	const char *path;
	char *type;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
			DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
			DBUS_TYPE_STRING_AS_STRING DBUS_TYPE_VARIANT_AS_STRING
			DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &dict);

	path = device_get_path(chan->dev->dev);
	dict_append_entry(&dict, "Device", DBUS_TYPE_OBJECT_PATH, &path);

	path = chan->app->path;
	dict_append_entry(&dict, "Application", DBUS_TYPE_OBJECT_PATH, &path);

	if (chan->config == HDP_RELIABLE_DC)
		type = g_strdup("Reliable");
	else
		type = g_strdup("Streaming");

	dict_append_entry(&dict, "Type", DBUS_TYPE_STRING, &type);

	g_free(type);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static void hdp_tmp_dc_data_destroy(gpointer data)
{
	struct hdp_tmp_dc_data *hdp_conn = data;

	hdp_tmp_dc_data_unref(hdp_conn);
}

static void abort_mdl_cb(GError *err, gpointer data)
{
	if (err)
		error("Aborting error: %s", err->message);
}

static void hdp_mdl_reconn_cb(struct mcap_mdl *mdl, GError *err, gpointer data)
{
	struct hdp_tmp_dc_data *dc_data = data;
	DBusMessage *reply;
	int fd;

	if (err) {
		struct hdp_channel *chan = dc_data->hdp_chann;
		GError *gerr = NULL;

		error("%s", err->message);
		reply = g_dbus_create_error(dc_data->msg,
					ERROR_INTERFACE ".HealthError",
					"Cannot reconnect: %s", err->message);
		g_dbus_send_message(dc_data->conn, reply);

		/* Send abort request because remote side */
		/* is now in PENDING state */
		if (!mcap_mdl_abort(chan->mdl, abort_mdl_cb, NULL, NULL,
								&gerr)) {
			error("%s", gerr->message);
			g_error_free(gerr);
		}
		return;
	}

	fd = mcap_mdl_get_fd(dc_data->hdp_chann->mdl);
	if (fd < 0) {
		reply = g_dbus_create_error(dc_data->msg,
						ERROR_INTERFACE ".HealthError",
						"Cannot get file descriptor");
		g_dbus_send_message(dc_data->conn, reply);
		return;
	}

	reply = g_dbus_create_reply(dc_data->msg, DBUS_TYPE_UNIX_FD,
							&fd, DBUS_TYPE_INVALID);
	g_dbus_send_message(dc_data->conn, reply);

	g_dbus_emit_signal(dc_data->conn,
			device_get_path(dc_data->hdp_chann->dev->dev),
			HEALTH_DEVICE, "ChannelConnected",
			DBUS_TYPE_OBJECT_PATH, &dc_data->hdp_chann->path,
			DBUS_TYPE_INVALID);
}

static void hdp_get_dcpsm_cb(uint16_t dcpsm, gpointer user_data, GError *err)
{
	struct hdp_tmp_dc_data *hdp_conn = user_data;
	struct hdp_channel *hdp_chann = hdp_conn->hdp_chann;
	GError *gerr = NULL;
	uint8_t mode;

	if (err) {
		hdp_conn->cb(hdp_chann->mdl, err, hdp_conn);
		return;
	}

	if (hdp_chann->config == HDP_RELIABLE_DC)
		mode = L2CAP_MODE_ERTM;
	else
		mode = L2CAP_MODE_STREAMING;

	if (mcap_connect_mdl(hdp_chann->mdl, mode, dcpsm, hdp_conn->cb,
					hdp_tmp_dc_data_ref(hdp_conn),
					hdp_tmp_dc_data_destroy, &gerr))
		return;

	hdp_tmp_dc_data_unref(hdp_conn);
	hdp_conn->cb(hdp_chann->mdl, err, hdp_conn);
	g_error_free(gerr);
	gerr = NULL;
}

static void device_reconnect_mdl_cb(struct mcap_mdl *mdl, GError *err,
								gpointer data)
{
	struct hdp_tmp_dc_data *dc_data = data;
	GError *gerr = NULL;
	DBusMessage *reply;

	if (err) {
		reply = g_dbus_create_error(dc_data->msg,
					ERROR_INTERFACE ".HealthError",
					"Cannot reconnect: %s", err->message);
		g_dbus_send_message(dc_data->conn, reply);
		return;
	}

	dc_data->cb = hdp_mdl_reconn_cb;

	if (hdp_get_dcpsm(dc_data->hdp_chann->dev, hdp_get_dcpsm_cb,
					hdp_tmp_dc_data_ref(dc_data),
					hdp_tmp_dc_data_destroy, &gerr))
		return;

	error("%s", gerr->message);

	reply = g_dbus_create_error(dc_data->msg,
					ERROR_INTERFACE ".HealthError",
					"Cannot reconnect: %s", gerr->message);
	g_dbus_send_message(dc_data->conn, reply);
	hdp_tmp_dc_data_unref(dc_data);
	g_error_free(gerr);
	gerr = NULL;

	/* Send abort request because remote side is now in PENDING state */
	if (!mcap_mdl_abort(mdl, abort_mdl_cb, NULL, NULL, &gerr)) {
		error("%s", gerr->message);
		g_error_free(gerr);
	}
}

static DBusMessage *channel_acquire_continue(struct hdp_tmp_dc_data *data,
								GError *err)
{
	DBusMessage *reply;
	GError *gerr = NULL;
	int fd;

	if (err) {
		return g_dbus_create_error(data->msg,
						ERROR_INTERFACE ".HealthError",
						"%s", err->message);
	}

	fd = mcap_mdl_get_fd(data->hdp_chann->mdl);
	if (fd >= 0)
		return g_dbus_create_reply(data->msg, DBUS_TYPE_UNIX_FD, &fd,
							DBUS_TYPE_INVALID);

	hdp_tmp_dc_data_ref(data);
	if (mcap_reconnect_mdl(data->hdp_chann->mdl, device_reconnect_mdl_cb,
					data, hdp_tmp_dc_data_destroy, &gerr))
		return NULL;

	hdp_tmp_dc_data_unref(data);
	reply = g_dbus_create_error(data->msg, ERROR_INTERFACE ".HealthError",
					"Cannot reconnect: %s", gerr->message);
	g_error_free(gerr);

	return reply;
}

static void channel_acquire_cb(gpointer data, GError *err)
{
	struct hdp_tmp_dc_data *dc_data = data;
	DBusMessage *reply;

	reply = channel_acquire_continue(data, err);

	if (reply)
		g_dbus_send_message(dc_data->conn, reply);
}

static DBusMessage *channel_acquire(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	struct hdp_channel *chan = user_data;
	struct hdp_tmp_dc_data *dc_data;
	GError *gerr = NULL;
	DBusMessage *reply;

	dc_data = g_new0(struct hdp_tmp_dc_data, 1);
	dc_data->conn = dbus_connection_ref(conn);
	dc_data->msg = dbus_message_ref(msg);
	dc_data->hdp_chann = hdp_channel_ref(chan);

	if (chan->dev->mcl_conn) {
		reply = channel_acquire_continue(hdp_tmp_dc_data_ref(dc_data),
									NULL);
		hdp_tmp_dc_data_unref(dc_data);
		return reply;
	}

	if (hdp_establish_mcl(chan->dev, channel_acquire_cb,
						hdp_tmp_dc_data_ref(dc_data),
						hdp_tmp_dc_data_destroy, &gerr))
		return NULL;

	reply = g_dbus_create_error(msg, ERROR_INTERFACE ".HealthError",
					"%s", gerr->message);
	hdp_tmp_dc_data_unref(dc_data);
	g_error_free(gerr);

	return reply;
}

static void close_mdl(struct hdp_channel *hdp_chann)
{
	int fd;

	fd = mcap_mdl_get_fd(hdp_chann->mdl);
	if (fd < 0)
		return;

	close(fd);
}

static DBusMessage *channel_release(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	struct hdp_channel *hdp_chann = user_data;

	close_mdl(hdp_chann);

	return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
}

static void free_echo_data(struct hdp_echo_data *edata)
{
	if (!edata)
		return;

	if (edata->tid)
		g_source_remove(edata->tid);

	if (edata->buf)
		g_free(edata->buf);


	g_free(edata);
}

static void health_channel_destroy(void *data)
{
	struct hdp_channel *hdp_chan = data;
	struct hdp_device *dev = hdp_chan->dev;

	DBG("Destroy Health Channel %s", hdp_chan->path);
	if (!g_slist_find(dev->channels, hdp_chan))
		goto end;

	dev->channels = g_slist_remove(dev->channels, hdp_chan);

	if (hdp_chan->mdep != HDP_MDEP_ECHO)
		g_dbus_emit_signal(dev->conn, device_get_path(dev->dev),
					HEALTH_DEVICE, "ChannelDeleted",
					DBUS_TYPE_OBJECT_PATH, &hdp_chan->path,
					DBUS_TYPE_INVALID);

	if (hdp_chan == dev->fr) {
		char *empty_path;

		hdp_channel_unref(dev->fr);
		dev->fr = NULL;
		empty_path = "/";
		emit_property_changed(dev->conn, device_get_path(dev->dev),
					HEALTH_DEVICE, "MainChannel",
					DBUS_TYPE_OBJECT_PATH, &empty_path);
	}

end:
	hdp_channel_unref(hdp_chan);
}

static GDBusMethodTable health_channels_methods[] = {
	{"GetProperties","",	"a{sv}",	channel_get_properties },
	{"Acquire",	"",	"h",		channel_acquire,
						G_DBUS_METHOD_FLAG_ASYNC },
	{"Release",	"",	"",		channel_release },
	{ NULL }
};

static struct hdp_channel *create_channel(struct hdp_device *dev,
						uint8_t config,
						struct mcap_mdl *mdl,
						uint16_t mdlid,
						struct hdp_application *app,
						GError **err)
{
	struct hdp_channel *hdp_chann;

	if (!dev)
		return NULL;

	hdp_chann = g_new0(struct hdp_channel, 1);
	hdp_chann->config = config;
	hdp_chann->dev = health_device_ref(dev);
	hdp_chann->mdlid = mdlid;

	if (mdl)
		hdp_chann->mdl = mcap_mdl_ref(mdl);

	if (app) {
		hdp_chann->mdep = app->id;
		hdp_chann->app = hdp_application_ref(app);
	} else
		hdp_chann->edata = g_new0(struct hdp_echo_data, 1);

	hdp_chann->path = g_strdup_printf("%s/chan%d",
					device_get_path(hdp_chann->dev->dev),
					hdp_chann->mdlid);

	dev->channels = g_slist_append(dev->channels,
						hdp_channel_ref(hdp_chann));

	if (hdp_chann->mdep == HDP_MDEP_ECHO)
		return hdp_channel_ref(hdp_chann);

	if (!g_dbus_register_interface(dev->conn, hdp_chann->path,
					HEALTH_CHANNEL,
					health_channels_methods, NULL, NULL,
					hdp_chann, health_channel_destroy)) {
		g_set_error(err, HDP_ERROR, HDP_UNSPECIFIED_ERROR,
					"Can't register the channel interface");
		health_channel_destroy(hdp_chann);
		return NULL;
	}

	return hdp_channel_ref(hdp_chann);
}

static void remove_channels(struct hdp_device *dev)
{
	struct hdp_channel *chan;
	char *path;

	while (dev->channels) {
		chan = dev->channels->data;

		path = g_strdup(chan->path);
		if (!g_dbus_unregister_interface(dev->conn, path,
								HEALTH_CHANNEL))
			health_channel_destroy(chan);
		g_free(path);
	}
}

static void close_device_con(struct hdp_device *dev, gboolean cache)
{
	if (!dev->mcl)
		return;

	mcap_close_mcl(dev->mcl, cache);
	dev->mcl_conn = FALSE;

	if (cache)
		return;

	device_unref_mcl(dev);
	remove_channels(dev);

	if (!dev->sdp_present) {
		const char *path;

		path = device_get_path(dev->dev);
		g_dbus_unregister_interface(dev->conn, path, HEALTH_DEVICE);
	}
}

static int send_echo_data(int sock, const void *buf, uint32_t size)
{
	const uint8_t *buf_b = buf;
	uint32_t sent = 0;

	while (sent < size) {
		int n = write(sock, buf_b + sent, size - sent);
		if (n < 0)
			return -1;
		sent += n;
	}

	return 0;
}

static gboolean serve_echo(GIOChannel *io_chan, GIOCondition cond,
								gpointer data)
{
	struct hdp_channel *chan = data;
	uint8_t buf[MCAP_DC_MTU];
	int fd, len;

	if (cond & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
		hdp_channel_unref(chan);
		return FALSE;
	}

	if (chan->edata->echo_done)
		goto fail;

	chan->edata->echo_done = TRUE;

	fd = g_io_channel_unix_get_fd(io_chan);
	len = read(fd, buf, sizeof(buf));

	if (send_echo_data(fd, buf, len)  >= 0)
		return TRUE;

fail:
	close_device_con(chan->dev, FALSE);
	hdp_channel_unref(chan);
	return FALSE;
}

static gboolean check_channel_conf(struct hdp_channel *chan)
{
	GError *err = NULL;
	GIOChannel *io;
	uint8_t mode;
	uint16_t imtu, omtu;
	int fd;

	fd = mcap_mdl_get_fd(chan->mdl);
	if (fd < 0)
		return FALSE;
	io = g_io_channel_unix_new(fd);

	if (!bt_io_get(io, BT_IO_L2CAP, &err,
			BT_IO_OPT_MODE, &mode,
			BT_IO_OPT_IMTU, &imtu,
			BT_IO_OPT_OMTU, &omtu,
			BT_IO_OPT_INVALID)) {
		error("Error: %s", err->message);
		g_io_channel_unref(io);
		g_error_free(err);
		return FALSE;
	}

	g_io_channel_unref(io);

	switch (chan->config) {
	case HDP_RELIABLE_DC:
		if (mode != L2CAP_MODE_ERTM)
			return FALSE;
		break;
	case HDP_STREAMING_DC:
		if (mode != L2CAP_MODE_STREAMING)
			return FALSE;
		break;
	default:
		error("Error: Connected with unknown configuration");
		return FALSE;
	}

	DBG("MDL imtu %d omtu %d Channel imtu %d omtu %d", imtu, omtu,
						chan->imtu, chan->omtu);

	if (!chan->imtu)
		chan->imtu = imtu;
	if (!chan->omtu)
		chan->omtu = omtu;

	if (chan->imtu != imtu || chan->omtu != omtu)
		return FALSE;

	return TRUE;
}

static void hdp_mcap_mdl_connected_cb(struct mcap_mdl *mdl, void *data)
{
	struct hdp_device *dev = data;
	struct hdp_channel *chan;

	DBG("hdp_mcap_mdl_connected_cb");
	if (!dev->ndc)
		return;

	chan = dev->ndc;
	if (!chan->mdl)
		chan->mdl = mcap_mdl_ref(mdl);

	if (!g_slist_find(dev->channels, chan))
		dev->channels = g_slist_prepend(dev->channels,
							hdp_channel_ref(chan));

	if (!check_channel_conf(chan)) {
		close_mdl(chan);
		goto end;
	}

	if (chan->mdep == HDP_MDEP_ECHO) {
		GIOChannel *io;
		int fd;

		fd = mcap_mdl_get_fd(chan->mdl);
		if (fd < 0)
			goto end;

		chan->edata->echo_done = FALSE;
		io = g_io_channel_unix_new(fd);
		g_io_add_watch(io, G_IO_ERR | G_IO_HUP | G_IO_NVAL | G_IO_IN,
				serve_echo, hdp_channel_ref(chan));
		g_io_channel_unref(io);
		goto end;
	}

	g_dbus_emit_signal(dev->conn, device_get_path(dev->dev), HEALTH_DEVICE,
					"ChannelConnected",
					DBUS_TYPE_OBJECT_PATH, &chan->path,
					DBUS_TYPE_INVALID);

	if (dev->fr)
		goto end;

	dev->fr = hdp_channel_ref(chan);

	emit_property_changed(dev->conn, device_get_path(dev->dev),
					HEALTH_DEVICE, "MainChannel",
					DBUS_TYPE_OBJECT_PATH, &dev->fr->path);

end:
	hdp_channel_unref(dev->ndc);
	dev->ndc = NULL;
}

static void hdp_mcap_mdl_closed_cb(struct mcap_mdl *mdl, void *data)
{
	/* struct hdp_device *dev = data; */

	DBG("hdp_mcap_mdl_closed_cb");

	/* Nothing to do */
}

static void hdp_mcap_mdl_deleted_cb(struct mcap_mdl *mdl, void *data)
{
	struct hdp_device *dev = data;
	struct hdp_channel *chan;
	char *path;
	GSList *l;

	DBG("hdp_mcap_mdl_deleted_cb");
	l = g_slist_find_custom(dev->channels, mdl, cmp_chan_mdl);
	if (!l)
		return;

	chan = l->data;

	path = g_strdup(chan->path);
	if (!g_dbus_unregister_interface(dev->conn, path, HEALTH_CHANNEL))
		health_channel_destroy(chan);
	g_free(path);
}

static void hdp_mcap_mdl_aborted_cb(struct mcap_mdl *mdl, void *data)
{
	struct hdp_device *dev = data;

	DBG("hdp_mcap_mdl_aborted_cb");
	if (!dev->ndc)
		return;

	dev->ndc->mdl = mcap_mdl_ref(mdl);

	if (!g_slist_find(dev->channels, dev->ndc))
		dev->channels = g_slist_prepend(dev->channels,
						hdp_channel_ref(dev->ndc));

	if (dev->ndc->mdep != HDP_MDEP_ECHO)
		g_dbus_emit_signal(dev->conn, device_get_path(dev->dev),
					HEALTH_DEVICE, "ChannelConnected",
					DBUS_TYPE_OBJECT_PATH, &dev->ndc->path,
					DBUS_TYPE_INVALID);

	hdp_channel_unref(dev->ndc);
	dev->ndc = NULL;
}

static uint8_t hdp2l2cap_mode(uint8_t hdp_mode)
{
	return hdp_mode == HDP_STREAMING_DC ? L2CAP_MODE_STREAMING :
								L2CAP_MODE_ERTM;
}

static uint8_t hdp_mcap_mdl_conn_req_cb(struct mcap_mcl *mcl, uint8_t mdepid,
				uint16_t mdlid, uint8_t *conf, void *data)
{
	struct hdp_device *dev = data;
	struct hdp_application *app;
	GError *err = NULL;
	GSList *l;

	DBG("Data channel request");

	if (mdepid == HDP_MDEP_ECHO) {
		switch (*conf) {
		case HDP_NO_PREFERENCE_DC:
			*conf = HDP_RELIABLE_DC;
		case HDP_RELIABLE_DC:
			break;
		case HDP_STREAMING_DC:
			return MCAP_CONFIGURATION_REJECTED;
		default:
			/* Special case defined in HDP spec 3.4. When an invalid
			* configuration is received we shall close the MCL when
			* we are still processing the callback. */
			close_device_con(dev, FALSE);
			return MCAP_CONFIGURATION_REJECTED; /* not processed */
		}

		if (!mcap_set_data_chan_mode(dev->hdp_adapter->mi,
						L2CAP_MODE_ERTM, &err)) {
			error("Error: %s", err->message);
			g_error_free(err);
			return MCAP_MDL_BUSY;
		}

		dev->ndc = create_channel(dev, *conf, NULL, mdlid, NULL, NULL);
		if (!dev->ndc)
			return MCAP_MDL_BUSY;

		return MCAP_SUCCESS;
	}

	l = g_slist_find_custom(applications, &mdepid, cmp_app_id);
	if (!l)
		return MCAP_INVALID_MDEP;

	app = l->data;

	/* Check if is the first dc if so,
	* only reliable configuration is allowed */
	switch (*conf) {
	case HDP_NO_PREFERENCE_DC:
		if (app->role == HDP_SINK)
			return MCAP_CONFIGURATION_REJECTED;
		else if (dev->fr && app->chan_type_set)
			*conf = app->chan_type;
		else
			*conf = HDP_RELIABLE_DC;
		break;
	case HDP_STREAMING_DC:
		if (!dev->fr || app->role == HDP_SOURCE)
			return MCAP_CONFIGURATION_REJECTED;
	case HDP_RELIABLE_DC:
		if (app->role == HDP_SOURCE)
			return MCAP_CONFIGURATION_REJECTED;
		break;
	default:
		/* Special case defined in HDP spec 3.4. When an invalid
		* configuration is received we shall close the MCL when
		* we are still processing the callback. */
		close_device_con(dev, FALSE);
		return MCAP_CONFIGURATION_REJECTED; /* not processed */
	}

	l = g_slist_find_custom(dev->channels, &mdlid, cmp_chan_mdlid);
	if (l) {
		struct hdp_channel *chan = l->data;
		char *path;

		path = g_strdup(chan->path);
		g_dbus_unregister_interface(dev->conn, path, HEALTH_CHANNEL);
		g_free(path);
	}

	if (!mcap_set_data_chan_mode(dev->hdp_adapter->mi,
						hdp2l2cap_mode(*conf), &err)) {
		error("Error: %s", err->message);
		g_error_free(err);
		return MCAP_MDL_BUSY;
	}

	dev->ndc = create_channel(dev, *conf, NULL, mdlid, app, NULL);
	if (!dev->ndc)
		return MCAP_MDL_BUSY;

	return MCAP_SUCCESS;
}

static uint8_t hdp_mcap_mdl_reconn_req_cb(struct mcap_mdl *mdl, void *data)
{
	struct hdp_device *dev = data;
	struct hdp_channel *chan;
	GError *err = NULL;
	GSList *l;

	l = g_slist_find_custom(dev->channels, mdl, cmp_chan_mdl);
	if (!l)
		return MCAP_INVALID_MDL;

	chan = l->data;

	if (!dev->fr && (chan->config != HDP_RELIABLE_DC) &&
						(chan->mdep != HDP_MDEP_ECHO))
		return MCAP_UNSPECIFIED_ERROR;

	if (!mcap_set_data_chan_mode(dev->hdp_adapter->mi,
					hdp2l2cap_mode(chan->config), &err)) {
		error("Error: %s", err->message);
		g_error_free(err);
		return MCAP_MDL_BUSY;
	}

	dev->ndc = hdp_channel_ref(chan);

	return MCAP_SUCCESS;
}

gboolean hdp_set_mcl_cb(struct hdp_device *device, GError **err)
{
	gboolean ret;

	if (!device->mcl)
		return FALSE;

	ret = mcap_mcl_set_cb(device->mcl, device, err,
		MCAP_MDL_CB_CONNECTED, hdp_mcap_mdl_connected_cb,
		MCAP_MDL_CB_CLOSED, hdp_mcap_mdl_closed_cb,
		MCAP_MDL_CB_DELETED, hdp_mcap_mdl_deleted_cb,
		MCAP_MDL_CB_ABORTED, hdp_mcap_mdl_aborted_cb,
		MCAP_MDL_CB_REMOTE_CONN_REQ, hdp_mcap_mdl_conn_req_cb,
		MCAP_MDL_CB_REMOTE_RECONN_REQ, hdp_mcap_mdl_reconn_req_cb,
		MCAP_MDL_CB_INVALID);

	if (ret)
		return TRUE;

	error("Can't set mcl callbacks, closing mcl");
	close_device_con(device, TRUE);

	return FALSE;
}

static void mcl_connected(struct mcap_mcl *mcl, gpointer data)
{
	struct hdp_device *hdp_device;
	bdaddr_t addr;
	GSList *l;

	mcap_mcl_get_addr(mcl, &addr);
	l = g_slist_find_custom(devices, &addr, cmp_dev_addr);
	if (!l) {
		struct hdp_adapter *hdp_adapter = data;
		struct btd_device *device;
		char str[18];

		ba2str(&addr, str);
		device = adapter_get_device(connection,
					hdp_adapter->btd_adapter, str);
		if (!device)
			return;
		hdp_device = create_health_device(connection, device);
		if (!hdp_device)
			return;
		devices = g_slist_append(devices, hdp_device);
	} else
		hdp_device = l->data;

	hdp_device->mcl = mcap_mcl_ref(mcl);
	hdp_device->mcl_conn = TRUE;

	DBG("New mcl connected from  %s", device_get_path(hdp_device->dev));

	hdp_set_mcl_cb(hdp_device, NULL);
}

static void mcl_reconnected(struct mcap_mcl *mcl, gpointer data)
{
	struct hdp_device *hdp_device;
	GSList *l;

	l = g_slist_find_custom(devices, mcl, cmp_dev_mcl);
	if (!l)
		return;

	hdp_device = l->data;
	hdp_device->mcl_conn = TRUE;

	DBG("MCL reconnected %s", device_get_path(hdp_device->dev));

	hdp_set_mcl_cb(hdp_device, NULL);
}

static void mcl_disconnected(struct mcap_mcl *mcl, gpointer data)
{
	struct hdp_device *hdp_device;
	GSList *l;

	l = g_slist_find_custom(devices, mcl, cmp_dev_mcl);
	if (!l)
		return;

	hdp_device = l->data;
	hdp_device->mcl_conn = FALSE;

	DBG("Mcl disconnected %s", device_get_path(hdp_device->dev));
}

static void mcl_uncached(struct mcap_mcl *mcl, gpointer data)
{
	struct hdp_device *hdp_device;
	const char *path;
	GSList *l;

	l = g_slist_find_custom(devices, mcl, cmp_dev_mcl);
	if (!l)
		return;

	hdp_device = l->data;
	device_unref_mcl(hdp_device);

	if (hdp_device->sdp_present)
		return;

	/* Because remote device hasn't announced an HDP record */
	/* the Bluetooth daemon won't notify when the device shall */
	/* be removed. Then we have to remove the HealthDevice */
	/* interface manually */
	path = device_get_path(hdp_device->dev);
	g_dbus_unregister_interface(hdp_device->conn, path, HEALTH_DEVICE);
	DBG("Mcl uncached %s", path);
}

static void check_devices_mcl()
{
	struct hdp_device *dev;
	GSList *l, *to_delete = NULL;

	for (l = devices; l; l = l->next) {
		dev = l->data;
		device_unref_mcl(dev);

		if (!dev->sdp_present)
			to_delete = g_slist_append(to_delete, dev);
		else
			remove_channels(dev);
	}

	for (l = to_delete; l; l = l->next) {
		const char *path;

		path = device_get_path(dev->dev);
		g_dbus_unregister_interface(dev->conn, path, HEALTH_DEVICE);
	}

	g_slist_free(to_delete);
}

static void release_adapter_instance(struct hdp_adapter *hdp_adapter)
{
	if (!hdp_adapter->mi)
		return;

	check_devices_mcl();
	mcap_release_instance(hdp_adapter->mi);
	mcap_instance_unref(hdp_adapter->mi);
	hdp_adapter->mi = NULL;
}

static gboolean update_adapter(struct hdp_adapter *hdp_adapter)
{
	GError *err = NULL;
	bdaddr_t addr;

	if (!applications) {
		release_adapter_instance(hdp_adapter);
		goto update;
	}

	if (hdp_adapter->mi)
		goto update;

	adapter_get_address(hdp_adapter->btd_adapter, &addr);
	hdp_adapter->mi = mcap_create_instance(&addr, BT_IO_SEC_MEDIUM, 0, 0,
					mcl_connected, mcl_reconnected,
					mcl_disconnected, mcl_uncached,
					NULL, /* CSP is not used by now */
					hdp_adapter, &err);

	if (!hdp_adapter->mi) {
		error("Error creating the MCAP instance: %s", err->message);
		g_error_free(err);
		return FALSE;
	}

	hdp_adapter->ccpsm = mcap_get_ctrl_psm(hdp_adapter->mi, &err);
	if (err) {
		error("Error getting MCAP control PSM: %s", err->message);
		goto fail;
	}

	hdp_adapter->dcpsm = mcap_get_data_psm(hdp_adapter->mi, &err);
	if (err) {
		error("Error getting MCAP data PSM: %s", err->message);
		goto fail;
	}

update:
	if (hdp_update_sdp_record(hdp_adapter, applications))
		return TRUE;
	error("Error updating the SDP record");

fail:
	release_adapter_instance(hdp_adapter);
	if (err)
		g_error_free(err);
	return FALSE;
}

int hdp_adapter_register(DBusConnection *conn, struct btd_adapter *adapter)
{
	struct hdp_adapter *hdp_adapter;

	hdp_adapter = g_new0(struct hdp_adapter, 1);
	hdp_adapter->btd_adapter = btd_adapter_ref(adapter);

	if(!update_adapter(hdp_adapter))
		goto fail;

	adapters = g_slist_append(adapters, hdp_adapter);

	return 0;

fail:
	btd_adapter_unref(hdp_adapter->btd_adapter);
	g_free(hdp_adapter);
	return -1;
}

void hdp_adapter_unregister(struct btd_adapter *adapter)
{
	struct hdp_adapter *hdp_adapter;
	GSList *l;

	l = g_slist_find_custom(adapters, adapter, cmp_adapter);

	if (!l)
		return;

	hdp_adapter = l->data;
	adapters = g_slist_remove(adapters, hdp_adapter);
	if (hdp_adapter->sdp_handler)
		remove_record_from_server(hdp_adapter->sdp_handler);
	release_adapter_instance(hdp_adapter);
	btd_adapter_unref(hdp_adapter->btd_adapter);
	g_free(hdp_adapter);
}

static void delete_echo_channel_cb(GError *err, gpointer chan)
{
	if (err && err->code != MCAP_INVALID_MDL) {
		/* TODO: Decide if more action is required here */
		error("Error deleting echo channel: %s", err->message);
		return;
	}

	health_channel_destroy(chan);
}

static void delete_echo_channel(struct hdp_channel *chan)
{
	GError *err = NULL;

	if (!chan->dev->mcl_conn) {
		error("Echo channel cannot be deleted: mcl closed");
		return;
	}

	if (mcap_delete_mdl(chan->mdl, delete_echo_channel_cb,
				hdp_channel_ref(chan),
				(GDestroyNotify) hdp_channel_unref, &err))
		return;

	hdp_channel_unref(chan);
	error("Error deleting the echo channel: %s", err->message);
	g_error_free(err);

	/* TODO: Decide if more action is required here */
}

static void abort_echo_channel_cb(GError *err, gpointer data)
{
	struct hdp_channel *chan = data;

	if (err && err->code != MCAP_ERROR_INVALID_OPERATION) {
		error("Aborting error: %s", err->message);
		if (err->code == MCAP_INVALID_MDL) {
			/* MDL is removed from MCAP so we can */
			/* free the data channel without sending */
			/* a MD_DELETE_MDL_REQ */
			/* TODO review the above comment */
			/* hdp_channel_unref(chan); */
		}
		return;
	}

	delete_echo_channel(chan);
}

static void destroy_create_dc_data(gpointer data)
{
	struct hdp_create_dc *dc_data = data;

	hdp_create_data_unref(dc_data);
}

static void *generate_echo_packet()
{
	uint8_t *buf;
	int i;

	buf = g_malloc(HDP_ECHO_LEN);
	srand(time(NULL));

	for(i = 0; i < HDP_ECHO_LEN; i++)
		buf[i] = rand() % UINT8_MAX;

	return buf;
}

static gboolean check_echo(GIOChannel *io_chan, GIOCondition cond,
								gpointer data)
{
	struct hdp_tmp_dc_data *hdp_conn =  data;
	struct hdp_echo_data *edata = hdp_conn->hdp_chann->edata;
	struct hdp_channel *chan = hdp_conn->hdp_chann;
	uint8_t buf[MCAP_DC_MTU];
	DBusMessage *reply;
	gboolean value;
	int fd, len;

	if (cond & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
		value = FALSE;
		goto end;
	}

	fd = g_io_channel_unix_get_fd(io_chan);
	len = read(fd, buf, sizeof(buf));

	if (len != HDP_ECHO_LEN) {
		value = FALSE;
		goto end;
	}

	value = (memcmp(buf, edata->buf, len) == 0);

end:
	reply = g_dbus_create_reply(hdp_conn->msg, DBUS_TYPE_BOOLEAN, &value,
							DBUS_TYPE_INVALID);
	g_dbus_send_message(hdp_conn->conn, reply);
	g_source_remove(edata->tid);
	edata->tid = 0;
	g_free(edata->buf);
	edata->buf = NULL;

	if (!value)
		close_device_con(chan->dev, FALSE);
	else
		delete_echo_channel(chan);
	hdp_tmp_dc_data_unref(hdp_conn);

	return FALSE;
}

static gboolean echo_timeout(gpointer data)
{
	struct hdp_channel *chan = data;
	GIOChannel *io;
	int fd;

	error("Error: Echo request timeout");
	chan->edata->tid = 0;

	fd = mcap_mdl_get_fd(chan->mdl);
	if (fd < 0)
		return FALSE;

	io = g_io_channel_unix_new(fd);
	g_io_channel_shutdown(io, TRUE, NULL);

	return FALSE;
}

static void hdp_echo_connect_cb(struct mcap_mdl *mdl, GError *err,
								gpointer data)
{
	struct hdp_tmp_dc_data *hdp_conn =  data;
	struct hdp_echo_data *edata;
	GError *gerr = NULL;
	DBusMessage *reply;
	GIOChannel *io;
	int fd;

	if (err) {
		reply = g_dbus_create_error(hdp_conn->msg,
						ERROR_INTERFACE ".HealthError",
						"%s", err->message);
		g_dbus_send_message(hdp_conn->conn, reply);

		/* Send abort request because remote */
		/* side is now in PENDING state. */
		if (!mcap_mdl_abort(hdp_conn->hdp_chann->mdl,
					abort_echo_channel_cb,
					hdp_channel_ref(hdp_conn->hdp_chann),
					(GDestroyNotify) hdp_channel_unref,
					&gerr)) {
			error("%s", gerr->message);
			g_error_free(gerr);
			hdp_channel_unref(hdp_conn->hdp_chann);
		}
		return;
	}

	fd = mcap_mdl_get_fd(hdp_conn->hdp_chann->mdl);
	if (fd < 0) {
		reply = g_dbus_create_error(hdp_conn->msg,
						ERROR_INTERFACE ".HealthError",
						"Can't write in echo channel");
		g_dbus_send_message(hdp_conn->conn, reply);
		delete_echo_channel(hdp_conn->hdp_chann);
		return;
	}

	edata = hdp_conn->hdp_chann->edata;
	edata->buf = generate_echo_packet();
	send_echo_data(fd, edata->buf, HDP_ECHO_LEN);

	io = g_io_channel_unix_new(fd);
	g_io_add_watch(io, G_IO_ERR | G_IO_HUP | G_IO_NVAL | G_IO_IN,
			check_echo, hdp_tmp_dc_data_ref(hdp_conn));

	edata->tid  = g_timeout_add_seconds_full(G_PRIORITY_DEFAULT,
					ECHO_TIMEOUT, echo_timeout,
					hdp_channel_ref(hdp_conn->hdp_chann),
					(GDestroyNotify) hdp_channel_unref);

	g_io_channel_unref(io);
}

static void delete_mdl_cb(GError *err, gpointer data)
{
	if (err)
		error("Deleting error: %s", err->message);
}

static void abort_and_del_mdl_cb(GError *err, gpointer data)
{
	struct mcap_mdl *mdl = data;
	GError *gerr = NULL;

	if (err) {
		error("%s", err->message);
		if (err->code == MCAP_INVALID_MDL) {
			/* MDL is removed from MCAP so we don't */
			/* need to delete it. */
			return;
		}
	}

	if (!mcap_delete_mdl(mdl, delete_mdl_cb, NULL, NULL, &gerr)) {
		error("%s", gerr->message);
		g_error_free(gerr);
	}
}

static void hdp_mdl_conn_cb(struct mcap_mdl *mdl, GError *err, gpointer data)
{
	struct hdp_tmp_dc_data *hdp_conn =  data;
	struct hdp_channel *hdp_chann = hdp_conn->hdp_chann;
	struct hdp_device *dev = hdp_chann->dev;
	DBusMessage *reply;
	GError *gerr = NULL;

	if (err) {
		error("%s", err->message);
		reply = g_dbus_create_reply(hdp_conn->msg,
					DBUS_TYPE_OBJECT_PATH, &hdp_chann->path,
					DBUS_TYPE_INVALID);
		g_dbus_send_message(hdp_conn->conn, reply);

		/* Send abort request because remote side */
		/* is now in PENDING state */
		if (!mcap_mdl_abort(hdp_chann->mdl, abort_mdl_cb, NULL,
								NULL, &gerr)) {
			error("%s", gerr->message);
			g_error_free(gerr);
		}
		return;
	}

	reply = g_dbus_create_reply(hdp_conn->msg,
					DBUS_TYPE_OBJECT_PATH, &hdp_chann->path,
					DBUS_TYPE_INVALID);
	g_dbus_send_message(hdp_conn->conn, reply);

	if (!check_channel_conf(hdp_chann)) {
		close_mdl(hdp_chann);
		return;
	}

	if (dev->fr)
		return;

	dev->fr = hdp_channel_ref(hdp_chann);

	emit_property_changed(dev->conn, device_get_path(dev->dev),
					HEALTH_DEVICE, "MainChannel",
					DBUS_TYPE_OBJECT_PATH, &dev->fr->path);
}

static void device_create_mdl_cb(struct mcap_mdl *mdl, uint8_t conf,
						GError *err, gpointer data)
{
	struct hdp_create_dc *user_data = data;
	struct hdp_tmp_dc_data *hdp_conn;
	struct hdp_channel *hdp_chan;
	GError *gerr = NULL;
	DBusMessage *reply;

	if (err) {
		reply = g_dbus_create_error(user_data->msg,
					ERROR_INTERFACE ".HealthError",
					"%s", err->message);
		g_dbus_send_message(user_data->conn, reply);
		return;
	}

	if (user_data->mdep != HDP_MDEP_ECHO &&
				user_data->config == HDP_NO_PREFERENCE_DC) {
		if (!user_data->dev->fr && (conf != HDP_RELIABLE_DC)) {
			g_set_error(&gerr, HDP_ERROR, HDP_CONNECTION_ERROR,
					"Data channel aborted, first data "
					"channel should be reliable");
			goto fail;
		} else if (conf == HDP_NO_PREFERENCE_DC ||
						conf > HDP_STREAMING_DC) {
			g_set_error(&gerr, HDP_ERROR, HDP_CONNECTION_ERROR,
							"Data channel aborted, "
							"configuration error");
			goto fail;
		}
	}

	hdp_chan = create_channel(user_data->dev, conf, mdl,
							mcap_mdl_get_mdlid(mdl),
							user_data->app, &gerr);
	if (!hdp_chan)
		goto fail;

	if (user_data->mdep != HDP_MDEP_ECHO)
		g_dbus_emit_signal(user_data->conn,
					device_get_path(hdp_chan->dev->dev),
					HEALTH_DEVICE,
					"ChannelConnected",
					DBUS_TYPE_OBJECT_PATH, &hdp_chan->path,
					DBUS_TYPE_INVALID);

	hdp_conn = g_new0(struct hdp_tmp_dc_data, 1);
	hdp_conn->msg = dbus_message_ref(user_data->msg);
	hdp_conn->conn = dbus_connection_ref(user_data->conn);
	hdp_conn->hdp_chann = hdp_chan;
	hdp_conn->cb = user_data->cb;
	hdp_chan->mdep = user_data->mdep;

	if (hdp_get_dcpsm(hdp_chan->dev, hdp_get_dcpsm_cb,
						hdp_tmp_dc_data_ref(hdp_conn),
						hdp_tmp_dc_data_destroy, &gerr))
		return;

	error("%s", gerr->message);
	g_error_free(gerr);
	gerr = NULL;

	reply = g_dbus_create_reply(hdp_conn->msg,
					DBUS_TYPE_OBJECT_PATH, &hdp_chan->path,
					DBUS_TYPE_INVALID);
	g_dbus_send_message(hdp_conn->conn, reply);
	hdp_tmp_dc_data_unref(hdp_conn);

	/* Send abort request because remote side is now in PENDING state */
	if (!mcap_mdl_abort(mdl, abort_mdl_cb, NULL, NULL, &gerr)) {
		error("%s", gerr->message);
		g_error_free(gerr);
	}

	return;

fail:
	reply = g_dbus_create_error(user_data->msg,
						ERROR_INTERFACE ".HealthError",
						"%s", gerr->message);
	g_dbus_send_message(user_data->conn, reply);
	g_error_free(gerr);
	gerr = NULL;

	/* Send abort request because remote side is now in PENDING */
	/* state. Then we have to delete it because we couldn't */
	/* register the HealthChannel interface */
	if (!mcap_mdl_abort(mdl, abort_and_del_mdl_cb, mcap_mdl_ref(mdl), NULL,
								&gerr)) {
		error("%s", gerr->message);
		g_error_free(gerr);
		mcap_mdl_unref(mdl);
	}
}

static void device_create_dc_cb(gpointer user_data, GError *err)
{
	struct hdp_create_dc *data = user_data;
	DBusMessage *reply;
	GError *gerr = NULL;

	if (err) {
		reply = g_dbus_create_error(data->msg,
					ERROR_INTERFACE ".HealthError",
					"%s", err->message);
		g_dbus_send_message(data->conn, reply);
		return;
	}

	if (!data->dev->mcl) {
		g_set_error(&gerr, HDP_ERROR, HDP_CONNECTION_ERROR,
				"Mcl was closed");
		goto fail;
	}

	hdp_create_data_ref(data);

	if (mcap_create_mdl(data->dev->mcl, data->mdep, data->config,
						device_create_mdl_cb, data,
						destroy_create_dc_data, &gerr))
		return;
	hdp_create_data_unref(data);

fail:
	reply = g_dbus_create_error(data->msg, ERROR_INTERFACE ".HealthError",
							"%s", gerr->message);
	g_error_free(gerr);
	g_dbus_send_message(data->conn, reply);
}

static DBusMessage *device_echo(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	struct hdp_device *device = user_data;
	struct hdp_create_dc *data;
	DBusMessage *reply;
	GError *err = NULL;

	data = g_new0(struct hdp_create_dc, 1);
	data->dev = health_device_ref(device);
	data->mdep = HDP_MDEP_ECHO;
	data->config = HDP_RELIABLE_DC;
	data->msg = dbus_message_ref(msg);
	data->conn = dbus_connection_ref(conn);
	data->cb = hdp_echo_connect_cb;
	hdp_create_data_ref(data);

	if (device->mcl_conn && device->mcl) {
		if (mcap_create_mdl(device->mcl, data->mdep, data->config,
						device_create_mdl_cb, data,
						destroy_create_dc_data, &err))
			return NULL;
		goto fail;
	}

	if (hdp_establish_mcl(data->dev, device_create_dc_cb,
					data, destroy_create_dc_data, &err))
		return NULL;

fail:
	reply = g_dbus_create_error(msg, ERROR_INTERFACE ".HealthError",
							"%s", err->message);
	g_error_free(err);
	hdp_create_data_unref(data);
	return reply;
}

static void device_get_mdep_cb(uint8_t mdep, gpointer data, GError *err)
{
	struct hdp_create_dc *dc_data, *user_data = data;
	DBusMessage *reply;
	GError *gerr = NULL;

	if (err) {
		reply = g_dbus_create_error(user_data->msg,
						ERROR_INTERFACE ".HealthError",
						"%s", err->message);
		g_dbus_send_message(user_data->conn, reply);
		return;
	}

	dc_data = hdp_create_data_ref(user_data);
	dc_data->mdep = mdep;

	if (user_data->dev->mcl_conn) {
		device_create_dc_cb(dc_data, NULL);
		hdp_create_data_unref(dc_data);
		return;
	}

	if (hdp_establish_mcl(dc_data->dev, device_create_dc_cb,
					dc_data, destroy_create_dc_data, &gerr))
		return;

	reply = g_dbus_create_error(user_data->msg,
						ERROR_INTERFACE ".HealthError",
						"%s", gerr->message);
	hdp_create_data_unref(dc_data);
	g_error_free(gerr);
	g_dbus_send_message(user_data->conn, reply);
}

static DBusMessage *device_create_channel(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	struct hdp_device *device = user_data;
	struct hdp_application *app;
	struct hdp_create_dc *data;
	char *app_path, *conf;
	DBusMessage *reply;
	GError *err = NULL;
	uint8_t config;
	GSList *l;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &app_path,
							DBUS_TYPE_STRING, &conf,
							DBUS_TYPE_INVALID))
		return btd_error_invalid_args(msg);

	l = g_slist_find_custom(applications, app_path, cmp_app);
	if (!l)
		return btd_error_invalid_args(msg);

	app = l->data;

	if (g_ascii_strcasecmp("Reliable", conf) == 0)
		config = HDP_RELIABLE_DC;
	else if (g_ascii_strcasecmp("Streaming", conf) == 0)
		config = HDP_STREAMING_DC;
	else if (g_ascii_strcasecmp("Any", conf) == 0)
		config = HDP_NO_PREFERENCE_DC;
	else
		return btd_error_invalid_args(msg);

	if (app->role == HDP_SINK && config != HDP_NO_PREFERENCE_DC)
		return btd_error_invalid_args(msg);

	if (app->role == HDP_SOURCE && config == HDP_NO_PREFERENCE_DC)
		return btd_error_invalid_args(msg);

	if (!device->fr && config == HDP_STREAMING_DC)
		return btd_error_invalid_args(msg);

	data = g_new0(struct hdp_create_dc, 1);
	data->dev = health_device_ref(device);
	data->config = config;
	data->app = hdp_application_ref(app);
	data->msg = dbus_message_ref(msg);
	data->conn = dbus_connection_ref(conn);
	data->cb = hdp_mdl_conn_cb;

	if (hdp_get_mdep(device, l->data, device_get_mdep_cb,
						hdp_create_data_ref(data),
						destroy_create_dc_data, &err))
		return NULL;

	reply = g_dbus_create_error(msg, ERROR_INTERFACE ".HealthError",
							"%s", err->message);
	g_error_free(err);
	hdp_create_data_unref(data);
	return reply;
}

static void hdp_mdl_delete_cb(GError *err, gpointer data)
{
	struct hdp_tmp_dc_data *del_data = data;
	DBusMessage *reply;
	char *path;

	if (err && err->code != MCAP_INVALID_MDL) {
		reply = g_dbus_create_error(del_data->msg,
						ERROR_INTERFACE ".HealthError",
						"%s", err->message);
		g_dbus_send_message(del_data->conn, reply);
		return;
	}

	path = g_strdup(del_data->hdp_chann->path);
	g_dbus_unregister_interface(del_data->conn, path, HEALTH_CHANNEL);
	g_free(path);

	reply = g_dbus_create_reply(del_data->msg, DBUS_TYPE_INVALID);
	g_dbus_send_message(del_data->conn, reply);
}

static void hdp_continue_del_cb(gpointer user_data, GError *err)
{
	struct hdp_tmp_dc_data *del_data = user_data;
	GError *gerr = NULL;
	DBusMessage *reply;

	if (err) {
		reply = g_dbus_create_error(del_data->msg,
					ERROR_INTERFACE ".HealthError",
					"%s", err->message);
		g_dbus_send_message(del_data->conn, reply);
		return;
	}

	if (mcap_delete_mdl(del_data->hdp_chann->mdl, hdp_mdl_delete_cb,
						hdp_tmp_dc_data_ref(del_data),
						hdp_tmp_dc_data_destroy, &gerr))
			return;

	reply = g_dbus_create_error(del_data->msg,
						ERROR_INTERFACE ".HealthError",
						"%s", gerr->message);
	hdp_tmp_dc_data_unref(del_data);
	g_error_free(gerr);
	g_dbus_send_message(del_data->conn, reply);
}

static DBusMessage *device_destroy_channel(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	struct hdp_device *device = user_data;
	struct hdp_tmp_dc_data *del_data;
	struct hdp_channel *hdp_chan;
	DBusMessage *reply;
	GError *err = NULL;
	char *path;
	GSList *l;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
							DBUS_TYPE_INVALID)){
		return btd_error_invalid_args(msg);
	}

	l = g_slist_find_custom(device->channels, path, cmp_chan_path);
	if (!l)
		return btd_error_invalid_args(msg);

	hdp_chan = l->data;
	del_data = g_new0(struct hdp_tmp_dc_data, 1);
	del_data->msg = dbus_message_ref(msg);
	del_data->conn = dbus_connection_ref(conn);
	del_data->hdp_chann = hdp_channel_ref(hdp_chan);

	if (device->mcl_conn) {
		if (mcap_delete_mdl(hdp_chan->mdl, hdp_mdl_delete_cb,
						hdp_tmp_dc_data_ref(del_data),
						hdp_tmp_dc_data_destroy, &err))
			return NULL;
		goto fail;
	}

	if (hdp_establish_mcl(device, hdp_continue_del_cb,
						hdp_tmp_dc_data_ref(del_data),
						hdp_tmp_dc_data_destroy, &err))
		return NULL;

fail:
	reply = g_dbus_create_error(msg, ERROR_INTERFACE ".HealthError",
							"%s", err->message);
	hdp_tmp_dc_data_unref(del_data);
	g_error_free(err);
	return reply;
}

static DBusMessage *device_get_properties(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	struct hdp_device *device = user_data;
	DBusMessageIter iter, dict;
	DBusMessage *reply;
	char *path;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
			DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
			DBUS_TYPE_STRING_AS_STRING DBUS_TYPE_VARIANT_AS_STRING
			DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &dict);

	if (device->fr)
		path = g_strdup(device->fr->path);
	else
		path = g_strdup("");
	dict_append_entry(&dict, "MainChannel", DBUS_TYPE_STRING, &path);
	g_free(path);
	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static void health_device_destroy(void *data)
{
	struct hdp_device *device = data;

	DBG("Unregistered interface %s on path %s", HEALTH_DEVICE,
						device_get_path(device->dev));

	remove_channels(device);
	if (device->ndc) {
		hdp_channel_unref(device->ndc);
		device->ndc = NULL;
	}

	devices = g_slist_remove(devices, device);
	health_device_unref(device);
}

static GDBusMethodTable health_device_methods[] = {
	{"Echo",		"",	"b",	device_echo,
						G_DBUS_METHOD_FLAG_ASYNC },
	{"CreateChannel",	"os",	"o",	device_create_channel,
						G_DBUS_METHOD_FLAG_ASYNC },
	{"DestroyChannel",	"o",	"",	device_destroy_channel,
						G_DBUS_METHOD_FLAG_ASYNC },
	{"GetProperties",	"",	"a{sv}", device_get_properties},
	{ NULL }
};

static GDBusSignalTable health_device_signals[] = {
	{"ChannelConnected",		"o"		},
	{"ChannelDeleted",		"o"		},
	{"PropertyChanged",		"sv"		},
	{ NULL }
};

static struct hdp_device *create_health_device(DBusConnection *conn,
						struct btd_device *device)
{
	struct btd_adapter *adapter = device_get_adapter(device);
	const gchar *path = device_get_path(device);
	struct hdp_device *dev;
	GSList *l;

	if (!device)
		return NULL;

	dev = g_new0(struct hdp_device, 1);
	dev->conn = dbus_connection_ref(conn);
	dev->dev = btd_device_ref(device);
	health_device_ref(dev);

	l = g_slist_find_custom(adapters, adapter, cmp_adapter);
	if (!l)
		goto fail;

	dev->hdp_adapter = l->data;

	if (!g_dbus_register_interface(conn, path,
					HEALTH_DEVICE,
					health_device_methods,
					health_device_signals, NULL,
					dev, health_device_destroy)) {
		error("D-Bus failed to register %s interface", HEALTH_DEVICE);
		goto fail;
	}

	DBG("Registered interface %s on path %s", HEALTH_DEVICE, path);
	return dev;

fail:
	health_device_unref(dev);
	return NULL;
}

int hdp_device_register(DBusConnection *conn, struct btd_device *device)
{
	struct hdp_device *hdev;
	GSList *l;

	l = g_slist_find_custom(devices, device, cmp_device);
	if (l) {
		hdev = l->data;
		hdev->sdp_present = TRUE;
		return 0;
	}

	hdev = create_health_device(conn, device);
	if (!hdev)
		return -1;

	hdev->sdp_present = TRUE;

	devices = g_slist_prepend(devices, hdev);
	return 0;
}

void hdp_device_unregister(struct btd_device *device)
{
	struct hdp_device *hdp_dev;
	const char *path;
	GSList *l;

	l = g_slist_find_custom(devices, device, cmp_device);
	if (!l)
		return;

	hdp_dev = l->data;
	path = device_get_path(hdp_dev->dev);
	g_dbus_unregister_interface(hdp_dev->conn, path, HEALTH_DEVICE);
}

int hdp_manager_start(DBusConnection *conn)
{
	DBG("Starting Health manager");

	if (!g_dbus_register_interface(conn, MANAGER_PATH,
					HEALTH_MANAGER,
					health_manager_methods, NULL, NULL,
					NULL, manager_path_unregister)) {
		error("D-Bus failed to register %s interface", HEALTH_MANAGER);
		return -1;
	}

	connection = dbus_connection_ref(conn);

	return 0;
}

void hdp_manager_stop()
{
	g_dbus_unregister_interface(connection, MANAGER_PATH, HEALTH_MANAGER);

	dbus_connection_unref(connection);
	DBG("Stopped Health manager");
}

struct hdp_device *health_device_ref(struct hdp_device *hdp_dev)
{
	hdp_dev->ref++;

	DBG("health_device_ref(%p): ref=%d", hdp_dev, hdp_dev->ref);

	return hdp_dev;
}

void health_device_unref(struct hdp_device *hdp_dev)
{
	hdp_dev->ref--;

	DBG("health_device_unref(%p): ref=%d", hdp_dev, hdp_dev->ref);

	if (hdp_dev->ref > 0)
		return;

	free_health_device(hdp_dev);
}