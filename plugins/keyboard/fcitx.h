/*
 * Copyright 2014 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors: William Hua <william.hua@canonical.com>
 */

#ifndef __FCITX_H__
#define __FCITX_H__

#include <gio/gio.h>

void          fcitx_start_async              (GCancellable         *cancellable,
                                              GAsyncReadyCallback   callback,
                                              gpointer              user_data)   G_GNUC_INTERNAL;

gboolean      fcitx_start_finish             (GAsyncResult         *result,
                                              GError              **error)       G_GNUC_INTERNAL;

void          fcitx_stop                     (void)                              G_GNUC_INTERNAL;

gchar **      fcitx_get_input_method_ids     (void)                              G_GNUC_INTERNAL;

const gchar * fcitx_get_input_method_name    (const gchar          *id)          G_GNUC_INTERNAL;

const gchar * fcitx_get_input_method_layout  (const gchar          *id)          G_GNUC_INTERNAL;

const gchar * fcitx_get_input_method_variant (const gchar          *id)          G_GNUC_INTERNAL;

void          fcitx_configure_input_method   (const gchar          *id)          G_GNUC_INTERNAL;

void          fcitx_activate_input_method    (const gchar          *id)          G_GNUC_INTERNAL;

void          fcitx_deactivate_input_method  (void)                              G_GNUC_INTERNAL;

#endif /* __FCITX_H__ */
