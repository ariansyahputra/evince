/* ev-metadata.c
 *  this file is part of evince, a gnome document viewer
 *
 * Copyright (C) 2009 Carlos Garcia Campos  <carlosgc@gnome.org>
 * Copyright © 2010 Christian Persch
 *
 * Evince is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Evince is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "config.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

#define EV_DBUS_DAEMON_NAME             "org.gnome.evince.Daemon"
#define EV_DBUS_DAEMON_INTERFACE_NAME   "org.gnome.evince.Daemon"
#define EV_DBUS_DAEMON_OBJECT_PATH      "/org/gnome/evince/Daemon"

#define DAEMON_TIMEOUT (30) /* seconds */

static GList *ev_daemon_docs = NULL;
static guint kill_timer_id;

typedef struct {
	gchar *dbus_name;
	gchar *uri;
        guint watch_id;
} EvDoc;

static void
ev_doc_free (EvDoc *doc)
{
	if (!doc)
		return;

	g_free (doc->dbus_name);
	g_free (doc->uri);

        g_bus_unwatch_name (doc->watch_id);

	g_free (doc);
}

static EvDoc *
ev_daemon_find_doc (const gchar *uri)
{
	GList *l;

	for (l = ev_daemon_docs; l != NULL; l = l->next) {
		EvDoc *doc = (EvDoc *)l->data;

		if (strcmp (doc->uri, uri) == 0)
			return doc;
	}

	return NULL;
}

static void
ev_daemon_stop_killtimer (void)
{
	if (kill_timer_id != 0)
		g_source_remove (kill_timer_id);
	kill_timer_id = 0;
}

static gboolean
ev_daemon_shutdown (gpointer user_data)
{
        GMainLoop *loop = (GMainLoop *) user_data;

        if (g_main_loop_is_running (loop))
                g_main_loop_quit (loop);

        return FALSE;
}

static void
ev_daemon_maybe_start_killtimer (gpointer data)
{
	ev_daemon_stop_killtimer ();
        if (ev_daemon_docs != NULL)
                return;

	kill_timer_id = g_timeout_add_seconds (DAEMON_TIMEOUT,
                                               (GSourceFunc) ev_daemon_shutdown,
                                               data);
}

static gboolean
convert_metadata (const gchar *metadata)
{
	GFile   *file;
	char    *argv[3];
	gint     exit_status;
	GFileAttributeInfoList *namespaces;
	gboolean supported = FALSE;
	GError  *error = NULL;
	gboolean retval;

	/* If metadata is not supported for a local file
	 * is likely because and old gvfs version is running.
	 */
	file = g_file_new_for_path (metadata);
	namespaces = g_file_query_writable_namespaces (file, NULL, NULL);
	if (namespaces) {
		gint i;

		for (i = 0; i < namespaces->n_infos; i++) {
			if (strcmp (namespaces->infos[i].name, "metadata") == 0) {
				supported = TRUE;
				break;
			}
		}
		g_file_attribute_info_list_unref (namespaces);
	}
	if (!supported) {
		g_warning ("GVFS metadata not supported. "
			   "Evince will run without metadata support.\n");
		g_object_unref (file);
		return FALSE;
	}
	g_object_unref (file);

	argv[0] = g_build_filename (LIBEXECDIR, "evince-convert-metadata", NULL);
	argv[1] = (char *) metadata;
	argv[2] = NULL;

	retval = g_spawn_sync (NULL /* wd */, argv, NULL /* env */,
			       0, NULL, NULL, NULL, NULL,
			       &exit_status, &error);
	g_free (argv[0]);

	if (!retval) {
		g_printerr ("Error migrating metadata: %s\n", error->message);
		g_error_free (error);
	}

	return retval && WIFEXITED (exit_status) && WEXITSTATUS (exit_status) == 0;
}

static void
ev_migrate_metadata (void)
{
	gchar       *updated;
	gchar       *metadata;
	gchar       *dot_dir;
	const gchar *userdir;

	userdir = g_getenv ("GNOME22_USER_DIR");
	if (userdir) {
		dot_dir = g_build_filename (userdir, "evince", NULL);
	} else {
		dot_dir = g_build_filename (g_get_home_dir (),
					    ".gnome2",
					    "evince",
					    NULL);
	}

	updated = g_build_filename (dot_dir, "migrated-to-gvfs", NULL);
	if (g_file_test (updated, G_FILE_TEST_EXISTS)) {
		/* Already migrated */
		g_free (updated);
		g_free (dot_dir);
		return;
	}

	metadata = g_build_filename (dot_dir, "ev-metadata.xml", NULL);
	if (g_file_test (metadata, G_FILE_TEST_EXISTS)) {
		if (convert_metadata (metadata)) {
			gint fd;

			fd = g_creat (updated, 0600);
			if (fd != -1) {
				close (fd);
			}
		}
	}

	g_free (dot_dir);
	g_free (updated);
	g_free (metadata);
}

static void
name_appeared_cb (GDBusConnection *connection,
                  const gchar     *name,
                  const gchar     *name_owner,
                  gpointer         user_data)
{
}

static void
name_vanished_cb (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
	GList *l;

        for (l = ev_daemon_docs; l != NULL; l = l->next) {
                EvDoc *doc = (EvDoc *) l->data;

                if (strcmp (doc->dbus_name, name) != 0)
                        continue;

                ev_daemon_docs = g_list_delete_link (ev_daemon_docs, l);
                ev_doc_free (doc);
                
                ev_daemon_maybe_start_killtimer (user_data);
                return;
        }
}

static void
method_call_cb (GDBusConnection       *connection,
                const gchar           *sender,
                const gchar           *object_path,
                const gchar           *interface_name,
                const gchar           *method_name,
                GVariant              *parameters,
                GDBusMethodInvocation *invocation,
                gpointer               user_data)
{
        if (g_strcmp0 (interface_name, EV_DBUS_DAEMON_INTERFACE_NAME) != 0)
                return;

        if (g_strcmp0 (method_name, "RegisterDocument") == 0) {
                EvDoc       *doc;
                const gchar *uri;
                const gchar *owner = NULL;

                g_variant_get (parameters, "(&s)", &uri);

                doc = ev_daemon_find_doc (uri);
                if (doc) {
                        /* Already registered */
                        owner = doc->dbus_name;
                } else {
                        ev_daemon_stop_killtimer ();

                        doc = g_new (EvDoc, 1);
                        doc->dbus_name = g_strdup (sender);
                        doc->uri = g_strdup (uri);

                        doc->watch_id = g_bus_watch_name (G_BUS_TYPE_STARTER,
                                                          sender,
                                                          G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                          name_appeared_cb,
                                                          name_vanished_cb,
                                                          user_data, NULL);

                        ev_daemon_docs = g_list_prepend (ev_daemon_docs, doc);
                }

                g_dbus_method_invocation_return_value (invocation,
                                                       g_variant_new_string (owner));
                return;

        } else if (g_strcmp0 (method_name, "UnregisterDocument") == 0) {
                EvDoc *doc;
                const gchar *uri;

                g_variant_get (parameters, "(&s)", &uri);

                doc = ev_daemon_find_doc (uri);
                if (doc == NULL) {
                        g_dbus_method_invocation_return_error_literal (invocation,
                                                                       G_DBUS_ERROR,
                                                                       G_DBUS_ERROR_INVALID_ARGS,
                                                                       "URI not registered");
                        return;
                }

                if (strcmp (doc->dbus_name, sender) != 0) {
                        g_dbus_method_invocation_return_error_literal (invocation,
                                                                       G_DBUS_ERROR,
                                                                       G_DBUS_ERROR_BAD_ADDRESS,
                                                                       "Only owner can call this method");
                        return;
                }

                ev_daemon_docs = g_list_remove (ev_daemon_docs, doc);
                ev_doc_free (doc);
                ev_daemon_maybe_start_killtimer (user_data);

                g_dbus_method_invocation_return_value (invocation, g_variant_new ("()"));
                return;
        }
}

static void
name_acquired_cb (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
	ev_migrate_metadata ();

	ev_daemon_maybe_start_killtimer (user_data);
}

static void
name_lost_cb (GDBusConnection *connection,
              const gchar     *name,
              gpointer         user_data)
{
          GMainLoop *loop = (GMainLoop *) user_data;

          /* Failed to acquire the name; exit daemon */
          if (g_main_loop_is_running (loop))
                  g_main_loop_quit (loop);
}

static const char introspection_xml[] =
  "<node>"
    "<interface name='org.gnome.evince.Daemon'>"
      "<method name='RegisterDocument'>"
        "<arg type='s' name='uri' direction='in'/>"
        "<arg type='s' name='owner' direction='out'/>"
      "</method>"
      "<method name='UnregisterDocument'>"
        "<arg type='s' name='uri' direction='in'/>"
      "</method>"
    "</interface>"
  "</node>";

static const GDBusInterfaceVTable interface_vtable = {
  method_call_cb,
  NULL,
  NULL
};

gint
main (gint argc, gchar **argv)
{
        GDBusConnection *connection;
	GMainLoop *loop;
        GError *error = NULL;
        guint registration_id, owner_id;
        GDBusNodeInfo *introspection_data;

	g_type_init ();

        connection = g_bus_get_sync (G_BUS_TYPE_STARTER, NULL, &error);
        if (connection == NULL) {
                g_printerr ("Failed to get bus connection: %s\n", error->message);
                g_error_free (error);
                return 1;
        }

        introspection_data = g_dbus_node_info_new_for_xml (introspection_xml, NULL);
        g_assert (introspection_data != NULL);

	loop = g_main_loop_new (NULL, FALSE);

        registration_id = g_dbus_connection_register_object (connection,
                                                             EV_DBUS_DAEMON_OBJECT_PATH,
                                                             EV_DBUS_DAEMON_NAME,
                                                             introspection_data->interfaces[0],
                                                             &interface_vtable,
                                                             g_main_loop_ref (loop),
                                                             (GDestroyNotify) g_main_loop_unref,
                                                             &error);
        if (registration_id == 0) {
                g_printerr ("Failed to register object: %s\n", error->message);
                g_error_free (error);
                g_object_unref (connection);
                return 1;
        }

        owner_id = g_bus_own_name_on_connection (connection,
                                                 EV_DBUS_DAEMON_NAME,
                                                 G_BUS_NAME_OWNER_FLAGS_NONE,
                                                 name_acquired_cb,
                                                 name_lost_cb,
                                                 g_main_loop_ref (loop),
                                                 (GDestroyNotify) g_main_loop_unref);

        g_main_loop_run (loop);

        g_bus_unown_name (owner_id);

        g_main_loop_unref (loop);
        g_dbus_node_info_unref (introspection_data);
        g_list_foreach (ev_daemon_docs, (GFunc)ev_doc_free, NULL);
        g_list_free (ev_daemon_docs);
        g_object_unref (connection);

	return 0;
}
