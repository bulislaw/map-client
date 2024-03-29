/*
 *
 *  OBEX Client
 *
 *  Copyright (C) 2011  Bartosz Szatkowski <bulislaw@linux.com> for Comarch
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

#include <errno.h>
#include <glib.h>
#include <gdbus.h>
#include <string.h>

#include "log.h"

#include "map.h"
#include "transfer.h"
#include "session.h"
#include "driver.h"

#define OBEX_MAS_UUID \
	"\xBB\x58\x2B\x40\x42\x0C\x11\xDB\xB0\xDE\x08\x00\x20\x0C\x9A\x66"
#define OBEX_MAS_UUID_LEN 16

#define MAP_INTERFACE  "org.openobex.MessageAccess"
#define MAS_UUID "00001132-0000-1000-8000-00805f9b34fb"

struct map_data {
	struct obc_session *session;
	DBusMessage *msg;
};

/* TODO: Remove this */
struct dummy_apparam {
	uint8_t tag;
	uint8_t len;
} __attribute__ ((packed));

static DBusConnection *conn = NULL;

static void simple_cb(GObex *obex, GError *err, GObexPacket *rsp,
							gpointer user_data)
{
	DBusMessage *reply;
	struct map_data *map = user_data;
	guint8 err_code = g_obex_packet_get_operation(rsp, NULL);

	if (err != NULL)
		reply = g_dbus_create_error(map->msg,
						"org.openobex.Error.Failed",
						"%s", err->message);
	else if (err_code != G_OBEX_RSP_SUCCESS)
		reply = g_dbus_create_error(map->msg,
						"org.openobex.Error.Failed",
						"%s (0x%02x)",
						g_obex_strerror(err_code),
						err_code);
	else
		reply = dbus_message_new_method_return(map->msg);

	g_dbus_send_message(conn, reply);
	dbus_message_unref(map->msg);
}

static void empty_cb(struct obc_session *session, GError *err,
							void *user_data)
{
	struct map_data *map = user_data;
	DBusMessage *reply;

	DBG("");

	if (err != NULL) {
		reply = g_dbus_create_error(map->msg,
						"org.openobex.Error.Failed",
						"%s", err->message);

		goto done;
	}

	reply = dbus_message_new_method_return(map->msg);

done:
	g_dbus_send_message(conn, reply);
	dbus_message_unref(map->msg);
}

static DBusMessage *map_setpath(DBusConnection *connection,
					DBusMessage *message, void *user_data)
{
	struct map_data *map = user_data;
	const char *folder;
	GObex *obex;
	GError *err = NULL;

	if (dbus_message_get_args(message, NULL, DBUS_TYPE_STRING, &folder,
						DBUS_TYPE_INVALID) == FALSE)
		return g_dbus_create_error(message,
					"org.openobex.Error.InvalidArguments",
					NULL);

	obex = obc_session_get_obex(map->session);

	g_obex_setpath(obex, folder, simple_cb, map, &err);
	if (err != NULL) {
		DBusMessage *reply;
		reply =  g_dbus_create_error(message,
						"org.openobex.Error.Failed",
						"%s", err->message);
		g_error_free(err);
		return reply;
	}

	map->msg = dbus_message_ref(message);

	return NULL;
}

static void buffer_cb(struct obc_session *session, GError *err,
							void *user_data)
{
	struct obc_transfer *transfer = obc_session_get_transfer(session);
	struct map_data *map = user_data;
	DBusMessage *reply;
	const char *buf;
	int size;

	if (err != NULL) {
		reply = g_dbus_create_error(map->msg,
						"org.openobex.Error.Failed",
						"%s", err->message);
		goto done;
	}

	buf = obc_transfer_get_buffer(transfer, &size);
	if (size == 0)
		buf = "";

	reply = g_dbus_create_reply(map->msg, DBUS_TYPE_STRING, &buf,
							DBUS_TYPE_INVALID);

	obc_transfer_clear_buffer(transfer);

done:
	g_dbus_send_message(conn, reply);
	dbus_message_unref(map->msg);
	obc_transfer_unregister(transfer);
}

static DBusMessage *map_get_folder_listing(DBusConnection *connection,
					DBusMessage *message, void *user_data)
{
	struct map_data *map = user_data;
	int err;

	err = obc_session_get(map->session, "x-obex/folder-listing",
							NULL, NULL, NULL, 0,
							buffer_cb, map);
	if (err < 0)
		return g_dbus_create_error(message, "org.openobex.Error.Failed",
									NULL);

	map->msg = dbus_message_ref(message);

	return NULL;
}

static DBusMessage *map_get_message_listing(DBusConnection *connection,
					DBusMessage *message, void *user_data)
{
	struct map_data *map = user_data;
	int err;
	const char *folder;
	DBusMessageIter msg_iter;

	dbus_message_iter_init(message, &msg_iter);

	if (dbus_message_iter_get_arg_type(&msg_iter) != DBUS_TYPE_STRING)
		return g_dbus_create_error(message,
				"org.openobex.Error.InvalidArguments", NULL);

	dbus_message_iter_get_basic(&msg_iter, &folder);

	err = obc_session_get(map->session, "x-bt/MAP-msg-listing", folder,
							NULL, NULL, 0,
							buffer_cb, map);
	if (err < 0)
		return g_dbus_create_error(message, "org.openobex.Error.Failed",
									NULL);

	map->msg = dbus_message_ref(message);

	return NULL;
}

static DBusMessage *map_get_message(DBusConnection *connection,
					DBusMessage *message, void *user_data)
{
	struct map_data *map = user_data;
	int err;
	const char *handle, *path, *transfer_path;
	DBusMessageIter msg_iter;
	struct obc_transfer *transfer;

	dbus_message_iter_init(message, &msg_iter);

	if (dbus_message_iter_get_arg_type(&msg_iter) != DBUS_TYPE_STRING)
		return g_dbus_create_error(message,
				"org.openobex.Error.InvalidArguments", NULL);

	dbus_message_iter_get_basic(&msg_iter, &handle);

	dbus_message_iter_next(&msg_iter);

	if (dbus_message_iter_get_arg_type(&msg_iter) != DBUS_TYPE_ARRAY)
		return g_dbus_create_error(message,
				"org.openobex.Error.InvalidArguments", NULL);

	dbus_message_iter_next(&msg_iter);

	if (dbus_message_iter_get_arg_type(&msg_iter) != DBUS_TYPE_STRING)
		return g_dbus_create_error(message,
				"org.openobex.Error.InvalidArguments", NULL);

	dbus_message_iter_get_basic(&msg_iter, &path);

	err = obc_session_get(map->session, "x-bt/message", handle, path,
							NULL, 0, NULL, NULL);
	if (err < 0)
		return g_dbus_create_error(message, "org.openobex.Error.Failed",
									NULL);

	transfer = obc_session_get_transfer(map->session);
	transfer_path = obc_transfer_get_path(transfer);

	return g_dbus_create_reply(message, DBUS_TYPE_OBJECT_PATH,
					&transfer_path, DBUS_TYPE_INVALID);
}

static DBusMessage *map_update_inbox(DBusConnection *connection,
					DBusMessage *message, void *user_data)
{
	struct map_data *map = user_data;
	int err;

	err = obc_session_put(map->session, g_strdup("\x30"),
						"x-bt/MAP-messageUpdate",
						NULL, NULL, NULL, 0,
						empty_cb, map);
	if (err < 0)
		return g_dbus_create_error(message, "org.openobex.Error.Failed",
									NULL);

	map->msg = dbus_message_ref(message);

	return NULL;
}

static DBusMessage *map_push_message(DBusConnection *connection,
					DBusMessage *message, void *user_data)
{
	struct map_data *map = user_data;
	int err;
	DBusMessageIter msg_iter;
	const char *folder, *msg_file;
	struct dummy_apparam app;
	uint8_t *buf;
	char charset[] = "<UTF-8>";

	dbus_message_iter_init(message, &msg_iter);

	if (dbus_message_iter_get_arg_type(&msg_iter) != DBUS_TYPE_STRING)
		return g_dbus_create_error(message,
				"org.openobex.Error.InvalidArguments", NULL);

	dbus_message_iter_get_basic(&msg_iter, &folder);

	dbus_message_iter_next(&msg_iter);

	if (dbus_message_iter_get_arg_type(&msg_iter) != DBUS_TYPE_STRING)
		return g_dbus_create_error(message,
				"org.openobex.Error.InvalidArguments", NULL);

	dbus_message_iter_get_basic(&msg_iter, &msg_file);

	/* TODO: Delete this */
	buf = g_new0(uint8_t, 2 + strlen(charset));
	app.tag = 0x14;
	app.len = 1;

	memcpy(buf, &app, 2);
	memcpy(buf+2, &charset, strlen(charset));

	err = obc_session_put(map->session, NULL, "x-bt/message", msg_file,
				folder, buf, 2+strlen(charset), empty_cb, map);
	if (err < 0)
		return g_dbus_create_error(message, "org.openobex.Error.Failed",
									NULL);

	map->msg = dbus_message_ref(message);

	return NULL;
}

static GDBusMethodTable map_methods[] = {
	{ "SetFolder",		"s", "",	map_setpath,
						G_DBUS_METHOD_FLAG_ASYNC },
	{ "GetFolderListing",	"a{ss}", "s",	map_get_folder_listing,
						G_DBUS_METHOD_FLAG_ASYNC },
	{ "GetMessageListing",	"sa{ss}", "s",	map_get_message_listing,
						G_DBUS_METHOD_FLAG_ASYNC },
	{ "GetMessage",		"sa{ss}s", "o",	map_get_message },
	{ "UpdateInbox",	"", "",		map_update_inbox,
						G_DBUS_METHOD_FLAG_ASYNC },
	{ "PushMessage",	"ss", "s",	map_push_message,
						G_DBUS_METHOD_FLAG_ASYNC },
	{ }
};

static void map_free(void *data)
{
	struct map_data *map = data;

	obc_session_unref(map->session);
	g_free(map);
}

static int map_probe(struct obc_session *session)
{
	struct map_data *map;
	const char *path;

	path = obc_session_get_path(session);

	DBG("%s", path);

	map = g_try_new0(struct map_data, 1);
	if (!map)
		return -ENOMEM;

	map->session = obc_session_ref(session);

	if (!g_dbus_register_interface(conn, path, MAP_INTERFACE, map_methods,
					NULL, NULL, map, map_free)) {
		map_free(map);

		return -ENOMEM;
	}

	return 0;
}

static void map_remove(struct obc_session *session)
{
	const char *path = obc_session_get_path(session);

	DBG("%s", path);

	g_dbus_unregister_interface(conn, path, MAP_INTERFACE);
}

static struct obc_driver map = {
	.service = "MAP",
	.uuid = MAS_UUID,
	.target = OBEX_MAS_UUID,
	.target_len = OBEX_MAS_UUID_LEN,
	.probe = map_probe,
	.remove = map_remove
};

int map_init(void)
{
	int err;

	DBG("");

	conn = dbus_bus_get(DBUS_BUS_SESSION, NULL);
	if (!conn)
		return -EIO;

	err = obc_driver_register(&map);
	if (err < 0) {
		dbus_connection_unref(conn);
		conn = NULL;
		return err;
	}

	return 0;
}

void map_exit(void)
{
	DBG("");

	dbus_connection_unref(conn);
	conn = NULL;

	obc_driver_unregister(&map);
}
