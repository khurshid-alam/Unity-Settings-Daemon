/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007 William Jon McCann <mccann@jhu.edu>
 * Copyright (C) 2010 Red Hat, Inc.
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
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/Xatom.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#include <Xwacom.h>

#include "gsd-enums.h"
#include "gsd-input-helper.h"
#include "gsd-keygrab.h"
#include "gnome-settings-profile.h"
#include "gsd-wacom-manager.h"
#include "gsd-wacom-device.h"

#define GSD_WACOM_MANAGER_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), GSD_TYPE_WACOM_MANAGER, GsdWacomManagerPrivate))

#define KEY_ROTATION            "rotation"
#define KEY_TOUCH               "touch"
#define KEY_TPCBUTTON           "tablet-pc-button"
#define KEY_IS_ABSOLUTE         "is-absolute"
#define KEY_AREA                "area"
#define KEY_DISPLAY             "display"

/* Stylus and Eraser settings */
#define KEY_BUTTON_MAPPING      "buttonmapping"
#define KEY_PRESSURETHRESHOLD   "pressurethreshold"
#define KEY_PRESSURECURVE       "pressurecurve"

/* Button settings */
#define KEY_ACTION_TYPE         "action-type"
#define KEY_CUSTOM_ACTION       "custom-action"
#define KEY_CUSTOM_ELEVATOR_ACTION "custom-elevator-action"

/* See "Wacom Pressure Threshold" */
#define DEFAULT_PRESSURE_THRESHOLD 27

struct GsdWacomManagerPrivate
{
        guint start_idle_id;
        GdkDeviceManager *device_manager;
        guint device_added_id;
        guint device_removed_id;
        GHashTable *devices; /* key = GdkDevice, value = GsdWacomDevice */

        /* button capture */
        GSList *screens;
        int      opcode;
};

static void     gsd_wacom_manager_class_init  (GsdWacomManagerClass *klass);
static void     gsd_wacom_manager_init        (GsdWacomManager      *wacom_manager);
static void     gsd_wacom_manager_finalize    (GObject              *object);

G_DEFINE_TYPE (GsdWacomManager, gsd_wacom_manager, G_TYPE_OBJECT)

static gpointer manager_object = NULL;

static GObject *
gsd_wacom_manager_constructor (GType                     type,
                               guint                      n_construct_properties,
                               GObjectConstructParam     *construct_properties)
{
        GsdWacomManager      *wacom_manager;

        wacom_manager = GSD_WACOM_MANAGER (G_OBJECT_CLASS (gsd_wacom_manager_parent_class)->constructor (type,
                                                                                                         n_construct_properties,
                                                                                                         construct_properties));

        return G_OBJECT (wacom_manager);
}

static void
gsd_wacom_manager_dispose (GObject *object)
{
        G_OBJECT_CLASS (gsd_wacom_manager_parent_class)->dispose (object);
}

static void
gsd_wacom_manager_class_init (GsdWacomManagerClass *klass)
{
        GObjectClass   *object_class = G_OBJECT_CLASS (klass);

        object_class->constructor = gsd_wacom_manager_constructor;
        object_class->dispose = gsd_wacom_manager_dispose;
        object_class->finalize = gsd_wacom_manager_finalize;

        g_type_class_add_private (klass, sizeof (GsdWacomManagerPrivate));
}

static int
get_device_id (GsdWacomDevice *device)
{
	GdkDevice *gdk_device;
	int id;

	g_object_get (device, "gdk-device", &gdk_device, NULL);
	if (gdk_device == NULL)
		return -1;
	g_object_get (gdk_device, "device-id", &id, NULL);
	return id;
}

static XDevice *
open_device (GsdWacomDevice *device)
{
	XDevice *xdev;
	int id;

	id = get_device_id (device);
	if (id < 0)
		return NULL;

	gdk_error_trap_push ();
	xdev = XOpenDevice (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), id);
	if (gdk_error_trap_pop () || (xdev == NULL))
		return NULL;

	return xdev;
}


static void
wacom_set_property (GsdWacomDevice *device,
		    PropertyHelper *property)
{
	XDevice *xdev;

	xdev = open_device (device);
	device_set_property (xdev, gsd_wacom_device_get_tool_name (device), property);
	XCloseDevice (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), xdev);
}

static void
set_rotation (GsdWacomDevice *device,
	      GsdWacomRotation rotation)
{
        gchar rot = rotation;
        PropertyHelper property = {
                .name = "Wacom Rotation",
                .nitems = 1,
                .format = 8,
                .type   = XA_INTEGER,
                .data.c = &rot,
        };

        wacom_set_property (device, &property);
}

static void
set_pressurecurve (GsdWacomDevice *device,
                   GVariant       *value)
{
        PropertyHelper property = {
                .name = "Wacom Pressurecurve",
                .nitems = 4,
                .type   = XA_INTEGER,
                .format = 32,
        };
        gsize nvalues;

        property.data.i = g_variant_get_fixed_array (value, &nvalues, sizeof (gint32));

        if (nvalues != 4) {
                g_error ("Pressurecurve requires 4 values.");
                return;
        }

        wacom_set_property (device, &property);
        g_variant_unref (value);
}

/* Area handling. Each area is defined as top x/y, bottom x/y and limits the
 * usable area of the physical device to the given area (in device coords)
 */
static void
set_area (GsdWacomDevice  *device,
          GVariant        *value)
{
        PropertyHelper property = {
                .name = "Wacom Tablet Area",
                .nitems = 4,
                .type   = XA_INTEGER,
                .format = 32,
        };
        gsize nvalues;

        property.data.i = g_variant_get_fixed_array (value, &nvalues, sizeof (gint32));

        if (nvalues != 4) {
                g_error ("Area configuration requires 4 values.");
                return;
        }

        wacom_set_property (device, &property);
        g_variant_unref (value);
}

static void
set_display (GsdWacomDevice  *device,
             GVariant        *value)
{
        float matrix[NUM_ELEMS_MATRIX];
        PropertyHelper property = {
                .name   = "Coordinate Transformation Matrix",
                .nitems = NUM_ELEMS_MATRIX,
                .format = 32,
                .type   = XInternAtom (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), "FLOAT", True),
        };

        gsd_wacom_device_get_display_matrix (device, matrix);

        property.data.i = (gint*)(&matrix);
        g_debug ("Applying matrix to device...");
        wacom_set_property (device, &property);

        g_variant_unref (value);
}

static void
set_absolute (GsdWacomDevice  *device,
              gint             is_absolute)
{
	XDevice *xdev;

	xdev = open_device (device);
	gdk_error_trap_push ();
	XSetDeviceMode (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), xdev, is_absolute ? Absolute : Relative);
	if (gdk_error_trap_pop ())
		g_error ("Failed to set mode \"%s\" for \"%s\".",
			 is_absolute ? "Absolute" : "Relative", gsd_wacom_device_get_tool_name (device));
	XCloseDevice (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), xdev);
}

static void
set_device_buttonmap (GsdWacomDevice *device,
                      GVariant       *value)
{
	XDevice *xdev;
	gsize nmap;
	const gint *intmap;
	unsigned char *map;
	int i, j, rc;

	xdev = open_device (device);

	intmap = g_variant_get_fixed_array (value, &nmap, sizeof (gint32));
	map = g_new0 (unsigned char, nmap);
	for (i = 0; i < nmap && i < sizeof (map); i++)
		map[i] = intmap[i];
        g_variant_unref (value);

	gdk_error_trap_push ();

	/* X refuses to change the mapping while buttons are engaged,
	 * so if this is the case we'll retry a few times
	 */
	for (j = 0;
	     j < 20 && (rc = XSetDeviceButtonMapping (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), xdev, map, nmap)) == MappingBusy;
	     ++j) {
		g_usleep (300);
	}

	if ((gdk_error_trap_pop () && rc != MappingSuccess) ||
	    rc != MappingSuccess)
		g_warning ("Error in setting button mapping for \"%s\"", gsd_wacom_device_get_tool_name (device));

	g_free (map);

	XCloseDevice (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), xdev);
}

static void
set_touch (GsdWacomDevice *device,
	   gboolean        touch)
{
        gchar data = touch;
        PropertyHelper property = {
                .name = "Wacom Enable Touch",
                .nitems = 1,
                .format = 8,
                .type   = XA_INTEGER,
                .data.c = &data,
        };

        wacom_set_property (device, &property);
}

static void
set_tpcbutton (GsdWacomDevice *device,
	       gboolean        tpcbutton)
{
        /* Wacom's TPCButton option which this setting emulates is to enable
         * Tablet PC stylus behaviour when on. The property "Hover Click"
         * works the other way round, i.e. if Hover Click is enabled this
         * is the equivalent of TPC behaviour disabled. */
        gchar data = tpcbutton ? 0 : 1;
        PropertyHelper property = {
                .name = "Wacom Hover Click",
                .nitems = 1,
                .format = 8,
                .type   = XA_INTEGER,
                .data.c = &data,
        };

        wacom_set_property (device, &property);
}

static void
set_pressurethreshold (GsdWacomDevice *device,
                       gint            threshold)
{
        PropertyHelper property = {
                .name = "Wacom Pressure Threshold",
                .nitems = 1,
                .format = 32,
                .type   = XA_INTEGER,
                .data.i = &threshold,
        };

        wacom_set_property (device, &property);
}

static void
apply_stylus_settings (GsdWacomDevice *device)
{
	GSettings *stylus_settings;
	GsdWacomStylus *stylus;
	int threshold;

	g_object_get (device, "last-stylus", &stylus, NULL);
	g_debug ("Applying setting for stylus '%s' on device '%s'",
		 gsd_wacom_stylus_get_name (stylus),
		 gsd_wacom_device_get_name (device));

	stylus_settings = gsd_wacom_stylus_get_settings (stylus);
	set_pressurecurve (device, g_settings_get_value (stylus_settings, KEY_PRESSURECURVE));
	set_device_buttonmap (device, g_settings_get_value (stylus_settings, KEY_BUTTON_MAPPING));

	threshold = g_settings_get_int (stylus_settings, KEY_PRESSURETHRESHOLD);
	if (threshold == -1)
		threshold = DEFAULT_PRESSURE_THRESHOLD;
	set_pressurethreshold (device, threshold);
}

static void
set_led (GsdWacomDevice *device,
	 int             group_id,
	 int             index)
{
	/* FIXME implement */
	g_message ("Switched group ID %d to index %d", group_id, index);
}

struct DefaultButtons {
	const char *button;
	int         num;
};

struct DefaultButtons def_touchrings_buttons[] = {
	/* Touchrings */
	{ "AbsWheelUp", 90 },
	{ "AbsWheelDown", 91 },
	{ "RelWheelUp", 90 },
	{ "RelWheelDown", 91 },
	{ "AbsWheel2Up", 92 },
	{ "AbsWheel2Down", 93 },
	{ NULL, 0 }
};

struct DefaultButtons def_touchstrip_buttons[] = {
	/* Touchstrips */
	{ "StripLeftUp", 94 },
	{ "StripLeftDown", 95 },
	{ "StripRightUp", 96 },
	{ "StripRightDown", 97 },
	{ NULL, 0 }
};

static void
reset_touch_buttons (XDevice               *xdev,
		     struct DefaultButtons *buttons,
		     const char            *device_property)
{
	Atom actions[6];
	Atom action_prop;
	guint i;

	/* Create a device property with the action for button i */
	for (i = 0; buttons[i].button != NULL; i++)
	{
		char *propname;
		int action[2]; /* press + release */
		Atom prop;
		int mapped_button = buttons[i].num;

		action[0] = AC_BUTTON | AC_KEYBTNPRESS | mapped_button;
		action[1] = AC_BUTTON | mapped_button;

		propname = g_strdup_printf ("Button %s action", buttons[i].button);
		prop = XInternAtom (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), propname, False);
		g_free (propname);
		XChangeDeviceProperty (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), xdev,
				       prop, XA_INTEGER, 32, PropModeReplace,
				       (const guchar *) &action, 2);

		/* prop now contains press + release for the mapped button */
		actions[i] = prop;
	}

	/* Now set the actual action property to contain references to the various
	 * actions */
	action_prop = XInternAtom (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), device_property, True);
	XChangeDeviceProperty (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), xdev,
			       action_prop, XA_ATOM, 32, PropModeReplace,
			       (const guchar *) actions, i);
}

static void
reset_pad_buttons (GsdWacomDevice *device)
{
	XDevice *xdev;
	int nmap;
	unsigned char *map;
	int i, j, rc;

	/* Normal buttons */
	xdev = open_device (device);

	gdk_error_trap_push ();

	nmap = 256;
	map = g_new0 (unsigned char, nmap);
	for (i = 0; i < nmap && i < sizeof (map); i++)
		map[i] = i + 1;

	/* X refuses to change the mapping while buttons are engaged,
	 * so if this is the case we'll retry a few times */
	for (j = 0;
	     j < 20 && (rc = XSetDeviceButtonMapping (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), xdev, map, nmap)) == MappingBusy;
	     ++j) {
		g_usleep (300);
	}

	if ((gdk_error_trap_pop () && rc != MappingSuccess) ||
	    rc != MappingSuccess)
		g_warning ("Error in resetting button mapping for \"%s\" (rc=%d)", gsd_wacom_device_get_tool_name (device), rc);

	g_free (map);

	gdk_error_trap_push ();
	reset_touch_buttons (xdev, def_touchrings_buttons, "Wacom Wheel Buttons");
	reset_touch_buttons (xdev, def_touchstrip_buttons, "Wacom Strip Buttons");
	gdk_error_trap_pop_ignored ();

	XCloseDevice (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), xdev);

	/* Reset all the LEDs */
	/* FIXME, get the number of modes somewhere else */
	for (i = 0; i < 4; i++)
		set_led (device, i, 0);
}

static void
set_wacom_settings (GsdWacomManager *manager,
		    GsdWacomDevice  *device)
{
	GsdWacomDeviceType type;
	GSettings *settings;

	g_debug ("Applying settings for device '%s' (type: %s)",
		 gsd_wacom_device_get_tool_name (device),
		 gsd_wacom_device_type_to_string (gsd_wacom_device_get_device_type (device)));

	settings = gsd_wacom_device_get_settings (device);
        set_rotation (device, g_settings_get_enum (settings, KEY_ROTATION));
        set_touch (device, g_settings_get_boolean (settings, KEY_TOUCH));

        type = gsd_wacom_device_get_device_type (device);

	if (type == WACOM_TYPE_TOUCH) {
		set_absolute (device, FALSE);
		return;
	}

	if (type == WACOM_TYPE_CURSOR) {
		GVariant *values[4], *variant;
		guint i;

		set_absolute (device, FALSE);

		for (i = 0; i < G_N_ELEMENTS (values); i++)
			values[i] = g_variant_new_int32 (-1);

		variant = g_variant_new_array (G_VARIANT_TYPE_INT32, values, G_N_ELEMENTS (values));
		set_area (device, variant);
		return;
	}

	if (type == WACOM_TYPE_PAD) {
		int id;

		id = get_device_id (device);
		reset_pad_buttons (device);
		grab_button (id, TRUE, manager->priv->screens);
		return;
	}

	if (type == WACOM_TYPE_STYLUS)
		set_tpcbutton (device, g_settings_get_boolean (settings, KEY_TPCBUTTON));

	set_absolute (device, g_settings_get_boolean (settings, KEY_IS_ABSOLUTE));
	set_area (device, g_settings_get_value (settings, KEY_AREA));
	set_display (device, g_settings_get_value (settings, KEY_DISPLAY));

        /* only pen and eraser have pressure threshold and curve settings */
        if (type == WACOM_TYPE_STYLUS ||
	    type == WACOM_TYPE_ERASER) {
		apply_stylus_settings (device);
	}
}

static void
wacom_settings_changed (GSettings      *settings,
			gchar          *key,
			GsdWacomDevice *device)
{
	GsdWacomDeviceType type;

	type = gsd_wacom_device_get_device_type (device);

	if (g_str_equal (key, KEY_ROTATION)) {
		set_rotation (device, g_settings_get_enum (settings, key));
	} else if (g_str_equal (key, KEY_TOUCH)) {
		set_touch (device, g_settings_get_boolean (settings, key));
	} else if (g_str_equal (key, KEY_TPCBUTTON)) {
		set_tpcbutton (device, g_settings_get_boolean (settings, key));
	} else if (g_str_equal (key, KEY_IS_ABSOLUTE)) {
		if (type != WACOM_TYPE_CURSOR &&
		    type != WACOM_TYPE_PAD &&
		    type != WACOM_TYPE_TOUCH)
			set_absolute (device, g_settings_get_boolean (settings, key));
	} else if (g_str_equal (key, KEY_AREA)) {
		if (type != WACOM_TYPE_CURSOR &&
		    type != WACOM_TYPE_PAD)
			set_area (device, g_settings_get_value (settings, key));
	} else if (g_str_equal (key, KEY_DISPLAY)) {
		if (type != WACOM_TYPE_CURSOR &&
		    type != WACOM_TYPE_PAD)
			set_display (device, g_settings_get_value (settings, key));
	} else {
		g_warning ("Unhandled tablet-wide setting '%s' changed", key);
	}
}

static void
stylus_settings_changed (GSettings      *settings,
			 gchar          *key,
			 GsdWacomStylus *stylus)
{
	GsdWacomDevice *device;
	GsdWacomStylus *last_stylus;

	device = gsd_wacom_stylus_get_device (stylus);

	g_object_get (device, "last-stylus", &last_stylus, NULL);
	if (last_stylus != stylus) {
		g_debug ("Not applying changed settings because '%s' is the current stylus, not '%s'",
			 last_stylus ? gsd_wacom_stylus_get_name (last_stylus) : "NONE",
			 gsd_wacom_stylus_get_name (stylus));
		return;
	}

	if (g_str_equal (key, KEY_PRESSURECURVE)) {
		set_pressurecurve (device, g_settings_get_value (settings, key));
	} else if (g_str_equal (key, KEY_PRESSURETHRESHOLD)) {
		int threshold;

		threshold = g_settings_get_int (settings, KEY_PRESSURETHRESHOLD);
		if (threshold == -1)
			threshold = DEFAULT_PRESSURE_THRESHOLD;
		set_pressurethreshold (device, threshold);
	} else if (g_str_equal (key, KEY_BUTTON_MAPPING)) {
		set_device_buttonmap (device, g_settings_get_value (settings, key));
	}  else {
		g_warning ("Unhandled stylus setting '%s' changed", key);
	}
}

static void
last_stylus_changed (GsdWacomDevice  *device,
		     GParamSpec      *pspec,
		     GsdWacomManager *manager)
{
	g_debug ("Stylus for device '%s' changed, applying settings",
		 gsd_wacom_device_get_name (device));
	apply_stylus_settings (device);
}

static void
device_added_cb (GdkDeviceManager *device_manager,
                 GdkDevice        *gdk_device,
                 GsdWacomManager  *manager)
{
	GsdWacomDevice *device;
	GSettings *settings;

	device = gsd_wacom_device_new (gdk_device);
	if (gsd_wacom_device_get_device_type (device) == WACOM_TYPE_INVALID) {
		g_object_unref (device);
		return;
	}
	g_debug ("Adding device '%s' (type: '%s') to known devices list",
		 gsd_wacom_device_get_tool_name (device),
		 gsd_wacom_device_type_to_string (gsd_wacom_device_get_device_type (device)));
	g_hash_table_insert (manager->priv->devices, (gpointer) gdk_device, device);

	settings = gsd_wacom_device_get_settings (device);
	g_signal_connect (G_OBJECT (settings), "changed",
			  G_CALLBACK (wacom_settings_changed), device);

	if (gsd_wacom_device_get_device_type (device) == WACOM_TYPE_STYLUS ||
	    gsd_wacom_device_get_device_type (device) == WACOM_TYPE_ERASER) {
		GList *styli, *l;

		styli = gsd_wacom_device_list_styli (device);

		for (l = styli ; l ; l = l->next) {
			settings = gsd_wacom_stylus_get_settings (l->data);
			g_signal_connect (G_OBJECT (settings), "changed",
					  G_CALLBACK (stylus_settings_changed), l->data);
		}

		g_list_free (styli);

		g_signal_connect (G_OBJECT (device), "notify::last-stylus",
				  G_CALLBACK (last_stylus_changed), manager);
	}

        set_wacom_settings (manager, device);
}

static void
device_removed_cb (GdkDeviceManager *device_manager,
                   GdkDevice        *gdk_device,
                   GsdWacomManager  *manager)
{
	g_debug ("Removing device '%s' from known devices list",
		 gdk_device_get_name (gdk_device));
	g_hash_table_remove (manager->priv->devices, gdk_device);
}

static GsdWacomDevice *
device_id_to_device (GsdWacomManager *manager,
		     int              deviceid)
{
	GList *devices, *l;
	GsdWacomDevice *ret;

	ret = NULL;
	devices = g_hash_table_get_keys (manager->priv->devices);

	for (l = devices; l != NULL; l = l->next) {
		GdkDevice *device = l->data;
		int id;

		g_object_get (device, "device-id", &id, NULL);
		if (id == deviceid) {
			ret = g_hash_table_lookup (manager->priv->devices, device);
			break;
		}
	}

	g_list_free (devices);
	return ret;
}

struct {
	guint mask;
	KeySym keysym;
} mods_keysyms[] = {
	{ GDK_MOD1_MASK, XK_Alt_L },
	{ GDK_SHIFT_MASK, XK_Shift_L },
	{ GDK_CONTROL_MASK, XK_Control_L },
};

static void
send_modifiers (Display *display,
		guint mask,
		gboolean is_press)
{
	guint i;

	if (mask == 0)
		return;

	for (i = 0; i < G_N_ELEMENTS(mods_keysyms); i++) {
		if (mask & mods_keysyms[i].mask) {
			guint keycode;

			keycode = XKeysymToKeycode (display, mods_keysyms[i].keysym);
			XTestFakeKeyEvent (display, keycode,
					   is_press ? True : False, 0);
		}
	}
}

static char *
get_elevator_shortcut_string (GSettings        *settings,
			      GtkDirectionType  dir)
{
	char **strv, *str;

	strv = g_settings_get_strv (settings, KEY_CUSTOM_ELEVATOR_ACTION);
	if (strv == NULL)
		return NULL;

	if (g_strv_length (strv) >= 1 && dir == GTK_DIR_UP)
		str = g_strdup (strv[0]);
	else if (g_strv_length (strv) >= 2 && dir == GTK_DIR_DOWN)
		str = g_strdup (strv[1]);
	else
		str = NULL;

	g_strfreev (strv);

	return str;
}

static void
generate_key (GsdWacomTabletButton *wbutton,
	      int                   group,
	      Display              *display,
	      GtkDirectionType      dir,
	      gboolean              is_press)
{
	char                 *str;
	guint                 keyval;
	guint                *keycodes;
	guint                 keycode;
	guint                 mods;
	GdkKeymapKey         *keys;
	int                   n_keys;
	guint                 i;

	if (wbutton->type == WACOM_TABLET_BUTTON_TYPE_ELEVATOR)
		str = get_elevator_shortcut_string (wbutton->settings, dir);
	else
		str = g_settings_get_string (wbutton->settings, KEY_CUSTOM_ACTION);

	if (str == NULL)
		return;

	gtk_accelerator_parse_with_keycode (str, &keyval, &keycodes, &mods);
	if (keycodes == NULL) {
		g_warning ("Failed to find a keycode for shortcut '%s'", str);
		g_free (str);
		return;
	}
	g_free (keycodes);

	/* Now look for our own keycode, in the group as us */
	if (!gdk_keymap_get_entries_for_keyval (gdk_keymap_get_default (), keyval, &keys, &n_keys)) {
		g_warning ("Failed to find a keycode for keyval '%s' (0x%x)", gdk_keyval_name (keyval), keyval);
		g_free (str);
		return;
	}

	keycode = 0;
	for (i = 0; i < n_keys; i++) {
		if (keys[i].group != group)
			continue;
		if (keys[i].level > 0)
			continue;
		keycode = keys[i].keycode;
	}
	/* Couldn't find it in the current group? Look in group 0 */
	if (keycode == 0) {
		for (i = 0; i < n_keys; i++) {
			if (keys[i].level > 0)
				continue;
			keycode = keys[i].keycode;
		}
	}
	g_free (keys);

	g_debug ("Emitting '%s' (keyval: %d, keycode: %d mods: 0x%x)",
		 str, keyval, keycode, mods);
	g_free (str);

	/* And send out the keys! */
	if (is_press)
		send_modifiers (display, mods, TRUE);
	XTestFakeKeyEvent (display, keycode,
			   is_press ? True : False, 0);
	if (is_press == FALSE)
		send_modifiers (display, mods, FALSE);
}

static GdkFilterReturn
filter_button_events (XEvent          *xevent,
                      GdkEvent        *event,
                      GsdWacomManager *manager)
{
	XIEvent             *xiev;
	XIDeviceEvent       *xev;
	XGenericEventCookie *cookie;
	guint                deviceid;
	GsdWacomDevice      *device;
	int                  button;
	GsdWacomTabletButton *wbutton;
	GtkDirectionType      dir;

        /* verify we have a key event */
	if (xevent->type != GenericEvent)
		return GDK_FILTER_CONTINUE;
	cookie = &xevent->xcookie;
	if (cookie->extension != manager->priv->opcode)
		return GDK_FILTER_CONTINUE;

	xiev = (XIEvent *) xevent->xcookie.data;

	if (xiev->evtype != XI_ButtonRelease &&
	    xiev->evtype != XI_ButtonPress)
		return GDK_FILTER_CONTINUE;

	xev = (XIDeviceEvent *) xiev;

	deviceid = xev->sourceid;
	device = device_id_to_device (manager, deviceid);
	if (gsd_wacom_device_get_device_type (device) != WACOM_TYPE_PAD)
		return GDK_FILTER_CONTINUE;

	button = xev->detail;

	wbutton = gsd_wacom_device_get_button (device, button, &dir);
	if (wbutton == NULL) {
		g_warning ("Could not find matching button for '%d' on '%s'",
			   button, gsd_wacom_device_get_name (device));
		return GDK_FILTER_CONTINUE;
	}

	g_debug ("Received event button %s '%s'%s ('%d') on device '%s' ('%d')",
		 xiev->evtype == XI_ButtonPress ? "press" : "release",
		 wbutton->id,
		 wbutton->type == WACOM_TABLET_BUTTON_TYPE_ELEVATOR ?
		 (dir == GTK_DIR_UP ? " 'up'" : " 'down'") : "",
		 button,
		 gsd_wacom_device_get_name (device),
		 deviceid);

	if (wbutton->type == WACOM_TABLET_BUTTON_TYPE_HARDCODED) {
		int new_mode;

		/* We switch modes on key release */
		if (xiev->evtype == XI_ButtonRelease)
			return GDK_FILTER_REMOVE;

		new_mode = gsd_wacom_device_set_next_mode (device, wbutton->group_id);
		set_led (device, wbutton->group_id, new_mode);
		return GDK_FILTER_REMOVE;
	}

	/* Nothing to do */
	if (g_settings_get_enum (wbutton->settings, KEY_ACTION_TYPE) == GSD_WACOM_ACTION_TYPE_NONE)
		return GDK_FILTER_REMOVE;

	/* Send a key combination out */
	generate_key (wbutton, xev->group.effective, xev->display, dir, xiev->evtype == XI_ButtonPress ? True : False);

	return GDK_FILTER_REMOVE;
}

static void
set_devicepresence_handler (GsdWacomManager *manager)
{
        GdkDeviceManager *device_manager;

        device_manager = gdk_display_get_device_manager (gdk_display_get_default ());
        if (device_manager == NULL)
                return;

        manager->priv->device_added_id = g_signal_connect (G_OBJECT (device_manager), "device-added",
                                                           G_CALLBACK (device_added_cb), manager);
        manager->priv->device_removed_id = g_signal_connect (G_OBJECT (device_manager), "device-removed",
                                                             G_CALLBACK (device_removed_cb), manager);
        manager->priv->device_manager = device_manager;
}

static void
gsd_wacom_manager_init (GsdWacomManager *manager)
{
        manager->priv = GSD_WACOM_MANAGER_GET_PRIVATE (manager);
}

static gboolean
gsd_wacom_manager_idle_cb (GsdWacomManager *manager)
{
	GList *devices, *l;
	GSList *ls;

        gnome_settings_profile_start (NULL);

        manager->priv->devices = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_object_unref);

        set_devicepresence_handler (manager);

        devices = gdk_device_manager_list_devices (manager->priv->device_manager, GDK_DEVICE_TYPE_SLAVE);
        for (l = devices; l ; l = l->next)
		device_added_cb (manager->priv->device_manager, l->data, manager);
        g_list_free (devices);

        /* Start filtering the button events */
        for (ls = manager->priv->screens; ls != NULL; ls = ls->next) {
                gdk_window_add_filter (gdk_screen_get_root_window (ls->data),
                                       (GdkFilterFunc) filter_button_events,
                                       manager);
        }

        gnome_settings_profile_end (NULL);

        manager->priv->start_idle_id = 0;

        return FALSE;
}

static void
init_screens (GsdWacomManager *manager)
{
        GdkDisplay *display;
        int i;

        display = gdk_display_get_default ();
        for (i = 0; i < gdk_display_get_n_screens (display); i++) {
                GdkScreen *screen;

                screen = gdk_display_get_screen (display, i);
                if (screen == NULL) {
                        continue;
                }
                manager->priv->screens = g_slist_append (manager->priv->screens, screen);
        }
}

gboolean
gsd_wacom_manager_start (GsdWacomManager *manager,
                         GError         **error)
{
	int a, b, c, d;

        gnome_settings_profile_start (NULL);

        if (supports_xinput2_devices (&manager->priv->opcode) == FALSE) {
                g_debug ("No Xinput2 support, disabling plugin");
                return TRUE;
        }

        if (!XTestQueryExtension (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()), &a, &b, &c, &d)) {
                g_debug ("No XTest extension support, disabling plugin");
                return TRUE;
        }

        init_screens (manager);

        manager->priv->start_idle_id = g_idle_add ((GSourceFunc) gsd_wacom_manager_idle_cb, manager);

        gnome_settings_profile_end (NULL);

        return TRUE;
}

void
gsd_wacom_manager_stop (GsdWacomManager *manager)
{
        GsdWacomManagerPrivate *p = manager->priv;
        GSList *ls;

        g_debug ("Stopping wacom manager");

        if (p->device_manager != NULL) {
                g_signal_handler_disconnect (p->device_manager, p->device_added_id);
                g_signal_handler_disconnect (p->device_manager, p->device_removed_id);
                p->device_manager = NULL;
        }

        for (ls = p->screens; ls != NULL; ls = ls->next) {
                gdk_window_remove_filter (gdk_screen_get_root_window (ls->data),
                                          (GdkFilterFunc) filter_button_events,
                                          manager);
        }
}

static void
gsd_wacom_manager_finalize (GObject *object)
{
        GsdWacomManager *wacom_manager;

        g_return_if_fail (object != NULL);
        g_return_if_fail (GSD_IS_WACOM_MANAGER (object));

        wacom_manager = GSD_WACOM_MANAGER (object);

        g_return_if_fail (wacom_manager->priv != NULL);

        if (wacom_manager->priv->devices) {
                g_hash_table_destroy (wacom_manager->priv->devices);
                wacom_manager->priv->devices = NULL;
        }

        if (wacom_manager->priv->screens != NULL) {
                g_slist_free (wacom_manager->priv->screens);
                wacom_manager->priv->screens = NULL;
        }

        if (wacom_manager->priv->start_idle_id != 0)
                g_source_remove (wacom_manager->priv->start_idle_id);

        G_OBJECT_CLASS (gsd_wacom_manager_parent_class)->finalize (object);
}

GsdWacomManager *
gsd_wacom_manager_new (void)
{
        if (manager_object != NULL) {
                g_object_ref (manager_object);
        } else {
                manager_object = g_object_new (GSD_TYPE_WACOM_MANAGER, NULL);
                g_object_add_weak_pointer (manager_object,
                                           (gpointer *) &manager_object);
        }

        return GSD_WACOM_MANAGER (manager_object);
}
