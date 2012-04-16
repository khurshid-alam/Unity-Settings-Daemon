/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * On-screen-display (OSD) window for gnome-settings-daemon's plugins
 *
 * Copyright (C) 2006-2007 William Jon McCann <mccann@jhu.edu> 
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

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#include "gsd-osd-window.h"

#define DIALOG_TIMEOUT 2000       /* dialog timeout in ms */
#define DIALOG_FADE_TIMEOUT 1500  /* timeout before fade starts */
#define FADE_TIMEOUT 10           /* timeout in ms between each frame of the fade */
#define ICON_SCALE 0.50           /* The size of the icon compared to the whole OSD */

#define BG_ALPHA 0.75

#define GSD_OSD_WINDOW_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), GSD_TYPE_OSD_WINDOW, GsdOsdWindowPrivate))

struct GsdOsdWindowPrivate
{
        guint                    hide_timeout_id;
        guint                    fade_timeout_id;
        double                   fade_out_alpha;

        gint                     screen_width;
        gint                     screen_height;
        gint                     monitor;

        GsdOsdWindowAction       action;
        char                    *icon_name;
        gboolean                 show_level;

        guint                    volume_muted : 1;
        int                      volume_level;
};

G_DEFINE_TYPE (GsdOsdWindow, gsd_osd_window, GTK_TYPE_WINDOW)

static void gsd_osd_window_update_and_hide (GsdOsdWindow *window);

static void
rgb_to_hls (gdouble *r,
            gdouble *g,
            gdouble *b)
{
        gdouble min;
        gdouble max;
        gdouble red;
        gdouble green;
        gdouble blue;
        gdouble h, l, s;
        gdouble delta;

        red = *r;
        green = *g;
        blue = *b;

        if (red > green)
        {
                if (red > blue)
                        max = red;
                else
                        max = blue;

                if (green < blue)
                        min = green;
                else
                        min = blue;
        }
        else
        {
                if (green > blue)
                        max = green;
                else
                        max = blue;

                if (red < blue)
                        min = red;
                else
                        min = blue;
        }

        l = (max + min) / 2;
        s = 0;
        h = 0;

        if (max != min)
        {
                if (l <= 0.5)
                        s = (max - min) / (max + min);
                else
                        s = (max - min) / (2 - max - min);

                delta = max -min;
                if (red == max)
                        h = (green - blue) / delta;
                else if (green == max)
                        h = 2 + (blue - red) / delta;
                else if (blue == max)
                        h = 4 + (red - green) / delta;

                h *= 60;
                if (h < 0.0)
                        h += 360;
        }

        *r = h;
        *g = l;
        *b = s;
}

static void
hls_to_rgb (gdouble *h,
            gdouble *l,
            gdouble *s)
{
        gdouble hue;
        gdouble lightness;
        gdouble saturation;
        gdouble m1, m2;
        gdouble r, g, b;

        lightness = *l;
        saturation = *s;

        if (lightness <= 0.5)
                m2 = lightness * (1 + saturation);
        else
                m2 = lightness + saturation - lightness * saturation;
        m1 = 2 * lightness - m2;

        if (saturation == 0)
        {
                *h = lightness;
                *l = lightness;
                *s = lightness;
        }
        else
        {
                hue = *h + 120;
                while (hue > 360)
                        hue -= 360;
                while (hue < 0)
                        hue += 360;

                if (hue < 60)
                        r = m1 + (m2 - m1) * hue / 60;
                else if (hue < 180)
                        r = m2;
                else if (hue < 240)
                        r = m1 + (m2 - m1) * (240 - hue) / 60;
                else
                        r = m1;

                hue = *h;
                while (hue > 360)
                        hue -= 360;
                while (hue < 0)
                        hue += 360;

                if (hue < 60)
                        g = m1 + (m2 - m1) * hue / 60;
                else if (hue < 180)
                        g = m2;
                else if (hue < 240)
                        g = m1 + (m2 - m1) * (240 - hue) / 60;
                else
                        g = m1;

                hue = *h - 120;
                while (hue > 360)
                        hue -= 360;
                while (hue < 0)
                        hue += 360;

                if (hue < 60)
                        b = m1 + (m2 - m1) * hue / 60;
                else if (hue < 180)
                        b = m2;
                else if (hue < 240)
                        b = m1 + (m2 - m1) * (240 - hue) / 60;
                else
                        b = m1;

                *h = r;
                *l = g;
                *s = b;
        }
}

static void
gsd_osd_window_color_shade (GdkRGBA *a,
                            gdouble   k)
{
        gdouble red;
        gdouble green;
        gdouble blue;

        red = a->red;
        green = a->green;
        blue = a->blue;

        rgb_to_hls (&red, &green, &blue);

        green *= k;
        if (green > 1.0)
                green = 1.0;
        else if (green < 0.0)
                green = 0.0;

        blue *= k;
        if (blue > 1.0)
                blue = 1.0;
        else if (blue < 0.0)
                blue = 0.0;

        hls_to_rgb (&red, &green, &blue);

        a->red = red;
        a->green = green;
        a->blue = blue;
}

static void
gsd_osd_window_color_reverse (GdkRGBA *a)
{
        gdouble red;
        gdouble green;
        gdouble blue;
        gdouble h;
        gdouble s;
        gdouble v;

        red = a->red;
        green = a->green;
        blue = a->blue;

        gtk_rgb_to_hsv (red, green, blue, &h, &s, &v);

        v = 0.5 + (0.5 - v);
        if (v > 1.0)
                v = 1.0;
        else if (v < 0.0)
                v = 0.0;

        gtk_hsv_to_rgb (h, s, v, &red, &green, &blue);

        a->red = red;
        a->green = green;
        a->blue = blue;
}

static void
gsd_osd_window_draw_rounded_rectangle (cairo_t* cr,
                                       gdouble  aspect,
                                       gdouble  x,
                                       gdouble  y,
                                       gdouble  corner_radius,
                                       gdouble  width,
                                       gdouble  height)
{
        gdouble radius = corner_radius / aspect;

        cairo_move_to (cr, x + radius, y);

        cairo_line_to (cr,
                       x + width - radius,
                       y);
        cairo_arc (cr,
                   x + width - radius,
                   y + radius,
                   radius,
                   -90.0f * G_PI / 180.0f,
                   0.0f * G_PI / 180.0f);
        cairo_line_to (cr,
                       x + width,
                       y + height - radius);
        cairo_arc (cr,
                   x + width - radius,
                   y + height - radius,
                   radius,
                   0.0f * G_PI / 180.0f,
                   90.0f * G_PI / 180.0f);
        cairo_line_to (cr,
                       x + radius,
                       y + height);
        cairo_arc (cr,
                   x + radius,
                   y + height - radius,
                   radius,
                   90.0f * G_PI / 180.0f,
                   180.0f * G_PI / 180.0f);
        cairo_line_to (cr,
                       x,
                       y + radius);
        cairo_arc (cr,
                   x + radius,
                   y + radius,
                   radius,
                   180.0f * G_PI / 180.0f,
                   270.0f * G_PI / 180.0f);
        cairo_close_path (cr);
}

static gboolean
fade_timeout (GsdOsdWindow *window)
{
        if (window->priv->fade_out_alpha <= 0.0) {
                gtk_widget_hide (GTK_WIDGET (window));

                /* Reset it for the next time */
                window->priv->fade_out_alpha = 1.0;
                window->priv->fade_timeout_id = 0;

                return FALSE;
        } else {
                GdkRectangle rect;
                GtkWidget *win = GTK_WIDGET (window);
                GtkAllocation allocation;

                window->priv->fade_out_alpha -= 0.10;

                rect.x = 0;
                rect.y = 0;
                gtk_widget_get_allocation (win, &allocation);
                rect.width = allocation.width;
                rect.height = allocation.height;

                gtk_widget_realize (win);
                gdk_window_invalidate_rect (gtk_widget_get_window (win), &rect, FALSE);
        }

        return TRUE;
}

static gboolean
hide_timeout (GsdOsdWindow *window)
{
	window->priv->hide_timeout_id = 0;
	window->priv->fade_timeout_id = g_timeout_add (FADE_TIMEOUT,
						       (GSourceFunc) fade_timeout,
						       window);

        return FALSE;
}

static void
remove_hide_timeout (GsdOsdWindow *window)
{
        if (window->priv->hide_timeout_id != 0) {
                g_source_remove (window->priv->hide_timeout_id);
                window->priv->hide_timeout_id = 0;
        }

        if (window->priv->fade_timeout_id != 0) {
                g_source_remove (window->priv->fade_timeout_id);
                window->priv->fade_timeout_id = 0;
                window->priv->fade_out_alpha = 1.0;
        }
}

static void
add_hide_timeout (GsdOsdWindow *window)
{
        window->priv->hide_timeout_id = g_timeout_add (DIALOG_FADE_TIMEOUT,
                                                       (GSourceFunc) hide_timeout,
                                                       window);
}

static const char *
get_image_name_for_volume (gboolean muted,
			   int volume)
{
        static const char *icon_names[] = {
                "audio-volume-muted-symbolic",
                "audio-volume-low-symbolic",
                "audio-volume-medium-symbolic",
                "audio-volume-high-symbolic",
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

	return icon_names[n];
}

static void
action_changed (GsdOsdWindow *window)
{
        gsd_osd_window_update_and_hide (GSD_OSD_WINDOW (window));
}

static void
volume_level_changed (GsdOsdWindow *window)
{
        gsd_osd_window_update_and_hide (GSD_OSD_WINDOW (window));
}

static void
volume_muted_changed (GsdOsdWindow *window)
{
        gsd_osd_window_update_and_hide (GSD_OSD_WINDOW (window));
}

void
gsd_osd_window_set_action (GsdOsdWindow      *window,
                           GsdOsdWindowAction action)
{
        g_return_if_fail (GSD_IS_OSD_WINDOW (window));
        g_return_if_fail (action == GSD_OSD_WINDOW_ACTION_VOLUME);

        if (window->priv->action != action) {
                window->priv->action = action;
                action_changed (window);
        } else {
                gsd_osd_window_update_and_hide (GSD_OSD_WINDOW (window));
        }
}

void
gsd_osd_window_set_action_custom (GsdOsdWindow      *window,
                                  const char        *icon_name,
                                  gboolean           show_level)
{
        g_return_if_fail (GSD_IS_OSD_WINDOW (window));
        g_return_if_fail (icon_name != NULL);

        if (window->priv->action != GSD_OSD_WINDOW_ACTION_CUSTOM ||
            g_strcmp0 (window->priv->icon_name, icon_name) != 0 ||
            window->priv->show_level != show_level) {
                window->priv->action = GSD_OSD_WINDOW_ACTION_CUSTOM;
                g_free (window->priv->icon_name);
                window->priv->icon_name = g_strdup (icon_name);
                window->priv->show_level = show_level;
                action_changed (window);
        } else {
                gsd_osd_window_update_and_hide (GSD_OSD_WINDOW (window));
        }
}

void
gsd_osd_window_set_volume_muted (GsdOsdWindow *window,
                                 gboolean      muted)
{
        g_return_if_fail (GSD_IS_OSD_WINDOW (window));

        if (window->priv->volume_muted != muted) {
                window->priv->volume_muted = muted;
                volume_muted_changed (window);
        }
}

void
gsd_osd_window_set_volume_level (GsdOsdWindow *window,
                                 int           level)
{
        g_return_if_fail (GSD_IS_OSD_WINDOW (window));

        if (window->priv->volume_level != level) {
                window->priv->volume_level = level;
                volume_level_changed (window);
        }
}

static GdkPixbuf *
load_pixbuf (GsdOsdWindow *window,
             const char   *name,
             int           icon_size)
{
        GtkIconTheme    *theme;
        GtkIconInfo     *info;
        GdkPixbuf       *pixbuf;
        GtkStyleContext *context;
        GdkRGBA          color;

        if (window != NULL && gtk_widget_has_screen (GTK_WIDGET (window))) {
                theme = gtk_icon_theme_get_for_screen (gtk_widget_get_screen (GTK_WIDGET (window)));
        } else {
                theme = gtk_icon_theme_get_default ();
        }

        context = gtk_widget_get_style_context (GTK_WIDGET (window));
        gtk_style_context_get_background_color (context, GTK_STATE_NORMAL, &color);
        info = gtk_icon_theme_lookup_icon (theme,
                                           name,
                                           icon_size,
                                           GTK_ICON_LOOKUP_FORCE_SIZE | GTK_ICON_LOOKUP_GENERIC_FALLBACK);

        if (info == NULL) {
                g_warning ("Failed to load '%s'", name);
                return NULL;
        }

        pixbuf = gtk_icon_info_load_symbolic (info,
                                              &color,
                                              NULL,
                                              NULL,
                                              NULL,
                                              NULL,
                                              NULL);
        gtk_icon_info_free (info);

        return pixbuf;
}

static void
draw_eject (cairo_t *cr,
            double   _x0,
            double   _y0,
            double   width,
            double   height)
{
        int box_height;
        int tri_height;
        int separation;

        box_height = height * 0.2;
        separation = box_height / 3;
        tri_height = height - box_height - separation;

        cairo_rectangle (cr, _x0, _y0 + height - box_height, width, box_height);

        cairo_move_to (cr, _x0, _y0 + tri_height);
        cairo_rel_line_to (cr, width, 0);
        cairo_rel_line_to (cr, -width / 2, -tri_height);
        cairo_rel_line_to (cr, -width / 2, tri_height);
        cairo_close_path (cr);
        cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, GSD_OSD_WINDOW_FG_ALPHA);
        cairo_fill_preserve (cr);

        cairo_set_source_rgba (cr, 0.6, 0.6, 0.6, GSD_OSD_WINDOW_FG_ALPHA / 2);
        cairo_set_line_width (cr, 2);
        cairo_stroke (cr);
}

static void
draw_waves (cairo_t *cr,
            double   cx,
            double   cy,
            double   max_radius,
            int      volume_level)
{
        const int n_waves = 3;
        int last_wave;
        int i;

        last_wave = n_waves * volume_level / 100;

        for (i = 0; i < n_waves; i++) {
                double angle1;
                double angle2;
                double radius;
                double alpha;

                angle1 = -M_PI / 4;
                angle2 = M_PI / 4;

                if (i < last_wave)
                        alpha = 1.0;
                else if (i > last_wave)
                        alpha = 0.1;
                else alpha = 0.1 + 0.9 * (n_waves * volume_level % 100) / 100.0;

                radius = (i + 1) * (max_radius / n_waves);
                cairo_arc (cr, cx, cy, radius, angle1, angle2);
                cairo_set_source_rgba (cr, 0.6, 0.6, 0.6, alpha / 2);
                cairo_set_line_width (cr, 14);
                cairo_set_line_cap  (cr, CAIRO_LINE_CAP_ROUND);
                cairo_stroke_preserve (cr);

                cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, alpha);
                cairo_set_line_width (cr, 10);
                cairo_set_line_cap  (cr, CAIRO_LINE_CAP_ROUND);
                cairo_stroke (cr);
        }
}

static void
draw_cross (cairo_t *cr,
            double   cx,
            double   cy,
            double   size)
{
        cairo_move_to (cr, cx, cy - size/2.0);
        cairo_rel_line_to (cr, size, size);

        cairo_move_to (cr, cx, cy + size/2.0);
        cairo_rel_line_to (cr, size, -size);

        cairo_set_source_rgba (cr, 0.6, 0.6, 0.6, GSD_OSD_WINDOW_FG_ALPHA / 2);
        cairo_set_line_width (cr, 14);
        cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
        cairo_stroke_preserve (cr);

        cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, GSD_OSD_WINDOW_FG_ALPHA);
        cairo_set_line_width (cr, 10);
        cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
        cairo_stroke (cr);
}

static void
draw_speaker (cairo_t *cr,
              double   cx,
              double   cy,
              double   width,
              double   height)
{
        double box_width;
        double box_height;
        double _x0;
        double _y0;

        box_width = width / 3;
        box_height = height / 3;

        _x0 = cx - (width / 2) + box_width;
        _y0 = cy - box_height / 2;

        cairo_move_to (cr, _x0, _y0);
        cairo_rel_line_to (cr, - box_width, 0);
        cairo_rel_line_to (cr, 0, box_height);
        cairo_rel_line_to (cr, box_width, 0);

        cairo_line_to (cr, cx + box_width, cy + height / 2);
        cairo_rel_line_to (cr, 0, -height);
        cairo_line_to (cr, _x0, _y0);
        cairo_close_path (cr);

        cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, GSD_OSD_WINDOW_FG_ALPHA);
        cairo_fill_preserve (cr);

        cairo_set_source_rgba (cr, 0.6, 0.6, 0.6, GSD_OSD_WINDOW_FG_ALPHA / 2);
        cairo_set_line_width (cr, 2);
        cairo_stroke (cr);
}

static gboolean
render_speaker (GsdOsdWindow *window,
                cairo_t            *cr,
                double              _x0,
                double              _y0,
                double              width,
                double              height)
{
        GdkPixbuf         *pixbuf;
        const char        *icon_name;
        int                icon_size;

	icon_name = get_image_name_for_volume (window->priv->volume_muted,
					       window->priv->volume_level);

        icon_size = (int)width;

        pixbuf = load_pixbuf (window, icon_name, icon_size);

        if (pixbuf == NULL) {
                return FALSE;
        }

        gdk_cairo_set_source_pixbuf (cr, pixbuf, _x0, _y0);
        cairo_paint_with_alpha (cr, GSD_OSD_WINDOW_FG_ALPHA);

        g_object_unref (pixbuf);

        return TRUE;
}

#define LIGHTNESS_MULT  1.3
#define DARKNESS_MULT   0.7

static void
draw_volume_boxes (GsdOsdWindow *window,
                   cairo_t            *cr,
                   double              percentage,
                   double              _x0,
                   double              _y0,
                   double              width,
                   double              height)
{
        gdouble   x1;
        GtkStyleContext *context;
        GdkRGBA  acolor;

        height = round (height) - 1;
        width = round (width) - 1;
        x1 = round ((width - 1) * percentage);
        context = gtk_widget_get_style_context (GTK_WIDGET (window));

        /* bar background */
        gtk_style_context_get_background_color (context, GTK_STATE_NORMAL, &acolor);
        gsd_osd_window_color_shade (&acolor, DARKNESS_MULT);
        gsd_osd_window_color_reverse (&acolor);
        acolor.alpha = GSD_OSD_WINDOW_FG_ALPHA / 2;
        gsd_osd_window_draw_rounded_rectangle (cr, 1.0, _x0, _y0, height / 6, width, height);
        gdk_cairo_set_source_rgba (cr, &acolor);
        cairo_fill (cr);

        /* bar progress */
        if (percentage < 0.01)
                return;
        gtk_style_context_get_background_color (context, GTK_STATE_NORMAL, &acolor);
        acolor.alpha = GSD_OSD_WINDOW_FG_ALPHA;
        gsd_osd_window_draw_rounded_rectangle (cr, 1.0, _x0, _y0, height / 6, x1, height);
        gdk_cairo_set_source_rgba (cr, &acolor);
        cairo_fill (cr);
}

static void
draw_action_volume (GsdOsdWindow *window,
                    cairo_t            *cr)
{
        int window_width;
        int window_height;
        double icon_box_width;
        double icon_box_height;
        double icon_box_x0;
        double icon_box_y0;
        double volume_box_x0;
        double volume_box_y0;
        double volume_box_width;
        double volume_box_height;
        gboolean res;

        gtk_window_get_size (GTK_WINDOW (window), &window_width, &window_height);

        icon_box_width = round (window_width * ICON_SCALE);
        icon_box_height = round (window_height * ICON_SCALE);
        volume_box_width = icon_box_width;
        volume_box_height = round (window_height * 0.05);

        icon_box_x0 = round ((window_width - icon_box_width) / 2);
        icon_box_y0 = round ((window_height - icon_box_height - volume_box_height) / 2 - volume_box_height);
        volume_box_x0 = round (icon_box_x0);
        volume_box_y0 = round (icon_box_height + icon_box_y0) + volume_box_height;

#if 0
        g_message ("icon box: w=%f h=%f _x0=%f _y0=%f",
                   icon_box_width,
                   icon_box_height,
                   icon_box_x0,
                   icon_box_y0);
        g_message ("volume box: w=%f h=%f _x0=%f _y0=%f",
                   volume_box_width,
                   volume_box_height,
                   volume_box_x0,
                   volume_box_y0);
#endif

        res = render_speaker (window,
                              cr,
                              icon_box_x0, icon_box_y0,
                              icon_box_width, icon_box_height);
        if (! res) {
                double speaker_width;
                double speaker_height;
                double speaker_cx;
                double speaker_cy;

                speaker_width = icon_box_width * 0.5;
                speaker_height = icon_box_height * 0.75;
                speaker_cx = icon_box_x0 + speaker_width / 2;
                speaker_cy = icon_box_y0 + speaker_height / 2;

#if 0
                g_message ("speaker box: w=%f h=%f cx=%f cy=%f",
                           speaker_width,
                           speaker_height,
                           speaker_cx,
                           speaker_cy);
#endif

                /* draw speaker symbol */
                draw_speaker (cr, speaker_cx, speaker_cy, speaker_width, speaker_height);

                if (! window->priv->volume_muted) {
                        /* draw sound waves */
                        double wave_x0;
                        double wave_y0;
                        double wave_radius;

                        wave_x0 = window_width / 2;
                        wave_y0 = speaker_cy;
                        wave_radius = icon_box_width / 2;

                        draw_waves (cr, wave_x0, wave_y0, wave_radius, window->priv->volume_level);
                } else {
                        /* draw 'mute' cross */
                        double cross_x0;
                        double cross_y0;
                        double cross_size;

                        cross_size = speaker_width * 3 / 4;
                        cross_x0 = icon_box_x0 + icon_box_width - cross_size;
                        cross_y0 = speaker_cy;

                        draw_cross (cr, cross_x0, cross_y0, cross_size);
                }
        }

        /* draw volume meter */
        draw_volume_boxes (window,
                           cr,
                           (double)window->priv->volume_level / 100.0,
                           volume_box_x0,
                           volume_box_y0,
                           volume_box_width,
                           volume_box_height);
}

static gboolean
render_custom (GsdOsdWindow *window,
               cairo_t            *cr,
               double              _x0,
               double              _y0,
               double              width,
               double              height)
{
        GdkPixbuf         *pixbuf;
        int                icon_size;

        icon_size = (int)width;

        pixbuf = load_pixbuf (window, window->priv->icon_name, icon_size);

        if (pixbuf == NULL) {
                char *name;
                if (gtk_widget_get_direction (GTK_WIDGET (window)) == GTK_TEXT_DIR_RTL)
                        name = g_strdup_printf ("%s-rtl", window->priv->icon_name);
                else
                        name = g_strdup_printf ("%s-ltr", window->priv->icon_name);
                pixbuf = load_pixbuf (window, name, icon_size);
                g_free (name);
                if (pixbuf == NULL)
                        return FALSE;
        }

        gdk_cairo_set_source_pixbuf (cr, pixbuf, _x0, _y0);
        cairo_paint_with_alpha (cr, GSD_OSD_WINDOW_FG_ALPHA);

        g_object_unref (pixbuf);

        return TRUE;
}

static void
draw_action_custom (GsdOsdWindow *window,
                    cairo_t            *cr)
{
        int window_width;
        int window_height;
        double icon_box_width;
        double icon_box_height;
        double icon_box_x0;
        double icon_box_y0;
        double bright_box_x0;
        double bright_box_y0;
        double bright_box_width;
        double bright_box_height;
        gboolean res;

        gtk_window_get_size (GTK_WINDOW (window), &window_width, &window_height);

        icon_box_width = round (window_width * ICON_SCALE);
        icon_box_height = round (window_height * ICON_SCALE);
        bright_box_width = round (icon_box_width);
        bright_box_height = round (window_height * 0.05);

        icon_box_x0 = round ((window_width - icon_box_width) / 2);
        icon_box_y0 = round ((window_height - icon_box_height - bright_box_height) / 2 - bright_box_height);
        bright_box_x0 = round (icon_box_x0);
        bright_box_y0 = round (icon_box_height + icon_box_y0) + bright_box_height;

#if 0
        g_message ("icon box: w=%f h=%f _x0=%f _y0=%f",
                   icon_box_width,
                   icon_box_height,
                   icon_box_x0,
                   icon_box_y0);
        g_message ("brightness box: w=%f h=%f _x0=%f _y0=%f",
                   bright_box_width,
                   bright_box_height,
                   bright_box_x0,
                   bright_box_y0);
#endif

        res = render_custom (window,
                             cr,
                             icon_box_x0, icon_box_y0,
                             icon_box_width, icon_box_height);
        if (! res && g_str_has_prefix (window->priv->icon_name, "media-eject")) {
                /* draw eject symbol */
                draw_eject (cr,
                            icon_box_x0, icon_box_y0,
                            icon_box_width, icon_box_height);
        }

        if (window->priv->show_level != FALSE) {
                /* draw volume meter */
                draw_volume_boxes (window,
                                   cr,
                                   (double)window->priv->volume_level / 100.0,
                                   bright_box_x0,
                                   bright_box_y0,
                                   bright_box_width,
                                   bright_box_height);
        }
}

static gboolean
gsd_osd_window_draw (GtkWidget *widget,
                     cairo_t   *orig_cr)
{
        GsdOsdWindow    *window;
        cairo_t         *cr;
        cairo_surface_t *surface;
        int              width;
        int              height;
        GtkStyleContext *context;
        GdkRGBA          acolor;
        gdouble          corner_radius;

        window = GSD_OSD_WINDOW (widget);

        context = gtk_widget_get_style_context (widget);
        cairo_set_operator (orig_cr, CAIRO_OPERATOR_SOURCE);
        gtk_window_get_size (GTK_WINDOW (widget), &width, &height);

        surface = cairo_surface_create_similar (cairo_get_target (orig_cr),
                                                CAIRO_CONTENT_COLOR_ALPHA,
                                                width,
                                                height);

        if (cairo_surface_status (surface) != CAIRO_STATUS_SUCCESS) {
                goto done;
        }

        cr = cairo_create (surface);
        if (cairo_status (cr) != CAIRO_STATUS_SUCCESS) {
                goto done;
        }
        cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.0);
        cairo_set_operator (cr, CAIRO_OPERATOR_OVER);
        cairo_paint (cr);

        /* draw a box */
	corner_radius = height / 10;
        gsd_osd_window_draw_rounded_rectangle (cr, 1.0, 0.0, 0.0, corner_radius, width-1, height-1);
        gtk_style_context_get_background_color (context, GTK_STATE_NORMAL, &acolor);
        gsd_osd_window_color_reverse (&acolor);
        acolor.alpha = BG_ALPHA;
        gdk_cairo_set_source_rgba (cr, &acolor);
        cairo_fill (cr);

        switch (window->priv->action) {
        case GSD_OSD_WINDOW_ACTION_VOLUME:
                draw_action_volume (window, cr);
                break;
        case GSD_OSD_WINDOW_ACTION_CUSTOM:
                draw_action_custom (window, cr);
                break;
        default:
                break;
        }

        cairo_destroy (cr);

        /* Make sure we have a transparent background */
        cairo_rectangle (orig_cr, 0, 0, width, height);
        cairo_set_source_rgba (orig_cr, 0.0, 0.0, 0.0, 0.0);
        cairo_fill (orig_cr);

        cairo_set_source_surface (orig_cr, surface, 0, 0);
        cairo_paint_with_alpha (orig_cr, window->priv->fade_out_alpha);

 done:
        if (surface != NULL) {
                cairo_surface_destroy (surface);
        }

        return FALSE;
}

static void
gsd_osd_window_real_show (GtkWidget *widget)
{
        GsdOsdWindow *window;

        if (GTK_WIDGET_CLASS (gsd_osd_window_parent_class)->show) {
                GTK_WIDGET_CLASS (gsd_osd_window_parent_class)->show (widget);
        }

        window = GSD_OSD_WINDOW (widget);
        remove_hide_timeout (window);
        add_hide_timeout (window);
}

static void
gsd_osd_window_real_hide (GtkWidget *widget)
{
        GsdOsdWindow *window;

        if (GTK_WIDGET_CLASS (gsd_osd_window_parent_class)->hide) {
                GTK_WIDGET_CLASS (gsd_osd_window_parent_class)->hide (widget);
        }

        window = GSD_OSD_WINDOW (widget);
        remove_hide_timeout (window);
}

static void
gsd_osd_window_real_realize (GtkWidget *widget)
{
        cairo_region_t *region;
        GdkScreen *screen;
        GdkVisual *visual;

        screen = gtk_widget_get_screen (widget);
        visual = gdk_screen_get_rgba_visual (screen);
        if (visual == NULL) {
                visual = gdk_screen_get_system_visual (screen);
        }

        gtk_widget_set_visual (widget, visual);

        if (GTK_WIDGET_CLASS (gsd_osd_window_parent_class)->realize) {
                GTK_WIDGET_CLASS (gsd_osd_window_parent_class)->realize (widget);
        }

        /* make the whole window ignore events */
        region = cairo_region_create ();
        gtk_widget_input_shape_combine_region (widget, region);
        cairo_region_destroy (region);
}

static GObject *
gsd_osd_window_constructor (GType                  type,
                            guint                  n_construct_properties,
                            GObjectConstructParam *construct_params)
{
        GObject *object;

        object = G_OBJECT_CLASS (gsd_osd_window_parent_class)->constructor (type, n_construct_properties, construct_params);

        g_object_set (object,
                      "type", GTK_WINDOW_POPUP,
                      "type-hint", GDK_WINDOW_TYPE_HINT_NOTIFICATION,
                      "skip-taskbar-hint", TRUE,
                      "skip-pager-hint", TRUE,
                      "focus-on-map", FALSE,
                      NULL);

        return object;
}

static void
gsd_osd_window_finalize (GObject *object)
{
	GsdOsdWindow *window;

	window = GSD_OSD_WINDOW (object);
	if (window->priv->icon_name) {
		g_free (window->priv->icon_name);
		window->priv->icon_name = NULL;
	}

	G_OBJECT_CLASS (gsd_osd_window_parent_class)->finalize (object);
}

static void
gsd_osd_window_class_init (GsdOsdWindowClass *klass)
{
        GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
        GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

        gobject_class->constructor = gsd_osd_window_constructor;
        gobject_class->finalize = gsd_osd_window_finalize;

        widget_class->show = gsd_osd_window_real_show;
        widget_class->hide = gsd_osd_window_real_hide;
        widget_class->realize = gsd_osd_window_real_realize;
        widget_class->draw = gsd_osd_window_draw;

        g_type_class_add_private (klass, sizeof (GsdOsdWindowPrivate));
}

/**
 * gsd_osd_window_is_valid:
 * @window: a #GsdOsdWindow
 *
 * Return value: TRUE if the @window's idea of the screen geometry is the
 * same as the current screen's.
 */
gboolean
gsd_osd_window_is_valid (GsdOsdWindow *window)
{
        GdkScreen *screen = gtk_widget_get_screen (GTK_WIDGET (window));
        gint monitor;
        GdkRectangle mon_rect;

	monitor = gdk_screen_get_primary_monitor (screen);
	if (monitor != window->priv->monitor)
		return FALSE;

	gdk_screen_get_monitor_geometry (screen, monitor, &mon_rect);

        if (window->priv->screen_width != mon_rect.width ||
            window->priv->screen_height != mon_rect.height)
                return FALSE;

	return TRUE;
}

static void
gsd_osd_window_init (GsdOsdWindow *window)
{
        GdkScreen *screen;
        gdouble scalew, scaleh, scale;
        GdkRectangle monitor;
        gint size;

        window->priv = GSD_OSD_WINDOW_GET_PRIVATE (window);

        screen = gtk_widget_get_screen (GTK_WIDGET (window));

        window->priv->monitor = gdk_screen_get_primary_monitor (screen);
        gdk_screen_get_monitor_geometry (screen, window->priv->monitor, &monitor);
        window->priv->screen_width = monitor.width;
        window->priv->screen_height = monitor.height;

        gtk_window_set_decorated (GTK_WINDOW (window), FALSE);
        gtk_widget_set_app_paintable (GTK_WIDGET (window), TRUE);

        /* assume 130x130 on a 640x480 display and scale from there */
        scalew = monitor.width / 640.0;
        scaleh = monitor.height / 480.0;
        scale = MIN (scalew, scaleh);
        size = 130 * MAX (1, scale);
        gtk_window_set_default_size (GTK_WINDOW (window), size, size);

        window->priv->fade_out_alpha = 1.0;
}

GtkWidget *
gsd_osd_window_new (void)
{
        return g_object_new (GSD_TYPE_OSD_WINDOW, NULL);
}

/**
 * gsd_osd_window_update_and_hide:
 * @window: a #GsdOsdWindow
 *
 * Queues the @window for immediate drawing, and queues a timer to hide the window.
 */
static void
gsd_osd_window_update_and_hide (GsdOsdWindow *window)
{
        remove_hide_timeout (window);
        add_hide_timeout (window);

        gtk_widget_queue_draw (GTK_WIDGET (window));
}
