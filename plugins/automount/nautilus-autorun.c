/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */

/*
 * Nautilus
 *
 * Copyright (C) 2008 Red Hat, Inc.
 *
 * Nautilus is free software; you can redistribute it and/or modify
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: David Zeuthen <davidz@redhat.com>
 */
 
#include <config.h>
#include <string.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/XKBlib.h>
#include <gdk/gdkkeysyms.h>

#include "nautilus-autorun.h"
#include "nautilus-open-with-dialog.h"

enum
{
	AUTORUN_ASK,
	AUTORUN_IGNORE,
	AUTORUN_APP,
	AUTORUN_OPEN_FOLDER,
	AUTORUN_SEP,
	AUTORUN_OTHER_APP,
};

enum
{
	COLUMN_AUTORUN_GICON,
	COLUMN_AUTORUN_NAME,
	COLUMN_AUTORUN_APP_INFO,
	COLUMN_AUTORUN_X_CONTENT_TYPE,
	COLUMN_AUTORUN_ITEM_TYPE,
};

enum {
        COMBO_ITEM_ASK_OR_LABEL = 0,
        COMBO_ITEM_DO_NOTHING,
        COMBO_ITEM_OPEN_FOLDER
};

static gboolean should_autorun_mount (GMount *mount);

static void nautilus_autorun_rebuild_combo_box (GtkWidget *combo_box);



GtkDialog *
show_error_dialog (const char *primary_text,
		   const char *secondary_text,
		   GtkWindow *parent)
{
	GtkWidget *dialog;

	dialog = gtk_message_dialog_new (parent,
					 0,
					 GTK_MESSAGE_ERROR,
					 GTK_BUTTONS_OK,
					 NULL);

	g_object_set (dialog,
		      "text", primary_text,
		      "secondary-text", secondary_text,
		      NULL);

	gtk_widget_show (GTK_WIDGET (dialog));

	g_signal_connect (dialog, "response",
			  G_CALLBACK (gtk_widget_destroy), NULL);

	gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_OK);

	return GTK_DIALOG (dialog);
}


/**
 * nautilus_autorun_g_strv_find
 *
 * Get index of string in array of strings.
 *
 * @strv: NULL-terminated array of strings.
 * @find_me: string to search for
 *
 * Return value: index of array entry in @strv that
 * matches @find_me, or -1 if no matching entry.
 */
static int
nautilus_autorun_g_strv_find (char **strv, const char *find_me)
{
	guint index;

	g_return_val_if_fail (find_me != NULL, -1);

	for (index = 0; strv[index] != NULL; ++index) {
		if (strcmp (strv[index], find_me) == 0) {
			return index;
		}
	}

	return -1;
}


#define ICON_SIZE_STANDARD 48

static gint
get_icon_size_for_stock_size (GtkIconSize size)
{
	gint w, h;

	if (gtk_icon_size_lookup (size, &w, &h)) {
		return MAX (w, h);
	}
	return ICON_SIZE_STANDARD;
}

static GdkPixbuf *
render_icon (GIcon *icon, gint icon_size)
{
	GdkPixbuf *pixbuf;
	GtkIconInfo *info;

	pixbuf = NULL;

	if (G_IS_THEMED_ICON (icon)) {
		gchar const * const *names;

		info = gtk_icon_theme_lookup_by_gicon (gtk_icon_theme_get_default (),
				                       icon,
				                       icon_size,
				                       0);

		if (info) {
			pixbuf = gtk_icon_info_load_icon (info, NULL);
		        gtk_icon_info_free (info);
		}

		if (pixbuf == NULL) {
			names = g_themed_icon_get_names (G_THEMED_ICON (icon));
			pixbuf = gtk_icon_theme_load_icon (gtk_icon_theme_get_default (),
							   *names,
							   icon_size,
							   0, NULL);
		}
	}
	else
	if (G_IS_FILE_ICON (icon)) {
		GFile *icon_file;
		gchar *path;

		icon_file = g_file_icon_get_file (G_FILE_ICON (icon));
		path = g_file_get_path (icon_file);
		pixbuf = gdk_pixbuf_new_from_file_at_size (path,
							   icon_size, icon_size, 
							   NULL);
		g_free (path);
		g_object_unref (G_OBJECT (icon_file));
	}

	return pixbuf;
}

static void
nautilus_autorun_get_preferences (const char *x_content_type,
				  gboolean *pref_start_app,
				  gboolean *pref_ignore,
				  gboolean *pref_open_folder)
{
        GSettings *settings;
	char **x_content_start_app;
	char **x_content_ignore;
	char **x_content_open_folder;

	g_return_if_fail (pref_start_app != NULL);
	g_return_if_fail (pref_ignore != NULL);
	g_return_if_fail (pref_open_folder != NULL);

        settings = g_settings_new ("org.gnome.media-handling");

	*pref_start_app = FALSE;
	*pref_ignore = FALSE;
	*pref_open_folder = FALSE;
	x_content_start_app = g_settings_get_strv (settings, "autorun-x-content-start-app");
	x_content_ignore = g_settings_get_strv (settings, "autorun-x-content-ignore");
	x_content_open_folder = g_settings_get_strv (settings, "autorun-x-content-open-folder");
	if (x_content_start_app != NULL) {
		*pref_start_app = nautilus_autorun_g_strv_find (x_content_start_app, x_content_type) != -1;
	}
	if (x_content_ignore != NULL) {
		*pref_ignore = nautilus_autorun_g_strv_find (x_content_ignore, x_content_type) != -1;
	}
	if (x_content_open_folder != NULL) {
		*pref_open_folder = nautilus_autorun_g_strv_find (x_content_open_folder, x_content_type) != -1;
	}
	g_strfreev (x_content_ignore);
	g_strfreev (x_content_start_app);
	g_strfreev (x_content_open_folder);
        g_object_unref (settings);
}

static char **
remove_elem_from_str_array (char **v,
                            const char *s)
{
        GPtrArray *array;
        guint idx;

        array = g_ptr_array_new ();

        for (idx = 0; v[idx] != NULL; idx++) {
                if (g_strcmp0 (v[idx], s) == 0) {
                        continue;
                }

                g_ptr_array_add (array, v[idx]);
        }

        g_ptr_array_add (array, NULL);

        g_free (v);

        return (char **) g_ptr_array_free (array, FALSE);
}

static char **
add_elem_to_str_array (char **v,
                       const char *s)
{
        GPtrArray *array;
        guint idx;

        array = g_ptr_array_new ();

        for (idx = 0; v[idx] != NULL; idx++) {
                g_ptr_array_add (array, v[idx]);
        }

        g_ptr_array_add (array, g_strdup (s));
        g_ptr_array_add (array, NULL);

        g_free (v);

        return (char **) g_ptr_array_free (array, FALSE);
}

static void
nautilus_autorun_set_preferences (const char *x_content_type,
				  gboolean pref_start_app,
				  gboolean pref_ignore,
				  gboolean pref_open_folder)
{
        GSettings *settings;
	char **x_content_start_app;
	char **x_content_ignore;
	char **x_content_open_folder;

	g_assert (x_content_type != NULL);

	settings = g_settings_new ("org.gnome.media-handling");

	x_content_start_app = g_settings_get_strv (settings, "autorun-x-content-start-app");
	x_content_ignore = g_settings_get_strv (settings, "autorun-x-content-ignore");
	x_content_open_folder = g_settings_get_strv (settings, "autorun-x-content-open-folder");

	x_content_start_app = remove_elem_from_str_array (x_content_start_app, x_content_type);
	if (pref_start_app) {
		x_content_start_app = add_elem_to_str_array (x_content_start_app, x_content_type);
	}
	g_settings_set_strv (settings, "autorun-x-content-start-app", (const gchar * const*) x_content_start_app);

	x_content_ignore = remove_elem_from_str_array (x_content_ignore, x_content_type);
	if (pref_ignore) {
		x_content_ignore = add_elem_to_str_array (x_content_ignore, x_content_type);
	}
	g_settings_set_strv (settings, "autorun-x-content-ignore", (const gchar * const*) x_content_ignore);

	x_content_open_folder = remove_elem_from_str_array (x_content_open_folder, x_content_type);
	if (pref_open_folder) {
		x_content_open_folder = add_elem_to_str_array (x_content_open_folder, x_content_type);
	}
	g_settings_set_strv (settings, "autorun-x-content-open-folder", (const gchar * const*) x_content_open_folder);

	g_strfreev (x_content_open_folder);
	g_strfreev (x_content_ignore);
	g_strfreev (x_content_start_app);
        g_object_unref (settings);
}

static gboolean
combo_box_separator_func (GtkTreeModel *model,
                          GtkTreeIter *iter,
                          gpointer data)
{
	char *str;

	gtk_tree_model_get (model, iter,
			    1, &str,
			    -1);
	if (str != NULL) {
		g_free (str);
		return FALSE;
	}
	return TRUE;
}

typedef void (*NautilusAutorunComboBoxChanged) (gboolean selected_ask,
                                                gboolean selected_ignore,
                                                gboolean selected_open_folder,
                                                GAppInfo *selected_app,
                                                gpointer user_data);

typedef struct
{
	guint changed_signal_id;
	GtkWidget *combo_box;

	char *x_content_type;
	gboolean include_ask;
	gboolean include_open_with_other_app;

	gboolean update_settings;
	NautilusAutorunComboBoxChanged changed_cb;
	gpointer user_data;

	gboolean other_application_selected;
} NautilusAutorunComboBoxData;

static void 
nautilus_autorun_combobox_data_destroy (NautilusAutorunComboBoxData *data)
{
	/* signal handler may be automatically disconnected by destroying the widget */
	if (g_signal_handler_is_connected (G_OBJECT (data->combo_box), data->changed_signal_id)) {
		g_signal_handler_disconnect (G_OBJECT (data->combo_box), data->changed_signal_id);
	}
	g_free (data->x_content_type);
	g_free (data);
}

static void
other_application_selected (NautilusOpenWithDialog *dialog,
			    GAppInfo *app_info,
			    NautilusAutorunComboBoxData *data)
{
	if (data->changed_cb != NULL) {
		data->changed_cb (TRUE, FALSE, FALSE, app_info, data->user_data);
	}
	if (data->update_settings) {
		nautilus_autorun_set_preferences (data->x_content_type, TRUE, FALSE, FALSE);
		g_app_info_set_as_default_for_type (app_info,
						    data->x_content_type,
						    NULL);
		data->other_application_selected = TRUE;
	}

	/* rebuild so we include and select the new application in the list */
	nautilus_autorun_rebuild_combo_box (data->combo_box);
}

static void
handle_dialog_closure (NautilusAutorunComboBoxData *data)
{
	if (!data->other_application_selected) {
		/* reset combo box so we don't linger on "Open with other Application..." */
		nautilus_autorun_rebuild_combo_box (data->combo_box);
	}
}

static void
dialog_response_cb (GtkDialog *dialog,
                    gint response,
                    NautilusAutorunComboBoxData *data)
{
	handle_dialog_closure (data);
}

static void
dialog_destroy_cb (GtkWidget *object,
                   NautilusAutorunComboBoxData *data)
{
	handle_dialog_closure (data);
}

static void 
combo_box_changed (GtkComboBox *combo_box,
                   NautilusAutorunComboBoxData *data)
{
	GtkTreeIter iter;
	GtkTreeModel *model;
	GAppInfo *app_info;
	char *x_content_type;
	int type;

	model = NULL;
	app_info = NULL;
	x_content_type = NULL;

	if (!gtk_combo_box_get_active_iter (combo_box, &iter)) {
		goto out;
	}

	model = gtk_combo_box_get_model (combo_box);
	if (model == NULL) {
		goto out;
	}

	gtk_tree_model_get (model, &iter, 
			    COLUMN_AUTORUN_APP_INFO, &app_info,
			    COLUMN_AUTORUN_X_CONTENT_TYPE, &x_content_type,
			    COLUMN_AUTORUN_ITEM_TYPE, &type,
			    -1);

	switch (type) {
	case AUTORUN_ASK:
		if (data->changed_cb != NULL) {
			data->changed_cb (TRUE, FALSE, FALSE, NULL, data->user_data);
		}
		if (data->update_settings) {
			nautilus_autorun_set_preferences (x_content_type, FALSE, FALSE, FALSE);
		}
		break;
	case AUTORUN_IGNORE:
		if (data->changed_cb != NULL) {
			data->changed_cb (FALSE, TRUE, FALSE, NULL, data->user_data);
		}
		if (data->update_settings) {
			nautilus_autorun_set_preferences (x_content_type, FALSE, TRUE, FALSE);
		}
		break;
	case AUTORUN_OPEN_FOLDER:
		if (data->changed_cb != NULL) {
			data->changed_cb (FALSE, FALSE, TRUE, NULL, data->user_data);
		}
		if (data->update_settings) {
			nautilus_autorun_set_preferences (x_content_type, FALSE, FALSE, TRUE);
		}
		break;

	case AUTORUN_APP:
		if (data->changed_cb != NULL) {
			data->changed_cb (TRUE, FALSE, FALSE, app_info, data->user_data);
		}
		if (data->update_settings) {
			nautilus_autorun_set_preferences (x_content_type, TRUE, FALSE, FALSE);
			g_app_info_set_as_default_for_type (app_info,
							    x_content_type,
							    NULL);
		}
		break;

	case AUTORUN_OTHER_APP:
	{
		GtkWidget *dialog;

		data->other_application_selected = FALSE;

		dialog = nautilus_add_application_dialog_new (NULL, x_content_type);
		gtk_window_set_transient_for (GTK_WINDOW (dialog),
					      GTK_WINDOW (gtk_widget_get_toplevel (GTK_WIDGET (combo_box))));
		gtk_window_set_modal (GTK_WINDOW (dialog), TRUE);
		g_signal_connect (dialog, "application_selected",
				  G_CALLBACK (other_application_selected),
				  data);
		g_signal_connect (dialog, "response",
				  G_CALLBACK (dialog_response_cb), data);
		g_signal_connect (dialog, "destroy",
				  G_CALLBACK (dialog_destroy_cb), data);
		gtk_widget_show (GTK_WIDGET (dialog));

		break;
	}

	}
 
out:
	if (app_info != NULL) {
		g_object_unref (app_info);
	}
	g_free (x_content_type);
}

/* TODO: we need some kind of way to remove user-defined associations,
 * e.g. the result of "Open with other Application...".
 *
 * However, this is a bit hard as
 * g_app_info_can_remove_supports_type() will always return TRUE
 * because we now have [Removed Applications] in the file
 * ~/.local/share/applications/mimeapps.list.
 *
 * We need the API outlined in
 *
 *  http://bugzilla.gnome.org/show_bug.cgi?id=545350
 *
 * to do this.
 *
 * Now, there's also the question about what the UI would look like
 * given this API. Ideally we'd include a small button on the right
 * side of the combo box that the user can press to delete an
 * association, e.g.:
 *
 *  +-------------------------------------+
 *  | Ask what to do                      |
 *  | Do Nothing                          |
 *  | Open Folder                         |
 *  +-------------------------------------+
 *  | Open Rhythmbox Music Player         |
 *  | Open Audio CD Extractor             |
 *  | Open Banshee Media Player           |
 *  | Open Frobnicator App            [x] |
 *  +-------------------------------------+
 *  | Open with other Application...      |
 *  +-------------------------------------+
 *
 * where "Frobnicator App" have been set up using "Open with other
 * Application...". However this is not accessible (which is a
 * GTK+ issue) but probably not a big deal.
 *
 * And we only want show these buttons (e.g. [x]) for associations with
 * GAppInfo instances that are deletable.
 */

static void
nautilus_autorun_prepare_combo_box (GtkWidget *combo_box,
				    const char *x_content_type,
				    gboolean include_ask,
				    gboolean include_open_with_other_app,
				    gboolean update_settings,
				    NautilusAutorunComboBoxChanged changed_cb,
				    gpointer user_data)
{
	GList *l;
	GList *app_info_list;
	GAppInfo *default_app_info;
	GtkListStore *list_store;
	GtkTreeIter iter;
	GIcon *icon;
	int set_active;
	int n;
	int num_apps;
	gboolean pref_ask;
	gboolean pref_start_app;
	gboolean pref_ignore;
	gboolean pref_open_folder;
	NautilusAutorunComboBoxData *data;
	GtkCellRenderer *renderer;
	gboolean new_data;

	nautilus_autorun_get_preferences (x_content_type, &pref_start_app, &pref_ignore, &pref_open_folder);
	pref_ask = !pref_start_app && !pref_ignore && !pref_open_folder;

	set_active = -1;
	data = NULL;
	new_data = TRUE;

	app_info_list = g_app_info_get_all_for_type (x_content_type);
	default_app_info = g_app_info_get_default_for_type (x_content_type, FALSE);
	num_apps = g_list_length (app_info_list);

	list_store = gtk_list_store_new (5,
					 G_TYPE_ICON,
					 G_TYPE_STRING,
					 G_TYPE_APP_INFO,
					 G_TYPE_STRING,
					 G_TYPE_INT);

	/* no apps installed */
	if (num_apps == 0) {
		gtk_list_store_append (list_store, &iter);
		icon = g_themed_icon_new (GTK_STOCK_DIALOG_ERROR);

		/* TODO: integrate with PackageKit-gnome to find applications */

		gtk_list_store_set (list_store, &iter,
				    COLUMN_AUTORUN_GICON, icon,
				    COLUMN_AUTORUN_NAME, _("No applications found"),
				    COLUMN_AUTORUN_APP_INFO, NULL, 
				    COLUMN_AUTORUN_X_CONTENT_TYPE, x_content_type,
				    COLUMN_AUTORUN_ITEM_TYPE, AUTORUN_ASK,
				    -1);
		g_object_unref (icon);
	} else {
		if (include_ask) {
			gtk_list_store_append (list_store, &iter);
			icon = g_themed_icon_new (GTK_STOCK_DIALOG_QUESTION);
			gtk_list_store_set (list_store, &iter,
					    COLUMN_AUTORUN_GICON, icon,
					    COLUMN_AUTORUN_NAME, _("Ask what to do"),
					    COLUMN_AUTORUN_APP_INFO, NULL,
					    COLUMN_AUTORUN_X_CONTENT_TYPE, x_content_type,
					    COLUMN_AUTORUN_ITEM_TYPE, AUTORUN_ASK,
					    -1);
			g_object_unref (icon);
		}
		
		gtk_list_store_append (list_store, &iter);
		icon = g_themed_icon_new (GTK_STOCK_CLOSE);
		gtk_list_store_set (list_store, &iter,
				    COLUMN_AUTORUN_GICON, icon,
				    COLUMN_AUTORUN_NAME, _("Do Nothing"),
				    COLUMN_AUTORUN_APP_INFO, NULL,
				    COLUMN_AUTORUN_X_CONTENT_TYPE, x_content_type,
				    COLUMN_AUTORUN_ITEM_TYPE, AUTORUN_IGNORE,
				    -1);
		g_object_unref (icon);

		gtk_list_store_append (list_store, &iter);
		icon = g_themed_icon_new ("folder-open");
		gtk_list_store_set (list_store, &iter,
				    COLUMN_AUTORUN_GICON, icon,
				    COLUMN_AUTORUN_NAME, _("Open Folder"),
				    COLUMN_AUTORUN_APP_INFO, NULL,
				    COLUMN_AUTORUN_X_CONTENT_TYPE, x_content_type,
				    COLUMN_AUTORUN_ITEM_TYPE, AUTORUN_OPEN_FOLDER,
				    -1);
		g_object_unref (icon);

		gtk_list_store_append (list_store, &iter);
		gtk_list_store_set (list_store, &iter,
				    COLUMN_AUTORUN_GICON, NULL,
				    COLUMN_AUTORUN_NAME, NULL,
				    COLUMN_AUTORUN_APP_INFO, NULL,
				    COLUMN_AUTORUN_X_CONTENT_TYPE, NULL,
				    COLUMN_AUTORUN_ITEM_TYPE, AUTORUN_SEP,
				    -1);

		for (l = app_info_list, n = include_ask ? 4 : 3; l != NULL; l = l->next, n++) {
			char *open_string;
			GAppInfo *app_info = l->data;
			
			/* we deliberately ignore should_show because some apps might want
			 * to install special handlers that should be hidden in the regular
			 * application launcher menus
			 */
			
			icon = g_app_info_get_icon (app_info);
			
			open_string = g_strdup_printf (_("Open %s"), g_app_info_get_display_name (app_info));

			gtk_list_store_append (list_store, &iter);
			gtk_list_store_set (list_store, &iter,
					    COLUMN_AUTORUN_GICON, icon,
					    COLUMN_AUTORUN_NAME, open_string,
					    COLUMN_AUTORUN_APP_INFO, app_info,
					    COLUMN_AUTORUN_X_CONTENT_TYPE, x_content_type,
					    COLUMN_AUTORUN_ITEM_TYPE, AUTORUN_APP,
					    -1);
			g_free (open_string);
			
			if (g_app_info_equal (app_info, default_app_info)) {
				set_active = n;
			}
		}
	}

	if (include_open_with_other_app) {
		gtk_list_store_append (list_store, &iter);
		gtk_list_store_set (list_store, &iter,
				    COLUMN_AUTORUN_GICON, NULL,
				    COLUMN_AUTORUN_NAME, NULL,
				    COLUMN_AUTORUN_APP_INFO, NULL,
				    COLUMN_AUTORUN_X_CONTENT_TYPE, NULL,
				    COLUMN_AUTORUN_ITEM_TYPE, AUTORUN_SEP,
				    -1);

		gtk_list_store_append (list_store, &iter);
		icon = g_themed_icon_new ("application-x-executable");
		gtk_list_store_set (list_store, &iter,
				    COLUMN_AUTORUN_GICON, icon,
				    COLUMN_AUTORUN_NAME, _("Open with other Application..."),
				    COLUMN_AUTORUN_APP_INFO, NULL,
				    COLUMN_AUTORUN_X_CONTENT_TYPE, x_content_type,
				    COLUMN_AUTORUN_ITEM_TYPE, AUTORUN_OTHER_APP,
				    -1);
		g_object_unref (icon);
	}

	if (default_app_info != NULL) {
		g_object_unref (default_app_info);
	}
	g_list_free_full (app_info_list, g_object_unref);

	gtk_combo_box_set_model (GTK_COMBO_BOX (combo_box), GTK_TREE_MODEL (list_store));
	g_object_unref (list_store);

	gtk_cell_layout_clear (GTK_CELL_LAYOUT (combo_box));

	renderer = gtk_cell_renderer_pixbuf_new ();
	gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (combo_box), renderer, FALSE);
	gtk_cell_layout_set_attributes (GTK_CELL_LAYOUT (combo_box), renderer,
					"gicon", COLUMN_AUTORUN_GICON,
					NULL);
	renderer = gtk_cell_renderer_text_new ();
	gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (combo_box), renderer, TRUE);
	gtk_cell_layout_set_attributes (GTK_CELL_LAYOUT (combo_box), renderer,
					"text", COLUMN_AUTORUN_NAME,
					NULL);
	gtk_combo_box_set_row_separator_func (GTK_COMBO_BOX (combo_box), combo_box_separator_func, NULL, NULL);

	if (num_apps == 0) {
		gtk_combo_box_set_active (GTK_COMBO_BOX (combo_box), COMBO_ITEM_ASK_OR_LABEL);
		gtk_widget_set_sensitive (combo_box, FALSE);
	} else {
		gtk_widget_set_sensitive (combo_box, TRUE);
		if (pref_ask && include_ask) {
			gtk_combo_box_set_active (GTK_COMBO_BOX (combo_box), COMBO_ITEM_ASK_OR_LABEL);
		} else if (pref_ignore) {
			gtk_combo_box_set_active (GTK_COMBO_BOX (combo_box), include_ask ? COMBO_ITEM_DO_NOTHING : COMBO_ITEM_ASK_OR_LABEL);
		} else if (pref_open_folder) {
			gtk_combo_box_set_active (GTK_COMBO_BOX (combo_box), include_ask ? COMBO_ITEM_OPEN_FOLDER : COMBO_ITEM_DO_NOTHING);
		} else if (set_active != -1) {
			gtk_combo_box_set_active (GTK_COMBO_BOX (combo_box), set_active);
		} else {
			gtk_combo_box_set_active (GTK_COMBO_BOX (combo_box), include_ask ? COMBO_ITEM_DO_NOTHING : COMBO_ITEM_ASK_OR_LABEL);
		}

		/* See if we have an old data around */
		data = g_object_get_data (G_OBJECT (combo_box), "nautilus_autorun_combobox_data");
		if (data) {
			new_data = FALSE;
			g_free (data->x_content_type);
		} else {
			data = g_new0 (NautilusAutorunComboBoxData, 1);
		}
	
		data->x_content_type = g_strdup (x_content_type);
		data->include_ask = include_ask;
		data->include_open_with_other_app = include_open_with_other_app;
		data->update_settings = update_settings;
		data->changed_cb = changed_cb;
		data->user_data = user_data;
		data->combo_box = combo_box;
		if (data->changed_signal_id == 0) {
			data->changed_signal_id = g_signal_connect (combo_box,
								    "changed",
								    G_CALLBACK (combo_box_changed),
								    data);
		}
	}

	if (new_data) {
		g_object_set_data_full (G_OBJECT (combo_box),
					"nautilus_autorun_combobox_data",
					data,
					(GDestroyNotify) nautilus_autorun_combobox_data_destroy);
	}
}

static void
nautilus_autorun_rebuild_combo_box (GtkWidget *combo_box)
{
        NautilusAutorunComboBoxData *data;
        char *x_content_type;

        data = g_object_get_data (G_OBJECT (combo_box), "nautilus_autorun_combobox_data");
        if (data == NULL) {
                g_warning ("no 'nautilus_autorun_combobox_data' data!");
                return;
        }

        x_content_type = g_strdup (data->x_content_type);
        nautilus_autorun_prepare_combo_box (combo_box,
                                            x_content_type,
                                            data->include_ask,
                                            data->include_open_with_other_app,
                                            data->update_settings,
                                            data->changed_cb,
                                            data->user_data);
        g_free (x_content_type);
}

static gboolean
is_shift_pressed (void)
{
	gboolean ret;
	XkbStateRec state;
	Bool status;

	ret = FALSE;

        gdk_error_trap_push ();
	status = XkbGetState (GDK_DISPLAY_XDISPLAY (gdk_display_get_default ()),
			      XkbUseCoreKbd, &state);
        gdk_error_trap_pop_ignored ();

	if (status == Success) {
		ret = state.mods & ShiftMask;
	}

	return ret;
}

enum {
	AUTORUN_DIALOG_RESPONSE_EJECT = 0
};

typedef struct
{
	GtkWidget *dialog;

	GMount *mount;
	gboolean should_eject;

	gboolean selected_ignore;
	gboolean selected_open_folder;
	GAppInfo *selected_app;

	gboolean remember;

	char *x_content_type;

	NautilusAutorunOpenWindow open_window_func;
	gpointer user_data;
} AutorunDialogData;


static void
nautilus_autorun_launch_for_mount (GMount *mount, GAppInfo *app_info)
{
	GFile *root;
	GdkAppLaunchContext *launch_context;
	GError *error;
	gboolean result;
	GList *list;
	gchar *uri_scheme;
	gchar *uri;

	root = g_mount_get_root (mount);
	list = g_list_append (NULL, root);

	launch_context = gdk_app_launch_context_new ();

	error = NULL;
	result = g_app_info_launch (app_info,
				    list,
				    G_APP_LAUNCH_CONTEXT (launch_context),
				    &error);

	g_object_unref (launch_context);

	if (!result) {
		if (error->domain == G_IO_ERROR &&
		    error->code == G_IO_ERROR_NOT_SUPPORTED) {
			uri = g_file_get_uri (root);
			uri_scheme = g_uri_parse_scheme (uri);
			
			/* FIXME: Present user a dialog to choose another app when the last one failed to handle a file */
			g_warning ("Cannot open location: %s\n", error->message);

			g_free (uri_scheme);
			g_free (uri);
		} else {
			g_warning ("Cannot open app: %s\n", error->message);
		}
		g_error_free (error);
	}

	g_list_free (list);
	g_object_unref (root);
}

static void autorun_dialog_mount_unmounted (GMount *mount, AutorunDialogData *data);

static void
autorun_dialog_destroy (AutorunDialogData *data)
{
	g_signal_handlers_disconnect_by_func (G_OBJECT (data->mount),
					      G_CALLBACK (autorun_dialog_mount_unmounted),
					      data);

	gtk_widget_destroy (GTK_WIDGET (data->dialog));
	if (data->selected_app != NULL) {
		g_object_unref (data->selected_app);
	}
	g_object_unref (data->mount);
	g_free (data->x_content_type);
	g_free (data);
}

static void
autorun_dialog_mount_unmounted (GMount *mount, AutorunDialogData *data)
{
	/* remove the dialog if the media is unmounted */
	autorun_dialog_destroy (data);
}

static void
unmount_mount_callback (GObject *source_object,
			GAsyncResult *res,
			gpointer user_data)
{
	GError *error;
	char *primary;
	gboolean unmounted;
	gboolean should_eject;
	GtkWidget *dialog;


	should_eject = user_data != NULL;

	error = NULL;
	if (should_eject) {
		unmounted = g_mount_eject_with_operation_finish (G_MOUNT (source_object),
								 res, &error);
	} else {
		unmounted = g_mount_unmount_with_operation_finish (G_MOUNT (source_object),
								   res, &error);
	}
	
	if (! unmounted) {
		if (error->code != G_IO_ERROR_FAILED_HANDLED) {
			if (should_eject) {
				primary = g_strdup_printf (_("Unable to eject %p"), source_object);
			} else {
				primary = g_strdup_printf (_("Unable to unmount %p"), source_object);
			}

			dialog = gtk_message_dialog_new (NULL,
							 0,
							 GTK_MESSAGE_INFO,
							 GTK_BUTTONS_OK,
							 "%s",
							 primary);
			gtk_message_dialog_format_secondary_markup (GTK_MESSAGE_DIALOG (dialog), error->message);
			
			gtk_widget_show (GTK_WIDGET (dialog));
			g_signal_connect (dialog, "response",
					  G_CALLBACK (gtk_widget_destroy), NULL);
			g_free (primary);
		}
	}

	if (error != NULL) {
		g_error_free (error);
	}
}

static void
do_unmount (GMount *mount, gboolean should_eject, GtkWindow *window)
{
	GMountOperation *mount_op;

	mount_op = gtk_mount_operation_new (window);
	if (should_eject) {
		g_mount_eject_with_operation (mount,
					      0,
					      mount_op,
					      NULL,
					      unmount_mount_callback,
					      (gpointer) 1);
	} else {
		g_mount_unmount_with_operation (mount,
						0,
						mount_op,
						NULL,
						unmount_mount_callback,
						(gpointer) 0);
	}
	g_object_unref (mount_op);
}

static void
autorun_dialog_response (GtkDialog *dialog, gint response, AutorunDialogData *data)
{
	switch (response) {
	case AUTORUN_DIALOG_RESPONSE_EJECT:
		do_unmount (data->mount, data->should_eject, GTK_WINDOW (dialog));
		break;

	case GTK_RESPONSE_NONE:
		/* window was closed */
		break;
	case GTK_RESPONSE_CANCEL:
		break;
	case GTK_RESPONSE_OK:
		/* do the selected action */

		if (data->remember) {
			/* make sure we don't ask again */
			nautilus_autorun_set_preferences (data->x_content_type, TRUE, data->selected_ignore, data->selected_open_folder);
			if (!data->selected_ignore && !data->selected_open_folder && data->selected_app != NULL) {
				g_app_info_set_as_default_for_type (data->selected_app,
								    data->x_content_type,
								    NULL);
			}
		} else {
			/* make sure we do ask again */
			nautilus_autorun_set_preferences (data->x_content_type, FALSE, FALSE, FALSE);
		}

		if (!data->selected_ignore && !data->selected_open_folder && data->selected_app != NULL) {
			nautilus_autorun_launch_for_mount (data->mount, data->selected_app);
		} else if (!data->selected_ignore && data->selected_open_folder) {
			if (data->open_window_func != NULL)
				data->open_window_func (data->mount, data->user_data);
		}
		break;
	}

	autorun_dialog_destroy (data);
}

static void
autorun_combo_changed (gboolean selected_ask,
		       gboolean selected_ignore,
		       gboolean selected_open_folder,
		       GAppInfo *selected_app,
		       gpointer user_data)
{
	AutorunDialogData *data = user_data;

	if (data->selected_app != NULL) {
		g_object_unref (data->selected_app);
	}
	data->selected_app = selected_app != NULL ? g_object_ref (selected_app) : NULL;
	data->selected_ignore = selected_ignore;
	data->selected_open_folder = selected_open_folder;
}


static void
autorun_always_toggled (GtkToggleButton *togglebutton, AutorunDialogData *data)
{
	data->remember = gtk_toggle_button_get_active (togglebutton);
}

static gboolean
combo_box_enter_ok (GtkWidget *togglebutton, GdkEventKey *event, GtkDialog *dialog)
{
	if (event->keyval == GDK_KEY_KP_Enter || event->keyval == GDK_KEY_Return) {
		gtk_dialog_response (dialog, GTK_RESPONSE_OK);
		return TRUE;
	}
	return FALSE;
}

/* returns TRUE if a folder window should be opened */
static gboolean
do_autorun_for_content_type (GMount *mount, const char *x_content_type, NautilusAutorunOpenWindow open_window_func, gpointer user_data)
{
	AutorunDialogData *data;
	GtkWidget *dialog;
	GtkWidget *hbox;
	GtkWidget *vbox;
	GtkWidget *label;
	GtkWidget *combo_box;
	GtkWidget *always_check_button;
	GtkWidget *eject_button;
	GtkWidget *image;
	char *markup;
	char *content_description;
	char *mount_name;
	GIcon *icon;
	GdkPixbuf *pixbuf;
	int icon_size;
	gboolean user_forced_dialog;
	gboolean pref_ask;
	gboolean pref_start_app;
	gboolean pref_ignore;
	gboolean pref_open_folder;
	char *media_greeting;
	gboolean ret;

	ret = FALSE;
	mount_name = NULL;

	if (g_content_type_is_a (x_content_type, "x-content/win32-software")) {
		/* don't pop up the dialog anyway if the content type says
 		 * windows software.
 		 */
		goto out;
	}

	user_forced_dialog = is_shift_pressed ();

	nautilus_autorun_get_preferences (x_content_type, &pref_start_app, &pref_ignore, &pref_open_folder);
	pref_ask = !pref_start_app && !pref_ignore && !pref_open_folder;

	if (user_forced_dialog) {
		goto show_dialog;
	}

	if (!pref_ask && !pref_ignore && !pref_open_folder) {
		GAppInfo *app_info;
		app_info = g_app_info_get_default_for_type (x_content_type, FALSE);
		if (app_info != NULL) {
			nautilus_autorun_launch_for_mount (mount, app_info);
		}
		goto out;
	}

	if (pref_open_folder) {
		ret = TRUE;
		goto out;
	}

	if (pref_ignore) {
		goto out;
	}

show_dialog:

	mount_name = g_mount_get_name (mount);

	dialog = gtk_dialog_new ();

	hbox = gtk_hbox_new (FALSE, 12);
	gtk_box_pack_start (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (dialog))), hbox, TRUE, TRUE, 0);
	gtk_container_set_border_width (GTK_CONTAINER (hbox), 12);

	icon = g_mount_get_icon (mount);
	icon_size = get_icon_size_for_stock_size (GTK_ICON_SIZE_DIALOG);
	image = gtk_image_new_from_gicon (icon, GTK_ICON_SIZE_DIALOG);
	pixbuf = render_icon (icon, icon_size);
	gtk_misc_set_alignment (GTK_MISC (image), 0.5, 0.0);
	gtk_box_pack_start (GTK_BOX (hbox), image, TRUE, TRUE, 0);
	/* also use the icon on the dialog */
	gtk_window_set_title (GTK_WINDOW (dialog), mount_name);
	gtk_window_set_icon (GTK_WINDOW (dialog), pixbuf);
	gtk_window_set_position (GTK_WINDOW (dialog), GTK_WIN_POS_CENTER);
	g_object_unref (icon);
	if (pixbuf) {
		g_object_unref (pixbuf);
	}
	vbox = gtk_vbox_new (FALSE, 12);
	gtk_box_pack_start (GTK_BOX (hbox), vbox, TRUE, TRUE, 0);

	label = gtk_label_new (NULL);


	/* Customize greeting for well-known x-content types */
	if (strcmp (x_content_type, "x-content/audio-cdda") == 0) {
		media_greeting = _("You have just inserted an Audio CD.");
	} else if (strcmp (x_content_type, "x-content/audio-dvd") == 0) {
		media_greeting = _("You have just inserted an Audio DVD.");
	} else if (strcmp (x_content_type, "x-content/video-dvd") == 0) {
		media_greeting = _("You have just inserted a Video DVD.");
	} else if (strcmp (x_content_type, "x-content/video-vcd") == 0) {
		media_greeting = _("You have just inserted a Video CD.");
	} else if (strcmp (x_content_type, "x-content/video-svcd") == 0) {
		media_greeting = _("You have just inserted a Super Video CD.");
	} else if (strcmp (x_content_type, "x-content/blank-cd") == 0) {
		media_greeting = _("You have just inserted a blank CD.");
	} else if (strcmp (x_content_type, "x-content/blank-dvd") == 0) {
		media_greeting = _("You have just inserted a blank DVD.");
	} else if (strcmp (x_content_type, "x-content/blank-cd") == 0) {
		media_greeting = _("You have just inserted a blank Blu-Ray disc.");
	} else if (strcmp (x_content_type, "x-content/blank-cd") == 0) {
		media_greeting = _("You have just inserted a blank HD DVD.");
	} else if (strcmp (x_content_type, "x-content/image-photocd") == 0) {
		media_greeting = _("You have just inserted a Photo CD.");
	} else if (strcmp (x_content_type, "x-content/image-picturecd") == 0) {
		media_greeting = _("You have just inserted a Picture CD.");
	} else if (strcmp (x_content_type, "x-content/image-dcf") == 0) {
		media_greeting = _("You have just inserted a medium with digital photos.");
	} else if (strcmp (x_content_type, "x-content/audio-player") == 0) {
		media_greeting = _("You have just inserted a digital audio player.");
	} else if (g_content_type_is_a (x_content_type, "x-content/software")) {
		media_greeting = _("You have just inserted a medium with software intended to be automatically started.");
	} else {
		/* fallback to generic greeting */
		media_greeting = _("You have just inserted a medium.");
	}
	markup = g_strdup_printf ("<big><b>%s %s</b></big>", media_greeting, _("Choose what application to launch."));
	gtk_label_set_markup (GTK_LABEL (label), markup);
	g_free (markup);
	gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
	gtk_misc_set_alignment (GTK_MISC (label), 0.0, 0.5);
	gtk_box_pack_start (GTK_BOX (vbox), label, TRUE, TRUE, 0);

	label = gtk_label_new (NULL);
	content_description = g_content_type_get_description (x_content_type);
	markup = g_strdup_printf (_("Select how to open \"%s\" and whether to perform this action in the future for other media of type \"%s\"."), mount_name, content_description);
	g_free (content_description);
	gtk_label_set_markup (GTK_LABEL (label), markup);
	g_free (markup);
	gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
	gtk_misc_set_alignment (GTK_MISC (label), 0.0, 0.5);
	gtk_box_pack_start (GTK_BOX (vbox), label, TRUE, TRUE, 0);
	
	data = g_new0 (AutorunDialogData, 1);
	data->dialog = dialog;
	data->mount = g_object_ref (mount);
	data->remember = !pref_ask;
	data->selected_ignore = pref_ignore;
	data->x_content_type = g_strdup (x_content_type);
	data->selected_app = g_app_info_get_default_for_type (x_content_type, FALSE);
	data->open_window_func = open_window_func;
	data->user_data = user_data;

	combo_box = gtk_combo_box_new ();
	nautilus_autorun_prepare_combo_box (combo_box, x_content_type, FALSE, TRUE, FALSE, autorun_combo_changed, data);
	g_signal_connect (G_OBJECT (combo_box),
			  "key-press-event",
			  G_CALLBACK (combo_box_enter_ok),
			  dialog);

	gtk_box_pack_start (GTK_BOX (vbox), combo_box, TRUE, TRUE, 0);

	always_check_button = gtk_check_button_new_with_mnemonic (_("_Always perform this action"));
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (always_check_button), data->remember);
	g_signal_connect (G_OBJECT (always_check_button),
			  "toggled",
			  G_CALLBACK (autorun_always_toggled),
			  data);
	gtk_box_pack_start (GTK_BOX (vbox), always_check_button, TRUE, TRUE, 0);

	gtk_dialog_add_buttons (GTK_DIALOG (dialog), 
				GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
				GTK_STOCK_OK, GTK_RESPONSE_OK,
				NULL);
	gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_OK);
	
	if (g_mount_can_eject (mount)) {
		GtkWidget *eject_image;
		eject_button = gtk_button_new_with_mnemonic (_("_Eject"));
		eject_image = gtk_image_new_from_icon_name ("media-eject", GTK_ICON_SIZE_BUTTON);
		gtk_button_set_image (GTK_BUTTON (eject_button), eject_image);
		data->should_eject = TRUE;
	} else {
		eject_button = gtk_button_new_with_mnemonic (_("_Unmount"));
		data->should_eject = FALSE;
	}
	gtk_dialog_add_action_widget (GTK_DIALOG (dialog), eject_button, AUTORUN_DIALOG_RESPONSE_EJECT);
	gtk_button_box_set_child_secondary (GTK_BUTTON_BOX (gtk_dialog_get_action_area (GTK_DIALOG (dialog))), eject_button, TRUE);

	/* show the dialog */
	gtk_widget_show_all (dialog);

	g_signal_connect (G_OBJECT (dialog),
			  "response",
			  G_CALLBACK (autorun_dialog_response),
			  data);

	g_signal_connect (G_OBJECT (data->mount),
			  "unmounted",
			  G_CALLBACK (autorun_dialog_mount_unmounted),
			  data);

out:
	g_free (mount_name);
	return ret;
}

typedef struct {
	GMount *mount;
	NautilusAutorunOpenWindow open_window_func;
	gpointer user_data;
	GSettings *settings;
} AutorunData;

static void
autorun_guessed_content_type_callback (GObject *source_object,
				       GAsyncResult *res,
				       gpointer user_data)
{
	GError *error;
	char **guessed_content_type;
	AutorunData *data = user_data;
	gboolean open_folder;

	open_folder = FALSE;

	error = NULL;
	guessed_content_type = g_mount_guess_content_type_finish (G_MOUNT (source_object), res, &error);
	g_object_set_data_full (source_object,
				"nautilus-content-type-cache",
				g_strdupv (guessed_content_type),
				(GDestroyNotify)g_strfreev);
	if (error != NULL) {
		g_warning ("Unable to guess content type for mount: %s", error->message);
		g_error_free (error);
	} else {
		if (guessed_content_type != NULL && g_strv_length (guessed_content_type) > 0) {
			int n;
			for (n = 0; guessed_content_type[n] != NULL; n++) {
				if (do_autorun_for_content_type (data->mount, guessed_content_type[n], 
								 data->open_window_func, data->user_data)) {
					open_folder = TRUE;
				}
			}
			g_strfreev (guessed_content_type);
		} else {
			if (g_settings_get_boolean (data->settings, "automount-open")) {
				open_folder = TRUE;
			}
		}
	}

	/* only open the folder once.. */
	if (open_folder && data->open_window_func != NULL) {
		data->open_window_func (data->mount, data->user_data);
	}

	g_object_unref (data->mount);
	g_object_unref (data->settings);
	g_free (data);
}

void
nautilus_autorun (GMount *mount, GSettings *settings, NautilusAutorunOpenWindow open_window_func, gpointer user_data)
{
	AutorunData *data;

	if (!should_autorun_mount (mount) ||
	    g_settings_get_boolean (settings, "autorun-never")) {
		return;
	}

	data = g_new0 (AutorunData, 1);
	data->mount = g_object_ref (mount);
	data->open_window_func = open_window_func;
	data->user_data = user_data;
	data->settings = g_object_ref (settings);

	g_mount_guess_content_type (mount,
				    FALSE,
				    NULL,
				    autorun_guessed_content_type_callback,
				    data);
}

static gboolean
remove_allow_volume (gpointer data)
{
	GVolume *volume = data;

	g_object_set_data (G_OBJECT (volume), "nautilus-allow-autorun", NULL);
	return FALSE;
}

void
nautilus_allow_autorun_for_volume (GVolume *volume)
{
	g_object_set_data (G_OBJECT (volume), "nautilus-allow-autorun", GINT_TO_POINTER (1));
}

#define INHIBIT_AUTORUN_SECONDS 10

void
nautilus_allow_autorun_for_volume_finish (GVolume *volume)
{
	if (g_object_get_data (G_OBJECT (volume), "nautilus-allow-autorun") != NULL) {
		g_timeout_add_seconds_full (0,
					    INHIBIT_AUTORUN_SECONDS,
					    remove_allow_volume,
					    g_object_ref (volume),
					    g_object_unref);
	}
}

static gboolean
should_skip_native_mount_root (GFile *root)
{
	char *path;
	gboolean should_skip;

	/* skip any mounts in hidden directory hierarchies */
	path = g_file_get_path (root);
	should_skip = strstr (path, "/.") != NULL;
	g_free (path);

	return should_skip;
}

static gboolean
should_autorun_mount (GMount *mount)
{
	GFile *root;
	GVolume *enclosing_volume;
	gboolean ignore_autorun;

	ignore_autorun = TRUE;
	enclosing_volume = g_mount_get_volume (mount);
	if (enclosing_volume != NULL) {
		if (g_object_get_data (G_OBJECT (enclosing_volume), "nautilus-allow-autorun") != NULL) {
			ignore_autorun = FALSE;
			g_object_set_data (G_OBJECT (enclosing_volume), "nautilus-allow-autorun", NULL);
		}
	}

	if (ignore_autorun) {
		if (enclosing_volume != NULL) {
			g_object_unref (enclosing_volume);
		}
		return FALSE;
	}
	
	root = g_mount_get_root (mount);

	/* only do autorun on local files or files where g_volume_should_automount() returns TRUE */
	ignore_autorun = TRUE;
	if ((g_file_is_native (root) && !should_skip_native_mount_root (root)) || 
	    (enclosing_volume != NULL && g_volume_should_automount (enclosing_volume))) {
		ignore_autorun = FALSE;
	}
	if (enclosing_volume != NULL) {
		g_object_unref (enclosing_volume);
	}
	g_object_unref (root);

	return !ignore_autorun;
}
