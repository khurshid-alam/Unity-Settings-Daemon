/*
 * Copyright (C) 2011 Red Hat, Inc.
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
 * Author: Bastien Nocera <hadess@hadess.net>
 *
 */

#include "config.h"

#include <gtk/gtk.h>

#include "gsd-wacom-device.h"

static gboolean fake_devices = FALSE;
static gboolean monitor_styli = FALSE;

static char *
get_loc (GSettings *settings)
{
	char *path, *schema, *ret;

	g_return_val_if_fail (G_IS_SETTINGS (settings), NULL);

	g_object_get (G_OBJECT (settings),
		      "path", &path,
		      "schema", &schema,
		      NULL);
	ret = g_strdup_printf ("schema: %s (path: %s)", schema, path);
	g_free (schema);
	g_free (path);

	return ret;
}

static void
last_stylus_changed (GsdWacomDevice  *device,
		     GParamSpec      *pspec,
		     gpointer         user_data)
{
	GsdWacomStylus *stylus;
	char *loc;

	g_object_get (device, "last-stylus", &stylus, NULL);

	g_print ("Stylus changed for device '%s'\n",
		 gsd_wacom_device_get_name (device));

	g_print ("\t*** Stylus: '%s'\n",
		 gsd_wacom_stylus_get_name (stylus));

	loc = get_loc (gsd_wacom_stylus_get_settings (stylus));
	g_print ("\t\tSettings: %s\n", loc);
	g_free (loc);

	g_print ("\t\tIcon name: %s\n", gsd_wacom_stylus_get_icon_name (stylus));
}

static const char *
stylus_type_to_string (GsdWacomStylusType type)
{
	switch (type) {
	case WACOM_STYLUS_TYPE_UNKNOWN:
		return "Unknown";
	case WACOM_STYLUS_TYPE_GENERAL:
		return "General";
	case WACOM_STYLUS_TYPE_INKING:
		return "Inking";
	case WACOM_STYLUS_TYPE_AIRBRUSH:
		return "Airbrush";
	case WACOM_STYLUS_TYPE_CLASSIC:
		return "Classic";
	case WACOM_STYLUS_TYPE_MARKER:
		return "Marker";
	case WACOM_STYLUS_TYPE_STROKE:
		return "Stroke";
	default:
		g_assert_not_reached ();
	}
	return NULL;
}

#define BOOL_AS_STR(x) (x ? "yes" : "no")

static void
list_devices (GList *devices)
{
	GList *l;

	for (l = devices; l ; l = l->next) {
		GsdWacomDevice *device;
		GsdWacomDeviceType type;
		char *loc;

		device = l->data;

		g_signal_connect (G_OBJECT (device), "notify::last-stylus",
				  G_CALLBACK (last_stylus_changed), NULL);

		g_print ("Device '%s' (type: %s)\n",
			 gsd_wacom_device_get_name (device),
			 gsd_wacom_device_type_to_string (gsd_wacom_device_get_device_type (device)));
		g_print ("\tReversible: %s\n", BOOL_AS_STR (gsd_wacom_device_reversible (device)));
		g_print ("\tScreen Tablet: %s\n", BOOL_AS_STR (gsd_wacom_device_is_screen_tablet (device)));

		loc = get_loc (gsd_wacom_device_get_settings (device));
		g_print ("\tGeneric settings: %s\n", loc);
		g_free (loc);

		type = gsd_wacom_device_get_device_type (device);
		if (type == WACOM_TYPE_STYLUS ||
		    type == WACOM_TYPE_ERASER) {
			GList *styli, *j;
			GsdWacomStylus *current_stylus;

			g_object_get (device, "last-stylus", &current_stylus, NULL);

			styli = gsd_wacom_device_list_styli (device);
			for (j = styli; j; j = j->next) {
				GsdWacomStylus *stylus;

				stylus = j->data;
				g_print ("\t%sStylus: '%s' (Type: %s)\n",
					 current_stylus == stylus ? "*** " : "",
					 gsd_wacom_stylus_get_name (stylus),
					 stylus_type_to_string (gsd_wacom_stylus_get_stylus_type (stylus)));

				loc = get_loc (gsd_wacom_stylus_get_settings (stylus));
				g_print ("\t\tSettings: %s\n", loc);
				g_free (loc);

				g_print ("\t\tIcon name: %s\n", gsd_wacom_stylus_get_icon_name (stylus));

				if (gsd_wacom_device_get_device_type (device) == WACOM_TYPE_STYLUS)
					g_print ("\t\tHas Eraser: %s\n", BOOL_AS_STR(gsd_wacom_stylus_get_has_eraser (stylus)));
			}
			g_list_free (styli);
		}

		if (monitor_styli == FALSE)
			g_object_unref (device);
	}
	g_list_free (devices);
}

static void
list_actual_devices (void)
{
	GdkDeviceManager *mgr;
	GList *list, *l, *devices;

	mgr = gdk_display_get_device_manager (gdk_display_get_default ());

	list = gdk_device_manager_list_devices (mgr, GDK_DEVICE_TYPE_SLAVE);
	devices = NULL;
	for (l = list; l ; l = l->next) {
		GsdWacomDevice *device;

		device = gsd_wacom_device_new (l->data);
		if (gsd_wacom_device_get_device_type (device) == WACOM_TYPE_INVALID) {
			g_object_unref (device);
			continue;
		}

		devices = g_list_prepend (devices, device);
	}
	g_list_free (list);

	list_devices (devices);
}

static void
list_fake_devices (void)
{
	GList *devices;

	devices = gsd_wacom_device_create_fake_cintiq ();
	list_devices (devices);

	devices = gsd_wacom_device_create_fake_bt ();
	list_devices (devices);

	devices = gsd_wacom_device_create_fake_x201 ();
	list_devices (devices);
}

int main (int argc, char **argv)
{
	GError *error = NULL;
	GOptionContext *context;
	const GOptionEntry entries[] = {
		{ "fake", 'f', 0, G_OPTION_ARG_NONE, &fake_devices, "Output fake devices", NULL },
		{ "monitor", 'm', 0, G_OPTION_ARG_NONE, &monitor_styli, "Monitor changing styli", NULL },
		{ NULL }
	};

	gtk_init (&argc, &argv);

	context = g_option_context_new ("- test parser functions");
	g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);

	if (g_option_context_parse (context, &argc, &argv, &error) == FALSE) {
		g_print ("Option parsing failed: %s\n", error->message);
		return 1;
	}

	if (fake_devices == FALSE)
		list_actual_devices ();
	else
		list_fake_devices ();

	if (monitor_styli)
		gtk_main ();

	return 0;
}
