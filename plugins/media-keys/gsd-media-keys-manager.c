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

#include <locale.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <gconf/gconf-client.h>

#define DBUS_API_SUBJECT_TO_CHANGE
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>

#include "gnome-settings-profile.h"
#include "gsd-marshal.h"
#include "gsd-media-keys-manager.h"
#include "gsd-media-keys-manager-glue.h"

#include "eggaccelerators.h"
#include "actions/acme.h"
#include "actions/acme-volume.h"
#include "gsd-media-keys-window.h"

#define GSD_DBUS_PATH "/org/gnome/SettingsDaemon"
#define GSD_DBUS_NAME "org.gnome.SettingsDaemon"
#define GSD_MEDIA_KEYS_DBUS_PATH GSD_DBUS_PATH "/MediaKeys"
#define GSD_MEDIA_KEYS_DBUS_NAME GSD_DBUS_NAME ".MediaKeys"

#define VOLUME_STEP 6           /* percents for one volume button press */

/* we exclude shift, GDK_CONTROL_MASK and GDK_MOD1_MASK since we know what
   these modifiers mean
   these are the mods whose combinations are bound by the keygrabbing code */
#define IGNORED_MODS (0x2000 /*Xkb modifier*/ | GDK_LOCK_MASK  | \
       GDK_MOD2_MASK | GDK_MOD3_MASK | GDK_MOD4_MASK | GDK_MOD5_MASK)
/* these are the ones we actually use for global keys, we always only check
 * for these set */
#define USED_MODS (GDK_SHIFT_MASK | GDK_CONTROL_MASK | GDK_MOD1_MASK)

#define GSD_MEDIA_KEYS_MANAGER_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), GSD_TYPE_MEDIA_KEYS_MANAGER, GsdMediaKeysManagerPrivate))

typedef struct {
        char   *application;
        guint32 time;
} MediaPlayer;

struct GsdMediaKeysManagerPrivate
{
        AcmeVolume      *volume;
        GtkWidget       *dialog;
        GConfClient     *conf_client;

        /* Multihead stuff */
        GdkScreen       *current_screen;
        GSList          *screens;

        GList           *media_players;

        DBusGConnection *connection;
};

enum {
        MEDIA_PLAYER_KEY_PRESSED,
        LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static void     gsd_media_keys_manager_class_init  (GsdMediaKeysManagerClass *klass);
static void     gsd_media_keys_manager_init        (GsdMediaKeysManager      *media_keys_manager);
static void     gsd_media_keys_manager_finalize    (GObject                  *object);

G_DEFINE_TYPE (GsdMediaKeysManager, gsd_media_keys_manager, G_TYPE_OBJECT)

static gpointer manager_object = NULL;


static void
init_screens (GsdMediaKeysManager *manager)
{
        GdkDisplay *display;
        int i;

        display = gdk_display_get_default ();
        if (gdk_display_get_n_screens (display) == 1) {
                GdkScreen *screen;

                screen = gdk_screen_get_default ();
                manager->priv->screens = g_slist_append (manager->priv->screens, screen);
                manager->priv->current_screen = screen;
                return;
        }

        for (i = 0; i < gdk_display_get_n_screens (display); i++) {
                GdkScreen *screen;

                screen = gdk_display_get_screen (display, i);
                if (screen == NULL) {
                        continue;
                }
                manager->priv->screens = g_slist_append (manager->priv->screens, screen);
        }

        manager->priv->current_screen = (GdkScreen *)manager->priv->screens->data;
}


static void
acme_error (char * msg)
{
        GtkWidget *error_dialog;

        error_dialog = gtk_message_dialog_new (NULL,
                                               GTK_DIALOG_MODAL,
                                               GTK_MESSAGE_ERROR,
                                               GTK_BUTTONS_OK,
                                               "%s", msg);
        gtk_dialog_set_default_response (GTK_DIALOG (error_dialog),
                                         GTK_RESPONSE_OK);
        gtk_widget_show (error_dialog);
        g_signal_connect (G_OBJECT (error_dialog),
                          "response",
                          G_CALLBACK (gtk_widget_destroy),
                          NULL);
}

static char *
get_term_command (GsdMediaKeysManager *manager)
{
        char *cmd_term;
        char *cmd = NULL;

        cmd_term = gconf_client_get_string (manager->priv->conf_client,
                                            "/desktop/gnome/applications/terminal/exec", NULL);
        if ((cmd_term != NULL) && (strcmp (cmd_term, "") != 0)) {
                char *cmd_args;
                cmd_args = gconf_client_get_string (manager->priv->conf_client,
                                                    "/desktop/gnome/applications/terminal/exec_arg", NULL);
                if ((cmd_args != NULL) && (strcmp (cmd_term, "") != 0)) {
                        cmd = g_strdup_printf ("%s %s -e", cmd_term, cmd_args);
                } else {
                        cmd = g_strdup_printf ("%s -e", cmd_term);
                }

                g_free (cmd_args);
        }

        g_free (cmd_term);

        return cmd;
}

static void
execute (GsdMediaKeysManager *manager,
         char                *cmd,
         gboolean             sync,
         gboolean             need_term)
{
        gboolean retval;
        char   **argv;
        int      argc;
        char    *exec;
        char    *term = NULL;

        retval = FALSE;

        if (need_term) {
                term = get_term_command (manager);
                if (term == NULL) {
                        acme_error (_("Could not get default terminal. Verify that your default "
                                      "terminal command is set and points to a valid application."));
                        return;
                }
        }

        if (term) {
                exec = g_strdup_printf ("%s %s", term, cmd);
                g_free (term);
        } else {
                exec = g_strdup (cmd);
        }

        if (g_shell_parse_argv (exec, &argc, &argv, NULL)) {
                if (sync != FALSE) {
                        retval = g_spawn_sync (g_get_home_dir (),
                                               argv,
                                               NULL,
                                               G_SPAWN_SEARCH_PATH,
                                               NULL,
                                               NULL,
                                               NULL,
                                               NULL,
                                               NULL,
                                               NULL);
                } else {
                        retval = g_spawn_async (g_get_home_dir (),
                                                argv,
                                                NULL,
                                                G_SPAWN_SEARCH_PATH,
                                                NULL,
                                                NULL,
                                                NULL,
                                                NULL);
                }
                g_strfreev (argv);
        }

        if (retval == FALSE) {
                char *msg;
                msg = g_strdup_printf (_("Couldn't execute command: %s\n"
                                         "Verify that this is a valid command."),
                                       exec);

                acme_error (msg);
                g_free (msg);
        }
        g_free (exec);
}

static void
do_sleep_action (char *cmd1,
                 char *cmd2)
{
        if (g_spawn_command_line_async (cmd1, NULL) == FALSE) {
                if (g_spawn_command_line_async (cmd2, NULL) == FALSE) {
                        acme_error (_("Couldn't put the machine to sleep.\n"
                                        "Verify that the machine is correctly configured."));
                }
        }
}

static void
dialog_init (GsdMediaKeysManager *manager)
{
        if (manager->priv->dialog != NULL
            && !gsd_media_keys_window_is_valid (GSD_MEDIA_KEYS_WINDOW (manager->priv->dialog))) {
                g_object_unref (manager->priv->dialog);
                manager->priv->dialog = NULL;
        }

        if (manager->priv->dialog == NULL) {
                manager->priv->dialog = gsd_media_keys_window_new ();
        }
}

static gboolean
grab_key_real (Key       *key,
               GdkWindow *root,
               gboolean   grab,
               int        result)
{
        gdk_error_trap_push ();
        if (grab) {
                g_debug ("Grab: %d %d %x %s", result, (int)key->keycode, key->state, XKeysymToString (key->keysym));
                XGrabKey (GDK_DISPLAY (),
                          key->keycode,
                          (result | key->state),
                          GDK_WINDOW_XID (root),
                          True,
                          GrabModeAsync,
                          GrabModeAsync);
        } else {
                g_debug ("UnGrab: %d %d %x", result, (int)key->keycode, key->state);
                XUngrabKey (GDK_DISPLAY (),
                            key->keycode,
                            (result | key->state),
                            GDK_WINDOW_XID (root));
        }

        gdk_flush ();

        gdk_error_trap_pop ();

        return TRUE;
}

/* Grab the key. In order to ignore IGNORED_MODS we need to grab
 * all combinations of the ignored modifiers and those actually used
 * for the binding (if any).
 *
 * inspired by all_combinations from gnome-panel/gnome-panel/global-keys.c */
#define N_BITS 32
static void
grab_key (GsdMediaKeysManager *manager,
          Key                 *key,
          gboolean             grab)
{
        int   indexes[N_BITS];/*indexes of bits we need to flip*/
        int   i;
        int   bit;
        int   bits_set_cnt;
        int   uppervalue;
        guint mask_to_traverse = IGNORED_MODS & ~key->state & GDK_MODIFIER_MASK;

        bit = 0;
        for (i = 0; i < N_BITS; i++) {
                if (mask_to_traverse & (1 << i)) {
                        indexes[bit++] = i;
                }
        }

        bits_set_cnt = bit;

        uppervalue = 1 << bits_set_cnt;
        for (i = 0; i < uppervalue; i++) {
                GSList *l;
                int     j;
                int     result = 0;

                for (j = 0; j < bits_set_cnt; j++) {
                        if (i & (1 << j)) {
                                result |= (1 << indexes[j]);
                        }
                }

                for (l = manager->priv->screens; l ; l = l->next) {
                        GdkScreen *screen = l->data;
                        if (grab_key_real (key, gdk_screen_get_root_window (screen), grab, result) == FALSE)
                                return;
                }
        }
}

static gboolean
is_valid_shortcut (const char *string)
{
        if (string == NULL || string[0] == '\0') {
                return FALSE;
        }
        if (strcmp (string, "disabled") == 0) {
                return FALSE;
        }

        return TRUE;
}

static void
update_kbd_cb (GConfClient         *client,
               guint                id,
               GConfEntry          *entry,
               GsdMediaKeysManager *manager)
{
        int      i;
        gboolean found = FALSE;

        g_return_if_fail (entry->key != NULL);

        /* Find the key that was modified */
        for (i = 0; i < HANDLED_KEYS; i++) {
                if (strcmp (entry->key, keys[i].gconf_key) == 0) {
                        char *tmp;
                        Key  *key;

                        found = TRUE;

                        if (keys[i].key != NULL) {
                                grab_key (manager, keys[i].key, FALSE);
                        }

                        g_free (keys[i].key);
                        keys[i].key = NULL;

                        tmp = gconf_client_get_string (manager->priv->conf_client,
                                                       keys[i].gconf_key, NULL);

                        if (is_valid_shortcut (tmp) == FALSE) {
                                g_free (tmp);
                                break;
                        }

                        key = g_new0 (Key, 1);
                        if (egg_accelerator_parse_virtual (tmp, &key->keysym, &key->keycode, &key->state) == FALSE
                            || key->keycode == 0) {
                                g_free (tmp);
                                g_free (key);
                                break;
                        }

                        grab_key (manager, key, TRUE);
                        keys[i].key = key;

                        g_free (tmp);

                        break;
                }
        }

        if (found != FALSE) {
                return;
        }
}

static void
init_kbd (GsdMediaKeysManager *manager)
{
        int i;

        gnome_settings_profile_start (NULL);

        for (i = 0; i < HANDLED_KEYS; i++) {
                char *tmp;
                Key  *key;

                gconf_client_notify_add (manager->priv->conf_client,
                                         keys[i].gconf_key,
                                         (GConfClientNotifyFunc)update_kbd_cb,
                                         manager,
                                         NULL,
                                         NULL);

                tmp = gconf_client_get_string (manager->priv->conf_client,
                                               keys[i].gconf_key,
                                               NULL);
                if (!is_valid_shortcut (tmp)) {
                        g_debug ("Not a valid shortcut: '%s'", tmp);
                        g_free (tmp);
                        continue;
                }

                key = g_new0 (Key, 1);
                if (!egg_accelerator_parse_virtual (tmp, &key->keysym, &key->keycode, &key->state)
                    || key->keycode == 0) {
                        g_debug ("Unable to parse: '%s'", tmp);

                        g_free (tmp);
                        g_free (key);
                        continue;
                }
        /*avoid grabbing all the keyboard when KeyCode cannot be retrieved */
                if (key->keycode == AnyKey)  {
                        g_warning ("The shortcut key \"%s\" cannot be found on the current system, ignoring the binding", tmp);
                        g_free (tmp);
                        g_free (key);
                        continue;
                }

                g_free (tmp);

                keys[i].key = key;

                grab_key (manager, key, TRUE);
        }

        gnome_settings_profile_end (NULL);
}

static void
dialog_show (GsdMediaKeysManager *manager)
{
        int            orig_w;
        int            orig_h;
        int            screen_w;
        int            screen_h;
        int            x;
        int            y;
        int            pointer_x;
        int            pointer_y;
        GtkRequisition win_req;
        GdkScreen     *pointer_screen;
        GdkRectangle   geometry;
        int            monitor;

        gtk_window_set_screen (GTK_WINDOW (manager->priv->dialog),
                               manager->priv->current_screen);

        /*
         * get the window size
         * if the window hasn't been mapped, it doesn't necessarily
         * know its true size, yet, so we need to jump through hoops
         */
        gtk_window_get_default_size (GTK_WINDOW (manager->priv->dialog), &orig_w, &orig_h);
        gtk_widget_size_request (manager->priv->dialog, &win_req);

        if (win_req.width > orig_w) {
                orig_w = win_req.width;
        }
        if (win_req.height > orig_h) {
                orig_h = win_req.height;
        }

        pointer_screen = NULL;
        gdk_display_get_pointer (gdk_screen_get_display (manager->priv->current_screen),
                                 &pointer_screen,
                                 &pointer_x,
                                 &pointer_y,
                                 NULL);
        if (pointer_screen != manager->priv->current_screen) {
                /* The pointer isn't on the current screen, so just
                 * assume the default monitor
                 */
                monitor = 0;
        } else {
                monitor = gdk_screen_get_monitor_at_point (manager->priv->current_screen,
                                                           pointer_x,
                                                           pointer_y);
        }

        gdk_screen_get_monitor_geometry (manager->priv->current_screen,
                                         monitor,
                                         &geometry);

        screen_w = geometry.width;
        screen_h = geometry.height;

        x = ((screen_w - orig_w) / 2) + geometry.x;
        y = geometry.y + (screen_h / 2) + (screen_h / 2 - orig_h) / 2;

        gtk_window_move (GTK_WINDOW (manager->priv->dialog), x, y);

        gtk_widget_show (manager->priv->dialog);

        gdk_display_sync (gdk_screen_get_display (manager->priv->current_screen));
}

static void
do_unknown_action (GsdMediaKeysManager *manager,
                   const char          *url)
{
        char *string;

        g_return_if_fail (url != NULL);

        string = gconf_client_get_string (manager->priv->conf_client,
                                          "/desktop/gnome/url-handlers/unknown/command",
                                          NULL);

        if ((string != NULL) && (strcmp (string, "") != 0)) {
                char *cmd;
                cmd = g_strdup_printf (string, url);
                execute (manager, cmd, FALSE, FALSE);
                g_free (cmd);
        }
        g_free (string);
}

static void
do_help_action (GsdMediaKeysManager *manager)
{
        char *string;

        string = gconf_client_get_string (manager->priv->conf_client,
                                          "/desktop/gnome/url-handlers/ghelp/command",
                                          NULL);

        if ((string != NULL) && (strcmp (string, "") != 0)) {
                char *cmd;
                cmd = g_strdup_printf (string, "");
                execute (manager, cmd, FALSE, FALSE);
                g_free (cmd);
        } else {
                do_unknown_action (manager, "ghelp:");
        }

        g_free (string);
}

static void
do_mail_action (GsdMediaKeysManager *manager)
{
        char *string;

        string = gconf_client_get_string (manager->priv->conf_client,
                                          "/desktop/gnome/url-handlers/mailto/command",
                                          NULL);

        if ((string != NULL) && (strcmp (string, "") != 0)) {
                char *cmd;
                cmd = g_strdup_printf (string, "");
                execute (manager,
                         cmd,
                         FALSE,
                         gconf_client_get_bool (manager->priv->conf_client,
                                                "/desktop/gnome/url-handlers/mailto/needs_terminal", NULL));
                g_free (cmd);
        }
        g_free (string);
}

static void
do_media_action (GsdMediaKeysManager *manager)
{
        char *command;

        command = gconf_client_get_string (manager->priv->conf_client,
                                           "/desktop/gnome/applications/media/exec", NULL);
        if ((command != NULL) && (strcmp (command, "") != 0)) {
                execute (manager,
                         command,
                         FALSE,
                         gconf_client_get_bool (manager->priv->conf_client,
                                                "/desktop/gnome/applications/media/needs_term", NULL));
        }
        g_free (command);
}

static void
do_www_action (GsdMediaKeysManager *manager,
               const char          *url)
{
        char *string;

        string = gconf_client_get_string (manager->priv->conf_client,
                                          "/desktop/gnome/url-handlers/http/command",
                                          NULL);

        if ((string != NULL) && (strcmp (string, "") != 0)) {
                gchar *cmd;

                if (url == NULL) {
                        cmd = g_strdup_printf (string, "");
                } else {
                        cmd = g_strdup_printf (string, url);
                }

                execute (manager,
                         cmd,
                         FALSE,
                         gconf_client_get_bool (manager->priv->conf_client,
                                                "/desktop/gnome/url-handlers/http/needs_terminal", NULL));
                g_free (cmd);
        } else {
                do_unknown_action (manager, url ? url : "");
        }
        g_free (string);
}

static void
do_exit_action (GsdMediaKeysManager *manager)
{
        execute (manager, "gnome-session-save --kill", FALSE, FALSE);
}

static void
do_eject_action (GsdMediaKeysManager *manager)
{
        char *command;

        dialog_init (manager);
        gsd_media_keys_window_set_action (GSD_MEDIA_KEYS_WINDOW (manager->priv->dialog),
                                          GSD_MEDIA_KEYS_WINDOW_ACTION_EJECT);
        dialog_show (manager);

        command = gconf_client_get_string (manager->priv->conf_client,
                                           GCONF_MISC_DIR "/eject_command",
                                           NULL);
        if ((command != NULL) && (strcmp (command, "") != 0)) {
                execute (manager, command, FALSE, FALSE);
        } else {
                execute (manager, "eject -T", FALSE, FALSE);
        }

        g_free (command);
}

static void
do_sound_action (GsdMediaKeysManager *manager,
                 int                  type)
{
        gboolean muted;
        int      vol;
        int      vol_step;
        GError  *error = NULL;

        if (manager->priv->volume == NULL) {
                return;
        }

        vol_step = gconf_client_get_int (manager->priv->conf_client,
                                         GCONF_MISC_DIR "/volume_step",
                                         &error);

        if (error) {
                vol_step = VOLUME_STEP;
                g_error_free (error);
        }

        /* FIXME: this is racy */
        vol = acme_volume_get_volume (manager->priv->volume);
        muted = acme_volume_get_mute (manager->priv->volume);

        switch (type) {
        case MUTE_KEY:
                acme_volume_mute_toggle (manager->priv->volume);
                break;
        case VOLUME_DOWN_KEY:
                if (!muted && (vol <= vol_step)) {
                        acme_volume_mute_toggle (manager->priv->volume);
                }
                acme_volume_set_volume (manager->priv->volume, vol - vol_step);
                break;
        case VOLUME_UP_KEY:
                if (muted) {
                        if (vol == 0) {
                                acme_volume_set_volume (manager->priv->volume, vol + vol_step);
                        }
                        acme_volume_mute_toggle (manager->priv->volume);
                } else {
                        acme_volume_set_volume (manager->priv->volume, vol + vol_step);
                }
                break;
        }

        muted = acme_volume_get_mute (manager->priv->volume);
        vol = acme_volume_get_volume (manager->priv->volume);

        /* FIXME: AcmeVolume should probably emit signals
           instead of doing it like this */
        dialog_init (manager);
        gsd_media_keys_window_set_volume_muted (GSD_MEDIA_KEYS_WINDOW (manager->priv->dialog),
                                                muted);
        gsd_media_keys_window_set_volume_level (GSD_MEDIA_KEYS_WINDOW (manager->priv->dialog),
                                                vol);
        gsd_media_keys_window_set_action (GSD_MEDIA_KEYS_WINDOW (manager->priv->dialog),
                                          GSD_MEDIA_KEYS_WINDOW_ACTION_VOLUME);
        dialog_show (manager);
}

static gint
find_by_application (gconstpointer a,
                     gconstpointer b)
{
        return strcmp (((MediaPlayer *)a)->application, b);
}

static gint
find_by_time (gconstpointer a,
              gconstpointer b)
{
        return ((MediaPlayer *)a)->time != 0 && ((MediaPlayer *)a)->time < ((MediaPlayer *)b)->time;
}


gboolean
gsd_media_keys_manager_grab_media_player_keys (GsdMediaKeysManager *manager,
                                               const char          *application,
                                               guint32              time,
                                               GError             **error)
{
        GList       *iter;
        MediaPlayer *media_player;

        iter = g_list_find_custom (manager->priv->media_players,
                                   application,
                                   find_by_application);

        if (iter != NULL) {
                if (time == 0 || ((MediaPlayer *)iter->data)->time < time) {
                        g_free (((MediaPlayer *)iter->data)->application);
                        g_free (iter->data);
                        manager->priv->media_players = g_list_delete_link (manager->priv->media_players, iter);
                } else {
                        return TRUE;
                }
        }

        media_player = g_new0 (MediaPlayer, 1);
        media_player->application = g_strdup (application);
        media_player->time = time;

        manager->priv->media_players = g_list_insert_sorted (manager->priv->media_players,
                                                             media_player,
                                                             find_by_time);

        return TRUE;
}

gboolean
gsd_media_keys_manager_release_media_player_keys (GsdMediaKeysManager *manager,
                                                  const char          *application,
                                                  GError             **error)
{
        GList *iter;

        iter = g_list_find_custom (manager->priv->media_players,
                                   application,
                                   find_by_application);

        if (iter != NULL) {
                g_free (((MediaPlayer *)iter->data)->application);
                g_free (iter->data);
                manager->priv->media_players = g_list_delete_link (manager->priv->media_players, iter);
        }

        return TRUE;
}

static gboolean
gsd_media_player_key_pressed (GsdMediaKeysManager *manager,
                              const char          *key)
{
        const char *application = NULL;
        gboolean    have_listeners;

        have_listeners = (manager->priv->media_players != NULL);

        if (have_listeners) {
                application = ((MediaPlayer *)manager->priv->media_players->data)->application;
        }

        g_signal_emit (manager, signals[MEDIA_PLAYER_KEY_PRESSED], 0, application, key);

        return !have_listeners;
}

static gboolean
do_multimedia_player_action (GsdMediaKeysManager *manager,
                             const char          *key)
{
        return gsd_media_player_key_pressed (manager, key);
}

static gboolean
do_action (GsdMediaKeysManager *manager,
           int                  type)
{
        char *cmd;
        char *path;

        switch (type) {
        case MUTE_KEY:
        case VOLUME_DOWN_KEY:
        case VOLUME_UP_KEY:
                do_sound_action (manager, type);
                break;
        case POWER_KEY:
                do_exit_action (manager);
                break;
        case EJECT_KEY:
                do_eject_action (manager);
                break;
        case HOME_KEY:
                path = g_shell_quote (g_get_home_dir ());
                cmd = g_strconcat ("nautilus --no-desktop ", path, NULL);
                g_free (path);
                execute (manager, cmd, FALSE, FALSE);
                g_free (cmd);
                break;
        case SEARCH_KEY:
                cmd = NULL;
                if ((cmd = g_find_program_in_path ("beagle-search"))) {
                        execute (manager, "beagle-search", FALSE, FALSE);
                } else if ((cmd = g_find_program_in_path ("tracker-search-tool"))) {
                        execute (manager, "tracker-search-tool", FALSE, FALSE);
                } else {
                        execute (manager, "gnome-search-tool", FALSE, FALSE);
                }
                g_free (cmd);
                break;
        case EMAIL_KEY:
                do_mail_action (manager);
                break;
        case SLEEP_KEY:
                do_sleep_action ("apm", "xset dpms force off");
                break;
        case SCREENSAVER_KEY:
                if ((cmd = g_find_program_in_path ("gnome-screensaver-command"))) {
                        execute (manager, "gnome-screensaver-command --lock", FALSE, FALSE);
                } else {
                        execute (manager, "xscreensaver-command -lock", FALSE, FALSE);
                }

                g_free (cmd);
                break;
        case HELP_KEY:
                do_help_action (manager);
                break;
        case WWW_KEY:
                do_www_action (manager, NULL);
                break;
        case MEDIA_KEY:
                do_media_action (manager);
                break;
        case CALCULATOR_KEY:
                execute (manager, "gcalctool", FALSE, FALSE);
                break;
        case PLAY_KEY:
                return do_multimedia_player_action (manager, "Play");
                break;
        case PAUSE_KEY:
                return do_multimedia_player_action (manager, "Pause");
                break;
        case STOP_KEY:
                return do_multimedia_player_action (manager, "Stop");
                break;
        case PREVIOUS_KEY:
                return do_multimedia_player_action (manager, "Previous");
                break;
        case NEXT_KEY:
                return do_multimedia_player_action (manager, "Next");
                break;
        default:
                g_assert_not_reached ();
        }

        return FALSE;
}

static GdkScreen *
acme_get_screen_from_event (GsdMediaKeysManager *manager,
                            XAnyEvent           *xanyev)
{
        GdkWindow *window;
        GdkScreen *screen;
        GSList    *l;

        /* Look for which screen we're receiving events */
        for (l = manager->priv->screens; l != NULL; l = l->next) {
                screen = (GdkScreen *) l->data;
                window = gdk_screen_get_root_window (screen);

                if (GDK_WINDOW_XID (window) == xanyev->window) {
                        return screen;
                }
        }

        return NULL;
}

static GdkFilterReturn
acme_filter_events (GdkXEvent           *xevent,
                    GdkEvent            *event,
                    GsdMediaKeysManager *manager)
{
        XEvent    *xev = (XEvent *) xevent;
        XAnyEvent *xanyev = (XAnyEvent *) xevent;
        guint      keycode;
        guint      state;
        int        i;

        /* verify we have a key event */
        if (xev->xany.type != KeyPress
            && xev->xany.type != KeyRelease) {
                return GDK_FILTER_CONTINUE;
        }

        keycode = xev->xkey.keycode;
        state = xev->xkey.state;

        for (i = 0; i < HANDLED_KEYS; i++) {
                if (keys[i].key == NULL) {
                        continue;
                }

                if (keys[i].key->keycode == keycode
                    && (state & USED_MODS) == keys[i].key->state) {
                        switch (keys[i].key_type) {
                        case VOLUME_DOWN_KEY:
                        case VOLUME_UP_KEY:
                                /* auto-repeatable keys */
                                if (xev->type != KeyPress) {
                                        return GDK_FILTER_CONTINUE;
                                }
                                break;
                        default:
                                if (xev->type != KeyRelease) {
                                        return GDK_FILTER_CONTINUE;
                                }
                        }

                        manager->priv->current_screen = acme_get_screen_from_event (manager, xanyev);

                        if (do_action (manager, keys[i].key_type) == FALSE) {
                                return GDK_FILTER_REMOVE;
                        } else {
                                return GDK_FILTER_CONTINUE;
                        }
                }
        }

        return GDK_FILTER_CONTINUE;
}

gboolean
gsd_media_keys_manager_start (GsdMediaKeysManager *manager,
                              GError             **error)
{
        GSList *l;

        g_debug ("Starting media_keys manager");
        gnome_settings_profile_start (NULL);
        manager->priv->conf_client = gconf_client_get_default ();

        gconf_client_add_dir (manager->priv->conf_client,
                              GCONF_BINDING_DIR,
                              GCONF_CLIENT_PRELOAD_ONELEVEL,
                              NULL);

        init_screens (manager);
        init_kbd (manager);

        /* initialise Volume handler */
        gnome_settings_profile_start ("acme_volume_new");
        manager->priv->volume = acme_volume_new ();
        gnome_settings_profile_end ("acme_volume_new");

        /* Start filtering the events */
        for (l = manager->priv->screens; l != NULL; l = l->next) {
                gnome_settings_profile_start ("gdk_window_add_filter");

                g_debug ("adding key filter for screen: %d",
                         gdk_screen_get_number (l->data));

                gdk_window_add_filter (gdk_screen_get_root_window (l->data),
                                       (GdkFilterFunc)acme_filter_events,
                                       manager);
                gnome_settings_profile_end ("gdk_window_add_filter");
        }

        gnome_settings_profile_end (NULL);

        return TRUE;
}

void
gsd_media_keys_manager_stop (GsdMediaKeysManager *manager)
{
        GsdMediaKeysManagerPrivate *priv = manager->priv;

        g_debug ("Stopping media_keys manager");

        if (priv->conf_client) {
                g_object_unref (priv->conf_client);
                priv->conf_client = NULL;
        }

        if (priv->volume) {
                g_object_unref (priv->volume);
                priv->volume = NULL;
        }

        g_slist_free (priv->screens);
        priv->screens = NULL;
}

static void
gsd_media_keys_manager_set_property (GObject        *object,
                               guint           prop_id,
                               const GValue   *value,
                               GParamSpec     *pspec)
{
        GsdMediaKeysManager *self;

        self = GSD_MEDIA_KEYS_MANAGER (object);

        switch (prop_id) {
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static void
gsd_media_keys_manager_get_property (GObject        *object,
                               guint           prop_id,
                               GValue         *value,
                               GParamSpec     *pspec)
{
        GsdMediaKeysManager *self;

        self = GSD_MEDIA_KEYS_MANAGER (object);

        switch (prop_id) {
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static GObject *
gsd_media_keys_manager_constructor (GType                  type,
                              guint                  n_construct_properties,
                              GObjectConstructParam *construct_properties)
{
        GsdMediaKeysManager      *media_keys_manager;
        GsdMediaKeysManagerClass *klass;

        klass = GSD_MEDIA_KEYS_MANAGER_CLASS (g_type_class_peek (GSD_TYPE_MEDIA_KEYS_MANAGER));

        media_keys_manager = GSD_MEDIA_KEYS_MANAGER (G_OBJECT_CLASS (gsd_media_keys_manager_parent_class)->constructor (type,
                                                                                                      n_construct_properties,
                                                                                                      construct_properties));

        return G_OBJECT (media_keys_manager);
}

static void
gsd_media_keys_manager_dispose (GObject *object)
{
        GsdMediaKeysManager *media_keys_manager;

        media_keys_manager = GSD_MEDIA_KEYS_MANAGER (object);

        G_OBJECT_CLASS (gsd_media_keys_manager_parent_class)->dispose (object);
}

static void
gsd_media_keys_manager_class_init (GsdMediaKeysManagerClass *klass)
{
        GObjectClass   *object_class = G_OBJECT_CLASS (klass);

        object_class->get_property = gsd_media_keys_manager_get_property;
        object_class->set_property = gsd_media_keys_manager_set_property;
        object_class->constructor = gsd_media_keys_manager_constructor;
        object_class->dispose = gsd_media_keys_manager_dispose;
        object_class->finalize = gsd_media_keys_manager_finalize;

       signals[MEDIA_PLAYER_KEY_PRESSED] =
               g_signal_new ("media-player-key-pressed",
                             G_OBJECT_CLASS_TYPE (klass),
                             G_SIGNAL_RUN_LAST,
                             G_STRUCT_OFFSET (GsdMediaKeysManagerClass, media_player_key_pressed),
                             NULL,
                             NULL,
                             gsd_marshal_VOID__STRING_STRING,
                             G_TYPE_NONE,
                             2,
                             G_TYPE_STRING,
                             G_TYPE_STRING);

        dbus_g_object_type_install_info (GSD_TYPE_MEDIA_KEYS_MANAGER, &dbus_glib_gsd_media_keys_manager_object_info);

        g_type_class_add_private (klass, sizeof (GsdMediaKeysManagerPrivate));
}

static void
gsd_media_keys_manager_init (GsdMediaKeysManager *manager)
{
        manager->priv = GSD_MEDIA_KEYS_MANAGER_GET_PRIVATE (manager);

}

static void
gsd_media_keys_manager_finalize (GObject *object)
{
        GsdMediaKeysManager *media_keys_manager;

        g_return_if_fail (object != NULL);
        g_return_if_fail (GSD_IS_MEDIA_KEYS_MANAGER (object));

        media_keys_manager = GSD_MEDIA_KEYS_MANAGER (object);

        g_return_if_fail (media_keys_manager->priv != NULL);

        G_OBJECT_CLASS (gsd_media_keys_manager_parent_class)->finalize (object);
}

static gboolean
register_manager (GsdMediaKeysManager *manager)
{
        GError *error = NULL;

        error = NULL;
        manager->priv->connection = dbus_g_bus_get (DBUS_BUS_SESSION, &error);
        if (manager->priv->connection == NULL) {
                if (error != NULL) {
                        g_critical ("error getting session bus: %s", error->message);
                        g_error_free (error);
                }
                exit (1);
        }

        dbus_g_connection_register_g_object (manager->priv->connection, GSD_MEDIA_KEYS_DBUS_PATH, G_OBJECT (manager));

        return TRUE;
}

GsdMediaKeysManager *
gsd_media_keys_manager_new (void)
{
        if (manager_object != NULL) {
                g_object_ref (manager_object);
        } else {
                gboolean res;

                manager_object = g_object_new (GSD_TYPE_MEDIA_KEYS_MANAGER, NULL);
                g_object_add_weak_pointer (manager_object,
                                           (gpointer *) &manager_object);
                res = register_manager (manager_object);
                if (! res) {
                        g_object_unref (manager_object);
                        return NULL;
                }
        }

        return GSD_MEDIA_KEYS_MANAGER (manager_object);
}
