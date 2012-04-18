/* -*- Mode: C; indent-tabs-mode: nil; c-basic-offset: 8; tab-width: 8 -*-
 *
 * On-screen-display (OSD) window for gnome-settings-daemon's plugins
 *
 * Copyright (C) 2006 William Jon McCann <mccann@jhu.edu> 
 * Copyright (C) 2009 Novell, Inc
 *
 * Authors:
 *   William Jon McCann <mccann@jhu.edu>
 *   Federico Mena-Quintero <federico@novell.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU Lesser General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

/* GsdOsdWindow is an "on-screen-display" window (OSD).  It is the cute,
 * semi-transparent, curved popup that appears when you press a hotkey global to
 * the desktop, such as to change the volume, switch your monitor's parameters,
 * etc.
 */

#ifndef GSD_OSD_WINDOW_H
#define GSD_OSD_WINDOW_H

#include <glib-object.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GSD_TYPE_OSD_WINDOW            (gsd_osd_window_get_type ())
#define GSD_OSD_WINDOW(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj),  GSD_TYPE_OSD_WINDOW, GsdOsdWindow))
#define GSD_OSD_WINDOW_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass),   GSD_TYPE_OSD_WINDOW, GsdOsdWindowClass))
#define GSD_IS_OSD_WINDOW(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj),  GSD_TYPE_OSD_WINDOW))
#define GSD_IS_OSD_WINDOW_CLASS(klass) (G_TYPE_INSTANCE_GET_CLASS ((klass), GSD_TYPE_OSD_WINDOW))
#define GSD_OSD_WINDOW_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), GSD_TYPE_OSD_WINDOW, GsdOsdWindowClass))

typedef struct GsdOsdWindow                   GsdOsdWindow;
typedef struct GsdOsdWindowClass              GsdOsdWindowClass;
typedef struct GsdOsdWindowPrivate            GsdOsdWindowPrivate;

struct GsdOsdWindow {
        GtkWindow                   parent;

        GsdOsdWindowPrivate  *priv;
};

struct GsdOsdWindowClass {
        GtkWindowClass parent_class;
};

typedef enum {
        GSD_OSD_WINDOW_ACTION_VOLUME,
        GSD_OSD_WINDOW_ACTION_CUSTOM
} GsdOsdWindowAction;

GType                 gsd_osd_window_get_type          (void);

GtkWidget *           gsd_osd_window_new               (void);
gboolean              gsd_osd_window_is_valid          (GsdOsdWindow       *window);
void                  gsd_osd_window_set_action        (GsdOsdWindow       *window,
                                                        GsdOsdWindowAction  action);
void                  gsd_osd_window_set_action_custom (GsdOsdWindow       *window,
                                                        const char         *icon_name,
                                                        gboolean            show_level);
void                  gsd_osd_window_set_volume_muted  (GsdOsdWindow       *window,
                                                        gboolean            muted);
void                  gsd_osd_window_set_volume_level  (GsdOsdWindow       *window,
                                                        int                 level);

G_END_DECLS

#endif
