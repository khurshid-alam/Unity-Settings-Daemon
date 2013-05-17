/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2001-2003 Bastien Nocera <hadess@hadess.net>
 * Copyright (C) 2006-2007 William Jon McCann <mccann@jhu.edu>
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#include "config.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <math.h>

#include <locale.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <gio/gdesktopappinfo.h>
#include <gio/gunixfdlist.h>

#ifdef HAVE_GUDEV
#include <gudev/gudev.h>
#endif

#include "gnome-settings-plugin.h"
#include "gnome-settings-session.h"
#include "gnome-settings-profile.h"
#include "gsd-marshal.h"
#include "gsd-media-keys-manager.h"

#include "shortcuts-list.h"
#include "shell-key-grabber.h"
#include "gsd-screenshot-utils.h"
#include "gsd-input-helper.h"
#include "gsd-enums.h"

#include <canberra.h>
#include <pulse/pulseaudio.h>
#include "gvc-mixer-control.h"
#include "gvc-mixer-sink.h"

#define GSD_MEDIA_KEYS_DBUS_PATH GSD_DBUS_PATH "/MediaKeys"
#define GSD_MEDIA_KEYS_DBUS_NAME GSD_DBUS_NAME ".MediaKeys"

#define GNOME_KEYRING_DBUS_NAME "org.gnome.keyring"
#define GNOME_KEYRING_DBUS_PATH "/org/gnome/keyring/daemon"
#define GNOME_KEYRING_DBUS_INTERFACE "org.gnome.keyring.Daemon"

#define GS_DBUS_NAME                            "org.gnome.ScreenSaver"
#define GS_DBUS_PATH                            "/org/gnome/ScreenSaver"
#define GS_DBUS_INTERFACE                       "org.gnome.ScreenSaver"

#define SHELL_DBUS_NAME "org.gnome.Shell"
#define SHELL_DBUS_PATH "/org/gnome/Shell"

#define CUSTOM_BINDING_SCHEMA SETTINGS_BINDING_DIR ".custom-keybinding"

#define SHELL_GRABBER_RETRY_INTERVAL 1

static const gchar introspection_xml[] =
"<node name='/org/gnome/SettingsDaemon/MediaKeys'>"
"  <interface name='org.gnome.SettingsDaemon.MediaKeys'>"
"    <annotation name='org.freedesktop.DBus.GLib.CSymbol' value='gsd_media_keys_manager'/>"
"    <method name='GrabMediaPlayerKeys'>"
"      <arg name='application' direction='in' type='s'/>"
"      <arg name='time' direction='in' type='u'/>"
"    </method>"
"    <method name='ReleaseMediaPlayerKeys'>"
"      <arg name='application' direction='in' type='s'/>"
"    </method>"
"    <signal name='MediaPlayerKeyPressed'>"
"      <arg name='application' type='s'/>"
"      <arg name='key' type='s'/>"
"    </signal>"
"  </interface>"
"</node>";

#define SETTINGS_INTERFACE_DIR "org.gnome.desktop.interface"
#define SETTINGS_POWER_DIR "org.gnome.settings-daemon.plugins.power"
#define SETTINGS_XSETTINGS_DIR "org.gnome.settings-daemon.plugins.xsettings"
#define SETTINGS_TOUCHPAD_DIR "org.gnome.settings-daemon.peripherals.touchpad"
#define TOUCHPAD_ENABLED_KEY "touchpad-enabled"
#define HIGH_CONTRAST "HighContrast"

#define VOLUME_STEP 6           /* percents for one volume button press */
#define MAX_VOLUME 65536.0

#define SYSTEMD_DBUS_NAME                       "org.freedesktop.login1"
#define SYSTEMD_DBUS_PATH                       "/org/freedesktop/login1"
#define SYSTEMD_DBUS_INTERFACE                  "org.freedesktop.login1.Manager"

#define GSD_MEDIA_KEYS_MANAGER_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), GSD_TYPE_MEDIA_KEYS_MANAGER, GsdMediaKeysManagerPrivate))

typedef struct {
        char   *application;
        char   *dbus_name;
        guint32 time;
        guint   watch_id;
} MediaPlayer;

typedef struct {
        MediaKeyType key_type;
        ShellKeyBindingMode modes;
        const char *settings_key;
        const char *hard_coded;
        char *custom_path;
        char *custom_command;
        guint accel_id;
} MediaKey;

typedef struct {
        GsdMediaKeysManager *manager;
        MediaKey *key;
} GrabData;

struct GsdMediaKeysManagerPrivate
{
        /* Volume bits */
        GvcMixerControl *volume;
        GvcMixerStream  *sink;
        GvcMixerStream  *source;
        ca_context      *ca;
        GtkSettings     *gtksettings;
#ifdef HAVE_GUDEV
        GHashTable      *streams; /* key = X device ID, value = stream id */
        GUdevClient     *udev_client;
#endif /* HAVE_GUDEV */

        GSettings       *settings;
        GHashTable      *custom_settings;

        GPtrArray       *keys;

        /* HighContrast theme settings */
        GSettings       *interface_settings;
        char            *icon_theme;
        char            *gtk_theme;

        /* Power stuff */
        GSettings       *power_settings;
        GDBusProxy      *power_proxy;
        GDBusProxy      *power_screen_proxy;
        GDBusProxy      *power_keyboard_proxy;

        /* Shell stuff */
        guint            name_owner_id;
        GDBusProxy      *shell_proxy;
        ShellKeyGrabber *key_grabber;
        GCancellable    *shell_cancellable;
        GCancellable    *grab_cancellable;

        /* systemd stuff */
        GDBusProxy      *logind_proxy;
        gint             inhibit_keys_fd;

        GList           *media_players;

        GDBusNodeInfo   *introspection_data;
        GDBusConnection *connection;
        GCancellable    *bus_cancellable;
        GDBusProxy      *xrandr_proxy;
        GCancellable    *cancellable;

        guint            start_idle_id;
};

static void     gsd_media_keys_manager_class_init  (GsdMediaKeysManagerClass *klass);
static void     gsd_media_keys_manager_init        (GsdMediaKeysManager      *media_keys_manager);
static void     gsd_media_keys_manager_finalize    (GObject                  *object);
static void     register_manager                   (GsdMediaKeysManager      *manager);
static void     custom_binding_changed             (GSettings           *settings,
                                                    const char          *settings_key,
                                                    GsdMediaKeysManager *manager);
static void     grab_media_keys                    (GsdMediaKeysManager *manager);
static void     grab_media_key                     (MediaKey            *key,
                                                    GsdMediaKeysManager *manager);
static void     ungrab_media_key                   (MediaKey            *key,
                                                    GsdMediaKeysManager *manager);
G_DEFINE_TYPE (GsdMediaKeysManager, gsd_media_keys_manager, G_TYPE_OBJECT)

static gpointer manager_object = NULL;


static void
media_key_free (MediaKey *key)
{
        if (key == NULL)
                return;
        g_free (key->custom_path);
        g_free (key->custom_command);
        g_free (key);
}

static char *
get_term_command (GsdMediaKeysManager *manager)
{
        char *cmd_term, *cmd_args;;
        char *cmd = NULL;
        GSettings *settings;

        settings = g_settings_new ("org.gnome.desktop.default-applications.terminal");
        cmd_term = g_settings_get_string (settings, "exec");
        if (cmd_term[0] == '\0')
                cmd_term = g_strdup ("gnome-terminal");

        cmd_args = g_settings_get_string (settings, "exec-arg");
        if (strcmp (cmd_term, "") != 0) {
                cmd = g_strdup_printf ("%s %s -e", cmd_term, cmd_args);
        } else {
                cmd = g_strdup_printf ("%s -e", cmd_term);
        }

        g_free (cmd_args);
        g_free (cmd_term);
        g_object_unref (settings);

        return cmd;
}

static char **
get_keyring_env (GsdMediaKeysManager *manager)
{
	GError *error = NULL;
	GVariant *variant, *item;
	GVariantIter *iter;
	char **envp;

	variant = g_dbus_connection_call_sync (manager->priv->connection,
					       GNOME_KEYRING_DBUS_NAME,
					       GNOME_KEYRING_DBUS_PATH,
					       GNOME_KEYRING_DBUS_INTERFACE,
					       "GetEnvironment",
					       NULL,
					       NULL,
					       G_DBUS_CALL_FLAGS_NONE,
					       -1,
					       NULL,
					       &error);
	if (variant == NULL) {
		g_warning ("Failed to call GetEnvironment on keyring daemon: %s", error->message);
		g_error_free (error);
		return NULL;
	}

	envp = g_get_environ ();
	envp = g_environ_unsetenv (envp, "DESKTOP_AUTOSTART_ID");

	g_variant_get (variant, "(a{ss})", &iter);

	while ((item = g_variant_iter_next_value (iter))) {
		char *key;
		char *value;

		g_variant_get (item,
			       "{ss}",
			       &key,
			       &value);

		envp = g_environ_setenv (envp, key, value, TRUE);

		g_variant_unref (item);
		g_free (key);
		g_free (value);
	}

	g_variant_iter_free (iter);
	g_variant_unref (variant);

	return envp;
}

static void
execute (GsdMediaKeysManager *manager,
         char                *cmd,
         gboolean             need_term)
{
        gboolean retval;
        char   **argv;
        int      argc;
        char    *exec;
        char    *term = NULL;
        GError  *error = NULL;

        retval = FALSE;

        if (need_term)
                term = get_term_command (manager);

        if (term) {
                exec = g_strdup_printf ("%s %s", term, cmd);
                g_free (term);
        } else {
                exec = g_strdup (cmd);
        }

        if (g_shell_parse_argv (exec, &argc, &argv, NULL)) {
		char   **envp;

		envp = get_keyring_env (manager);

                retval = g_spawn_async (g_get_home_dir (),
                                        argv,
                                        envp,
                                        G_SPAWN_SEARCH_PATH,
                                        NULL,
                                        NULL,
                                        NULL,
                                        &error);

                g_strfreev (argv);
                g_strfreev (envp);
        }

        if (retval == FALSE) {
                g_warning ("Couldn't execute command: %s: %s", exec, error->message);
                g_error_free (error);
        }
        g_free (exec);
}

static char *
get_key_string (GsdMediaKeysManager *manager,
		MediaKey            *key)
{
	if (key->settings_key != NULL)
		return g_settings_get_string (manager->priv->settings, key->settings_key);
	else if (key->hard_coded != NULL)
		return g_strdup (key->hard_coded);
	else if (key->custom_path != NULL) {
                GSettings *settings;

                settings = g_hash_table_lookup (manager->priv->custom_settings,
                                                key->custom_path);
		return g_settings_get_string (settings, "binding");
	} else
		g_assert_not_reached ();
}

static void
ensure_cancellable (GCancellable **cancellable)
{
        if (*cancellable == NULL) {
                *cancellable = g_cancellable_new ();
                g_object_add_weak_pointer (G_OBJECT (*cancellable),
                                           (gpointer *)cancellable);
        } else {
                g_object_ref (*cancellable);
        }
}

static void
show_osd (GsdMediaKeysManager *manager,
          const char          *icon,
          const char          *label,
          int                  level)
{
        GVariantBuilder builder;

        if (manager->priv->shell_proxy == NULL)
                return;

        g_variant_builder_init (&builder, G_VARIANT_TYPE_TUPLE);
        g_variant_builder_open (&builder, G_VARIANT_TYPE_VARDICT);
        if (icon)
                g_variant_builder_add (&builder, "{sv}",
                                       "icon", g_variant_new_string (icon));
        if (label)
                g_variant_builder_add (&builder, "{sv}",
                                       "label", g_variant_new_string (label));
        if (level >= 0)
                g_variant_builder_add (&builder, "{sv}",
                                       "level", g_variant_new_int32 (level));
        g_variant_builder_close (&builder);

        g_dbus_proxy_call (manager->priv->shell_proxy,
                           "ShowOSD",
                           g_variant_builder_end (&builder),
                           G_DBUS_CALL_FLAGS_NO_AUTO_START,
                           -1,
                           manager->priv->shell_cancellable,
                           NULL, NULL);
}

static const char *
get_icon_name_for_volume (gboolean is_mic,
                          gboolean muted,
                          int volume)
{
        static const char *icon_names[] = {
                "audio-volume-muted-symbolic",
                "audio-volume-low-symbolic",
                "audio-volume-medium-symbolic",
                "audio-volume-high-symbolic",
                NULL
        };
        static const char *mic_icon_names[] = {
                "microphone-sensitivity-muted-symbolic",
                "microphone-sensitivity-low-symbolic",
                "microphone-sensitivity-medium-symbolic",
                "microphone-sensitivity-high-symbolic",
                NULL
        };
        int n;

        if (muted) {
                n = 0;
        } else {
                /* select image */
                n = 3 * volume / 100 + 1;
                if (n < 1) {
                        n = 1;
                } else if (n > 3) {
                        n = 3;
                }
        }

	if (is_mic)
		return mic_icon_names[n];
	else
		return icon_names[n];
}

static gboolean
retry_grabs (gpointer data)
{
        GsdMediaKeysManager *manager = data;

        grab_media_keys (manager);
        return FALSE;
}

static void
grab_accelerators_complete (GObject      *object,
                            GAsyncResult *result,
                            gpointer      user_data)
{
        GVariant *actions;
        gboolean retry = FALSE;
        GError *error = NULL;
        GsdMediaKeysManager *manager = user_data;

        shell_key_grabber_call_grab_accelerators_finish (SHELL_KEY_GRABBER (object),
                                                         &actions, result, &error);

        if (error) {
                retry = (error->code == G_DBUS_ERROR_UNKNOWN_METHOD);
                if (!retry)
                        g_warning ("%d: %s", error->code, error->message);
                g_error_free (error);
        } else {
                int i;
                for (i = 0; i < manager->priv->keys->len; i++) {
                        MediaKey *key;

                        key = g_ptr_array_index (manager->priv->keys, i);
                        g_variant_get_child (actions, i, "u", &key->accel_id);
                }
        }

        if (retry)
                g_timeout_add_seconds (SHELL_GRABBER_RETRY_INTERVAL,
                                       retry_grabs, manager);
}

static void
grab_media_keys (GsdMediaKeysManager *manager)
{
        GVariantBuilder builder;
        int i;

        g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(su)"));

        for (i = 0; i < manager->priv->keys->len; i++) {
                MediaKey *key;
                char *tmp;

                key = g_ptr_array_index (manager->priv->keys, i);
                tmp = get_key_string (manager, key);
                g_variant_builder_add (&builder, "(su)", tmp, key->modes);
                g_free (tmp);
        }

	shell_key_grabber_call_grab_accelerators (manager->priv->key_grabber,
	                                          g_variant_builder_end (&builder),
	                                          manager->priv->grab_cancellable,
	                                          grab_accelerators_complete,
	                                          manager);
}

static void
grab_accelerator_complete (GObject      *object,
                           GAsyncResult *result,
                           gpointer      user_data)
{
        GrabData *data = user_data;
        MediaKey *key = data->key;

        shell_key_grabber_call_grab_accelerator_finish (SHELL_KEY_GRABBER (object),
                                                        &key->accel_id, result, NULL);

        g_slice_free (GrabData, data);
}

static void
grab_media_key (MediaKey            *key,
		GsdMediaKeysManager *manager)
{
	GrabData *data;
	char *tmp;

	ungrab_media_key (key, manager);

	tmp = get_key_string (manager, key);

	data = g_slice_new0 (GrabData);
	data->manager = manager;
	data->key = key;

	shell_key_grabber_call_grab_accelerator (manager->priv->key_grabber,
	                                         tmp, key->modes,
	                                         manager->priv->grab_cancellable,
	                                         grab_accelerator_complete,
	                                         data);

	g_free (tmp);
}

static void
ungrab_accelerator_complete (GObject      *object,
                             GAsyncResult *result,
                             gpointer      user_data)
{
	GsdMediaKeysManager *manager = user_data;
	shell_key_grabber_call_ungrab_accelerator_finish (SHELL_KEY_GRABBER (object),
	                                                  NULL, result, NULL);
}

static void
ungrab_media_key (MediaKey            *key,
                  GsdMediaKeysManager *manager)
{
	if (key->accel_id == 0)
		return;

	shell_key_grabber_call_ungrab_accelerator (manager->priv->key_grabber,
	                                           key->accel_id,
	                                           manager->priv->grab_cancellable,
	                                           ungrab_accelerator_complete,
	                                           manager);
	key->accel_id = 0;
}

static void
gsettings_changed_cb (GSettings           *settings,
                      const gchar         *settings_key,
                      GsdMediaKeysManager *manager)
{
        int      i;

        /* Give up if we don't have proxy to the shell */
        if (!manager->priv->key_grabber)
                return;

	/* handled in gsettings_custom_changed_cb() */
        if (g_str_equal (settings_key, "custom-keybindings"))
		return;

        /* Find the key that was modified */
        for (i = 0; i < manager->priv->keys->len; i++) {
                MediaKey *key;

                key = g_ptr_array_index (manager->priv->keys, i);

                /* Skip over hard-coded and GConf keys */
                if (key->settings_key == NULL)
                        continue;
                if (strcmp (settings_key, key->settings_key) == 0) {
                        grab_media_key (key, manager);
                        break;
                }
        }
}

static MediaKey *
media_key_new_for_path (GsdMediaKeysManager *manager,
			char                *path)
{
        GSettings *settings;
        char *command, *binding;
        MediaKey *key;

        g_debug ("media_key_new_for_path: %s", path);

	settings = g_hash_table_lookup (manager->priv->custom_settings, path);
	if (settings == NULL) {
		settings = g_settings_new_with_path (CUSTOM_BINDING_SCHEMA, path);

		g_signal_connect (settings, "changed",
				  G_CALLBACK (custom_binding_changed), manager);
		g_hash_table_insert (manager->priv->custom_settings,
				     g_strdup (path), settings);
	}

        command = g_settings_get_string (settings, "command");
        binding = g_settings_get_string (settings, "binding");

        if (*command == '\0' && *binding == '\0') {
                g_debug ("Key binding (%s) is incomplete", path);
                g_free (command);
                g_free (binding);
                return NULL;
        }
        g_free (binding);

        key = g_new0 (MediaKey, 1);
        key->key_type = CUSTOM_KEY;
        key->modes = GSD_KEYBINDING_MODE_LAUNCHER;
        key->custom_path = g_strdup (path);
        key->custom_command = command;

        return key;
}

static void
update_custom_binding (GsdMediaKeysManager *manager,
                       char                *path)
{
        MediaKey *key;
        int i;

        /* Remove the existing key */
        for (i = 0; i < manager->priv->keys->len; i++) {
                key = g_ptr_array_index (manager->priv->keys, i);

                if (key->custom_path == NULL)
                        continue;
                if (strcmp (key->custom_path, path) == 0) {
                        g_debug ("Removing custom key binding %s", path);
                        ungrab_media_key (key, manager);
                        g_ptr_array_remove_index_fast (manager->priv->keys, i);
                        break;
                }
        }

        /* And create a new one! */
        key = media_key_new_for_path (manager, path);
        if (key) {
                g_debug ("Adding new custom key binding %s", path);
                g_ptr_array_add (manager->priv->keys, key);

                grab_media_key (key, manager);
        }
}

static void
custom_binding_changed (GSettings           *settings,
                        const char          *settings_key,
                        GsdMediaKeysManager *manager)
{
        char *path;

        if (strcmp (settings_key, "name") == 0)
                return; /* we don't care */

        g_object_get (settings, "path", &path, NULL);
        update_custom_binding (manager, path);
        g_free (path);
}

static void
gsettings_custom_changed_cb (GSettings           *settings,
                             const char          *settings_key,
                             GsdMediaKeysManager *manager)
{
        char **bindings;
        int i, j, n_bindings;

        bindings = g_settings_get_strv (settings, settings_key);
        n_bindings = g_strv_length (bindings);

        /* Handle additions */
        for (i = 0; i < n_bindings; i++) {
                if (g_hash_table_lookup (manager->priv->custom_settings,
                                         bindings[i]))
                        continue;
                update_custom_binding (manager, bindings[i]);
        }

        /* Handle removals */
        for (i = 0; i < manager->priv->keys->len; i++) {
                gboolean found = FALSE;
                MediaKey *key = g_ptr_array_index (manager->priv->keys, i);
                if (key->key_type != CUSTOM_KEY)
                        continue;

                for (j = 0; j < n_bindings && !found; j++)
                        found = strcmp (bindings[j], key->custom_path) == 0;

                if (found)
                        continue;

                ungrab_media_key (key, manager);
                g_hash_table_remove (manager->priv->custom_settings,
                                     key->custom_path);
                g_ptr_array_remove_index_fast (manager->priv->keys, i);
                --i; /* make up for the removed key */
        }
        g_strfreev (bindings);
}

static void
add_key (GsdMediaKeysManager *manager, guint i)
{
	MediaKey *key;

	key = g_new0 (MediaKey, 1);
	key->key_type = media_keys[i].key_type;
	key->settings_key = media_keys[i].settings_key;
	key->hard_coded = media_keys[i].hard_coded;
	key->modes = media_keys[i].modes;

	g_ptr_array_add (manager->priv->keys, key);
}

static void
init_kbd (GsdMediaKeysManager *manager)
{
        char **custom_paths;
        int i;

        gnome_settings_profile_start (NULL);

        /* Media keys
         * Add hard-coded shortcuts first so that they can't be preempted */
        for (i = 0; i < G_N_ELEMENTS (media_keys); i++) {
                if (media_keys[i].hard_coded)
                        add_key (manager, i);
        }
        for (i = 0; i < G_N_ELEMENTS (media_keys); i++) {
                if (media_keys[i].hard_coded == NULL)
                        add_key (manager, i);
        }

        /* Custom shortcuts */
        custom_paths = g_settings_get_strv (manager->priv->settings,
                                            "custom-keybindings");

        for (i = 0; i < g_strv_length (custom_paths); i++) {
                MediaKey *key;

                g_debug ("Setting up custom keybinding %s", custom_paths[i]);

                key = media_key_new_for_path (manager, custom_paths[i]);
                if (!key) {
                        continue;
                }
                g_ptr_array_add (manager->priv->keys, key);
        }
        g_strfreev (custom_paths);

        grab_media_keys (manager);

        gnome_settings_profile_end (NULL);
}

static void
launch_app (GAppInfo *app_info,
	    gint64    timestamp)
{
	GError *error = NULL;
        GdkAppLaunchContext *launch_context;

        /* setup the launch context so the startup notification is correct */
        launch_context = gdk_display_get_app_launch_context (gdk_display_get_default ());
        gdk_app_launch_context_set_timestamp (launch_context, timestamp);

	if (!g_app_info_launch (app_info, NULL, G_APP_LAUNCH_CONTEXT (launch_context), &error)) {
		g_warning ("Could not launch '%s': %s",
			   g_app_info_get_commandline (app_info),
			   error->message);
		g_error_free (error);
	}
        g_object_unref (launch_context);
}

static void
do_url_action (GsdMediaKeysManager *manager,
               const char          *scheme,
               gint64               timestamp)
{
        GAppInfo *app_info;

        app_info = g_app_info_get_default_for_uri_scheme (scheme);
        if (app_info != NULL) {
                launch_app (app_info, timestamp);
                g_object_unref (app_info);
        } else {
                g_warning ("Could not find default application for '%s' scheme", scheme);
	}
}

static void
do_media_action (GsdMediaKeysManager *manager,
		 gint64               timestamp)
{
        GAppInfo *app_info;

        app_info = g_app_info_get_default_for_type ("audio/x-vorbis+ogg", FALSE);
        if (app_info != NULL) {
                launch_app (app_info, timestamp);
                g_object_unref (app_info);
        } else {
                g_warning ("Could not find default application for '%s' mime-type", "audio/x-vorbis+ogg");
        }
}

static void
gnome_session_shutdown (GsdMediaKeysManager *manager)
{
	GError *error = NULL;
	GVariant *variant;
        GDBusProxy *proxy;

        proxy = gnome_settings_session_get_session_proxy ();
	variant = g_dbus_proxy_call_sync (proxy,
					  "Shutdown",
					  NULL,
					  G_DBUS_CALL_FLAGS_NONE,
					  -1,
					  NULL,
					  &error);
	if (variant == NULL) {
		g_warning ("Failed to call Shutdown on session manager: %s", error->message);
		g_error_free (error);
		return;
	}
	g_variant_unref (variant);
        g_object_unref (proxy);
}

static void
do_eject_action_cb (GDrive              *drive,
                    GAsyncResult        *res,
                    GsdMediaKeysManager *manager)
{
        g_drive_eject_with_operation_finish (drive, res, NULL);
}

#define NO_SCORE 0
#define SCORE_CAN_EJECT 50
#define SCORE_HAS_MEDIA 100
static void
do_eject_action (GsdMediaKeysManager *manager)
{
        GList *drives, *l;
        GDrive *fav_drive;
        guint score;
        GVolumeMonitor *volume_monitor;

        volume_monitor = g_volume_monitor_get ();


        /* Find the best drive to eject */
        fav_drive = NULL;
        score = NO_SCORE;
        drives = g_volume_monitor_get_connected_drives (volume_monitor);
        for (l = drives; l != NULL; l = l->next) {
                GDrive *drive = l->data;

                if (g_drive_can_eject (drive) == FALSE)
                        continue;
                if (g_drive_is_media_removable (drive) == FALSE)
                        continue;
                if (score < SCORE_CAN_EJECT) {
                        fav_drive = drive;
                        score = SCORE_CAN_EJECT;
                }
                if (g_drive_has_media (drive) == FALSE)
                        continue;
                if (score < SCORE_HAS_MEDIA) {
                        fav_drive = drive;
                        score = SCORE_HAS_MEDIA;
                        break;
                }
        }

        /* Show OSD */
        show_osd (manager, "media-eject-symbolic", NULL, -1);

        /* Clean up the drive selection and exit if no suitable
         * drives are found */
        if (fav_drive != NULL)
                fav_drive = g_object_ref (fav_drive);

        g_list_foreach (drives, (GFunc) g_object_unref, NULL);
        if (fav_drive == NULL)
                return;

        /* Eject! */
        g_drive_eject_with_operation (fav_drive, G_MOUNT_UNMOUNT_FORCE,
                                      NULL, NULL,
                                      (GAsyncReadyCallback) do_eject_action_cb,
                                      manager);
        g_object_unref (fav_drive);
        g_object_unref (volume_monitor);
}

static void
do_home_key_action (GsdMediaKeysManager *manager,
		    gint64               timestamp)
{
	GFile *file;
	GError *error = NULL;
	char *uri;

	file = g_file_new_for_path (g_get_home_dir ());
	uri = g_file_get_uri (file);
	g_object_unref (file);

	if (gtk_show_uri (NULL, uri, timestamp, &error) == FALSE) {
		g_warning ("Failed to launch '%s': %s", uri, error->message);
		g_error_free (error);
	}
	g_free (uri);
}

static void
do_search_action (GsdMediaKeysManager *manager,
		  gint64               timestamp)
{
        g_dbus_proxy_call (manager->priv->shell_proxy,
                           "FocusSearch",
                           NULL,
                           G_DBUS_CALL_FLAGS_NO_AUTO_START,
                           -1,
                           manager->priv->shell_cancellable,
                           NULL, NULL);
}

static void
do_execute_desktop_or_desktop (GsdMediaKeysManager *manager,
			       const char          *desktop,
			       const char          *alt_desktop,
			       gint64               timestamp)
{
        GDesktopAppInfo *app_info;

        app_info = g_desktop_app_info_new (desktop);
        if (app_info == NULL)
                app_info = g_desktop_app_info_new (alt_desktop);

        if (app_info != NULL) {
                launch_app (G_APP_INFO (app_info), timestamp);
                g_object_unref (app_info);
                return;
        }

        g_warning ("Could not find application '%s' or '%s'", desktop, alt_desktop);
}

static void
do_touchpad_osd_action (GsdMediaKeysManager *manager, gboolean state)
{
        show_osd (manager, state ? "input-touchpad-symbolic"
                                 : "touchpad-disabled-symbolic", NULL, -1);
}

static void
do_touchpad_action (GsdMediaKeysManager *manager)
{
        GSettings *settings;
        gboolean state;

        if (touchpad_is_present () == FALSE) {
                do_touchpad_osd_action (manager, FALSE);
                return;
        }

        settings = g_settings_new (SETTINGS_TOUCHPAD_DIR);
        state = g_settings_get_boolean (settings, TOUCHPAD_ENABLED_KEY);

        do_touchpad_osd_action (manager, !state);

        g_settings_set_boolean (settings, TOUCHPAD_ENABLED_KEY, !state);
        g_object_unref (settings);
}

static void
do_lock_screensaver (GsdMediaKeysManager *manager)
{
        GsdMediaKeysManagerPrivate *priv = manager->priv;

        if (priv->connection == NULL) {
                g_warning ("No existing D-Bus connection trying to handle screensaver lock key");
                return;
        }
        g_dbus_connection_call (manager->priv->connection,
                                GS_DBUS_NAME,
                                GS_DBUS_PATH,
                                GS_DBUS_INTERFACE,
                                "Lock",
                                NULL, NULL,
                                G_DBUS_CALL_FLAGS_NONE, -1,
                                NULL, NULL, NULL);
}

static void
update_dialog (GsdMediaKeysManager *manager,
               GvcMixerStream      *stream,
               guint                vol,
               gboolean             muted,
               gboolean             sound_changed,
               gboolean             quiet)
{
        GvcMixerUIDevice *device;
        const GvcMixerStreamPort *port;
        const char *icon;

        if (!muted) {
                vol = (int) (100 * (double) vol / PA_VOLUME_NORM);
                vol = CLAMP (vol, 0, 100);
        } else {
                vol = 0.0;
        }

        icon = get_icon_name_for_volume (!GVC_IS_MIXER_SINK (stream), muted, vol);
        port = gvc_mixer_stream_get_port (stream);
        if (g_strcmp0 (gvc_mixer_stream_get_form_factor (stream), "internal") != 0 ||
            (port != NULL &&
             g_strcmp0 (port->port, "analog-output-speaker") != 0 &&
             g_strcmp0 (port->port, "analog-output") != 0)) {
                device = gvc_mixer_control_lookup_device_from_stream (manager->priv->volume, stream);
                show_osd (manager, icon,
                          gvc_mixer_ui_device_get_description (device), vol);
        } else {
                show_osd (manager, icon, NULL, vol);
        }

        if (quiet == FALSE && sound_changed != FALSE && muted == FALSE) {
                ca_context_change_device (manager->priv->ca,
                                          gvc_mixer_stream_get_name (stream));
                ca_context_play (manager->priv->ca, 1,
                                        CA_PROP_EVENT_ID, "audio-volume-change",
                                        CA_PROP_EVENT_DESCRIPTION, "volume changed through key press",
                                        CA_PROP_CANBERRA_CACHE_CONTROL, "permanent",
                                        NULL);
        }
}

#ifdef HAVE_GUDEV
/* PulseAudio gives us /devices/... paths, when udev
 * expects /sys/devices/... paths. */
static GUdevDevice *
get_udev_device_for_sysfs_path (GsdMediaKeysManager *manager,
				const char *sysfs_path)
{
	char *path;
	GUdevDevice *dev;

	path = g_strdup_printf ("/sys%s", sysfs_path);
	dev = g_udev_client_query_by_sysfs_path (manager->priv->udev_client, path);
	g_free (path);

	return dev;
}

static GvcMixerStream *
get_stream_for_device_id (GsdMediaKeysManager *manager,
			  gboolean             is_output,
			  guint                deviceid)
{
	char *devnode;
	gpointer id_ptr;
	GvcMixerStream *res;
	GUdevDevice *dev, *parent;
	GSList *streams, *l;

	id_ptr = g_hash_table_lookup (manager->priv->streams, GUINT_TO_POINTER (deviceid));
	if (id_ptr != NULL) {
		if (GPOINTER_TO_UINT (id_ptr) == (guint) -1)
			return NULL;
		else
			return gvc_mixer_control_lookup_stream_id (manager->priv->volume, GPOINTER_TO_UINT (id_ptr));
	}

	devnode = xdevice_get_device_node (deviceid);
	if (devnode == NULL) {
		g_debug ("Could not find device node for XInput device %d", deviceid);
		return NULL;
	}

	dev = g_udev_client_query_by_device_file (manager->priv->udev_client, devnode);
	if (dev == NULL) {
		g_debug ("Could not find udev device for device path '%s'", devnode);
		g_free (devnode);
		return NULL;
	}
	g_free (devnode);

	if (g_strcmp0 (g_udev_device_get_property (dev, "ID_BUS"), "usb") != 0) {
		g_debug ("Not handling XInput device %d, not USB", deviceid);
		g_hash_table_insert (manager->priv->streams,
				     GUINT_TO_POINTER (deviceid),
				     GUINT_TO_POINTER ((guint) -1));
		g_object_unref (dev);
		return NULL;
	}

	parent = g_udev_device_get_parent_with_subsystem (dev, "usb", "usb_device");
	if (parent == NULL) {
		g_warning ("No USB device parent for XInput device %d even though it's USB", deviceid);
		g_object_unref (dev);
		return NULL;
	}

	res = NULL;
	if (is_output)
		streams = gvc_mixer_control_get_sinks (manager->priv->volume);
	else
		streams = gvc_mixer_control_get_sources (manager->priv->volume);
	for (l = streams; l; l = l->next) {
		GvcMixerStream *stream = l->data;
		const char *sysfs_path;
		GUdevDevice *stream_dev, *stream_parent;

		sysfs_path = gvc_mixer_stream_get_sysfs_path (stream);
		stream_dev = get_udev_device_for_sysfs_path (manager, sysfs_path);
		if (stream_dev == NULL)
			continue;
		stream_parent = g_udev_device_get_parent_with_subsystem (stream_dev, "usb", "usb_device");
		g_object_unref (stream_dev);
		if (stream_parent == NULL)
			continue;

		if (g_strcmp0 (g_udev_device_get_sysfs_path (stream_parent),
			       g_udev_device_get_sysfs_path (parent)) == 0) {
			res = stream;
		}
		g_object_unref (stream_parent);
		if (res != NULL)
			break;
	}

	g_slist_free (streams);

	if (res)
		g_hash_table_insert (manager->priv->streams,
				     GUINT_TO_POINTER (deviceid),
				     GUINT_TO_POINTER (gvc_mixer_stream_get_id (res)));
	else
		g_hash_table_insert (manager->priv->streams,
				     GUINT_TO_POINTER (deviceid),
				     GUINT_TO_POINTER ((guint) -1));

	return res;
}
#endif /* HAVE_GUDEV */

static void
do_sound_action (GsdMediaKeysManager *manager,
		 guint                deviceid,
                 int                  type,
                 gboolean             is_output,
                 gboolean             quiet)
{
	GvcMixerStream *stream;
        gboolean old_muted, new_muted;
        guint old_vol, new_vol, norm_vol_step;
        gboolean sound_changed;

        /* Find the stream that corresponds to the device, if any */
        stream = NULL;
#ifdef HAVE_GUDEV
        stream = get_stream_for_device_id (manager, is_output, deviceid);
#endif /* HAVE_GUDEV */

        if (stream == NULL) {
                if (is_output)
                        stream = manager->priv->sink;
                else
                        stream = manager->priv->source;
        }

        if (stream == NULL)
                return;

        norm_vol_step = PA_VOLUME_NORM * VOLUME_STEP / 100;

        /* FIXME: this is racy */
        new_vol = old_vol = gvc_mixer_stream_get_volume (stream);
        new_muted = old_muted = gvc_mixer_stream_get_is_muted (stream);
        sound_changed = FALSE;

        switch (type) {
        case MUTE_KEY:
                new_muted = !old_muted;
                break;
        case VOLUME_DOWN_KEY:
                if (old_vol <= norm_vol_step) {
                        new_vol = 0;
                        new_muted = TRUE;
                } else {
                        new_vol = old_vol - norm_vol_step;
                }
                break;
        case VOLUME_UP_KEY:
                new_muted = FALSE;
                /* When coming out of mute only increase the volume if it was 0 */
                if (!old_muted || old_vol == 0)
                        new_vol = MIN (old_vol + norm_vol_step, MAX_VOLUME);
                break;
        }

        if (old_muted != new_muted) {
                gvc_mixer_stream_change_is_muted (stream, new_muted);
                sound_changed = TRUE;
        }

        if (old_vol != new_vol) {
                if (gvc_mixer_stream_set_volume (stream, new_vol) != FALSE) {
                        gvc_mixer_stream_push_volume (stream);
                        sound_changed = TRUE;
                }
        }

        update_dialog (manager, stream, new_vol, new_muted, sound_changed, quiet);
}

static void
sound_theme_changed (GtkSettings         *settings,
                     GParamSpec          *pspec,
                     GsdMediaKeysManager *manager)
{
        char *theme_name;

        g_object_get (G_OBJECT (manager->priv->gtksettings), "gtk-sound-theme-name", &theme_name, NULL);
        if (theme_name)
                ca_context_change_props (manager->priv->ca, CA_PROP_CANBERRA_XDG_THEME_NAME, theme_name, NULL);
        g_free (theme_name);
}

static void
update_default_sink (GsdMediaKeysManager *manager)
{
        GvcMixerStream *stream;

        stream = gvc_mixer_control_get_default_sink (manager->priv->volume);
        if (stream == manager->priv->sink)
                return;

        g_clear_object (&manager->priv->sink);

        if (stream != NULL) {
                manager->priv->sink = g_object_ref (stream);
        } else {
                g_warning ("Unable to get default sink");
        }
}

static void
update_default_source (GsdMediaKeysManager *manager)
{
        GvcMixerStream *stream;

        stream = gvc_mixer_control_get_default_source (manager->priv->volume);
        if (stream == manager->priv->source)
                return;

        g_clear_object (&manager->priv->source);

        if (stream != NULL) {
                manager->priv->source = g_object_ref (stream);
        } else {
                g_warning ("Unable to get default source");
        }
}

static void
on_control_state_changed (GvcMixerControl     *control,
                          GvcMixerControlState new_state,
                          GsdMediaKeysManager *manager)
{
        update_default_sink (manager);
        update_default_source (manager);
}

static void
on_control_default_sink_changed (GvcMixerControl     *control,
                                 guint                id,
                                 GsdMediaKeysManager *manager)
{
        update_default_sink (manager);
}

static void
on_control_default_source_changed (GvcMixerControl     *control,
                                   guint                id,
                                   GsdMediaKeysManager *manager)
{
        update_default_source (manager);
}

#ifdef HAVE_GUDEV
static gboolean
remove_stream (gpointer key,
	       gpointer value,
	       gpointer id)
{
	if (GPOINTER_TO_UINT (value) == GPOINTER_TO_UINT (id))
		return TRUE;
	return FALSE;
}
#endif /* HAVE_GUDEV */

static void
on_control_stream_removed (GvcMixerControl     *control,
                           guint                id,
                           GsdMediaKeysManager *manager)
{
        if (manager->priv->sink != NULL) {
		if (gvc_mixer_stream_get_id (manager->priv->sink) == id)
			g_clear_object (&manager->priv->sink);
        }
        if (manager->priv->source != NULL) {
		if (gvc_mixer_stream_get_id (manager->priv->source) == id)
			g_clear_object (&manager->priv->source);
        }

#ifdef HAVE_GUDEV
	g_hash_table_foreach_remove (manager->priv->streams, (GHRFunc) remove_stream, GUINT_TO_POINTER (id));
#endif
}

static void
free_media_player (MediaPlayer *player)
{
        if (player->watch_id > 0) {
                g_bus_unwatch_name (player->watch_id);
                player->watch_id = 0;
        }
        g_free (player->application);
        g_free (player->dbus_name);
        g_free (player);
}

static gint
find_by_application (gconstpointer a,
                     gconstpointer b)
{
        return strcmp (((MediaPlayer *)a)->application, b);
}

static gint
find_by_name (gconstpointer a,
              gconstpointer b)
{
        return strcmp (((MediaPlayer *)a)->dbus_name, b);
}

static gint
find_by_time (gconstpointer a,
              gconstpointer b)
{
        return ((MediaPlayer *)a)->time < ((MediaPlayer *)b)->time;
}

static void
name_vanished_handler (GDBusConnection     *connection,
                       const gchar         *name,
                       GsdMediaKeysManager *manager)
{
        GList *iter;

        iter = g_list_find_custom (manager->priv->media_players,
                                   name,
                                   find_by_name);

        if (iter != NULL) {
                MediaPlayer *player;

                player = iter->data;
                g_debug ("Deregistering vanished %s (dbus_name: %s)", player->application, player->dbus_name);
                free_media_player (player);
                manager->priv->media_players = g_list_delete_link (manager->priv->media_players, iter);
        }
}

/*
 * Register a new media player. Most applications will want to call
 * this with time = GDK_CURRENT_TIME. This way, the last registered
 * player will receive media events. In some cases, applications
 * may want to register with a lower priority (usually 1), to grab
 * events only nobody is interested.
 */
static void
gsd_media_keys_manager_grab_media_player_keys (GsdMediaKeysManager *manager,
                                               const char          *application,
                                               const char          *dbus_name,
                                               guint32              time)
{
        GList       *iter;
        MediaPlayer *media_player;
        guint        watch_id;

        if (time == GDK_CURRENT_TIME) {
                GTimeVal tv;

                g_get_current_time (&tv);
                time = tv.tv_sec * 1000 + tv.tv_usec / 1000;
        }

        iter = g_list_find_custom (manager->priv->media_players,
                                   application,
                                   find_by_application);

        if (iter != NULL) {
                if (((MediaPlayer *)iter->data)->time < time) {
                        MediaPlayer *player = iter->data;
                        free_media_player (player);
                        manager->priv->media_players = g_list_delete_link (manager->priv->media_players, iter);
                } else {
                        return;
                }
        }

        watch_id = g_bus_watch_name (G_BUS_TYPE_SESSION,
                                     dbus_name,
                                     G_BUS_NAME_WATCHER_FLAGS_NONE,
                                     NULL,
                                     (GBusNameVanishedCallback) name_vanished_handler,
                                     manager,
                                     NULL);

        g_debug ("Registering %s at %u", application, time);
        media_player = g_new0 (MediaPlayer, 1);
        media_player->application = g_strdup (application);
        media_player->dbus_name = g_strdup (dbus_name);
        media_player->time = time;
        media_player->watch_id = watch_id;

        manager->priv->media_players = g_list_insert_sorted (manager->priv->media_players,
                                                             media_player,
                                                             find_by_time);
}

static void
gsd_media_keys_manager_release_media_player_keys (GsdMediaKeysManager *manager,
                                                  const char          *application,
                                                  const char          *name)
{
        GList *iter = NULL;

        g_return_if_fail (application != NULL || name != NULL);

        if (application != NULL) {
                iter = g_list_find_custom (manager->priv->media_players,
                                           application,
                                           find_by_application);
        }

        if (iter == NULL && name != NULL) {
                iter = g_list_find_custom (manager->priv->media_players,
                                           name,
                                           find_by_name);
        }

        if (iter != NULL) {
                MediaPlayer *player;

                player = iter->data;
                g_debug ("Deregistering %s (dbus_name: %s)", application, player->dbus_name);
                free_media_player (player);
                manager->priv->media_players = g_list_delete_link (manager->priv->media_players, iter);
        }
}

static gboolean
gsd_media_player_key_pressed (GsdMediaKeysManager *manager,
                              const char          *key)
{
        const char  *application;
        gboolean     have_listeners;
        GError      *error = NULL;
        MediaPlayer *player;

        g_return_val_if_fail (key != NULL, FALSE);

        g_debug ("Media key '%s' pressed", key);

        have_listeners = (manager->priv->media_players != NULL);

        if (!have_listeners) {
                /* Popup a dialog with an (/) icon */
                show_osd (manager, "action-unavailable-symbolic", NULL, -1);
                return TRUE;
        }

        player = manager->priv->media_players->data;
        application = player->application;

        if (g_dbus_connection_emit_signal (manager->priv->connection,
                                           player->dbus_name,
                                           GSD_MEDIA_KEYS_DBUS_PATH,
                                           GSD_MEDIA_KEYS_DBUS_NAME,
                                           "MediaPlayerKeyPressed",
                                           g_variant_new ("(ss)", application ? application : "", key),
                                           &error) == FALSE) {
                g_debug ("Error emitting signal: %s", error->message);
                g_error_free (error);
        }

        return !have_listeners;
}

static void
handle_method_call (GDBusConnection       *connection,
                    const gchar           *sender,
                    const gchar           *object_path,
                    const gchar           *interface_name,
                    const gchar           *method_name,
                    GVariant              *parameters,
                    GDBusMethodInvocation *invocation,
                    gpointer               user_data)
{
        GsdMediaKeysManager *manager = (GsdMediaKeysManager *) user_data;

        g_debug ("Calling method '%s' for media-keys", method_name);

        if (g_strcmp0 (method_name, "ReleaseMediaPlayerKeys") == 0) {
                const char *app_name;

                g_variant_get (parameters, "(&s)", &app_name);
                gsd_media_keys_manager_release_media_player_keys (manager, app_name, sender);
                g_dbus_method_invocation_return_value (invocation, NULL);
        } else if (g_strcmp0 (method_name, "GrabMediaPlayerKeys") == 0) {
                const char *app_name;
                guint32 time;

                g_variant_get (parameters, "(&su)", &app_name, &time);
                gsd_media_keys_manager_grab_media_player_keys (manager, app_name, sender, time);
                g_dbus_method_invocation_return_value (invocation, NULL);
        }
}

static const GDBusInterfaceVTable interface_vtable =
{
        handle_method_call,
        NULL, /* Get Property */
        NULL, /* Set Property */
};

static gboolean
do_multimedia_player_action (GsdMediaKeysManager *manager,
                             const char          *key)
{
        return gsd_media_player_key_pressed (manager, key);
}

static void
on_xrandr_action_call_finished (GObject             *source_object,
                                GAsyncResult        *res,
                                GsdMediaKeysManager *manager)
{
        GError *error = NULL;
        GVariant *variant;
        char *action;

        action = g_object_get_data (G_OBJECT (source_object),
                                    "gsd-media-keys-manager-xrandr-action");

        variant = g_dbus_proxy_call_finish (G_DBUS_PROXY (source_object), res, &error);

        g_object_unref (manager->priv->cancellable);
        manager->priv->cancellable = NULL;

        if (error != NULL) {
                g_warning ("Unable to call '%s': %s", action, error->message);
                g_error_free (error);
        } else {
                g_variant_unref (variant);
        }

        g_free (action);
}

static void
do_xrandr_action (GsdMediaKeysManager *manager,
                  const char          *action,
                  gint64               timestamp)
{
        GsdMediaKeysManagerPrivate *priv = manager->priv;

        if (priv->connection == NULL || priv->xrandr_proxy == NULL) {
                g_warning ("No existing D-Bus connection trying to handle XRANDR keys");
                return;
        }

        if (priv->cancellable != NULL) {
                g_debug ("xrandr action already in flight");
                return;
        }

        priv->cancellable = g_cancellable_new ();

        g_object_set_data (G_OBJECT (priv->xrandr_proxy),
                           "gsd-media-keys-manager-xrandr-action",
                           g_strdup (action));

        g_dbus_proxy_call (priv->xrandr_proxy,
                           action,
                           g_variant_new ("(x)", timestamp),
                           G_DBUS_CALL_FLAGS_NONE,
                           -1,
                           priv->cancellable,
                           (GAsyncReadyCallback) on_xrandr_action_call_finished,
                           manager);
}

static gboolean
do_video_out_action (GsdMediaKeysManager *manager,
                     gint64               timestamp)
{
        do_xrandr_action (manager, "VideoModeSwitch", timestamp);
        return FALSE;
}

static gboolean
do_video_rotate_action (GsdMediaKeysManager *manager,
                        gint64               timestamp)
{
        do_xrandr_action (manager, "Rotate", timestamp);
        return FALSE;
}

static void
do_toggle_accessibility_key (const char *key)
{
        GSettings *settings;
        gboolean state;

        settings = g_settings_new ("org.gnome.desktop.a11y.applications");
        state = g_settings_get_boolean (settings, key);
        g_settings_set_boolean (settings, key, !state);
        g_object_unref (settings);
}

static void
do_magnifier_action (GsdMediaKeysManager *manager)
{
        do_toggle_accessibility_key ("screen-magnifier-enabled");
}

static void
do_screenreader_action (GsdMediaKeysManager *manager)
{
        do_toggle_accessibility_key ("screen-reader-enabled");
}

static void
do_on_screen_keyboard_action (GsdMediaKeysManager *manager)
{
        do_toggle_accessibility_key ("screen-keyboard-enabled");
}

static void
do_text_size_action (GsdMediaKeysManager *manager,
		     MediaKeyType         type)
{
	gdouble factor, best, distance;
	guint i;

	/* Same values used in the Seeing tab of the Universal Access panel */
	static gdouble factors[] = {
		0.75,
		1.0,
		1.25,
		1.5
	};

	/* Figure out the current DPI scaling factor */
	factor = g_settings_get_double (manager->priv->interface_settings, "text-scaling-factor");
	factor += (type == INCREASE_TEXT_KEY ? 0.25 : -0.25);

	/* Try to find a matching value */
	distance = 1e6;
	best = 1.0;
	for (i = 0; i < G_N_ELEMENTS(factors); i++) {
		gdouble d;
		d = fabs (factor - factors[i]);
		if (d < distance) {
			best = factors[i];
			distance = d;
		}
	}

	if (best == 1.0)
		g_settings_reset (manager->priv->interface_settings, "text-scaling-factor");
	else
		g_settings_set_double (manager->priv->interface_settings, "text-scaling-factor", best);
}

static void
do_magnifier_zoom_action (GsdMediaKeysManager *manager,
			  MediaKeyType         type)
{
	GSettings *settings;
	gdouble offset, value;

	if (type == MAGNIFIER_ZOOM_IN_KEY)
		offset = 1.0;
	else
		offset = -1.0;

	settings = g_settings_new ("org.gnome.desktop.a11y.magnifier");
	value = g_settings_get_double (settings, "mag-factor");
	value += offset;
	value = roundl (value);
	g_settings_set_double (settings, "mag-factor", value);
	g_object_unref (settings);
}

static void
do_toggle_contrast_action (GsdMediaKeysManager *manager)
{
	gboolean high_contrast;
	char *theme;

	/* Are we using HighContrast now? */
	theme = g_settings_get_string (manager->priv->interface_settings, "gtk-theme");
	high_contrast = g_str_equal (theme, HIGH_CONTRAST);
	g_free (theme);

	if (high_contrast != FALSE) {
		if (manager->priv->gtk_theme == NULL)
			g_settings_reset (manager->priv->interface_settings, "gtk-theme");
		else
			g_settings_set (manager->priv->interface_settings, "gtk-theme", manager->priv->gtk_theme);
		g_settings_set (manager->priv->interface_settings, "icon-theme", manager->priv->icon_theme);
	} else {
		g_settings_set (manager->priv->interface_settings, "gtk-theme", HIGH_CONTRAST);
		g_settings_set (manager->priv->interface_settings, "icon-theme", HIGH_CONTRAST);
	}
}

static void
power_action_suspend (GsdMediaKeysManager *manager)
{
        g_dbus_proxy_call (manager->priv->logind_proxy,
                           "Suspend",
                           g_variant_new ("(b)", TRUE),
                           G_DBUS_CALL_FLAGS_NONE,
                           G_MAXINT,
                           manager->priv->bus_cancellable,
                           NULL, NULL);
}

static void
power_action_hibernate (GsdMediaKeysManager *manager)
{
        g_dbus_proxy_call (manager->priv->logind_proxy,
                           "Hibernate",
                           g_variant_new ("(b)", TRUE),
                           G_DBUS_CALL_FLAGS_NONE,
                           G_MAXINT,
                           manager->priv->bus_cancellable,
                           NULL, NULL);
}

static void
do_config_power_action (GsdMediaKeysManager *manager,
                        const gchar *config_key)
{
        GsdPowerActionType action_type;

        action_type = g_settings_get_enum (manager->priv->power_settings,
                                           config_key);
        switch (action_type) {
        case GSD_POWER_ACTION_SUSPEND:
                power_action_suspend (manager);
                break;
        case GSD_POWER_ACTION_INTERACTIVE:
        case GSD_POWER_ACTION_SHUTDOWN:
                gnome_session_shutdown (manager);
                break;
        case GSD_POWER_ACTION_HIBERNATE:
                power_action_hibernate (manager);
                break;
        case GSD_POWER_ACTION_BLANK:
        case GSD_POWER_ACTION_NOTHING:
                /* these actions cannot be handled by media-keys and
                 * are not used in this context */
                break;
        }
}

static void
update_screen_cb (GObject             *source_object,
                  GAsyncResult        *res,
                  gpointer             user_data)
{
        GError *error = NULL;
        guint percentage;
        GVariant *new_percentage;
        GsdMediaKeysManager *manager = GSD_MEDIA_KEYS_MANAGER (user_data);

        new_percentage = g_dbus_proxy_call_finish (G_DBUS_PROXY (source_object),
                                                   res, &error);
        if (new_percentage == NULL) {
                g_warning ("Failed to set new screen percentage: %s",
                           error->message);
                g_error_free (error);
                return;
        }

        /* update the dialog with the new value */
        g_variant_get (new_percentage, "(u)", &percentage);
        show_osd (manager, "display-brightness-symbolic", NULL, percentage);
        g_variant_unref (new_percentage);
}

static void
do_screen_brightness_action (GsdMediaKeysManager *manager,
                             MediaKeyType type)
{
        if (manager->priv->connection == NULL ||
            manager->priv->power_screen_proxy == NULL) {
                g_warning ("No existing D-Bus connection trying to handle power keys");
                return;
        }

        /* call into the power plugin */
        g_dbus_proxy_call (manager->priv->power_screen_proxy,
                           type == SCREEN_BRIGHTNESS_UP_KEY ? "StepUp" : "StepDown",
                           NULL,
                           G_DBUS_CALL_FLAGS_NONE,
                           -1,
                           NULL,
                           update_screen_cb,
                           manager);
}

static void
update_keyboard_cb (GObject             *source_object,
                    GAsyncResult        *res,
                    gpointer             user_data)
{
        GError *error = NULL;
        guint percentage;
        GVariant *new_percentage;
        GsdMediaKeysManager *manager = GSD_MEDIA_KEYS_MANAGER (user_data);

        new_percentage = g_dbus_proxy_call_finish (G_DBUS_PROXY (source_object),
                                                   res, &error);
        if (new_percentage == NULL) {
                g_warning ("Failed to set new keyboard percentage: %s",
                           error->message);
                g_error_free (error);
                return;
        }

        /* update the dialog with the new value */
        g_variant_get (new_percentage, "(u)", &percentage);
        show_osd (manager, "keyboard-brightness-symbolic", NULL, percentage);
        g_variant_unref (new_percentage);
}

static void
do_keyboard_brightness_action (GsdMediaKeysManager *manager,
                               MediaKeyType type)
{
        const char *cmd;

        if (manager->priv->connection == NULL ||
            manager->priv->power_keyboard_proxy == NULL) {
                g_warning ("No existing D-Bus connection trying to handle power keys");
                return;
        }

        switch (type) {
        case KEYBOARD_BRIGHTNESS_UP_KEY:
                cmd = "StepUp";
                break;
        case KEYBOARD_BRIGHTNESS_DOWN_KEY:
                cmd = "StepDown";
                break;
        case KEYBOARD_BRIGHTNESS_TOGGLE_KEY:
                cmd = "Toggle";
                break;
        default:
                g_assert_not_reached ();
        }

        /* call into the power plugin */
        g_dbus_proxy_call (manager->priv->power_keyboard_proxy,
                           cmd,
                           NULL,
                           G_DBUS_CALL_FLAGS_NONE,
                           -1,
                           NULL,
                           update_keyboard_cb,
                           manager);
}

static void
do_battery_action (GsdMediaKeysManager *manager)
{
        GVariant *icon_var, *percentage;
        char *label = NULL;

        if (manager->priv->power_proxy == NULL)
                return;

        icon_var = g_dbus_proxy_get_cached_property (manager->priv->power_proxy, "Icon");
        percentage = g_dbus_proxy_get_cached_property (manager->priv->power_proxy, "Percentage");

        if (g_variant_get_double (percentage) >= 0.0)
                label = g_strdup_printf ("%d %%", (int) g_variant_get_double (percentage));

        show_osd (manager, g_variant_get_string (icon_var, NULL),
                  label, g_variant_get_double (percentage));
        g_free (label);
}

static void
do_custom_action (GsdMediaKeysManager *manager,
                  guint                deviceid,
                  MediaKey            *key,
                  gint64               timestamp)
{
        g_debug ("Launching custom action for key (on device id %d)", deviceid);

	execute (manager, key->custom_command, FALSE);
}

static gboolean
do_action (GsdMediaKeysManager *manager,
           guint                deviceid,
           MediaKeyType         type,
           gint64               timestamp)
{
        g_debug ("Launching action for key type '%d' (on device id %d)", type, deviceid);

        switch (type) {
        case TOUCHPAD_KEY:
                do_touchpad_action (manager);
                break;
        case TOUCHPAD_ON_KEY:
                do_touchpad_osd_action (manager, TRUE);
                break;
        case TOUCHPAD_OFF_KEY:
                do_touchpad_osd_action (manager, FALSE);
                break;
        case MUTE_KEY:
        case VOLUME_DOWN_KEY:
        case VOLUME_UP_KEY:
                do_sound_action (manager, deviceid, type, TRUE, FALSE);
                break;
        case MIC_MUTE_KEY:
                do_sound_action (manager, deviceid, MUTE_KEY, FALSE, TRUE);
                break;
        case MUTE_QUIET_KEY:
                do_sound_action (manager, deviceid, MUTE_KEY, TRUE, TRUE);
                break;
        case VOLUME_DOWN_QUIET_KEY:
                do_sound_action (manager, deviceid, VOLUME_DOWN_KEY, TRUE, TRUE);
                break;
        case VOLUME_UP_QUIET_KEY:
                do_sound_action (manager, deviceid, VOLUME_UP_KEY, TRUE, TRUE);
                break;
        case LOGOUT_KEY:
                gnome_session_shutdown (manager);
                break;
        case EJECT_KEY:
                do_eject_action (manager);
                break;
        case HOME_KEY:
                do_home_key_action (manager, timestamp);
                break;
        case SEARCH_KEY:
                do_search_action (manager, timestamp);
                break;
        case EMAIL_KEY:
                do_url_action (manager, "mailto", timestamp);
                break;
        case SCREENSAVER_KEY:
                do_lock_screensaver (manager);
                break;
        case HELP_KEY:
                do_url_action (manager, "ghelp", timestamp);
                break;
        case SCREENSHOT_KEY:
        case SCREENSHOT_CLIP_KEY:
        case WINDOW_SCREENSHOT_KEY:
        case WINDOW_SCREENSHOT_CLIP_KEY:
        case AREA_SCREENSHOT_KEY:
        case AREA_SCREENSHOT_CLIP_KEY:
                gsd_screenshot_take (type);
                break;
        case WWW_KEY:
                do_url_action (manager, "http", timestamp);
                break;
        case MEDIA_KEY:
                do_media_action (manager, timestamp);
                break;
        case CALCULATOR_KEY:
                do_execute_desktop_or_desktop (manager, "gcalctool.desktop", "gnome-calculator.desktop", timestamp);
                break;
        case PLAY_KEY:
                return do_multimedia_player_action (manager, "Play");
        case PAUSE_KEY:
                return do_multimedia_player_action (manager, "Pause");
        case STOP_KEY:
                return do_multimedia_player_action (manager, "Stop");
        case PREVIOUS_KEY:
                return do_multimedia_player_action (manager, "Previous");
        case NEXT_KEY:
                return do_multimedia_player_action (manager, "Next");
        case REWIND_KEY:
                return do_multimedia_player_action (manager, "Rewind");
        case FORWARD_KEY:
                return do_multimedia_player_action (manager, "FastForward");
        case REPEAT_KEY:
                return do_multimedia_player_action (manager, "Repeat");
        case RANDOM_KEY:
                return do_multimedia_player_action (manager, "Shuffle");
        case VIDEO_OUT_KEY:
                do_video_out_action (manager, timestamp);
                break;
        case ROTATE_VIDEO_KEY:
                do_video_rotate_action (manager, timestamp);
                break;
        case MAGNIFIER_KEY:
                do_magnifier_action (manager);
                break;
        case SCREENREADER_KEY:
                do_screenreader_action (manager);
                break;
        case ON_SCREEN_KEYBOARD_KEY:
                do_on_screen_keyboard_action (manager);
                break;
	case INCREASE_TEXT_KEY:
	case DECREASE_TEXT_KEY:
		do_text_size_action (manager, type);
		break;
	case MAGNIFIER_ZOOM_IN_KEY:
	case MAGNIFIER_ZOOM_OUT_KEY:
		do_magnifier_zoom_action (manager, type);
		break;
	case TOGGLE_CONTRAST_KEY:
		do_toggle_contrast_action (manager);
		break;
        case POWER_KEY:
                do_config_power_action (manager, "button-power");
                break;
        case SLEEP_KEY:
                do_config_power_action (manager, "button-sleep");
                break;
        case SUSPEND_KEY:
                do_config_power_action (manager, "button-suspend");
                break;
        case HIBERNATE_KEY:
                do_config_power_action (manager, "button-hibernate");
                break;
        case SCREEN_BRIGHTNESS_UP_KEY:
        case SCREEN_BRIGHTNESS_DOWN_KEY:
                do_screen_brightness_action (manager, type);
                break;
        case KEYBOARD_BRIGHTNESS_UP_KEY:
        case KEYBOARD_BRIGHTNESS_DOWN_KEY:
        case KEYBOARD_BRIGHTNESS_TOGGLE_KEY:
                do_keyboard_brightness_action (manager, type);
                break;
        case BATTERY_KEY:
                do_battery_action (manager);
                break;
        /* Note, no default so compiler catches missing keys */
        case CUSTOM_KEY:
                g_assert_not_reached ();
        }

        return FALSE;
}

static void
on_accelerator_activated (ShellKeyGrabber     *grabber,
                          guint                accel_id,
                          guint                deviceid,
                          GsdMediaKeysManager *manager)
{
        guint i;

        for (i = 0; i < manager->priv->keys->len; i++) {
                MediaKey *key;

                key = g_ptr_array_index (manager->priv->keys, i);

                if (key->accel_id != accel_id)
                        continue;

                if (key->key_type == CUSTOM_KEY)
                        do_custom_action (manager, deviceid, key, GDK_CURRENT_TIME);
                else
                        do_action (manager, deviceid, key->key_type, GDK_CURRENT_TIME);
                return;
        }
}

static void
update_theme_settings (GSettings           *settings,
		       const char          *key,
		       GsdMediaKeysManager *manager)
{
	char *theme;

	theme = g_settings_get_string (manager->priv->interface_settings, key);
	if (g_str_equal (theme, HIGH_CONTRAST)) {
		g_free (theme);
	} else {
		if (g_str_equal (key, "gtk-theme")) {
			g_free (manager->priv->gtk_theme);
			manager->priv->gtk_theme = theme;
		} else {
			g_free (manager->priv->icon_theme);
			manager->priv->icon_theme = theme;
		}
	}
}

static void
initialize_volume_handler (GsdMediaKeysManager *manager)
{
        /* initialise Volume handler
         *
         * We do this one here to force checking gstreamer cache, etc.
         * The rest (grabbing and setting the keys) can happen in an
         * idle.
         */
        gnome_settings_profile_start ("gvc_mixer_control_new");

        manager->priv->volume = gvc_mixer_control_new ("GNOME Volume Control Media Keys");

        g_signal_connect (manager->priv->volume,
                          "state-changed",
                          G_CALLBACK (on_control_state_changed),
                          manager);
        g_signal_connect (manager->priv->volume,
                          "default-sink-changed",
                          G_CALLBACK (on_control_default_sink_changed),
                          manager);
        g_signal_connect (manager->priv->volume,
                          "default-source-changed",
                          G_CALLBACK (on_control_default_source_changed),
                          manager);
        g_signal_connect (manager->priv->volume,
                          "stream-removed",
                          G_CALLBACK (on_control_stream_removed),
                          manager);

        gvc_mixer_control_open (manager->priv->volume);

        gnome_settings_profile_end ("gvc_mixer_control_new");
}

static void
on_shell_proxy_ready (GObject      *source,
                      GAsyncResult *result,
                      gpointer      data)
{
        GsdMediaKeysManager *manager = data;

        manager->priv->shell_proxy =
                g_dbus_proxy_new_for_bus_finish (result, NULL);
}

static void
on_key_grabber_ready (GObject      *source,
                      GAsyncResult *result,
                      gpointer      data)
{
        GsdMediaKeysManager *manager = data;

        manager->priv->key_grabber =
		shell_key_grabber_proxy_new_for_bus_finish (result, NULL);

        if (!manager->priv->key_grabber)
                return;

        g_signal_connect (manager->priv->key_grabber, "accelerator-activated",
                          G_CALLBACK (on_accelerator_activated), manager);

        init_kbd (manager);
}

static void
on_shell_appeared (GDBusConnection   *connection,
                   const char        *name,
                   const char        *name_owner,
                   gpointer           user_data)
{
        GsdMediaKeysManager *manager = user_data;

        shell_key_grabber_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                             0,
                                             name_owner,
                                             SHELL_DBUS_PATH,
                                             manager->priv->grab_cancellable,
                                             on_key_grabber_ready, manager);

        g_dbus_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                  0, NULL,
                                  name_owner,
                                  SHELL_DBUS_PATH,
                                  SHELL_DBUS_NAME,
                                  manager->priv->shell_cancellable,
                                  on_shell_proxy_ready, manager);
}

static void
on_shell_vanished (GDBusConnection  *connection,
                   const char       *name,
                   gpointer          user_data)
{
        GsdMediaKeysManager *manager = user_data;

        g_ptr_array_set_size (manager->priv->keys, 0);

        g_clear_object (&manager->priv->key_grabber);
        g_clear_object (&manager->priv->shell_proxy);
}

static gboolean
start_media_keys_idle_cb (GsdMediaKeysManager *manager)
{
        char *theme_name;

        g_debug ("Starting media_keys manager");
        gnome_settings_profile_start (NULL);

        manager->priv->keys = g_ptr_array_new_with_free_func ((GDestroyNotify) media_key_free);

        initialize_volume_handler (manager);

        manager->priv->settings = g_settings_new (SETTINGS_BINDING_DIR);
        g_signal_connect (G_OBJECT (manager->priv->settings), "changed",
                          G_CALLBACK (gsettings_changed_cb), manager);
        g_signal_connect (G_OBJECT (manager->priv->settings), "changed::custom-keybindings",
                          G_CALLBACK (gsettings_custom_changed_cb), manager);

        manager->priv->custom_settings =
          g_hash_table_new_full (g_str_hash, g_str_equal,
                                 g_free, g_object_unref);

        /* Sound events */
        ca_context_create (&manager->priv->ca);
        ca_context_set_driver (manager->priv->ca, "pulse");
        ca_context_change_props (manager->priv->ca, 0,
                                 CA_PROP_APPLICATION_ID, "org.gnome.VolumeControl",
                                 NULL);
        manager->priv->gtksettings = gtk_settings_get_for_screen (gdk_screen_get_default ());
        g_object_get (G_OBJECT (manager->priv->gtksettings), "gtk-sound-theme-name", &theme_name, NULL);
        if (theme_name)
                ca_context_change_props (manager->priv->ca, CA_PROP_CANBERRA_XDG_THEME_NAME, theme_name, NULL);
        g_free (theme_name);
        g_signal_connect (manager->priv->gtksettings, "notify::gtk-sound-theme-name",
                          G_CALLBACK (sound_theme_changed), manager);

        /* for the power plugin interface code */
        manager->priv->power_settings = g_settings_new (SETTINGS_POWER_DIR);

        /* Logic from http://git.gnome.org/browse/gnome-shell/tree/js/ui/status/accessibility.js#n163 */
        manager->priv->interface_settings = g_settings_new (SETTINGS_INTERFACE_DIR);
        g_signal_connect (G_OBJECT (manager->priv->interface_settings), "changed::gtk-theme",
			  G_CALLBACK (update_theme_settings), manager);
        g_signal_connect (G_OBJECT (manager->priv->interface_settings), "changed::icon-theme",
			  G_CALLBACK (update_theme_settings), manager);
	manager->priv->gtk_theme = g_settings_get_string (manager->priv->interface_settings, "gtk-theme");
	if (g_str_equal (manager->priv->gtk_theme, HIGH_CONTRAST)) {
		g_free (manager->priv->gtk_theme);
		manager->priv->gtk_theme = NULL;
	}
	manager->priv->icon_theme = g_settings_get_string (manager->priv->interface_settings, "icon-theme");

        ensure_cancellable (&manager->priv->grab_cancellable);
        ensure_cancellable (&manager->priv->shell_cancellable);

        manager->priv->name_owner_id =
                g_bus_watch_name (G_BUS_TYPE_SESSION,
                                  SHELL_DBUS_NAME, 0,
                                  on_shell_appeared,
                                  on_shell_vanished,
                                  manager, NULL);

        gnome_settings_profile_end (NULL);

        manager->priv->start_idle_id = 0;

        return FALSE;
}

gboolean
gsd_media_keys_manager_start (GsdMediaKeysManager *manager,
                              GError             **error)
{
        const char * const subsystems[] = { "input", "usb", "sound", NULL };

        gnome_settings_profile_start (NULL);

#ifdef HAVE_GUDEV
        manager->priv->streams = g_hash_table_new (g_direct_hash, g_direct_equal);
        manager->priv->udev_client = g_udev_client_new (subsystems);
#endif

        manager->priv->start_idle_id = g_idle_add ((GSourceFunc) start_media_keys_idle_cb, manager);

        register_manager (manager_object);

        gnome_settings_profile_end (NULL);

        return TRUE;
}

void
gsd_media_keys_manager_stop (GsdMediaKeysManager *manager)
{
        GsdMediaKeysManagerPrivate *priv = manager->priv;
        int i;

        g_debug ("Stopping media_keys manager");

        if (priv->bus_cancellable != NULL) {
                g_cancellable_cancel (priv->bus_cancellable);
                g_object_unref (priv->bus_cancellable);
                priv->bus_cancellable = NULL;
        }

        if (manager->priv->gtksettings != NULL) {
                g_signal_handlers_disconnect_by_func (manager->priv->gtksettings, sound_theme_changed, manager);
                manager->priv->gtksettings = NULL;
        }

        g_clear_pointer (&manager->priv->ca, ca_context_destroy);

#ifdef HAVE_GUDEV
        g_clear_pointer (&priv->streams, g_hash_table_destroy);
        g_clear_object (&priv->udev_client);
#endif /* HAVE_GUDEV */

        g_clear_object (&priv->logind_proxy);
        g_clear_object (&priv->settings);
        g_clear_object (&priv->power_settings);
        g_clear_object (&priv->power_proxy);
        g_clear_object (&priv->power_screen_proxy);
        g_clear_object (&priv->power_keyboard_proxy);

        if (manager->priv->name_owner_id) {
                g_bus_unwatch_name (manager->priv->name_owner_id);
                manager->priv->name_owner_id = 0;
        }

        if (priv->cancellable != NULL) {
                g_cancellable_cancel (priv->cancellable);
                g_clear_object (&priv->cancellable);
        }

        g_clear_pointer (&priv->introspection_data, g_dbus_node_info_unref);
        g_clear_object (&priv->connection);

        if (priv->keys != NULL) {
                for (i = 0; i < priv->keys->len; ++i) {
                        MediaKey *key;

                        key = g_ptr_array_index (manager->priv->keys, i);
                        ungrab_media_key (key, manager);
                }
                g_ptr_array_free (priv->keys, TRUE);
                priv->keys = NULL;
        }

        if (priv->grab_cancellable != NULL) {
                g_cancellable_cancel (priv->grab_cancellable);
                g_clear_object (&priv->grab_cancellable);
        }

        if (priv->shell_cancellable != NULL) {
                g_cancellable_cancel (priv->shell_cancellable);
                g_clear_object (&priv->shell_cancellable);
        }

        g_clear_object (&priv->sink);
        g_clear_object (&priv->source);
        g_clear_object (&priv->volume);

        if (priv->media_players != NULL) {
                g_list_free_full (priv->media_players, (GDestroyNotify) free_media_player);
                priv->media_players = NULL;
        }
}

static GObject *
gsd_media_keys_manager_constructor (GType                  type,
                              guint                  n_construct_properties,
                              GObjectConstructParam *construct_properties)
{
        GsdMediaKeysManager      *media_keys_manager;

        media_keys_manager = GSD_MEDIA_KEYS_MANAGER (G_OBJECT_CLASS (gsd_media_keys_manager_parent_class)->constructor (type,
                                                                                                      n_construct_properties,
                                                                                                      construct_properties));

        return G_OBJECT (media_keys_manager);
}

static void
gsd_media_keys_manager_class_init (GsdMediaKeysManagerClass *klass)
{
        GObjectClass   *object_class = G_OBJECT_CLASS (klass);

        object_class->constructor = gsd_media_keys_manager_constructor;
        object_class->finalize = gsd_media_keys_manager_finalize;

        g_type_class_add_private (klass, sizeof (GsdMediaKeysManagerPrivate));
}

static void
inhibit_done (GObject      *source,
              GAsyncResult *result,
              gpointer      user_data)
{
        GDBusProxy *proxy = G_DBUS_PROXY (source);
        GsdMediaKeysManager *manager = GSD_MEDIA_KEYS_MANAGER (user_data);
        GError *error = NULL;
        GVariant *res;
        GUnixFDList *fd_list = NULL;
        gint idx;

        res = g_dbus_proxy_call_with_unix_fd_list_finish (proxy, &fd_list, result, &error);
        if (res == NULL) {
                g_warning ("Unable to inhibit keypresses: %s", error->message);
                g_error_free (error);
        } else {
                g_variant_get (res, "(h)", &idx);
                manager->priv->inhibit_keys_fd = g_unix_fd_list_get (fd_list, idx, &error);
                if (manager->priv->inhibit_keys_fd == -1) {
                        g_warning ("Failed to receive system inhibitor fd: %s", error->message);
                        g_error_free (error);
                }
                g_debug ("System inhibitor fd is %d", manager->priv->inhibit_keys_fd);
                g_object_unref (fd_list);
                g_variant_unref (res);
        }
}

static void
gsd_media_keys_manager_init (GsdMediaKeysManager *manager)
{
        GError *error;
        GDBusConnection *bus;

        error = NULL;
        manager->priv = GSD_MEDIA_KEYS_MANAGER_GET_PRIVATE (manager);

        bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
        if (bus == NULL) {
                g_warning ("Failed to connect to system bus: %s",
                           error->message);
                g_error_free (error);
                return;
        }

        manager->priv->logind_proxy =
                g_dbus_proxy_new_sync (bus,
                                       0,
                                       NULL,
                                       SYSTEMD_DBUS_NAME,
                                       SYSTEMD_DBUS_PATH,
                                       SYSTEMD_DBUS_INTERFACE,
                                       NULL,
                                       &error);

        if (manager->priv->logind_proxy == NULL) {
                g_warning ("Failed to connect to systemd: %s",
                           error->message);
                g_error_free (error);
        }

        g_object_unref (bus);

        g_debug ("Adding system inhibitors for power keys");
        manager->priv->inhibit_keys_fd = -1;
        g_dbus_proxy_call_with_unix_fd_list (manager->priv->logind_proxy,
                                             "Inhibit",
                                             g_variant_new ("(ssss)",
                                                            "handle-power-key:handle-suspend-key:handle-hibernate-key",
                                                            g_get_user_name (),
                                                            "GNOME handling keypresses",
                                                            "block"),
                                             0,
                                             G_MAXINT,
                                             NULL,
                                             NULL,
                                             inhibit_done,
                                             manager);

}

static void
gsd_media_keys_manager_finalize (GObject *object)
{
        GsdMediaKeysManager *media_keys_manager;

        g_return_if_fail (object != NULL);
        g_return_if_fail (GSD_IS_MEDIA_KEYS_MANAGER (object));

        media_keys_manager = GSD_MEDIA_KEYS_MANAGER (object);

        g_return_if_fail (media_keys_manager->priv != NULL);

        if (media_keys_manager->priv->start_idle_id != 0)
                g_source_remove (media_keys_manager->priv->start_idle_id);
        if (media_keys_manager->priv->inhibit_keys_fd != -1)
                close (media_keys_manager->priv->inhibit_keys_fd);

        G_OBJECT_CLASS (gsd_media_keys_manager_parent_class)->finalize (object);
}

static void
xrandr_ready_cb (GObject             *source_object,
                 GAsyncResult        *res,
                 GsdMediaKeysManager *manager)
{
        GError *error = NULL;

        manager->priv->xrandr_proxy = g_dbus_proxy_new_finish (res, &error);
        if (manager->priv->xrandr_proxy == NULL) {
                g_warning ("Failed to get proxy for XRandR operations: %s", error->message);
                g_error_free (error);
        }
}

static void
power_ready_cb (GObject             *source_object,
                GAsyncResult        *res,
                GsdMediaKeysManager *manager)
{
        GError *error = NULL;

        manager->priv->power_proxy = g_dbus_proxy_new_finish (res, &error);
        if (manager->priv->power_proxy == NULL) {
                g_warning ("Failed to get proxy for power: %s",
                           error->message);
                g_error_free (error);
        }
}

static void
power_screen_ready_cb (GObject             *source_object,
                       GAsyncResult        *res,
                       GsdMediaKeysManager *manager)
{
        GError *error = NULL;

        manager->priv->power_screen_proxy = g_dbus_proxy_new_finish (res, &error);
        if (manager->priv->power_screen_proxy == NULL) {
                g_warning ("Failed to get proxy for power (screen): %s",
                           error->message);
                g_error_free (error);
        }
}

static void
power_keyboard_ready_cb (GObject             *source_object,
                         GAsyncResult        *res,
                         GsdMediaKeysManager *manager)
{
        GError *error = NULL;

        manager->priv->power_keyboard_proxy = g_dbus_proxy_new_finish (res, &error);
        if (manager->priv->power_keyboard_proxy == NULL) {
                g_warning ("Failed to get proxy for power (keyboard): %s",
                           error->message);
                g_error_free (error);
        }
}

static void
on_bus_gotten (GObject             *source_object,
               GAsyncResult        *res,
               GsdMediaKeysManager *manager)
{
        GDBusConnection *connection;
        GError *error = NULL;

        if (manager->priv->bus_cancellable == NULL ||
            g_cancellable_is_cancelled (manager->priv->bus_cancellable)) {
                g_warning ("Operation has been cancelled, so not retrieving session bus");
                return;
        }

        connection = g_bus_get_finish (res, &error);
        if (connection == NULL) {
                g_warning ("Could not get session bus: %s", error->message);
                g_error_free (error);
                return;
        }
        manager->priv->connection = connection;

        g_dbus_connection_register_object (connection,
                                           GSD_MEDIA_KEYS_DBUS_PATH,
                                           manager->priv->introspection_data->interfaces[0],
                                           &interface_vtable,
                                           manager,
                                           NULL,
                                           NULL);

        g_dbus_proxy_new (manager->priv->connection,
                          G_DBUS_PROXY_FLAGS_NONE,
                          NULL,
                          GSD_DBUS_NAME ".XRANDR",
                          GSD_DBUS_PATH "/XRANDR",
                          GSD_DBUS_BASE_INTERFACE ".XRANDR_2",
                          NULL,
                          (GAsyncReadyCallback) xrandr_ready_cb,
                          manager);

        g_dbus_proxy_new (manager->priv->connection,
                          G_DBUS_PROXY_FLAGS_NONE,
                          NULL,
                          GSD_DBUS_NAME ".Power",
                          GSD_DBUS_PATH "/Power",
                          GSD_DBUS_BASE_INTERFACE ".Power",
                          NULL,
                          (GAsyncReadyCallback) power_ready_cb,
                          manager);

        g_dbus_proxy_new (manager->priv->connection,
                          G_DBUS_PROXY_FLAGS_NONE,
                          NULL,
                          GSD_DBUS_NAME ".Power",
                          GSD_DBUS_PATH "/Power",
                          GSD_DBUS_BASE_INTERFACE ".Power.Screen",
                          NULL,
                          (GAsyncReadyCallback) power_screen_ready_cb,
                          manager);

        g_dbus_proxy_new (manager->priv->connection,
                          G_DBUS_PROXY_FLAGS_NONE,
                          NULL,
                          GSD_DBUS_NAME ".Power",
                          GSD_DBUS_PATH "/Power",
                          GSD_DBUS_BASE_INTERFACE ".Power.Keyboard",
                          NULL,
                          (GAsyncReadyCallback) power_keyboard_ready_cb,
                          manager);
}

static void
register_manager (GsdMediaKeysManager *manager)
{
        manager->priv->introspection_data = g_dbus_node_info_new_for_xml (introspection_xml, NULL);
        manager->priv->bus_cancellable = g_cancellable_new ();
        g_assert (manager->priv->introspection_data != NULL);

        g_bus_get (G_BUS_TYPE_SESSION,
                   manager->priv->bus_cancellable,
                   (GAsyncReadyCallback) on_bus_gotten,
                   manager);
}

GsdMediaKeysManager *
gsd_media_keys_manager_new (void)
{
        if (manager_object != NULL) {
                g_object_ref (manager_object);
        } else {
                manager_object = g_object_new (GSD_TYPE_MEDIA_KEYS_MANAGER, NULL);
                g_object_add_weak_pointer (manager_object,
                                           (gpointer *) &manager_object);
        }

        return GSD_MEDIA_KEYS_MANAGER (manager_object);
}
