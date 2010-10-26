/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager system settings service
 *
 * Søren Sandmann <sandmann@daimi.au.dk>
 * Dan Williams <dcbw@redhat.com>
 * Tambet Ingo <tambet@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * (C) Copyright 2007 - 2010 Red Hat, Inc.
 * (C) Copyright 2008 Novell, Inc.
 */

#include <unistd.h>
#include <string.h>
#include <gmodule.h>
#include <net/ethernet.h>
#include <netinet/ether.h>

#include <NetworkManager.h>
#include <nm-connection.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib-lowlevel.h>

#include <nm-setting-8021x.h>
#include <nm-setting-bluetooth.h>
#include <nm-setting-cdma.h>
#include <nm-setting-connection.h>
#include <nm-setting-gsm.h>
#include <nm-setting-ip4-config.h>
#include <nm-setting-ip6-config.h>
#include <nm-setting-olpc-mesh.h>
#include <nm-setting-ppp.h>
#include <nm-setting-pppoe.h>
#include <nm-setting-serial.h>
#include <nm-setting-vpn.h>
#include <nm-setting-wired.h>
#include <nm-setting-wireless.h>
#include <nm-setting-wireless-security.h>

#include "../nm-device-ethernet.h"
#include "nm-dbus-glib-types.h"
#include "nm-sysconfig-settings.h"
#include "nm-sysconfig-connection.h"
#include "nm-polkit-helpers.h"
#include "nm-system-config-error.h"
#include "nm-default-wired-connection.h"
#include "nm-logging.h"
#include "nm-dbus-manager.h"
#include "nm-manager-auth.h"
#include "nm-session-monitor.h"

#define CONFIG_KEY_NO_AUTO_DEFAULT "no-auto-default"

/* LINKER CRACKROCK */
#define EXPORT(sym) void * __export_##sym = &sym;

#include "nm-inotify-helper.h"
EXPORT(nm_inotify_helper_get_type)
EXPORT(nm_inotify_helper_get)
EXPORT(nm_inotify_helper_add_watch)
EXPORT(nm_inotify_helper_remove_watch)

EXPORT(nm_sysconfig_connection_get_type)
EXPORT(nm_sysconfig_connection_replace_settings)
EXPORT(nm_sysconfig_connection_replace_and_commit)
/* END LINKER CRACKROCK */

static void claim_connection (NMSysconfigSettings *self,
                              NMSysconfigConnection *connection,
                              gboolean do_export);

static gboolean impl_settings_list_connections (NMSysconfigSettings *self,
                                                GPtrArray **connections,
                                                GError **error);

static void impl_settings_add_connection (NMSysconfigSettings *self,
                                          GHashTable *settings,
                                          DBusGMethodInvocation *context);

static void impl_settings_save_hostname (NMSysconfigSettings *self,
                                         const char *hostname,
                                         DBusGMethodInvocation *context);

#include "nm-settings-glue.h"

static void unmanaged_specs_changed (NMSystemConfigInterface *config, gpointer user_data);

typedef struct {
	DBusGConnection *bus;
	gboolean exported;

	NMSessionMonitor *session_monitor;
	guint session_monitor_id;

	PolkitAuthority *authority;
	guint auth_changed_id;
	char *config_file;

	GSList *pk_calls;

	GSList *plugins;
	gboolean connections_loaded;
	GHashTable *visible_connections;
	GHashTable *all_connections;
	GSList *unmanaged_specs;
} NMSysconfigSettingsPrivate;

G_DEFINE_TYPE (NMSysconfigSettings, nm_sysconfig_settings, G_TYPE_OBJECT)

#define NM_SYSCONFIG_SETTINGS_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), NM_TYPE_SYSCONFIG_SETTINGS, NMSysconfigSettingsPrivate))

enum {
	PROPERTIES_CHANGED,
	NEW_CONNECTION,

	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

enum {
	PROP_0,
	PROP_UNMANAGED_SPECS,
	PROP_HOSTNAME,
	PROP_CAN_MODIFY,

	LAST_PROP
};

static void
load_connections (NMSysconfigSettings *self)
{
	NMSysconfigSettingsPrivate *priv = NM_SYSCONFIG_SETTINGS_GET_PRIVATE (self);
	GSList *iter;

	if (priv->connections_loaded)
		return;

	for (iter = priv->plugins; iter; iter = g_slist_next (iter)) {
		NMSystemConfigInterface *plugin = NM_SYSTEM_CONFIG_INTERFACE (iter->data);
		GSList *plugin_connections;
		GSList *elt;

		plugin_connections = nm_system_config_interface_get_connections (plugin);

		// FIXME: ensure connections from plugins loaded with a lower priority
		// get rejected when they conflict with connections from a higher
		// priority plugin.

		for (elt = plugin_connections; elt; elt = g_slist_next (elt))
			claim_connection (self, NM_SYSCONFIG_CONNECTION (elt->data), TRUE);

		g_slist_free (plugin_connections);
	}

	priv->connections_loaded = TRUE;

	/* FIXME: Bad hack */
	unmanaged_specs_changed (NULL, self);
}

void
nm_sysconfig_settings_for_each_connection (NMSysconfigSettings *self,
                                           NMSysconfigSettingsForEachFunc for_each_func,
                                           gpointer user_data)
{
	NMSysconfigSettingsPrivate *priv;
	GHashTableIter iter;
	gpointer key;

	g_return_if_fail (self != NULL);
	g_return_if_fail (NM_IS_SYSCONFIG_SETTINGS (self));
	g_return_if_fail (for_each_func != NULL);
	
	priv = NM_SYSCONFIG_SETTINGS_GET_PRIVATE (self);

	load_connections (self);

	g_hash_table_iter_init (&iter, priv->visible_connections);
	while (g_hash_table_iter_next (&iter, &key, NULL))
		for_each_func (self, NM_SYSCONFIG_CONNECTION (key), user_data);
}

static gboolean
impl_settings_list_connections (NMSysconfigSettings *self,
                                GPtrArray **connections,
                                GError **error)
{
	NMSysconfigSettingsPrivate *priv = NM_SYSCONFIG_SETTINGS_GET_PRIVATE (self);
	GHashTableIter iter;
	gpointer key;

	load_connections (self);

	*connections = g_ptr_array_sized_new (g_hash_table_size (priv->visible_connections) + 1);
	g_hash_table_iter_init (&iter, priv->visible_connections);
	while (g_hash_table_iter_next (&iter, &key, NULL))
		g_ptr_array_add (*connections, g_strdup (nm_connection_get_path (NM_CONNECTION (key))));
	return TRUE;
}

NMSysconfigConnection *
nm_sysconfig_settings_get_connection_by_path (NMSysconfigSettings *self, const char *path)
{
	NMSysconfigSettingsPrivate *priv;
	GHashTableIter iter;
	gpointer key;

	g_return_val_if_fail (self != NULL, NULL);
	g_return_val_if_fail (NM_IS_SYSCONFIG_SETTINGS (self), NULL);
	g_return_val_if_fail (path != NULL, NULL);

	priv = NM_SYSCONFIG_SETTINGS_GET_PRIVATE (self);

	load_connections (self);

	g_hash_table_iter_init (&iter, priv->visible_connections);
	while (g_hash_table_iter_next (&iter, &key, NULL)) {
		if (!strcmp (nm_connection_get_path (NM_CONNECTION (key)), path))
			return NM_SYSCONFIG_CONNECTION (key);
	}
	return NULL;
}

static void
clear_unmanaged_specs (NMSysconfigSettings *self)
{
	NMSysconfigSettingsPrivate *priv = NM_SYSCONFIG_SETTINGS_GET_PRIVATE (self);

	g_slist_foreach (priv->unmanaged_specs, (GFunc) g_free, NULL);
	g_slist_free (priv->unmanaged_specs);
	priv->unmanaged_specs = NULL;
}

static char*
uscore_to_wincaps (const char *uscore)
{
	const char *p;
	GString *str;
	gboolean last_was_uscore;

	last_was_uscore = TRUE;
  
	str = g_string_new (NULL);
	p = uscore;
	while (p && *p) {
		if (*p == '-' || *p == '_')
			last_was_uscore = TRUE;
		else {
			if (last_was_uscore) {
				g_string_append_c (str, g_ascii_toupper (*p));
				last_was_uscore = FALSE;
			} else
				g_string_append_c (str, *p);
		}
		++p;
	}

	return g_string_free (str, FALSE);
}

static void
notify (GObject *object, GParamSpec *pspec)
{
	GValue *value;
	GHashTable *hash;

	value = g_slice_new0 (GValue);
	hash = g_hash_table_new_full (g_str_hash, g_str_equal, (GDestroyNotify) g_free, NULL);

	g_value_init (value, pspec->value_type);
	g_object_get_property (object, pspec->name, value);
	g_hash_table_insert (hash, uscore_to_wincaps (pspec->name), value);
	g_signal_emit (object, signals[PROPERTIES_CHANGED], 0, hash);
	g_hash_table_destroy (hash);
	g_value_unset (value);
	g_slice_free (GValue, value);
}

const GSList *
nm_sysconfig_settings_get_unmanaged_specs (NMSysconfigSettings *self)
{
	NMSysconfigSettingsPrivate *priv = NM_SYSCONFIG_SETTINGS_GET_PRIVATE (self);

	load_connections (self);
	return priv->unmanaged_specs;
}

static NMSystemConfigInterface *
get_plugin (NMSysconfigSettings *self, guint32 capability)
{
	NMSysconfigSettingsPrivate *priv = NM_SYSCONFIG_SETTINGS_GET_PRIVATE (self);
	GSList *iter;

	g_return_val_if_fail (self != NULL, NULL);

	/* Do any of the plugins support setting the hostname? */
	for (iter = priv->plugins; iter; iter = iter->next) {
		NMSystemConfigInterfaceCapabilities caps = NM_SYSTEM_CONFIG_INTERFACE_CAP_NONE;

		g_object_get (G_OBJECT (iter->data), NM_SYSTEM_CONFIG_INTERFACE_CAPABILITIES, &caps, NULL);
		if (caps & capability)
			return NM_SYSTEM_CONFIG_INTERFACE (iter->data);
	}

	return NULL;
}

/* Returns an allocated string which the caller owns and must eventually free */
char *
nm_sysconfig_settings_get_hostname (NMSysconfigSettings *self)
{
	NMSysconfigSettingsPrivate *priv = NM_SYSCONFIG_SETTINGS_GET_PRIVATE (self);
	GSList *iter;
	char *hostname = NULL;

	/* Hostname returned is the hostname returned from the first plugin
	 * that provides one.
	 */
	for (iter = priv->plugins; iter; iter = iter->next) {
		NMSystemConfigInterfaceCapabilities caps = NM_SYSTEM_CONFIG_INTERFACE_CAP_NONE;

		g_object_get (G_OBJECT (iter->data), NM_SYSTEM_CONFIG_INTERFACE_CAPABILITIES, &caps, NULL);
		if (caps & NM_SYSTEM_CONFIG_INTERFACE_CAP_MODIFY_HOSTNAME) {
			g_object_get (G_OBJECT (iter->data), NM_SYSTEM_CONFIG_INTERFACE_HOSTNAME, &hostname, NULL);
			if (hostname && strlen (hostname))
				return hostname;
			g_free (hostname);
		}
	}

	return NULL;
}

static void
plugin_connection_added (NMSystemConfigInterface *config,
                         NMSysconfigConnection *connection,
                         gpointer user_data)
{
	claim_connection (NM_SYSCONFIG_SETTINGS (user_data), connection, TRUE);
}

static gboolean
find_unmanaged_device (NMSysconfigSettings *self, const char *needle)
{
	NMSysconfigSettingsPrivate *priv = NM_SYSCONFIG_SETTINGS_GET_PRIVATE (self);
	GSList *iter;

	for (iter = priv->unmanaged_specs; iter; iter = g_slist_next (iter)) {
		if (!strcmp ((const char *) iter->data, needle))
			return TRUE;
	}
	return FALSE;
}

static void
unmanaged_specs_changed (NMSystemConfigInterface *config,
                         gpointer user_data)
{
	NMSysconfigSettings *self = NM_SYSCONFIG_SETTINGS (user_data);
	NMSysconfigSettingsPrivate *priv = NM_SYSCONFIG_SETTINGS_GET_PRIVATE (self);
	GSList *iter;

	clear_unmanaged_specs (self);

	/* Ask all the plugins for their unmanaged specs */
	for (iter = priv->plugins; iter; iter = g_slist_next (iter)) {
		GSList *specs, *specs_iter;

		specs = nm_system_config_interface_get_unmanaged_specs (NM_SYSTEM_CONFIG_INTERFACE (iter->data));
		for (specs_iter = specs; specs_iter; specs_iter = specs_iter->next) {
			if (!find_unmanaged_device (self, (const char *) specs_iter->data)) {
				priv->unmanaged_specs = g_slist_prepend (priv->unmanaged_specs, specs_iter->data);
			} else
				g_free (specs_iter->data);
		}

		g_slist_free (specs);
	}

	g_object_notify (G_OBJECT (self), NM_SYSCONFIG_SETTINGS_UNMANAGED_SPECS);
}

static void
hostname_changed (NMSystemConfigInterface *config,
                  GParamSpec *pspec,
                  gpointer user_data)
{
	g_object_notify (G_OBJECT (user_data), NM_SYSCONFIG_SETTINGS_HOSTNAME);
}

static void
add_plugin (NMSysconfigSettings *self, NMSystemConfigInterface *plugin)
{
	NMSysconfigSettingsPrivate *priv;
	char *pname = NULL;
	char *pinfo = NULL;

	g_return_if_fail (NM_IS_SYSCONFIG_SETTINGS (self));
	g_return_if_fail (NM_IS_SYSTEM_CONFIG_INTERFACE (plugin));

	priv = NM_SYSCONFIG_SETTINGS_GET_PRIVATE (self);

	priv->plugins = g_slist_append (priv->plugins, g_object_ref (plugin));

	g_signal_connect (plugin, NM_SYSTEM_CONFIG_INTERFACE_CONNECTION_ADDED,
	                  G_CALLBACK (plugin_connection_added), self);
	g_signal_connect (plugin, "notify::hostname", G_CALLBACK (hostname_changed), self);

	nm_system_config_interface_init (plugin, NULL);

	g_object_get (G_OBJECT (plugin),
	              NM_SYSTEM_CONFIG_INTERFACE_NAME, &pname,
	              NM_SYSTEM_CONFIG_INTERFACE_INFO, &pinfo,
	              NULL);

	g_signal_connect (plugin, NM_SYSTEM_CONFIG_INTERFACE_UNMANAGED_SPECS_CHANGED,
	                  G_CALLBACK (unmanaged_specs_changed), self);

	nm_log_info (LOGD_SYS_SET, "Loaded plugin %s: %s", pname, pinfo);
	g_free (pname);
	g_free (pinfo);
}

static GObject *
find_plugin (GSList *list, const char *pname)
{
	GSList *iter;
	GObject *obj = NULL;

	g_return_val_if_fail (pname != NULL, FALSE);

	for (iter = list; iter && !obj; iter = g_slist_next (iter)) {
		NMSystemConfigInterface *plugin = NM_SYSTEM_CONFIG_INTERFACE (iter->data);
		char *list_pname = NULL;

		g_object_get (G_OBJECT (plugin),
		              NM_SYSTEM_CONFIG_INTERFACE_NAME,
		              &list_pname,
		              NULL);
		if (list_pname && !strcmp (pname, list_pname))
			obj = G_OBJECT (plugin);

		g_free (list_pname);
	}

	return obj;
}

static gboolean
load_plugins (NMSysconfigSettings *self, const char *plugins, GError **error)
{
	GSList *list = NULL;
	char **plist;
	char **iter;
	gboolean success = TRUE;

	plist = g_strsplit (plugins, ",", 0);
	if (!plist)
		return FALSE;

	for (iter = plist; *iter; iter++) {
		GModule *plugin;
		char *full_name, *path;
		const char *pname = g_strstrip (*iter);
		GObject *obj;
		GObject * (*factory_func) (void);

		/* ifcfg-fedora was renamed ifcfg-rh; handle old configs here */
		if (!strcmp (pname, "ifcfg-fedora"))
			pname = "ifcfg-rh";

		obj = find_plugin (list, pname);
		if (obj)
			continue;

		full_name = g_strdup_printf ("nm-settings-plugin-%s", pname);
		path = g_module_build_path (PLUGINDIR, full_name);

		plugin = g_module_open (path, G_MODULE_BIND_LOCAL);
		if (!plugin) {
			g_set_error (error, 0, 0,
			             "Could not load plugin '%s': %s",
			             pname, g_module_error ());
			g_free (full_name);
			g_free (path);
			success = FALSE;
			break;
		}

		g_free (full_name);
		g_free (path);

		if (!g_module_symbol (plugin, "nm_system_config_factory", (gpointer) (&factory_func))) {
			g_set_error (error, 0, 0,
			             "Could not find plugin '%s' factory function.",
			             pname);
			success = FALSE;
			break;
		}

		obj = (*factory_func) ();
		if (!obj || !NM_IS_SYSTEM_CONFIG_INTERFACE (obj)) {
			g_set_error (error, 0, 0,
			             "Plugin '%s' returned invalid system config object.",
			             pname);
			success = FALSE;
			break;
		}

		g_module_make_resident (plugin);
		g_object_weak_ref (obj, (GWeakNotify) g_module_close, plugin);
		add_plugin (self, NM_SYSTEM_CONFIG_INTERFACE (obj));
		list = g_slist_append (list, obj);
	}

	g_strfreev (plist);

	g_slist_foreach (list, (GFunc) g_object_unref, NULL);
	g_slist_free (list);

	return success;
}

static void
session_monitor_changed_cb (NMSessionMonitor *monitor, gpointer user_data)
{
	NMSysconfigSettings *self = NM_SYSCONFIG_SETTINGS (user_data);
	NMSysconfigSettingsPrivate *priv = NM_SYSCONFIG_SETTINGS_GET_PRIVATE (self);
	GHashTableIter iter;
	gpointer data;

	/* Update visibility on all connections */
	g_hash_table_iter_init (&iter, priv->all_connections);
	while (g_hash_table_iter_next (&iter, NULL, &data)) {
	}
}

static void
connection_removed (NMSysconfigConnection *connection,
                    gpointer user_data)
{
	NMSysconfigSettingsPrivate *priv = NM_SYSCONFIG_SETTINGS_GET_PRIVATE (user_data);

	g_hash_table_remove (priv->visible_connections, connection);
}

static void
connection_purged (NMSysconfigConnection *connection,
                   gpointer user_data)
{
	NMSysconfigSettingsPrivate *priv = NM_SYSCONFIG_SETTINGS_GET_PRIVATE (user_data);

	g_hash_table_remove (priv->all_connections, connection);
}

static void
claim_connection (NMSysconfigSettings *self,
                  NMSysconfigConnection *connection,
                  gboolean do_export)
{
	NMSysconfigSettingsPrivate *priv = NM_SYSCONFIG_SETTINGS_GET_PRIVATE (self);

	g_return_if_fail (NM_IS_SYSCONFIG_SETTINGS (self));
	g_return_if_fail (NM_IS_SYSCONFIG_CONNECTION (connection));

	if (g_hash_table_lookup (priv->all_connections, connection))
		/* A plugin is lying to us. */
		return;

	g_hash_table_insert (priv->all_connections, g_object_ref (connection), GINT_TO_POINTER (1));
	g_signal_connect (connection,
	                  NM_SYSCONFIG_CONNECTION_REMOVED,
	                  G_CALLBACK (connection_removed),
	                  self);
	g_signal_connect (connection,
	                  NM_SYSCONFIG_CONNECTION_PURGED,
	                  G_CALLBACK (connection_purged),
	                  self);

	if (nm_sysconfig_connection_is_visible (connection)) {
		g_hash_table_insert (priv->visible_connections, connection, GINT_TO_POINTER (1));
		g_signal_emit (self, signals[NEW_CONNECTION], 0, connection);
	}
}

// TODO it seems that this is only ever used to remove a
// NMDefaultWiredConnection, and it probably needs to stay that way. So this
// *needs* a better name!
static void
remove_connection (NMSysconfigSettings *self,
                   NMSysconfigConnection *connection,
                   gboolean do_signal)
{
	NMSysconfigSettingsPrivate *priv = NM_SYSCONFIG_SETTINGS_GET_PRIVATE (self);

	if (g_hash_table_lookup (priv->visible_connections, connection)) {
		g_signal_emit_by_name (G_OBJECT (connection), NM_SYSCONFIG_CONNECTION_REMOVED);
		g_hash_table_remove (priv->visible_connections, connection);
	}

	if (g_hash_table_lookup (priv->all_connections, connection)) {
		g_signal_emit_by_name (G_OBJECT (connection), NM_SYSCONFIG_CONNECTION_PURGED);
		g_hash_table_remove (priv->all_connections, connection);
	}
}

typedef struct {
	NMSysconfigSettings *self;
	DBusGMethodInvocation *context;
	PolkitSubject *subject;
	GCancellable *cancellable;
	gboolean disposed;

	NMConnection *connection;
	gpointer callback_data;

	char *hostname;
} PolkitCall;

#include "nm-dbus-manager.h"

static PolkitCall *
polkit_call_new (NMSysconfigSettings *self,
                 DBusGMethodInvocation *context,
                 NMConnection *connection,
                 const char *hostname)
{
	PolkitCall *call;
	char *sender;

	g_return_val_if_fail (self != NULL, NULL);
	g_return_val_if_fail (context != NULL, NULL);

	call = g_malloc0 (sizeof (PolkitCall));
	call->self = self;
	call->cancellable = g_cancellable_new ();
	call->context = context;
	if (connection)
		call->connection = g_object_ref (connection);
	if (hostname)
		call->hostname = g_strdup (hostname);

 	sender = dbus_g_method_get_sender (context);
	call->subject = polkit_system_bus_name_new (sender);
	g_free (sender);

	return call;
}

static void
polkit_call_free (PolkitCall *call)
{
	if (call->connection)
		g_object_unref (call->connection);
	g_object_unref (call->cancellable);
	g_free (call->hostname);
	g_object_unref (call->subject);
	g_free (call);
}

static gboolean
add_new_connection (NMSysconfigSettings *self,
                    NMConnection *connection,
                    GError **error)
{
	NMSysconfigSettingsPrivate *priv = NM_SYSCONFIG_SETTINGS_GET_PRIVATE (self);
	GError *tmp_error = NULL, *last_error = NULL;
	GSList *iter;
	gboolean success = FALSE;

	/* Here's how it works:
	   1) plugin writes a connection.
	   2) plugin notices that a new connection is available for reading.
	   3) plugin reads the new connection (the one it wrote in 1) and emits 'connection-added' signal.
	   4) NMSysconfigSettings receives the signal and adds it to it's connection list.
	*/

	for (iter = priv->plugins; iter && !success; iter = iter->next) {
		success = nm_system_config_interface_add_connection (NM_SYSTEM_CONFIG_INTERFACE (iter->data),
		                                                     connection,
		                                                     &tmp_error);
		g_clear_error (&last_error);
		if (!success) {
			last_error = tmp_error;
			tmp_error = NULL;
		}
	}

	if (!success)
		*error = last_error;
	return success;
}

static void
pk_add_cb (GObject *object, GAsyncResult *result, gpointer user_data)
{
	PolkitCall *call = user_data;
	NMSysconfigSettings *self = call->self;
	NMSysconfigSettingsPrivate *priv;
	PolkitAuthorizationResult *pk_result;
	GError *error = NULL, *add_error = NULL;

	/* If NMSysconfigSettings is already gone, do nothing */
	if (call->disposed) {
		error = g_error_new_literal (NM_SYSCONFIG_SETTINGS_ERROR,
		                             NM_SYSCONFIG_SETTINGS_ERROR_GENERAL,
		                             "Request was canceled.");
		dbus_g_method_return_error (call->context, error);
		g_error_free (error);
		polkit_call_free (call);
		return;
	}

	priv = NM_SYSCONFIG_SETTINGS_GET_PRIVATE (self);

	priv->pk_calls = g_slist_remove (priv->pk_calls, call);

	pk_result = polkit_authority_check_authorization_finish (priv->authority,
	                                                         result,
	                                                         &error);
	/* Some random error happened */
	if (error) {
		dbus_g_method_return_error (call->context, error);
		goto out;
	}

	/* Caller didn't successfully authenticate */
	if (!polkit_authorization_result_get_is_authorized (pk_result)) {
		error = g_error_new_literal (NM_SYSCONFIG_SETTINGS_ERROR,
		                             NM_SYSCONFIG_SETTINGS_ERROR_NOT_PRIVILEGED,
		                             "Insufficient privileges.");
		dbus_g_method_return_error (call->context, error);
		goto out;
	}

	if (add_new_connection (self, call->connection, &add_error))
		dbus_g_method_return (call->context);
	else {
		error = g_error_new (NM_SYSCONFIG_SETTINGS_ERROR,
		                     NM_SYSCONFIG_SETTINGS_ERROR_ADD_FAILED,
		                     "Saving connection failed: (%d) %s",
		                     add_error ? add_error->code : -1,
		                     (add_error && add_error->message) ? add_error->message : "(unknown)");
		g_error_free (add_error);
		dbus_g_method_return_error (call->context, error);
	}

out:
	g_clear_error (&error);
	polkit_call_free (call);
	if (pk_result)
		g_object_unref (pk_result);
}

static void
impl_settings_add_connection (NMSysconfigSettings *self,
                              GHashTable *settings,
                              DBusGMethodInvocation *context)
{
	NMSysconfigSettingsPrivate *priv = NM_SYSCONFIG_SETTINGS_GET_PRIVATE (self);
	PolkitCall *call;
	NMConnection *connection;
	GError *error = NULL;

	connection = nm_connection_new_from_hash (settings, &error);
	if (!connection) {
		g_assert (error);
		dbus_g_method_return_error (context, error);
		g_error_free (error);
		return;
	}

	/* Do any of the plugins support adding? */
	if (!get_plugin (self, NM_SYSTEM_CONFIG_INTERFACE_CAP_MODIFY_CONNECTIONS)) {
		error = g_error_new_literal (NM_SYSCONFIG_SETTINGS_ERROR,
		                             NM_SYSCONFIG_SETTINGS_ERROR_ADD_NOT_SUPPORTED,
		                             "None of the registered plugins support add.");
		dbus_g_method_return_error (context, error);
		g_error_free (error);
		return;
	}

	call = polkit_call_new (self, context, connection, NULL);
	g_assert (call);
	polkit_authority_check_authorization (priv->authority,
	                                      call->subject,
	                                      NM_AUTH_PERMISSION_SETTINGS_CONNECTION_MODIFY,
	                                      NULL,
	                                      POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION,
	                                      call->cancellable,
	                                      pk_add_cb,
	                                      call);
	priv->pk_calls = g_slist_append (priv->pk_calls, call);

	g_object_unref (connection);
}

static void
pk_hostname_cb (GObject *object, GAsyncResult *result, gpointer user_data)
{
	PolkitCall *call = user_data;
	NMSysconfigSettings *self = call->self;
	NMSysconfigSettingsPrivate *priv;
	PolkitAuthorizationResult *pk_result;
	GError *error = NULL;
	GSList *iter;
	gboolean success = FALSE;

	/* If our NMSysconfigConnection is already gone, do nothing */
	if (call->disposed) {
		error = g_error_new_literal (NM_SYSCONFIG_SETTINGS_ERROR,
		                             NM_SYSCONFIG_SETTINGS_ERROR_GENERAL,
		                             "Request was canceled.");
		dbus_g_method_return_error (call->context, error);
		g_error_free (error);
		polkit_call_free (call);
		return;
	}

	priv = NM_SYSCONFIG_SETTINGS_GET_PRIVATE (self);

	priv->pk_calls = g_slist_remove (priv->pk_calls, call);

	pk_result = polkit_authority_check_authorization_finish (priv->authority,
	                                                         result,
	                                                         &error);
	/* Some random error happened */
	if (error) {
		dbus_g_method_return_error (call->context, error);
		goto out;
	}

	/* Caller didn't successfully authenticate */
	if (!polkit_authorization_result_get_is_authorized (pk_result)) {
		error = g_error_new_literal (NM_SYSCONFIG_SETTINGS_ERROR,
		                             NM_SYSCONFIG_SETTINGS_ERROR_NOT_PRIVILEGED,
		                             "Insufficient privileges.");
		dbus_g_method_return_error (call->context, error);
		goto out;
	}

	/* Set the hostname in all plugins */
	for (iter = priv->plugins; iter; iter = iter->next) {
		NMSystemConfigInterfaceCapabilities caps = NM_SYSTEM_CONFIG_INTERFACE_CAP_NONE;

		g_object_get (G_OBJECT (iter->data), NM_SYSTEM_CONFIG_INTERFACE_CAPABILITIES, &caps, NULL);
		if (caps & NM_SYSTEM_CONFIG_INTERFACE_CAP_MODIFY_HOSTNAME) {
			g_object_set (G_OBJECT (iter->data), NM_SYSTEM_CONFIG_INTERFACE_HOSTNAME, call->hostname, NULL);
			success = TRUE;
		}
	}

	if (success) {
		dbus_g_method_return (call->context);
	} else {
		error = g_error_new_literal (NM_SYSCONFIG_SETTINGS_ERROR,
		                             NM_SYSCONFIG_SETTINGS_ERROR_SAVE_HOSTNAME_FAILED,
		                             "Saving the hostname failed.");
		dbus_g_method_return_error (call->context, error);
	}

out:
	g_clear_error (&error);
	polkit_call_free (call);
	if (pk_result)
		g_object_unref (pk_result);
}

static void
impl_settings_save_hostname (NMSysconfigSettings *self,
                             const char *hostname,
                             DBusGMethodInvocation *context)
{
	NMSysconfigSettingsPrivate *priv = NM_SYSCONFIG_SETTINGS_GET_PRIVATE (self);
	PolkitCall *call;
	GError *error = NULL;

	/* Do any of the plugins support setting the hostname? */
	if (!get_plugin (self, NM_SYSTEM_CONFIG_INTERFACE_CAP_MODIFY_HOSTNAME)) {
		error = g_error_new_literal (NM_SYSCONFIG_SETTINGS_ERROR,
		                             NM_SYSCONFIG_SETTINGS_ERROR_SAVE_HOSTNAME_NOT_SUPPORTED,
		                             "None of the registered plugins support setting the hostname.");
		dbus_g_method_return_error (context, error);
		g_error_free (error);
		return;
	}

	call = polkit_call_new (self, context, NULL, hostname);
	g_assert (call);
	polkit_authority_check_authorization (priv->authority,
	                                      call->subject,
	                                      NM_AUTH_PERMISSION_SETTINGS_HOSTNAME_MODIFY,
	                                      NULL,
	                                      POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION,
	                                      call->cancellable,
	                                      pk_hostname_cb,
	                                      call);
	priv->pk_calls = g_slist_append (priv->pk_calls, call);
}

static gboolean
have_connection_for_device (NMSysconfigSettings *self, GByteArray *mac)
{
	NMSysconfigSettingsPrivate *priv = NM_SYSCONFIG_SETTINGS_GET_PRIVATE (self);
	GHashTableIter iter;
	gpointer key;
	NMSettingConnection *s_con;
	NMSettingWired *s_wired;
	const GByteArray *setting_mac;
	gboolean ret = FALSE;

	g_return_val_if_fail (NM_IS_SYSCONFIG_SETTINGS (self), FALSE);
	g_return_val_if_fail (mac != NULL, FALSE);

	/* Find a wired connection locked to the given MAC address, if any */
	g_hash_table_iter_init (&iter, priv->visible_connections);
	while (g_hash_table_iter_next (&iter, &key, NULL)) {
		NMConnection *connection = NM_CONNECTION (key);
		const char *connection_type;

		s_con = NM_SETTING_CONNECTION (nm_connection_get_setting (connection, NM_TYPE_SETTING_CONNECTION));
		connection_type = nm_setting_connection_get_connection_type (s_con);

		if (   strcmp (connection_type, NM_SETTING_WIRED_SETTING_NAME)
		    && strcmp (connection_type, NM_SETTING_PPPOE_SETTING_NAME))
			continue;

		s_wired = (NMSettingWired *) nm_connection_get_setting (connection, NM_TYPE_SETTING_WIRED);

		/* No wired setting; therefore the PPPoE connection applies to any device */
		if (!s_wired && !strcmp (connection_type, NM_SETTING_PPPOE_SETTING_NAME)) {
			ret = TRUE;
			break;
		}

		setting_mac = nm_setting_wired_get_mac_address (s_wired);
		if (setting_mac) {
			/* A connection mac-locked to this device */
			if (!memcmp (setting_mac->data, mac->data, ETH_ALEN)) {
				ret = TRUE;
				break;
			}
		} else {
			/* A connection that applies to any wired device */
			ret = TRUE;
			break;
		}
	}

	return ret;
}

/* Search through the list of blacklisted MAC addresses in the config file. */
static gboolean
is_mac_auto_wired_blacklisted (NMSysconfigSettings *self, const GByteArray *mac)
{
	NMSysconfigSettingsPrivate *priv = NM_SYSCONFIG_SETTINGS_GET_PRIVATE (self);
	GKeyFile *config;
	char **list, **iter;
	gboolean found = FALSE;

	g_return_val_if_fail (mac != NULL, FALSE);

	if (!priv->config_file)
		return FALSE;

	config = g_key_file_new ();
	if (!config) {
		nm_log_warn (LOGD_SYS_SET, "not enough memory to load config file.");
		return FALSE;
	}

	g_key_file_set_list_separator (config, ',');
	if (!g_key_file_load_from_file (config, priv->config_file, G_KEY_FILE_NONE, NULL))
		goto out;

	list = g_key_file_get_string_list (config, "main", CONFIG_KEY_NO_AUTO_DEFAULT, NULL, NULL);
	for (iter = list; iter && *iter; iter++) {
		struct ether_addr *candidate;

		if (strcmp(g_strstrip(*iter), "*") == 0) {
			found = TRUE;
			break;
		}

		candidate = ether_aton (*iter);
		if (candidate && !memcmp (mac->data, candidate->ether_addr_octet, ETH_ALEN)) {
			found = TRUE;
			break;
		}
	}

	if (list)
		g_strfreev (list);

out:
	g_key_file_free (config);
	return found;
}

#define DEFAULT_WIRED_TAG "default-wired"

static void
default_wired_deleted (NMDefaultWiredConnection *wired,
                       const GByteArray *mac,
                       NMSysconfigSettings *self)
{
	NMSysconfigSettingsPrivate *priv = NM_SYSCONFIG_SETTINGS_GET_PRIVATE (self);
	NMSettingConnection *s_con;
	char *tmp;
	GKeyFile *config;
	char **list, **iter, **updated;
	gboolean found = FALSE;
	gsize len = 0, i;
	char *data;

	/* If there was no config file specified, there's nothing to do */
	if (!priv->config_file)
		goto cleanup;

	/* When the default wired connection is removed (either deleted or saved
	 * to a new persistent connection by a plugin), write the MAC address of
	 * the wired device to the config file and don't create a new default wired
	 * connection for that device again.
	 */

	s_con = (NMSettingConnection *) nm_connection_get_setting (NM_CONNECTION (wired),
	                                                           NM_TYPE_SETTING_CONNECTION);
	g_assert (s_con);

	/* Ignore removals of read-only connections, since they couldn't have
	 * been removed by the user.
	 */
	if (nm_setting_connection_get_read_only (s_con))
		goto cleanup;

	config = g_key_file_new ();
	if (!config)
		goto cleanup;

	g_key_file_set_list_separator (config, ',');
	g_key_file_load_from_file (config, priv->config_file, G_KEY_FILE_KEEP_COMMENTS, NULL);

	list = g_key_file_get_string_list (config, "main", CONFIG_KEY_NO_AUTO_DEFAULT, &len, NULL);
	for (iter = list; iter && *iter; iter++) {
		struct ether_addr *candidate;

		if (strcmp(g_strstrip(*iter), "*") == 0) {
			found = TRUE;
			break;
		}

		candidate = ether_aton (*iter);
		if (candidate && !memcmp (mac->data, candidate->ether_addr_octet, ETH_ALEN)) {
			found = TRUE;
			break;
		}
	}

	/* Add this device's MAC to the list */
	if (!found) {
		tmp = g_strdup_printf ("%02x:%02x:%02x:%02x:%02x:%02x",
		                       mac->data[0], mac->data[1], mac->data[2],
		                       mac->data[3], mac->data[4], mac->data[5]);

		/* New list; size + 1 for the new element, + 1 again for ending NULL */
		updated = g_malloc0 (sizeof (char*) * (len + 2));

		/* Copy original list and add new MAC */
		for (i = 0; list && list[i]; i++)
			updated[i] = list[i];
		updated[i++] = tmp;
		updated[i] = NULL;

		g_key_file_set_string_list (config,
		                            "main", CONFIG_KEY_NO_AUTO_DEFAULT,
		                            (const char **) updated,
		                            len + 2);
		/* g_free() not g_strfreev() since 'updated' isn't a deep-copy */
		g_free (updated);
		g_free (tmp);

		data = g_key_file_to_data (config, &len, NULL);
		if (data) {
			g_file_set_contents (priv->config_file, data, len, NULL);
			g_free (data);
		}
	}

	if (list)
		g_strfreev (list);
	g_key_file_free (config);

cleanup:
	g_object_set_data (G_OBJECT (nm_default_wired_connection_get_device (wired)),
	                   DEFAULT_WIRED_TAG,
	                   NULL);
}

static void
delete_cb (NMSysconfigConnection *connection, GError *error, gpointer user_data)
{
}

static gboolean
default_wired_try_update (NMDefaultWiredConnection *wired,
                          NMSysconfigSettings *self)
{
	GError *error = NULL;
	NMSettingConnection *s_con;
	const char *id;

	/* Try to move this default wired conneciton to a plugin so that it has
	 * persistent storage.
	 */

	s_con = (NMSettingConnection *) nm_connection_get_setting (NM_CONNECTION (wired),
	                                                           NM_TYPE_SETTING_CONNECTION);
	g_assert (s_con);
	id = nm_setting_connection_get_id (s_con);
	g_assert (id);

	remove_connection (self, NM_SYSCONFIG_CONNECTION (wired), FALSE);
	if (add_new_connection (self, NM_CONNECTION (wired), &error)) {
		nm_sysconfig_connection_delete (NM_SYSCONFIG_CONNECTION (wired),
		                                delete_cb,
		                                NULL);

		g_object_set_data (G_OBJECT (nm_default_wired_connection_get_device (wired)),
		                   DEFAULT_WIRED_TAG,
		                   NULL);
		nm_log_info (LOGD_SYS_SET, "Saved default wired connection '%s' to persistent storage", id);
		return FALSE;
	}

	nm_log_warn (LOGD_SYS_SET, "couldn't save default wired connection '%s': %d / %s",
	             id,
	             error ? error->code : -1,
	             (error && error->message) ? error->message : "(unknown)");

	/* If there was an error, don't destroy the default wired connection,
	 * but add it back to the system settings service. Connection is already
	 * exported on the bus, don't export it again, thus do_export == FALSE.
	 */
	claim_connection (self, NM_SYSCONFIG_CONNECTION (wired), FALSE);
	return TRUE;
}

void
nm_sysconfig_settings_device_added (NMSysconfigSettings *self, NMDevice *device)
{
	GByteArray *mac = NULL;
	struct ether_addr tmp;
	NMDefaultWiredConnection *wired;
	NMSettingConnection *s_con;
	gboolean read_only = TRUE;
	const char *id;

	if (nm_device_get_device_type (device) != NM_DEVICE_TYPE_ETHERNET)
		return;

	/* If the device isn't managed or it already has a default wired connection,
	 * ignore it.
	 */
	if (   !nm_device_get_managed (device)
	    || g_object_get_data (G_OBJECT (device), DEFAULT_WIRED_TAG))
		return;

	nm_device_ethernet_get_address (NM_DEVICE_ETHERNET (device), &tmp);

	mac = g_byte_array_sized_new (ETH_ALEN);
	g_byte_array_append (mac, tmp.ether_addr_octet, ETH_ALEN);

	if (   have_connection_for_device (self, mac)
	    || is_mac_auto_wired_blacklisted (self, mac))
		goto ignore;

	if (get_plugin (self, NM_SYSTEM_CONFIG_INTERFACE_CAP_MODIFY_CONNECTIONS))
		read_only = FALSE;

	wired = nm_default_wired_connection_new (mac, device, read_only);
	if (!wired)
		goto ignore;

	s_con = (NMSettingConnection *) nm_connection_get_setting (NM_CONNECTION (wired),
	                                                           NM_TYPE_SETTING_CONNECTION);
	g_assert (s_con);
	id = nm_setting_connection_get_id (s_con);
	g_assert (id);

	nm_log_info (LOGD_SYS_SET, "Added default wired connection '%s' for %s",
	             id, nm_device_get_udi (device));

	g_signal_connect (wired, "try-update", (GCallback) default_wired_try_update, self);
	g_signal_connect (wired, "deleted", (GCallback) default_wired_deleted, self);
	claim_connection (self, NM_SYSCONFIG_CONNECTION (wired), TRUE);
	g_object_unref (wired);

	g_object_set_data (G_OBJECT (device), DEFAULT_WIRED_TAG, wired);

ignore:
	g_byte_array_free (mac, TRUE);
}

void
nm_sysconfig_settings_device_removed (NMSysconfigSettings *self, NMDevice *device)
{
	NMDefaultWiredConnection *connection;

	if (nm_device_get_device_type (device) != NM_DEVICE_TYPE_ETHERNET)
		return;

	connection = (NMDefaultWiredConnection *) g_object_get_data (G_OBJECT (device), DEFAULT_WIRED_TAG);
	if (connection)
		remove_connection (self, NM_SYSCONFIG_CONNECTION (connection), TRUE);
}

NMSysconfigSettings *
nm_sysconfig_settings_new (const char *config_file,
                           const char *plugins,
                           GError **error)
{
	NMSysconfigSettings *self;
	NMSysconfigSettingsPrivate *priv;
	NMDBusManager *dbus_mgr;
	DBusGConnection *bus;

	self = g_object_new (NM_TYPE_SYSCONFIG_SETTINGS,
	                     NULL);
	if (!self)
		return NULL;

	priv = NM_SYSCONFIG_SETTINGS_GET_PRIVATE (self);

	priv->config_file = g_strdup (config_file);

	if (plugins) {
		/* Load the plugins; fail if a plugin is not found. */
		if (!load_plugins (self, plugins, error)) {
			g_object_unref (self);
			return NULL;
		}
		unmanaged_specs_changed (NULL, self);
	}

	dbus_mgr = nm_dbus_manager_get ();
	bus = nm_dbus_manager_get_connection (dbus_mgr);
	dbus_g_connection_register_g_object (bus, NM_DBUS_PATH_SETTINGS, G_OBJECT (self));
	g_object_unref (dbus_mgr);

	return self;
}

/***************************************************************/

static void
dispose (GObject *object)
{
	NMSysconfigSettings *self = NM_SYSCONFIG_SETTINGS (object);
	NMSysconfigSettingsPrivate *priv = NM_SYSCONFIG_SETTINGS_GET_PRIVATE (self);
	GSList *iter;

	if (priv->auth_changed_id) {
		g_signal_handler_disconnect (priv->authority, priv->auth_changed_id);
		priv->auth_changed_id = 0;
	}

	/* Cancel PolicyKit requests */
	for (iter = priv->pk_calls; iter; iter = g_slist_next (iter)) {
		PolkitCall *call = iter->data;

		call->disposed = TRUE;
		g_cancellable_cancel (call->cancellable);
	}
	g_slist_free (priv->pk_calls);
	priv->pk_calls = NULL;

	if (priv->session_monitor) {
		g_signal_handler_disconnect (priv->session_monitor, priv->session_monitor_id);
		g_object_unref (priv->session_monitor);
		priv->session_monitor = NULL;
	}

	G_OBJECT_CLASS (nm_sysconfig_settings_parent_class)->dispose (object);
}

static void
finalize (GObject *object)
{
	NMSysconfigSettings *self = NM_SYSCONFIG_SETTINGS (object);
	NMSysconfigSettingsPrivate *priv = NM_SYSCONFIG_SETTINGS_GET_PRIVATE (self);

	g_hash_table_destroy (priv->visible_connections);
	g_hash_table_destroy (priv->all_connections);

	clear_unmanaged_specs (self);

	g_slist_foreach (priv->plugins, (GFunc) g_object_unref, NULL);
	g_slist_free (priv->plugins);

	g_free (priv->config_file);

	G_OBJECT_CLASS (nm_sysconfig_settings_parent_class)->finalize (object);
}

static void
get_property (GObject *object, guint prop_id,
			  GValue *value, GParamSpec *pspec)
{
	NMSysconfigSettings *self = NM_SYSCONFIG_SETTINGS (object);
	const GSList *specs, *iter;
	GSList *copy = NULL;

	switch (prop_id) {
	case PROP_UNMANAGED_SPECS:
		specs = nm_sysconfig_settings_get_unmanaged_specs (self);
		for (iter = specs; iter; iter = g_slist_next (iter))
			copy = g_slist_append (copy, g_strdup (iter->data));
		g_value_take_boxed (value, copy);
		break;
	case PROP_HOSTNAME:
		g_value_take_string (value, nm_sysconfig_settings_get_hostname (self));

		/* Don't ever pass NULL through D-Bus */
		if (!g_value_get_string (value))
			g_value_set_static_string (value, "");
		break;
	case PROP_CAN_MODIFY:
		g_value_set_boolean (value, !!get_plugin (self, NM_SYSTEM_CONFIG_INTERFACE_CAP_MODIFY_CONNECTIONS));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
nm_sysconfig_settings_class_init (NMSysconfigSettingsClass *class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (class);
	
	g_type_class_add_private (class, sizeof (NMSysconfigSettingsPrivate));

	/* virtual methods */
	object_class->notify = notify;
	object_class->get_property = get_property;
	object_class->dispose = dispose;
	object_class->finalize = finalize;

	/* properties */

	g_object_class_install_property
		(object_class, PROP_UNMANAGED_SPECS,
		 g_param_spec_boxed (NM_SYSCONFIG_SETTINGS_UNMANAGED_SPECS,
							 "Unamanged device specs",
							 "Unmanaged device specs",
							 DBUS_TYPE_G_LIST_OF_STRING,
							 G_PARAM_READABLE));

	g_object_class_install_property
		(object_class, PROP_HOSTNAME,
		 g_param_spec_string (NM_SYSCONFIG_SETTINGS_HOSTNAME,
		                      "Hostname",
		                      "Persistent hostname",
		                      NULL,
		                      G_PARAM_READABLE));

	g_object_class_install_property
		(object_class, PROP_CAN_MODIFY,
		 g_param_spec_boolean (NM_SYSCONFIG_SETTINGS_CAN_MODIFY,
		                       "CanModify",
		                       "Can modify anything (hostname, connections, etc)",
		                       FALSE,
		                       G_PARAM_READABLE));

	/* signals */
	signals[PROPERTIES_CHANGED] = 
	                g_signal_new ("properties-changed",
	                              G_OBJECT_CLASS_TYPE (object_class),
	                              G_SIGNAL_RUN_FIRST,
	                              G_STRUCT_OFFSET (NMSysconfigSettingsClass, properties_changed),
	                              NULL, NULL,
	                              g_cclosure_marshal_VOID__BOXED,
	                              G_TYPE_NONE, 1, DBUS_TYPE_G_MAP_OF_VARIANT);
	signals[NEW_CONNECTION] = 
	                g_signal_new (NM_SYSCONFIG_SETTINGS_NEW_CONNECTION,
	                              G_OBJECT_CLASS_TYPE (object_class),
	                              G_SIGNAL_RUN_FIRST,
	                              0,
	                              NULL, NULL,
	                              g_cclosure_marshal_VOID__OBJECT,
	                              G_TYPE_NONE, 1, G_TYPE_OBJECT);

	dbus_g_error_domain_register (NM_SYSCONFIG_SETTINGS_ERROR,
	                              NM_DBUS_IFACE_SETTINGS,
	                              NM_TYPE_SYSCONFIG_SETTINGS_ERROR);

	/* And register all the settings errors with D-Bus */
	dbus_g_error_domain_register (NM_CONNECTION_ERROR, NULL, NM_TYPE_CONNECTION_ERROR);
	dbus_g_error_domain_register (NM_SETTING_802_1X_ERROR, NULL, NM_TYPE_SETTING_802_1X_ERROR);
	dbus_g_error_domain_register (NM_SETTING_BLUETOOTH_ERROR, NULL, NM_TYPE_SETTING_BLUETOOTH_ERROR);
	dbus_g_error_domain_register (NM_SETTING_CDMA_ERROR, NULL, NM_TYPE_SETTING_CDMA_ERROR);
	dbus_g_error_domain_register (NM_SETTING_CONNECTION_ERROR, NULL, NM_TYPE_SETTING_CONNECTION_ERROR);
	dbus_g_error_domain_register (NM_SETTING_GSM_ERROR, NULL, NM_TYPE_SETTING_GSM_ERROR);
	dbus_g_error_domain_register (NM_SETTING_IP4_CONFIG_ERROR, NULL, NM_TYPE_SETTING_IP4_CONFIG_ERROR);
	dbus_g_error_domain_register (NM_SETTING_IP6_CONFIG_ERROR, NULL, NM_TYPE_SETTING_IP6_CONFIG_ERROR);
	dbus_g_error_domain_register (NM_SETTING_OLPC_MESH_ERROR, NULL, NM_TYPE_SETTING_OLPC_MESH_ERROR);
	dbus_g_error_domain_register (NM_SETTING_PPP_ERROR, NULL, NM_TYPE_SETTING_PPP_ERROR);
	dbus_g_error_domain_register (NM_SETTING_PPPOE_ERROR, NULL, NM_TYPE_SETTING_PPPOE_ERROR);
	dbus_g_error_domain_register (NM_SETTING_SERIAL_ERROR, NULL, NM_TYPE_SETTING_SERIAL_ERROR);
	dbus_g_error_domain_register (NM_SETTING_VPN_ERROR, NULL, NM_TYPE_SETTING_VPN_ERROR);
	dbus_g_error_domain_register (NM_SETTING_WIRED_ERROR, NULL, NM_TYPE_SETTING_WIRED_ERROR);
	dbus_g_error_domain_register (NM_SETTING_WIRELESS_SECURITY_ERROR, NULL, NM_TYPE_SETTING_WIRELESS_SECURITY_ERROR);
	dbus_g_error_domain_register (NM_SETTING_WIRELESS_ERROR, NULL, NM_TYPE_SETTING_WIRELESS_ERROR);
	dbus_g_error_domain_register (NM_SETTING_ERROR, NULL, NM_TYPE_SETTING_ERROR);

	dbus_g_object_type_install_info (NM_TYPE_SYSCONFIG_SETTINGS,
	                                 &dbus_glib_nm_settings_object_info);

}

static void
nm_sysconfig_settings_init (NMSysconfigSettings *self)
{
	NMSysconfigSettingsPrivate *priv = NM_SYSCONFIG_SETTINGS_GET_PRIVATE (self);
	GError *error = NULL;

	priv->visible_connections = g_hash_table_new (g_direct_hash, g_direct_equal);
	priv->all_connections = g_hash_table_new_full (g_direct_hash, g_direct_equal, g_object_unref, NULL);

	priv->authority = polkit_authority_get_sync (NULL, &error);
	if (!priv->authority) {
		nm_log_warn (LOGD_SYS_SET, "failed to create PolicyKit authority: (%d) %s",
		             error ? error->code : -1,
		             error && error->message ? error->message : "(unknown)");
		g_clear_error (&error);
	}

	priv->session_monitor = nm_session_monitor_get ();
	g_assert (priv->session_monitor);
	priv->session_monitor_id = g_signal_connect (priv->session_monitor,
	                                             "changed",
	                                             G_CALLBACK (session_monitor_changed_cb),
	                                             self);
}

