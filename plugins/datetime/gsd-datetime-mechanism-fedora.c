/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007 David Zeuthen <david@fubar.dk>
 * Copyright (C) 2011 Bastien Nocera <hadess@hadess.net>
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

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <string.h>

#include "gsd-datetime-mechanism-fedora.h"
#include "gsd-datetime-mechanism.h"

gboolean
_get_using_ntp_fedora  (DBusGMethodInvocation   *context)
{
        int exit_status;
        GError *error = NULL;
        gboolean can_use_ntp;
        gboolean is_using_ntp;

        if (g_file_test ("/etc/ntp.conf", G_FILE_TEST_EXISTS)) {
                can_use_ntp = TRUE;
                if (!g_spawn_command_line_sync ("/sbin/service ntpd status",
                                                NULL, NULL, &exit_status, &error)) {
                        GError *error2;
                        error2 = g_error_new (GSD_DATETIME_MECHANISM_ERROR,
                                              GSD_DATETIME_MECHANISM_ERROR_GENERAL,
                                              "Error spawning /sbin/service: %s", error->message);
                        g_error_free (error);
                        dbus_g_method_return_error (context, error2);
                        g_error_free (error2);
                        return FALSE;
                }
                if (exit_status == 0)
                        is_using_ntp = TRUE;
                else
                        is_using_ntp = FALSE;
        }
        else {
                can_use_ntp = FALSE;
                is_using_ntp = FALSE;
        }

        dbus_g_method_return (context, can_use_ntp, is_using_ntp);
        return TRUE;
}

gboolean
_set_using_ntp_fedora  (DBusGMethodInvocation   *context,
                        gboolean                 using_ntp)
{
        GError *error;
        int exit_status;
        char *cmd;

        error = NULL;

        cmd = g_strconcat ("/sbin/chkconfig ntpd --level 2345", using_ntp ? "on" : "off", NULL);

        if (!g_spawn_command_line_sync (cmd,
                                        NULL, NULL, &exit_status, &error)) {
                GError *error2;
                error2 = g_error_new (GSD_DATETIME_MECHANISM_ERROR,
                                      GSD_DATETIME_MECHANISM_ERROR_GENERAL,
                                      "Error spawning '%s': %s", cmd, error->message);
                g_error_free (error);
                dbus_g_method_return_error (context, error2);
                g_error_free (error2);
                g_free (cmd);
                return FALSE;
        }

        g_free (cmd);

        cmd = g_strconcat ("/sbin/service ntpd ", using_ntp ? "restart" : "stop", NULL);;

        if (!g_spawn_command_line_sync (cmd,
                                        NULL, NULL, &exit_status, &error)) {
                GError *error2;
                error2 = g_error_new (GSD_DATETIME_MECHANISM_ERROR,
                                      GSD_DATETIME_MECHANISM_ERROR_GENERAL,
                                      "Error spawning '%s': %s", cmd, error->message);
                g_error_free (error);
                dbus_g_method_return_error (context, error2);
                g_error_free (error2);
                g_free (cmd);
                return FALSE;
        }

        g_free (cmd);

        dbus_g_method_return (context);
        return TRUE;
}

gboolean
_update_etc_sysconfig_clock_fedora (DBusGMethodInvocation *context, const char *key, const char *value)
{
        char **lines;
        int n;
        gboolean replaced;
        char *data;
        gsize len;
        GError *error;

        /* On Red Hat / Fedora, the /etc/sysconfig/clock file needs to be kept in sync */
        if (!g_file_test ("/etc/sysconfig/clock", G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR)) {
                error = g_error_new (GSD_DATETIME_MECHANISM_ERROR,
                                      GSD_DATETIME_MECHANISM_ERROR_GENERAL,
                                      "Error reading /etc/sysconfig/clock file: %s", "No such file");
                dbus_g_method_return_error (context, error);
                g_error_free (error);
                return FALSE;
	}

        error = NULL;

        if (!g_file_get_contents ("/etc/sysconfig/clock", &data, &len, &error)) {
                GError *error2;
                error2 = g_error_new (GSD_DATETIME_MECHANISM_ERROR,
                                      GSD_DATETIME_MECHANISM_ERROR_GENERAL,
                                      "Error reading /etc/sysconfig/clock file: %s", error->message);
                g_error_free (error);
                dbus_g_method_return_error (context, error2);
                g_error_free (error2);
                return FALSE;
        }
        replaced = FALSE;
        lines = g_strsplit (data, "\n", 0);
        g_free (data);

        for (n = 0; lines[n] != NULL; n++) {
                if (g_str_has_prefix (lines[n], key)) {
                        g_free (lines[n]);
                        lines[n] = g_strdup_printf ("%s%s", key, value);
                        replaced = TRUE;
                }
        }
        if (replaced) {
                GString *str;

                str = g_string_new (NULL);
                for (n = 0; lines[n] != NULL; n++) {
                        g_string_append (str, lines[n]);
                        if (lines[n + 1] != NULL)
                                g_string_append_c (str, '\n');
                }
                data = g_string_free (str, FALSE);
                len = strlen (data);
                if (!g_file_set_contents ("/etc/sysconfig/clock", data, len, &error)) {
                        GError *error2;
                        error2 = g_error_new (GSD_DATETIME_MECHANISM_ERROR,
                                              GSD_DATETIME_MECHANISM_ERROR_GENERAL,
                                              "Error updating /etc/sysconfig/clock: %s", error->message);
                        g_error_free (error);
                        dbus_g_method_return_error (context, error2);
                        g_error_free (error2);
                        g_free (data);
                        return FALSE;
                }
                g_free (data);
        }
        g_strfreev (lines);

        return TRUE;
}


