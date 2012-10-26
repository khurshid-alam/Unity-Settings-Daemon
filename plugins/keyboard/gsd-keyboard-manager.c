/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright © 2001 Ximian, Inc.
 * Copyright (C) 2007 William Jon McCann <mccann@jhu.edu>
 * Written by Sergey V. Oudaltsov <svu@users.sourceforge.net>
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

#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XKBrules.h>

#define GNOME_DESKTOP_USE_UNSTABLE_API
#include <libgnome-desktop/gnome-xkb-info.h>

#ifdef HAVE_IBUS
#include <ibus.h>
#endif

#include "gnome-settings-session.h"
#include "gnome-settings-profile.h"
#include "gsd-keyboard-manager.h"
#include "gsd-input-helper.h"
#include "gsd-enums.h"

#define GSD_KEYBOARD_MANAGER_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), GSD_TYPE_KEYBOARD_MANAGER, GsdKeyboardManagerPrivate))

#define GSD_KEYBOARD_DIR "org.gnome.settings-daemon.peripherals.keyboard"

#define KEY_REPEAT         "repeat"
#define KEY_CLICK          "click"
#define KEY_INTERVAL       "repeat-interval"
#define KEY_DELAY          "delay"
#define KEY_CLICK_VOLUME   "click-volume"
#define KEY_REMEMBER_NUMLOCK_STATE "remember-numlock-state"
#define KEY_NUMLOCK_STATE  "numlock-state"

#define KEY_BELL_VOLUME    "bell-volume"
#define KEY_BELL_PITCH     "bell-pitch"
#define KEY_BELL_DURATION  "bell-duration"
#define KEY_BELL_MODE      "bell-mode"

#define KEY_SWITCHER "input-sources-switcher"

#define GNOME_DESKTOP_INTERFACE_DIR "org.gnome.desktop.interface"

#define KEY_GTK_IM_MODULE    "gtk-im-module"
#define GTK_IM_MODULE_SIMPLE "gtk-im-context-simple"
#define GTK_IM_MODULE_IBUS   "ibus"

#define GNOME_DESKTOP_INPUT_SOURCES_DIR "org.gnome.desktop.input-sources"

#define KEY_CURRENT_INPUT_SOURCE "current"
#define KEY_INPUT_SOURCES        "sources"
#define KEY_KEYBOARD_OPTIONS     "xkb-options"

#define INPUT_SOURCE_TYPE_XKB  "xkb"
#define INPUT_SOURCE_TYPE_IBUS "ibus"

#define DEFAULT_LANGUAGE "en_US"

struct GsdKeyboardManagerPrivate
{
	guint      start_idle_id;
        GSettings *settings;
        GSettings *input_sources_settings;
        GSettings *interface_settings;
        GnomeXkbInfo *xkb_info;
#ifdef HAVE_IBUS
        IBusBus   *ibus;
        GHashTable *ibus_engines;
        GHashTable *ibus_xkb_engines;
        GCancellable *ibus_cancellable;
        gboolean session_is_fallback;
#endif
        gint       xkb_event_base;
        GsdNumLockState old_state;
        GdkDeviceManager *device_manager;
        guint device_added_id;
        guint device_removed_id;

        gboolean input_sources_switcher_spawned;
        GPid input_sources_switcher_pid;
};

static void     gsd_keyboard_manager_class_init  (GsdKeyboardManagerClass *klass);
static void     gsd_keyboard_manager_init        (GsdKeyboardManager      *keyboard_manager);
static void     gsd_keyboard_manager_finalize    (GObject                 *object);
static gboolean apply_input_sources_settings     (GSettings               *settings,
                                                  gpointer                 keys,
                                                  gint                     n_keys,
                                                  GsdKeyboardManager      *manager);
static void     set_gtk_im_module                (GsdKeyboardManager      *manager,
                                                  const gchar             *new_module);

G_DEFINE_TYPE (GsdKeyboardManager, gsd_keyboard_manager, G_TYPE_OBJECT)

static gpointer manager_object = NULL;

static void
init_builder_with_sources (GVariantBuilder *builder,
                           GSettings       *settings)
{
        const gchar *type;
        const gchar *id;
        GVariantIter iter;
        GVariant *sources;

        sources = g_settings_get_value (settings, KEY_INPUT_SOURCES);

        g_variant_builder_init (builder, G_VARIANT_TYPE ("a(ss)"));

        g_variant_iter_init (&iter, sources);
        while (g_variant_iter_next (&iter, "(&s&s)", &type, &id))
                g_variant_builder_add (builder, "(ss)", type, id);

        g_variant_unref (sources);
}

static gboolean
schema_is_installed (const gchar *name)
{
        const gchar * const *schemas;
        const gchar * const *s;

        schemas = g_settings_list_schemas ();
        for (s = schemas; *s; ++s)
                if (g_str_equal (*s, name))
                        return TRUE;

        return FALSE;
}

#ifdef HAVE_IBUS
static void
clear_ibus (GsdKeyboardManager *manager)
{
        GsdKeyboardManagerPrivate *priv = manager->priv;

        g_cancellable_cancel (priv->ibus_cancellable);
        g_clear_object (&priv->ibus_cancellable);
        g_clear_pointer (&priv->ibus_engines, g_hash_table_destroy);
        g_clear_pointer (&priv->ibus_xkb_engines, g_hash_table_destroy);
        g_clear_object (&priv->ibus);
}

static gchar *
make_xkb_source_id (const gchar *engine_id)
{
        gchar *id;
        gchar *p;
        gint n_colons = 0;

        /* engine_id is like "xkb:layout:variant:lang" where
         * 'variant' and 'lang' might be empty */

        engine_id += 4;

        for (p = (gchar *)engine_id; *p; ++p)
                if (*p == ':')
                        if (++n_colons == 2)
                                break;
        if (!*p)
                return NULL;

        id = g_strndup (engine_id, p - engine_id + 1);

        id[p - engine_id] = '\0';

        /* id is "layout:variant" where 'variant' might be empty */

        for (p = id; *p; ++p)
                if (*p == ':') {
                        if (*(p + 1) == '\0')
                                *p = '\0';
                        else
                                *p = '+';
                        break;
                }

        /* id is "layout+variant" or "layout" */

        return id;
}

static void
fetch_ibus_engines_result (GObject            *object,
                           GAsyncResult       *result,
                           GsdKeyboardManager *manager)
{
        GsdKeyboardManagerPrivate *priv = manager->priv;
        GList *list, *l;
        GError *error = NULL;

        /* engines shouldn't be there yet */
        g_return_if_fail (priv->ibus_engines == NULL);

        g_clear_object (&priv->ibus_cancellable);

        list = ibus_bus_list_engines_async_finish (priv->ibus,
                                                   result,
                                                   &error);
        if (!list && error) {
                g_warning ("Couldn't finish IBus request: %s", error->message);
                g_error_free (error);

                clear_ibus (manager);
                return;
        }

        /* Maps IBus engine ids to engine description objects */
        priv->ibus_engines = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_object_unref);
        /* Maps XKB source id strings to engine description objects */
        priv->ibus_xkb_engines = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

        for (l = list; l; l = l->next) {
                IBusEngineDesc *engine = l->data;
                const gchar *engine_id = ibus_engine_desc_get_name (engine);

                g_hash_table_replace (priv->ibus_engines, (gpointer)engine_id, engine);

                if (strncmp ("xkb:", engine_id, 4) == 0) {
                        gchar *xkb_source_id = make_xkb_source_id (engine_id);
                        if (xkb_source_id)
                                g_hash_table_replace (priv->ibus_xkb_engines,
                                                      xkb_source_id,
                                                      engine);
                }
        }
        g_list_free (list);

        apply_input_sources_settings (priv->input_sources_settings, NULL, 0, manager);
}

static void
fetch_ibus_engines (GsdKeyboardManager *manager)
{
        GsdKeyboardManagerPrivate *priv = manager->priv;

        /* engines shouldn't be there yet */
        g_return_if_fail (priv->ibus_engines == NULL);
        g_return_if_fail (priv->ibus_cancellable == NULL);

        priv->ibus_cancellable = g_cancellable_new ();

        ibus_bus_list_engines_async (priv->ibus,
                                     -1,
                                     priv->ibus_cancellable,
                                     (GAsyncReadyCallback)fetch_ibus_engines_result,
                                     manager);
}

static void
maybe_start_ibus (GsdKeyboardManager *manager,
                  GVariant           *sources)
{
        gboolean need_ibus = FALSE;
        GVariantIter iter;
        const gchar *type;

        if (manager->priv->session_is_fallback)
                return;

        g_variant_iter_init (&iter, sources);
        while (g_variant_iter_next (&iter, "(&s&s)", &type, NULL))
                if (g_str_equal (type, INPUT_SOURCE_TYPE_IBUS)) {
                        need_ibus = TRUE;
                        break;
                }

        if (!need_ibus)
                return;

        if (!manager->priv->ibus) {
                ibus_init ();
                manager->priv->ibus = ibus_bus_new_async ();
                g_signal_connect_swapped (manager->priv->ibus, "connected",
                                          G_CALLBACK (fetch_ibus_engines), manager);
                g_signal_connect_swapped (manager->priv->ibus, "disconnected",
                                          G_CALLBACK (clear_ibus), manager);
        }
        /* IBus doesn't export API in the session bus. The only thing
         * we have there is a well known name which we can use as a
         * sure-fire way to activate it. */
        g_bus_unwatch_name (g_bus_watch_name (G_BUS_TYPE_SESSION,
                                              IBUS_SERVICE_IBUS,
                                              G_BUS_NAME_WATCHER_FLAGS_AUTO_START,
                                              NULL,
                                              NULL,
                                              NULL,
                                              NULL));
}

static void
set_ibus_engine_finish (GObject            *object,
                        GAsyncResult       *res,
                        GsdKeyboardManager *manager)
{
        gboolean result;
        IBusBus *ibus = IBUS_BUS (object);
        GsdKeyboardManagerPrivate *priv = manager->priv;
        GError *error = NULL;

        g_clear_object (&priv->ibus_cancellable);

        result = ibus_bus_set_global_engine_async_finish (ibus, res, &error);
        if (!result) {
                g_warning ("Couldn't set IBus engine: %s", error->message);
                g_error_free (error);
        }
}

static void
set_ibus_engine (GsdKeyboardManager *manager,
                 const gchar        *engine_id)
{
        GsdKeyboardManagerPrivate *priv = manager->priv;

        g_return_if_fail (priv->ibus != NULL);
        g_return_if_fail (priv->ibus_engines != NULL);

        g_cancellable_cancel (priv->ibus_cancellable);
        g_clear_object (&priv->ibus_cancellable);
        priv->ibus_cancellable = g_cancellable_new ();

        ibus_bus_set_global_engine_async (priv->ibus,
                                          engine_id,
                                          -1,
                                          priv->ibus_cancellable,
                                          (GAsyncReadyCallback)set_ibus_engine_finish,
                                          manager);
}

static void
set_ibus_xkb_engine (GsdKeyboardManager *manager,
                     const gchar        *xkb_id)
{
        IBusEngineDesc *engine;
        GsdKeyboardManagerPrivate *priv = manager->priv;

        if (!priv->ibus_xkb_engines)
                return;

        engine = g_hash_table_lookup (priv->ibus_xkb_engines, xkb_id);
        if (!engine)
                return;

        set_ibus_engine (manager, ibus_engine_desc_get_name (engine));
}

/* XXX: See upstream bug:
 * https://codereview.appspot.com/6586075/ */
static gchar *
layout_from_ibus_layout (const gchar *ibus_layout)
{
        const gchar *p;

        /* we get something like "layout(variant)[option1,option2]" */

        p = ibus_layout;
        while (*p) {
                if (*p == '(' || *p == '[')
                        break;
                p += 1;
        }

        return g_strndup (ibus_layout, p - ibus_layout);
}

static gchar *
variant_from_ibus_layout (const gchar *ibus_layout)
{
        const gchar *a, *b;

        /* we get something like "layout(variant)[option1,option2]" */

        a = ibus_layout;
        while (*a) {
                if (*a == '(')
                        break;
                a += 1;
        }
        if (!*a)
                return NULL;

        a += 1;
        b = a;
        while (*b) {
                if (*b == ')')
                        break;
                b += 1;
        }
        if (!*b)
                return NULL;

        return g_strndup (a, b - a);
}

static gchar **
options_from_ibus_layout (const gchar *ibus_layout)
{
        const gchar *a, *b;
        GPtrArray *opt_array;

        /* we get something like "layout(variant)[option1,option2]" */

        a = ibus_layout;
        while (*a) {
                if (*a == '[')
                        break;
                a += 1;
        }
        if (!*a)
                return NULL;

        opt_array = g_ptr_array_new ();

        do {
                a += 1;
                b = a;
                while (*b) {
                        if (*b == ',' || *b == ']')
                                break;
                        b += 1;
                }
                if (!*b)
                        goto out;

                g_ptr_array_add (opt_array, g_strndup (a, b - a));

                a = b;
        } while (*a && *a == ',');

out:
        g_ptr_array_add (opt_array, NULL);
        return (gchar **) g_ptr_array_free (opt_array, FALSE);
}

static const gchar *
engine_from_locale (void)
{
        const gchar *locale;
        const gchar *locale_engine[][2] = {
                { "as_IN", "m17n:as:phonetic" },
                { "bn_IN", "m17n:bn:inscript" },
                { "gu_IN", "m17n:gu:inscript" },
                { "hi_IN", "m17n:hi:inscript" },
                { "ja_JP", "anthy" },
                { "kn_IN", "m17n:kn:kgp" },
                { "ko_KR", "hangul" },
                { "mai_IN", "m17n:mai:inscript" },
                { "ml_IN", "m17n:ml:inscript" },
                { "mr_IN", "m17n:mr:inscript" },
                { "or_IN", "m17n:or:inscript" },
                { "pa_IN", "m17n:pa:inscript" },
                { "sd_IN", "m17n:sd:inscript" },
                { "ta_IN", "m17n:ta:tamil99" },
                { "te_IN", "m17n:te:inscript" },
                { "zh_CN", "pinyin" },
                { "zh_HK", "cangjie3" },
                { "zh_TW", "chewing" },
        };
        gint i;

        locale = setlocale (LC_CTYPE, NULL);
        if (!locale)
                return NULL;

        for (i = 0; i < G_N_ELEMENTS (locale_engine); ++i)
                if (g_str_has_prefix (locale, locale_engine[i][0]))
                        return locale_engine[i][1];

        return NULL;
}

static void
add_ibus_sources_from_locale (GSettings *settings)
{
        const gchar *locale_engine;
        GVariantBuilder builder;

        locale_engine = engine_from_locale ();
        if (!locale_engine)
                return;

        init_builder_with_sources (&builder, settings);
        g_variant_builder_add (&builder, "(ss)", INPUT_SOURCE_TYPE_IBUS, locale_engine);
        g_settings_set_value (settings, KEY_INPUT_SOURCES, g_variant_builder_end (&builder));
}

static void
convert_ibus (GSettings *settings)
{
        GVariantBuilder builder;
        GSettings *ibus_settings;
        gchar **engines, **e;

        if (!schema_is_installed ("org.freedesktop.ibus.general"))
                return;

        init_builder_with_sources (&builder, settings);

        ibus_settings = g_settings_new ("org.freedesktop.ibus.general");
        engines = g_settings_get_strv (ibus_settings, "preload-engines");
        for (e = engines; *e; ++e)
                g_variant_builder_add (&builder, "(ss)", INPUT_SOURCE_TYPE_IBUS, *e);

        g_settings_set_value (settings, KEY_INPUT_SOURCES, g_variant_builder_end (&builder));

        g_strfreev (engines);
        g_object_unref (ibus_settings);
}
#endif  /* HAVE_IBUS */

static gboolean
xkb_set_keyboard_autorepeat_rate (guint delay, guint interval)
{
        return XkbSetAutoRepeatRate (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()),
                                     XkbUseCoreKbd,
                                     delay,
                                     interval);
}

static gboolean
check_xkb_extension (GsdKeyboardManager *manager)
{
        Display *dpy = GDK_DISPLAY_XDISPLAY (gdk_display_get_default ());
        int opcode, error_base, major, minor;
        gboolean have_xkb;

        have_xkb = XkbQueryExtension (dpy,
                                      &opcode,
                                      &manager->priv->xkb_event_base,
                                      &error_base,
                                      &major,
                                      &minor);
        return have_xkb;
}

static void
xkb_init (GsdKeyboardManager *manager)
{
        Display *dpy;

        dpy = GDK_DISPLAY_XDISPLAY (gdk_display_get_default ());
        XkbSelectEventDetails (dpy,
                               XkbUseCoreKbd,
                               XkbStateNotify,
                               XkbModifierLockMask,
                               XkbModifierLockMask);
}

static unsigned
numlock_NumLock_modifier_mask (void)
{
        Display *dpy = GDK_DISPLAY_XDISPLAY (gdk_display_get_default ());
        return XkbKeysymToModifiers (dpy, XK_Num_Lock);
}

static void
numlock_set_xkb_state (GsdNumLockState new_state)
{
        unsigned int num_mask;
        Display *dpy = GDK_DISPLAY_XDISPLAY (gdk_display_get_default ());
        if (new_state != GSD_NUM_LOCK_STATE_ON && new_state != GSD_NUM_LOCK_STATE_OFF)
                return;
        num_mask = numlock_NumLock_modifier_mask ();
        XkbLockModifiers (dpy, XkbUseCoreKbd, num_mask, new_state == GSD_NUM_LOCK_STATE_ON ? num_mask : 0);
}

static const char *
num_lock_state_to_string (GsdNumLockState numlock_state)
{
	switch (numlock_state) {
	case GSD_NUM_LOCK_STATE_UNKNOWN:
		return "GSD_NUM_LOCK_STATE_UNKNOWN";
	case GSD_NUM_LOCK_STATE_ON:
		return "GSD_NUM_LOCK_STATE_ON";
	case GSD_NUM_LOCK_STATE_OFF:
		return "GSD_NUM_LOCK_STATE_OFF";
	default:
		return "UNKNOWN";
	}
}

static GdkFilterReturn
xkb_events_filter (GdkXEvent *xev_,
		   GdkEvent  *gdkev_,
		   gpointer   user_data)
{
        XEvent *xev = (XEvent *) xev_;
	XkbEvent *xkbev = (XkbEvent *) xev;
        GsdKeyboardManager *manager = (GsdKeyboardManager *) user_data;

        if (xev->type != manager->priv->xkb_event_base ||
            xkbev->any.xkb_type != XkbStateNotify)
		return GDK_FILTER_CONTINUE;

	if (xkbev->state.changed & XkbModifierLockMask) {
		unsigned num_mask = numlock_NumLock_modifier_mask ();
		unsigned locked_mods = xkbev->state.locked_mods;
		GsdNumLockState numlock_state;

		numlock_state = (num_mask & locked_mods) ? GSD_NUM_LOCK_STATE_ON : GSD_NUM_LOCK_STATE_OFF;

		if (numlock_state != manager->priv->old_state) {
			g_debug ("New num-lock state '%s' != Old num-lock state '%s'",
				 num_lock_state_to_string (numlock_state),
				 num_lock_state_to_string (manager->priv->old_state));
			g_settings_set_enum (manager->priv->settings,
					     KEY_NUMLOCK_STATE,
					     numlock_state);
			manager->priv->old_state = numlock_state;
		}
	}

        return GDK_FILTER_CONTINUE;
}

static void
install_xkb_filter (GsdKeyboardManager *manager)
{
        gdk_window_add_filter (NULL,
                               xkb_events_filter,
                               manager);
}

static void
remove_xkb_filter (GsdKeyboardManager *manager)
{
        gdk_window_remove_filter (NULL,
                                  xkb_events_filter,
                                  manager);
}

static void
free_xkb_component_names (XkbComponentNamesRec *p)
{
        g_return_if_fail (p != NULL);

        free (p->keymap);
        free (p->keycodes);
        free (p->types);
        free (p->compat);
        free (p->symbols);
        free (p->geometry);

        g_free (p);
}

static void
upload_xkb_description (const gchar          *rules_file_path,
                        XkbRF_VarDefsRec     *var_defs,
                        XkbComponentNamesRec *comp_names)
{
        Display *display = GDK_DISPLAY_XDISPLAY (gdk_display_get_default ());
        XkbDescRec *xkb_desc;
        gchar *rules_file;

        /* Upload it to the X server using the same method as setxkbmap */
        xkb_desc = XkbGetKeyboardByName (display,
                                         XkbUseCoreKbd,
                                         comp_names,
                                         XkbGBN_AllComponentsMask,
                                         XkbGBN_AllComponentsMask &
                                         (~XkbGBN_GeometryMask), True);
        if (!xkb_desc) {
                g_warning ("Couldn't upload new XKB keyboard description");
                return;
        }

        XkbFreeKeyboard (xkb_desc, 0, True);

        rules_file = g_path_get_basename (rules_file_path);

        if (!XkbRF_SetNamesProp (display, rules_file, var_defs))
                g_warning ("Couldn't update the XKB root window property");

        g_free (rules_file);
}

static gchar *
language_code_from_locale (const gchar *locale)
{
        if (!locale || !locale[0] || !locale[1])
                return NULL;

        if (!locale[2] || locale[2] == '_' || locale[2] == '.')
                return g_strndup (locale, 2);

        if (!locale[3] || locale[3] == '_' || locale[3] == '.')
                return g_strndup (locale, 3);

        return NULL;
}

static gchar *
build_xkb_group_string (const gchar *user,
                        const gchar *locale,
                        const gchar *latin)
{
        gchar *string;
        gsize length = 0;
        guint commas = 2;

        if (latin)
                length += strlen (latin);
        else
                commas -= 1;

        if (locale)
                length += strlen (locale);
        else
                commas -= 1;

        length += strlen (user) + commas + 1;

        string = malloc (length);

        if (locale && latin)
                sprintf (string, "%s,%s,%s", user, locale, latin);
        else if (locale)
                sprintf (string, "%s,%s", user, locale);
        else if (latin)
                sprintf (string, "%s,%s", user, latin);
        else
                sprintf (string, "%s", user);

        return string;
}

static gboolean
layout_equal (const gchar *layout_a,
              const gchar *variant_a,
              const gchar *layout_b,
              const gchar *variant_b)
{
        return !g_strcmp0 (layout_a, layout_b) && !g_strcmp0 (variant_a, variant_b);
}

static void
replace_layout_and_variant (GsdKeyboardManager *manager,
                            XkbRF_VarDefsRec   *xkb_var_defs,
                            const gchar        *layout,
                            const gchar        *variant)
{
        /* Toolkits need to know about both a latin layout to handle
         * accelerators which are usually defined like Ctrl+C and a
         * layout with the symbols for the language used in UI strings
         * to handle mnemonics like Alt+Ф, so we try to find and add
         * them in XKB group slots after the layout which the user
         * actually intends to type with. */
        const gchar *latin_layout = "us";
        const gchar *latin_variant = "";
        const gchar *locale_layout = NULL;
        const gchar *locale_variant = NULL;
        const gchar *locale;
        gchar *language;

        if (!layout)
                return;

        if (!variant)
                variant = "";

        locale = setlocale (LC_MESSAGES, NULL);
        /* If LANG is empty, default to en_US */
        if (!locale)
                language = g_strdup (DEFAULT_LANGUAGE);
        else
                language = language_code_from_locale (locale);

        if (!language)
                language = language_code_from_locale (DEFAULT_LANGUAGE);

        gnome_xkb_info_get_layout_info_for_language (manager->priv->xkb_info,
                                                     language,
                                                     NULL,
                                                     NULL,
                                                     NULL,
                                                     &locale_layout,
                                                     &locale_variant);
        g_free (language);

        /* We want to minimize the number of XKB groups if we have
         * duplicated layout+variant pairs.
         *
         * Also, if a layout doesn't have a variant we still have to
         * include it in the variants string because the number of
         * variants must agree with the number of layouts. For
         * instance:
         *
         * layouts:  "us,ru,us"
         * variants: "dvorak,,"
         */
        if (layout_equal (latin_layout, latin_variant, locale_layout, locale_variant) ||
            layout_equal (latin_layout, latin_variant, layout, variant)) {
                latin_layout = NULL;
                latin_variant = NULL;
        }

        if (layout_equal (locale_layout, locale_variant, layout, variant)) {
                locale_layout = NULL;
                locale_variant = NULL;
        }

        free (xkb_var_defs->layout);
        xkb_var_defs->layout = build_xkb_group_string (layout, locale_layout, latin_layout);

        free (xkb_var_defs->variant);
        xkb_var_defs->variant = build_xkb_group_string (variant, locale_variant, latin_variant);
}

static gchar *
build_xkb_options_string (gchar **options)
{
        gchar *string;

        if (*options) {
                gint i;
                gsize len;
                gchar *ptr;

                /* First part, getting length */
                len = 1 + strlen (options[0]);
                for (i = 1; options[i] != NULL; i++)
                        len += strlen (options[i]);
                len += (i - 1); /* commas */

                /* Second part, building string */
                string = malloc (len);
                ptr = g_stpcpy (string, *options);
                for (i = 1; options[i] != NULL; i++) {
                        ptr = g_stpcpy (ptr, ",");
                        ptr = g_stpcpy (ptr, options[i]);
                }
        } else {
                string = malloc (1);
                *string = '\0';
        }

        return string;
}

static gchar **
append_options (gchar **a,
                gchar **b)
{
        gchar **c, **p;

        if (!a && !b)
                return NULL;
        else if (!a)
                return g_strdupv (b);
        else if (!b)
                return g_strdupv (a);

        c = g_new0 (gchar *, g_strv_length (a) + g_strv_length (b) + 1);
        p = c;

        while (*a) {
                *p = g_strdup (*a);
                p += 1;
                a += 1;
        }
        while (*b) {
                *p = g_strdup (*b);
                p += 1;
                b += 1;
        }

        return c;
}

static void
add_xkb_options (GsdKeyboardManager *manager,
                 XkbRF_VarDefsRec   *xkb_var_defs,
                 gchar             **extra_options)
{
        gchar **options;
        gchar **settings_options;

        settings_options = g_settings_get_strv (manager->priv->input_sources_settings,
                                                KEY_KEYBOARD_OPTIONS);
        options = append_options (settings_options, extra_options);
        g_strfreev (settings_options);

        free (xkb_var_defs->options);
        xkb_var_defs->options = build_xkb_options_string (options);

        g_strfreev (options);
}

static void
apply_xkb_settings (GsdKeyboardManager *manager,
                    const gchar        *layout,
                    const gchar        *variant,
                    gchar             **options)
{
        XkbRF_RulesRec *xkb_rules;
        XkbRF_VarDefsRec *xkb_var_defs;
        gchar *rules_file_path;

        gnome_xkb_info_get_var_defs (&rules_file_path, &xkb_var_defs);

        add_xkb_options (manager, xkb_var_defs, options);
        replace_layout_and_variant (manager, xkb_var_defs, layout, variant);

        gdk_error_trap_push ();

        xkb_rules = XkbRF_Load (rules_file_path, NULL, True, True);
        if (xkb_rules) {
                XkbComponentNamesRec *xkb_comp_names;
                xkb_comp_names = g_new0 (XkbComponentNamesRec, 1);

                XkbRF_GetComponents (xkb_rules, xkb_var_defs, xkb_comp_names);
                upload_xkb_description (rules_file_path, xkb_var_defs, xkb_comp_names);

                free_xkb_component_names (xkb_comp_names);
                XkbRF_Free (xkb_rules, True);
        } else {
                g_warning ("Couldn't load XKB rules");
        }

        if (gdk_error_trap_pop ())
                g_warning ("Error loading XKB rules");

        gnome_xkb_info_free_var_defs (xkb_var_defs);
        g_free (rules_file_path);
}

static void
set_gtk_im_module (GsdKeyboardManager *manager,
                   const gchar        *new_module)
{
        GsdKeyboardManagerPrivate *priv = manager->priv;
        gchar *current_module;

        current_module = g_settings_get_string (priv->interface_settings,
                                                KEY_GTK_IM_MODULE);
        if (!g_str_equal (current_module, new_module))
                g_settings_set_string (priv->interface_settings,
                                       KEY_GTK_IM_MODULE,
                                       new_module);
        g_free (current_module);
}

static gboolean
apply_input_sources_settings (GSettings          *settings,
                              gpointer            keys,
                              gint                n_keys,
                              GsdKeyboardManager *manager)
{
        GsdKeyboardManagerPrivate *priv = manager->priv;
        GVariant *sources;
        guint current;
        guint n_sources;
        const gchar *type = NULL;
        const gchar *id = NULL;
        gchar *layout = NULL;
        gchar *variant = NULL;
        gchar **options = NULL;

        sources = g_settings_get_value (priv->input_sources_settings, KEY_INPUT_SOURCES);
        current = g_settings_get_uint (priv->input_sources_settings, KEY_CURRENT_INPUT_SOURCE);
        n_sources = g_variant_n_children (sources);

        if (n_sources < 1)
                goto exit;

        if (current >= n_sources) {
                g_settings_set_uint (priv->input_sources_settings,
                                     KEY_CURRENT_INPUT_SOURCE,
                                     n_sources - 1);
                goto exit;
        }

#ifdef HAVE_IBUS
        maybe_start_ibus (manager, sources);
#endif

        g_variant_get_child (sources, current, "(&s&s)", &type, &id);

        if (g_str_equal (type, INPUT_SOURCE_TYPE_XKB)) {
                const gchar *l, *v;
                gnome_xkb_info_get_layout_info (priv->xkb_info, id, NULL, NULL, &l, &v);

                layout = g_strdup (l);
                variant = g_strdup (v);

                if (!layout || !layout[0]) {
                        g_warning ("Couldn't find XKB input source '%s'", id);
                        goto exit;
                }
                set_gtk_im_module (manager, GTK_IM_MODULE_SIMPLE);
#ifdef HAVE_IBUS
                set_ibus_xkb_engine (manager, id);
#endif
        } else if (g_str_equal (type, INPUT_SOURCE_TYPE_IBUS)) {
#ifdef HAVE_IBUS
                IBusEngineDesc *engine_desc = NULL;

                if (priv->session_is_fallback)
                        goto exit;

                if (priv->ibus_engines)
                        engine_desc = g_hash_table_lookup (priv->ibus_engines, id);
                else
                        goto exit; /* we'll be called again when ibus is up and running */

                if (engine_desc) {
                        const gchar *ibus_layout;
                        ibus_layout = ibus_engine_desc_get_layout (engine_desc);

                        if (ibus_layout) {
                                layout = layout_from_ibus_layout (ibus_layout);
                                variant = variant_from_ibus_layout (ibus_layout);
                                options = options_from_ibus_layout (ibus_layout);
                        }
                } else {
                        g_warning ("Couldn't find IBus input source '%s'", id);
                        goto exit;
                }

                set_gtk_im_module (manager, GTK_IM_MODULE_IBUS);
                set_ibus_engine (manager, id);
#else
                g_warning ("IBus input source type specified but IBus support was not compiled");
#endif
        } else {
                g_warning ("Unknown input source type '%s'", type);
        }

 exit:
        apply_xkb_settings (manager, layout, variant, options);
        g_variant_unref (sources);
        g_free (layout);
        g_free (variant);
        g_strfreev (options);
        /* Prevent individual "changed" signal invocations since we
           don't need them. */
        return TRUE;
}

static void
apply_bell (GsdKeyboardManager *manager)
{
	GSettings       *settings;
        XKeyboardControl kbdcontrol;
        gboolean         click;
        int              bell_volume;
        int              bell_pitch;
        int              bell_duration;
        GsdBellMode      bell_mode;
        int              click_volume;

        g_debug ("Applying the bell settings");
        settings      = manager->priv->settings;
        click         = g_settings_get_boolean  (settings, KEY_CLICK);
        click_volume  = g_settings_get_int   (settings, KEY_CLICK_VOLUME);

        bell_pitch    = g_settings_get_int   (settings, KEY_BELL_PITCH);
        bell_duration = g_settings_get_int   (settings, KEY_BELL_DURATION);

        bell_mode = g_settings_get_enum (settings, KEY_BELL_MODE);
        bell_volume   = (bell_mode == GSD_BELL_MODE_ON) ? 50 : 0;

        /* as percentage from 0..100 inclusive */
        if (click_volume < 0) {
                click_volume = 0;
        } else if (click_volume > 100) {
                click_volume = 100;
        }
        kbdcontrol.key_click_percent = click ? click_volume : 0;
        kbdcontrol.bell_percent = bell_volume;
        kbdcontrol.bell_pitch = bell_pitch;
        kbdcontrol.bell_duration = bell_duration;

        gdk_error_trap_push ();
        XChangeKeyboardControl (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()),
                                KBKeyClickPercent | KBBellPercent | KBBellPitch | KBBellDuration,
                                &kbdcontrol);

        XSync (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), FALSE);
        gdk_error_trap_pop_ignored ();
}

static void
apply_numlock (GsdKeyboardManager *manager)
{
	GSettings *settings;
        gboolean rnumlock;

        g_debug ("Applying the num-lock settings");
        settings = manager->priv->settings;
        rnumlock = g_settings_get_boolean  (settings, KEY_REMEMBER_NUMLOCK_STATE);
        manager->priv->old_state = g_settings_get_enum (manager->priv->settings, KEY_NUMLOCK_STATE);

        gdk_error_trap_push ();
        if (rnumlock) {
                g_debug ("Remember num-lock is set, so applying setting '%s'",
                         num_lock_state_to_string (manager->priv->old_state));
                numlock_set_xkb_state (manager->priv->old_state);
        }

        XSync (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), FALSE);
        gdk_error_trap_pop_ignored ();
}

static void
apply_repeat (GsdKeyboardManager *manager)
{
	GSettings       *settings;
        gboolean         repeat;
        guint            interval;
        guint            delay;

        g_debug ("Applying the repeat settings");
        settings      = manager->priv->settings;
        repeat        = g_settings_get_boolean  (settings, KEY_REPEAT);
        interval      = g_settings_get_uint  (settings, KEY_INTERVAL);
        delay         = g_settings_get_uint  (settings, KEY_DELAY);

        gdk_error_trap_push ();
        if (repeat) {
                gboolean rate_set = FALSE;

                XAutoRepeatOn (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()));
                /* Use XKB in preference */
                rate_set = xkb_set_keyboard_autorepeat_rate (delay, interval);

                if (!rate_set)
                        g_warning ("Neither XKeyboard not Xfree86's keyboard extensions are available,\n"
                                   "no way to support keyboard autorepeat rate settings");
        } else {
                XAutoRepeatOff (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()));
        }

        XSync (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), FALSE);
        gdk_error_trap_pop_ignored ();
}

static void
apply_all_settings (GsdKeyboardManager *manager)
{
	apply_repeat (manager);
	apply_bell (manager);
	apply_numlock (manager);
}

static void
set_input_sources_switcher (GsdKeyboardManager *manager,
                            gboolean            state)
{
        if (state) {
                GError *error = NULL;
                char *args[2];

                if (manager->priv->input_sources_switcher_spawned)
                        set_input_sources_switcher (manager, FALSE);

                args[0] = LIBEXECDIR "/gsd-input-sources-switcher";
                args[1] = NULL;

                g_spawn_async (NULL, args, NULL,
                               0, NULL, NULL,
                               &manager->priv->input_sources_switcher_pid, &error);

                manager->priv->input_sources_switcher_spawned = (error == NULL);

                if (error) {
                        g_warning ("Couldn't spawn %s: %s", args[0], error->message);
                        g_error_free (error);
                }
        } else if (manager->priv->input_sources_switcher_spawned) {
                kill (manager->priv->input_sources_switcher_pid, SIGHUP);
                g_spawn_close_pid (manager->priv->input_sources_switcher_pid);
                manager->priv->input_sources_switcher_spawned = FALSE;
        }
}

static gboolean
enable_switcher (GsdKeyboardManager *manager)
{
        GsdInputSourcesSwitcher switcher;

        switcher = g_settings_get_enum (manager->priv->settings, KEY_SWITCHER);

        return switcher != GSD_INPUT_SOURCES_SWITCHER_OFF;
}

static void
settings_changed (GSettings          *settings,
                  const char         *key,
                  GsdKeyboardManager *manager)
{
	if (g_strcmp0 (key, KEY_CLICK) == 0||
	    g_strcmp0 (key, KEY_CLICK_VOLUME) == 0 ||
	    g_strcmp0 (key, KEY_BELL_PITCH) == 0 ||
	    g_strcmp0 (key, KEY_BELL_DURATION) == 0 ||
	    g_strcmp0 (key, KEY_BELL_MODE) == 0) {
		g_debug ("Bell setting '%s' changed, applying bell settings", key);
		apply_bell (manager);
	} else if (g_strcmp0 (key, KEY_REMEMBER_NUMLOCK_STATE) == 0 ||
		 g_strcmp0 (key, KEY_NUMLOCK_STATE) == 0) {
		g_debug ("Num-Lock state '%s' changed, applying num-lock state settings", key);
		apply_numlock (manager);
	} else if (g_strcmp0 (key, KEY_REPEAT) == 0 ||
		 g_strcmp0 (key, KEY_INTERVAL) == 0 ||
		 g_strcmp0 (key, KEY_DELAY) == 0) {
		g_debug ("Key repeat setting '%s' changed, applying key repeat settings", key);
		apply_repeat (manager);
        } else if (g_strcmp0 (key, KEY_SWITCHER) == 0) {
                set_input_sources_switcher (manager, enable_switcher (manager));
	} else {
		g_warning ("Unhandled settings change, key '%s'", key);
	}

}

static void
device_added_cb (GdkDeviceManager   *device_manager,
                 GdkDevice          *device,
                 GsdKeyboardManager *manager)
{
        GdkInputSource source;

        source = gdk_device_get_source (device);
        if (source == GDK_SOURCE_KEYBOARD) {
                g_debug ("New keyboard plugged in, applying all settings");
                apply_input_sources_settings (manager->priv->input_sources_settings, NULL, 0, manager);
                run_custom_command (device, COMMAND_DEVICE_ADDED);
        }
}

static void
device_removed_cb (GdkDeviceManager   *device_manager,
                   GdkDevice          *device,
                   GsdKeyboardManager *manager)
{
        GdkInputSource source;

        source = gdk_device_get_source (device);
        if (source == GDK_SOURCE_KEYBOARD) {
                run_custom_command (device, COMMAND_DEVICE_REMOVED);
        }
}

static void
set_devicepresence_handler (GsdKeyboardManager *manager)
{
        GdkDeviceManager *device_manager;

        device_manager = gdk_display_get_device_manager (gdk_display_get_default ());

        manager->priv->device_added_id = g_signal_connect (G_OBJECT (device_manager), "device-added",
                                                           G_CALLBACK (device_added_cb), manager);
        manager->priv->device_removed_id = g_signal_connect (G_OBJECT (device_manager), "device-removed",
                                                             G_CALLBACK (device_removed_cb), manager);
        manager->priv->device_manager = device_manager;
}

static void
create_sources_from_current_xkb_config (GSettings *settings)
{
        GVariantBuilder builder;
        XkbRF_VarDefsRec *xkb_var_defs;
        gchar *tmp;
        gchar **layouts = NULL;
        gchar **variants = NULL;
        guint i, n;

        gnome_xkb_info_get_var_defs (&tmp, &xkb_var_defs);
        g_free (tmp);

        if (xkb_var_defs->layout)
                layouts = g_strsplit (xkb_var_defs->layout, ",", 0);
        if (xkb_var_defs->variant)
                variants = g_strsplit (xkb_var_defs->variant, ",", 0);

        gnome_xkb_info_free_var_defs (xkb_var_defs);

        if (!layouts)
                goto out;

        if (variants && variants[0])
                n = MIN (g_strv_length (layouts), g_strv_length (variants));
        else
                n = g_strv_length (layouts);

        g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ss)"));
        for (i = 0; i < n && layouts[i][0]; ++i) {
                if (variants && variants[i] && variants[i][0])
                        tmp = g_strdup_printf ("%s+%s", layouts[i], variants[i]);
                else
                        tmp = g_strdup (layouts[i]);

                g_variant_builder_add (&builder, "(ss)", INPUT_SOURCE_TYPE_XKB, tmp);
                g_free (tmp);
        }
        g_settings_set_value (settings, KEY_INPUT_SOURCES, g_variant_builder_end (&builder));
out:
        g_strfreev (layouts);
        g_strfreev (variants);
}

static void
convert_libgnomekbd_options (GSettings *settings)
{
        GPtrArray *opt_array;
        GSettings *libgnomekbd_settings;
        gchar **options, **o;

        if (!schema_is_installed ("org.gnome.libgnomekbd.keyboard"))
                return;

        opt_array = g_ptr_array_new_with_free_func (g_free);

        libgnomekbd_settings = g_settings_new ("org.gnome.libgnomekbd.keyboard");
        options = g_settings_get_strv (libgnomekbd_settings, "options");

        for (o = options; *o; ++o) {
                gchar **strv;

                strv = g_strsplit (*o, "\t", 2);
                if (strv[0] && strv[1]) {
                        /* We don't want the group switcher because
                         * it's incompatible with the way we use XKB
                         * groups. */
                        if (!g_str_has_prefix (strv[1], "grp:"))
                                g_ptr_array_add (opt_array, g_strdup (strv[1]));
                }
                g_strfreev (strv);
        }
        g_ptr_array_add (opt_array, NULL);

        g_settings_set_strv (settings, KEY_KEYBOARD_OPTIONS, (const gchar * const*) opt_array->pdata);

        g_strfreev (options);
        g_object_unref (libgnomekbd_settings);
        g_ptr_array_free (opt_array, TRUE);
}

static void
convert_libgnomekbd_layouts (GSettings *settings)
{
        GVariantBuilder builder;
        GSettings *libgnomekbd_settings;
        gchar **layouts, **l;

        if (!schema_is_installed ("org.gnome.libgnomekbd.keyboard"))
                return;

        init_builder_with_sources (&builder, settings);

        libgnomekbd_settings = g_settings_new ("org.gnome.libgnomekbd.keyboard");
        layouts = g_settings_get_strv (libgnomekbd_settings, "layouts");

        for (l = layouts; *l; ++l) {
                gchar *id;
                gchar **strv;

                strv = g_strsplit (*l, "\t", 2);
                if (strv[0] && !strv[1])
                        id = g_strdup (strv[0]);
                else if (strv[0] && strv[1])
                        id = g_strdup_printf ("%s+%s", strv[0], strv[1]);
                else
                        id = NULL;

                if (id)
                        g_variant_builder_add (&builder, "(ss)", INPUT_SOURCE_TYPE_XKB, id);

                g_free (id);
                g_strfreev (strv);
        }

        g_settings_set_value (settings, KEY_INPUT_SOURCES, g_variant_builder_end (&builder));

        g_strfreev (layouts);
        g_object_unref (libgnomekbd_settings);
}

static void
maybe_convert_old_settings (GSettings *settings)
{
        GVariant *sources;
        gchar **options;
        gchar *stamp_dir_path = NULL;
        gchar *stamp_file_path = NULL;
        GError *error = NULL;

        stamp_dir_path = g_build_filename (g_get_user_data_dir (), PACKAGE_NAME, NULL);
        if (g_mkdir_with_parents (stamp_dir_path, 0755)) {
                g_warning ("Failed to create directory %s: %s", stamp_dir_path, g_strerror (errno));
                goto out;
        }

        stamp_file_path = g_build_filename (stamp_dir_path, "input-sources-converted", NULL);
        if (g_file_test (stamp_file_path, G_FILE_TEST_EXISTS))
                goto out;

        sources = g_settings_get_value (settings, KEY_INPUT_SOURCES);
        if (g_variant_n_children (sources) < 1) {
                convert_libgnomekbd_layouts (settings);
#ifdef HAVE_IBUS
                convert_ibus (settings);
#endif
        }
        g_variant_unref (sources);

        options = g_settings_get_strv (settings, KEY_KEYBOARD_OPTIONS);
        if (g_strv_length (options) < 1)
                convert_libgnomekbd_options (settings);
        g_strfreev (options);

        if (!g_file_set_contents (stamp_file_path, "", 0, &error)) {
                g_warning ("%s", error->message);
                g_error_free (error);
        }
out:
        g_free (stamp_file_path);
        g_free (stamp_dir_path);
}

static void
maybe_create_input_sources (GsdKeyboardManager *manager)
{
        GSettings *settings;
        GVariant *sources;

        settings = manager->priv->input_sources_settings;

        if (g_getenv ("RUNNING_UNDER_GDM")) {
                create_sources_from_current_xkb_config (settings);
                return;
        }

        maybe_convert_old_settings (settings);

        /* if we still don't have anything do some educated guesses */
        sources = g_settings_get_value (settings, KEY_INPUT_SOURCES);
        if (g_variant_n_children (sources) < 1) {
                create_sources_from_current_xkb_config (settings);
#ifdef HAVE_IBUS
                add_ibus_sources_from_locale (settings);
#endif
        }
        g_variant_unref (sources);
}

static gboolean
start_keyboard_idle_cb (GsdKeyboardManager *manager)
{
#ifdef HAVE_IBUS
        GDBusProxy *proxy;
#endif
        GVariant *prop;
        const gchar *name;

        gnome_settings_profile_start (NULL);

        g_debug ("Starting keyboard manager");

        manager->priv->settings = g_settings_new (GSD_KEYBOARD_DIR);

	xkb_init (manager);

	set_devicepresence_handler (manager);

        manager->priv->input_sources_settings = g_settings_new (GNOME_DESKTOP_INPUT_SOURCES_DIR);
        manager->priv->interface_settings = g_settings_new (GNOME_DESKTOP_INTERFACE_DIR);
        manager->priv->xkb_info = gnome_xkb_info_new ();

        maybe_create_input_sources (manager);

#ifdef HAVE_IBUS
        proxy = gnome_settings_session_get_session_proxy ();
        prop = g_dbus_proxy_get_cached_property (proxy, "session-name");
        if (prop) {
                g_variant_get (prop, "&s", &name);
                manager->priv->session_is_fallback = g_strcmp0 (name, "gnome") != 0;
                g_variant_unref (prop);
        } else {
                manager->priv->session_is_fallback = FALSE;
                g_warning ("failed to get SessionName, assuming gnome\n");
        }
        g_object_unref (proxy);
#endif /* HAVE_IBUS */

        apply_input_sources_settings (manager->priv->input_sources_settings, NULL, 0, manager);
        /* apply current settings before we install the callback */
        g_debug ("Started the keyboard plugin, applying all settings");
        apply_all_settings (manager);

        g_signal_connect (G_OBJECT (manager->priv->settings), "changed",
                          G_CALLBACK (settings_changed), manager);
        g_signal_connect (G_OBJECT (manager->priv->input_sources_settings), "change-event",
                          G_CALLBACK (apply_input_sources_settings), manager);

	install_xkb_filter (manager);
        set_input_sources_switcher (manager, enable_switcher (manager));

        gnome_settings_profile_end (NULL);

        manager->priv->start_idle_id = 0;

        return FALSE;
}

gboolean
gsd_keyboard_manager_start (GsdKeyboardManager *manager,
                            GError            **error)
{
        gnome_settings_profile_start (NULL);

	if (check_xkb_extension (manager) == FALSE) {
		g_debug ("XKB is not supported, not applying any settings");
		return TRUE;
	}

        manager->priv->start_idle_id = g_idle_add ((GSourceFunc) start_keyboard_idle_cb, manager);

        gnome_settings_profile_end (NULL);

        return TRUE;
}

void
gsd_keyboard_manager_stop (GsdKeyboardManager *manager)
{
        GsdKeyboardManagerPrivate *p = manager->priv;

        g_debug ("Stopping keyboard manager");

        g_clear_object (&p->settings);
        g_clear_object (&p->input_sources_settings);
        g_clear_object (&p->interface_settings);
        g_clear_object (&p->xkb_info);

#ifdef HAVE_IBUS
        clear_ibus (manager);
#endif

        if (p->device_manager != NULL) {
                g_signal_handler_disconnect (p->device_manager, p->device_added_id);
                g_signal_handler_disconnect (p->device_manager, p->device_removed_id);
                p->device_manager = NULL;
        }

	remove_xkb_filter (manager);

        set_input_sources_switcher (manager, FALSE);
}

static void
gsd_keyboard_manager_class_init (GsdKeyboardManagerClass *klass)
{
        GObjectClass   *object_class = G_OBJECT_CLASS (klass);

        object_class->finalize = gsd_keyboard_manager_finalize;

        g_type_class_add_private (klass, sizeof (GsdKeyboardManagerPrivate));
}

static void
gsd_keyboard_manager_init (GsdKeyboardManager *manager)
{
        manager->priv = GSD_KEYBOARD_MANAGER_GET_PRIVATE (manager);
}

static void
gsd_keyboard_manager_finalize (GObject *object)
{
        GsdKeyboardManager *keyboard_manager;

        g_return_if_fail (object != NULL);
        g_return_if_fail (GSD_IS_KEYBOARD_MANAGER (object));

        keyboard_manager = GSD_KEYBOARD_MANAGER (object);

        g_return_if_fail (keyboard_manager->priv != NULL);

        if (keyboard_manager->priv->start_idle_id != 0)
                g_source_remove (keyboard_manager->priv->start_idle_id);

        G_OBJECT_CLASS (gsd_keyboard_manager_parent_class)->finalize (object);
}

GsdKeyboardManager *
gsd_keyboard_manager_new (void)
{
        if (manager_object != NULL) {
                g_object_ref (manager_object);
        } else {
                manager_object = g_object_new (GSD_TYPE_KEYBOARD_MANAGER, NULL);
                g_object_add_weak_pointer (manager_object,
                                           (gpointer *) &manager_object);
        }

        return GSD_KEYBOARD_MANAGER (manager_object);
}
