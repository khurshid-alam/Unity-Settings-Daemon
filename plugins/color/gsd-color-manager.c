/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007 William Jon McCann <mccann@jhu.edu>
 * Copyright (C) 2011 Richard Hughes <richard@hughsie.com>
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

#include <glib/gi18n.h>
#include <colord.h>
#include <libnotify/notify.h>
#include <gdk/gdk.h>
#include <stdlib.h>

#ifdef HAVE_LIBCANBERRA
#include <canberra-gtk.h>
#endif

#ifdef HAVE_LCMS
  #include <lcms2.h>
#endif

#define GNOME_DESKTOP_USE_UNSTABLE_API
#include <libgnome-desktop/gnome-rr.h>

#include "gnome-settings-profile.h"
#include "gsd-color-manager.h"
#include "gcm-profile-store.h"
#include "gcm-dmi.h"
#include "gcm-edid.h"

#define GSD_COLOR_MANAGER_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), GSD_TYPE_COLOR_MANAGER, GsdColorManagerPrivate))

#define GCM_SESSION_NOTIFY_TIMEOUT                      30000 /* ms */
#define GCM_SETTINGS_SHOW_NOTIFICATIONS                 "show-notifications"
#define GCM_SETTINGS_RECALIBRATE_PRINTER_THRESHOLD      "recalibrate-printer-threshold"
#define GCM_SETTINGS_RECALIBRATE_DISPLAY_THRESHOLD      "recalibrate-display-threshold"

struct GsdColorManagerPrivate
{
        CdClient        *client;
        GSettings       *settings;
        GcmProfileStore *profile_store;
        GcmDmi          *dmi;
        GnomeRRScreen   *x11_screen;
        GHashTable      *edid_cache;
        GdkWindow       *gdk_window;
};

enum {
        PROP_0,
};

static void     gsd_color_manager_class_init  (GsdColorManagerClass *klass);
static void     gsd_color_manager_init        (GsdColorManager      *color_manager);
static void     gsd_color_manager_finalize    (GObject             *object);

G_DEFINE_TYPE (GsdColorManager, gsd_color_manager, G_TYPE_OBJECT)

static gpointer manager_object = NULL;

/* see http://www.oyranos.org/wiki/index.php?title=ICC_Profiles_in_X_Specification_0.3 */
#define GCM_ICC_PROFILE_IN_X_VERSION_MAJOR      0
#define GCM_ICC_PROFILE_IN_X_VERSION_MINOR      3

typedef struct {
        guint32          red;
        guint32          green;
        guint32          blue;
} GnomeRROutputClutItem;

GQuark
gsd_color_manager_error_quark (void)
{
	static GQuark quark = 0;
	if (!quark)
		quark = g_quark_from_static_string ("gsd_color_manager_error");
	return quark;
}

static GcmEdid *
gcm_session_get_output_edid (GsdColorManager *manager, GnomeRROutput *output, GError **error)
{
        const guint8 *data;
        gsize size;
        GcmEdid *edid = NULL;
        gboolean ret;

        /* can we find it in the cache */
        edid = g_hash_table_lookup (manager->priv->edid_cache,
                                    gnome_rr_output_get_name (output));
        if (edid != NULL) {
                g_object_ref (edid);
                goto out;
        }

        /* parse edid */
        data = gnome_rr_output_get_edid_data (output, &size);
        if (data == NULL || size == 0) {
                g_set_error_literal (error,
                                     GNOME_RR_ERROR,
                                     GNOME_RR_ERROR_UNKNOWN,
                                     "unable to get EDID for output");
                goto out;
        }
        edid = gcm_edid_new ();
        ret = gcm_edid_parse (edid, data, size, error);
        if (!ret) {
                g_object_unref (edid);
                edid = NULL;
                goto out;
        }

        /* add to cache */
        g_hash_table_insert (manager->priv->edid_cache,
                             g_strdup (gnome_rr_output_get_name (output)),
                             g_object_ref (edid));
out:
        return edid;
}

static gboolean
gcm_session_screen_set_icc_profile (GsdColorManager *manager,
                                    const gchar *filename,
                                    GError **error)
{
        gboolean ret;
        gchar *data = NULL;
        gsize length;
        guint version_data;
        GsdColorManagerPrivate *priv = manager->priv;

        g_return_val_if_fail (filename != NULL, FALSE);

        g_debug ("setting root window ICC profile atom from %s", filename);

        /* get contents of file */
        ret = g_file_get_contents (filename, &data, &length, error);
        if (!ret)
                goto out;

        /* set profile property */
        gdk_property_change (priv->gdk_window,
                             gdk_atom_intern_static_string ("_ICC_PROFILE"),
                             gdk_atom_intern_static_string ("CARDINAL"),
                             8,
                             GDK_PROP_MODE_REPLACE,
                             (const guchar *) data, length);

        /* set version property */
        version_data = GCM_ICC_PROFILE_IN_X_VERSION_MAJOR * 100 +
                        GCM_ICC_PROFILE_IN_X_VERSION_MINOR * 1;
        gdk_property_change (priv->gdk_window,
                             gdk_atom_intern_static_string ("_ICC_PROFILE_IN_X_VERSION"),
                             gdk_atom_intern_static_string ("CARDINAL"),
                             8,
                             GDK_PROP_MODE_REPLACE,
                             (const guchar *) &version_data, 1);
out:
        g_free (data);
        return ret;
}

static gchar *
gcm_session_get_output_id (GsdColorManager *manager, GnomeRROutput *output)
{
        const gchar *name;
        const gchar *serial;
        const gchar *vendor;
        GcmEdid *edid = NULL;
        GString *device_id;
        GError *error = NULL;

        /* all output devices are prefixed with this */
        device_id = g_string_new ("xrandr");

        /* get the output EDID if possible */
        edid = gcm_session_get_output_edid (manager, output, &error);
        if (edid == NULL) {
                g_debug ("no edid for %s [%s], falling back to connection name",
                         gnome_rr_output_get_name (output),
                         error->message);
                g_error_free (error);
                g_string_append_printf (device_id,
                                        "_%s",
                                        gnome_rr_output_get_name (output));
                goto out;
        }

        /* get EDID data */
        vendor = gcm_edid_get_vendor_name (edid);
        if (vendor != NULL)
                g_string_append_printf (device_id, "-%s", vendor);
        name = gcm_edid_get_monitor_name (edid);
        if (name != NULL)
                g_string_append_printf (device_id, "-%s", name);
        serial = gcm_edid_get_serial_number (edid);
        if (serial != NULL)
                g_string_append_printf (device_id, "-%s", serial);
out:
        if (edid != NULL)
                g_object_unref (edid);
        return g_string_free (device_id, FALSE);
}

static GnomeRROutput *
gcm_session_get_output_by_edid_checksum (GnomeRRScreen *screen,
                                         const gchar *edid_md5,
                                         GError **error)
{
        const guint8 *data;
        gchar *checksum;
        GnomeRROutput *output = NULL;
        GnomeRROutput **outputs;
        gsize size;
        guint i;

        outputs = gnome_rr_screen_list_outputs (screen);
        if (outputs == NULL) {
                g_set_error_literal (error,
                                     GSD_COLOR_MANAGER_ERROR,
                                     GSD_COLOR_MANAGER_ERROR_FAILED,
                                     "Failed to get outputs");
                goto out;
        }

        /* find the output */
        for (i = 0; outputs[i] != NULL && output == NULL; i++) {

                /* get edid */
                data = gnome_rr_output_get_edid_data (outputs[i], &size);
                if (data == NULL || size < 0x6c)
                        continue;
                checksum = g_compute_checksum_for_data (G_CHECKSUM_MD5, data, 0x6c);
                if (g_strcmp0 (checksum, edid_md5) == 0)
                        output = outputs[i];
                g_free (checksum);
        }
        if (output == NULL) {
                g_set_error_literal (error,
                                     GSD_COLOR_MANAGER_ERROR,
                                     GSD_COLOR_MANAGER_ERROR_FAILED,
                                     "no connected output with that edid hash");
        }
out:
        return output;
}

typedef struct {
        GsdColorManager         *manager;
        CdProfile               *profile;
        CdDevice                *device;
        guint32                  output_id;
} GcmSessionAsyncHelper;

static void
gcm_session_async_helper_free (GcmSessionAsyncHelper *helper)
{
        if (helper->manager != NULL)
                g_object_unref (helper->manager);
        if (helper->profile != NULL)
                g_object_unref (helper->profile);
        if (helper->device != NULL)
                g_object_unref (helper->device);
        g_free (helper);
}

static void
gcm_session_profile_assign_add_profile_cb (GObject *object,
                                           GAsyncResult *res,
                                           gpointer user_data)
{
        CdDevice *device = CD_DEVICE (object);
        gboolean ret;
        GError *error = NULL;
        GcmSessionAsyncHelper *helper = (GcmSessionAsyncHelper *) user_data;

        /* add the profile to the device */
        ret = cd_device_add_profile_finish (device,
                                            res,
                                            &error);
        if (!ret) {
                /* this will fail if the profile is already added */
                g_debug ("failed to assign auto-edid profile to device %s: %s",
                         cd_device_get_id (device),
                         error->message);
                g_error_free (error);
                goto out;
        }

        /* phew! */
        g_debug ("successfully assigned %s to %s",
                 cd_profile_get_object_path (helper->profile),
                 cd_device_get_object_path (device));
out:
        gcm_session_async_helper_free (helper);
}

static void
gcm_session_profile_assign_device_connect_cb (GObject *object,
                                              GAsyncResult *res,
                                              gpointer user_data)
{
        CdDevice *device = CD_DEVICE (object);
        gboolean ret;
        GError *error = NULL;
        GcmSessionAsyncHelper *helper = (GcmSessionAsyncHelper *) user_data;

        /* get properties */
        ret = cd_device_connect_finish (device, res, &error);
        if (!ret) {
                g_warning ("cannot connect to device: %s",
                           error->message);
                g_error_free (error);
                gcm_session_async_helper_free (helper);
                goto out;
        }

        /* add the profile to the device */
        cd_device_add_profile (device,
                               CD_DEVICE_RELATION_SOFT,
                               helper->profile,
                               NULL,
                               gcm_session_profile_assign_add_profile_cb,
                               helper);
out:
        return;
}

static void
gcm_session_profile_assign_find_device_cb (GObject *object,
                                           GAsyncResult *res,
                                           gpointer user_data)
{
        CdClient *client = CD_CLIENT (object);
        CdDevice *device = NULL;
        GcmSessionAsyncHelper *helper = (GcmSessionAsyncHelper *) user_data;
        GError *error = NULL;

        device = cd_client_find_device_finish (client,
                                               res,
                                               &error);
        if (device == NULL) {
                g_warning ("not found device %s which should have been added: %s",
                           cd_device_get_id (device),
                           error->message);
                g_error_free (error);
                gcm_session_async_helper_free (helper);
                goto out;
        }

        /* get properties */
        cd_device_connect (device,
                           NULL,
                           gcm_session_profile_assign_device_connect_cb,
                           helper);
out:
        if (device != NULL)
                g_object_unref (device);
}

static void
gcm_session_profile_assign_profile_connect_cb (GObject *object,
                                               GAsyncResult *res,
                                               gpointer user_data)
{
        CdProfile *profile = CD_PROFILE (object);
        const gchar *edid_md5;
        gboolean ret;
        gchar *device_id = NULL;
        GcmSessionAsyncHelper *helper;
        GError *error = NULL;
        GHashTable *metadata = NULL;
        GnomeRROutput *output = NULL;
        GsdColorManager *manager = GSD_COLOR_MANAGER (user_data);

        /* get properties */
        ret = cd_profile_connect_finish (profile, res, &error);
        if (!ret) {
                g_warning ("cannot connect to profile: %s",
                           error->message);
                g_error_free (error);
                goto out;
        }

        /* does the profile have EDID metadata? */
        metadata = cd_profile_get_metadata (profile);
        edid_md5 = g_hash_table_lookup (metadata,
                                        CD_PROFILE_METADATA_EDID_MD5);
        if (edid_md5 == NULL)
                goto out;

        /* get the GnomeRROutput for the edid */
        output = gcm_session_get_output_by_edid_checksum (manager->priv->x11_screen,
                                                          edid_md5,
                                                          &error);
        if (output == NULL) {
                g_debug ("edid hash %s ignored: %s",
                         edid_md5,
                         error->message);
                g_error_free (error);
                goto out;
        }

        /* get the CdDevice for this ID */
        helper = g_new0 (GcmSessionAsyncHelper, 1);
        helper->manager = g_object_ref (manager);
        helper->profile = g_object_ref (profile);
        device_id = gcm_session_get_output_id (manager, output);
        cd_client_find_device (manager->priv->client,
                               device_id,
                               NULL,
                               gcm_session_profile_assign_find_device_cb,
                               helper);
out:
        g_free (device_id);
        if (metadata != NULL)
                g_hash_table_unref (metadata);
}

static void
gcm_session_profile_added_assign_cb (CdClient *client,
                                     CdProfile *profile,
                                     GsdColorManager *manager)
{
        cd_profile_connect (profile,
                            NULL,
                            gcm_session_profile_assign_profile_connect_cb,
                            manager);
}

static cmsBool
_cmsWriteTagTextAscii (cmsHPROFILE lcms_profile,
                       cmsTagSignature sig,
                       const gchar *text)
{
        cmsBool ret;
        cmsMLU *mlu = cmsMLUalloc (0, 1);
        cmsMLUsetASCII (mlu, "EN", "us", text);
        ret = cmsWriteTag (lcms_profile, sig, mlu);
        cmsMLUfree (mlu);
        return ret;
}

static gboolean
gcm_utils_mkdir_for_filename (const gchar *filename, GError **error)
{
        gboolean ret = FALSE;
        GFile *file;
        GFile *parent_dir = NULL;

        /* get parent directory */
        file = g_file_new_for_path (filename);
        parent_dir = g_file_get_parent (file);
        if (parent_dir == NULL) {
                g_set_error (error,
                             GSD_COLOR_MANAGER_ERROR,
                             GSD_COLOR_MANAGER_ERROR_FAILED,
                             "could not get parent dir %s",
                             filename);
                goto out;
        }

        /* ensure desination exists */
        ret = g_file_make_directory_with_parents (parent_dir, NULL, error);
        if (!ret)
                goto out;
out:
        if (file != NULL)
                g_object_unref (file);
        if (parent_dir != NULL)
                g_object_unref (parent_dir);
        return ret;
}

#ifdef HAVE_NEW_LCMS
static cmsBool
_cmsDictAddEntryAscii (cmsHANDLE dict,
                       const gchar *key,
                       const gchar *value)
{
        cmsBool ret;
        wchar_t mb_key[1024];
        wchar_t mb_value[1024];
        mbstowcs (mb_key, key, sizeof (mb_key));
        mbstowcs (mb_value, value, sizeof (mb_value));
        ret = cmsDictAddEntry (dict, mb_key, mb_value, NULL, NULL);
        return ret;
}
#endif /* HAVE_NEW_LCMS */

static gboolean
gcm_apply_create_icc_profile_for_edid (GsdColorManager *manager,
                                       GcmEdid *edid,
                                       const gchar *filename,
                                       GError **error)
{
        const CdColorYxy *tmp;
        cmsCIExyYTRIPLE chroma;
        cmsCIExyY white_point;
        cmsHPROFILE lcms_profile = NULL;
        cmsToneCurve *transfer_curve[3];
        const gchar *data;
        gboolean ret = FALSE;
        gchar *title = NULL;
        gfloat localgamma;
#ifdef HAVE_NEW_LCMS
        cmsHANDLE dict = NULL;
#endif
        GsdColorManagerPrivate *priv = manager->priv;

        /* ensure the per-user directory exists */
        ret = gcm_utils_mkdir_for_filename (filename, error);
        if (!ret)
                goto out;

        /* copy color data from our structures */
        tmp = gcm_edid_get_red (edid);
        chroma.Red.x = tmp->x;
        chroma.Red.y = tmp->y;
        tmp = gcm_edid_get_green (edid);
        chroma.Green.x = tmp->x;
        chroma.Green.y = tmp->y;
        tmp = gcm_edid_get_blue (edid);
        chroma.Blue.x = tmp->x;
        chroma.Blue.y = tmp->y;
        tmp = gcm_edid_get_white (edid);
        white_point.x = tmp->x;
        white_point.y = tmp->y;
        white_point.Y = 1.0;

        /* estimate the transfer function for the gamma */
        localgamma = gcm_edid_get_gamma (edid);
        transfer_curve[0] = transfer_curve[1] = transfer_curve[2] = cmsBuildGamma (NULL, localgamma);

        /* create our generated profile */
        lcms_profile = cmsCreateRGBProfile (&white_point, &chroma, transfer_curve);
        if (lcms_profile == NULL) {
                g_set_error (error,
                             GSD_COLOR_MANAGER_ERROR,
                             GSD_COLOR_MANAGER_ERROR_FAILED,
                             "failed to create profile");
                goto out;
        }

        cmsSetColorSpace (lcms_profile, cmsSigRgbData);
        cmsSetPCS (lcms_profile, cmsSigXYZData);
        cmsSetHeaderRenderingIntent (lcms_profile,
                                     INTENT_RELATIVE_COLORIMETRIC);
        cmsSetDeviceClass (lcms_profile, cmsSigDisplayClass);

        /* copyright */
        ret = _cmsWriteTagTextAscii (lcms_profile,
                                     cmsSigCopyrightTag,
                                     "No copyright");
        if (!ret) {
                g_set_error_literal (error,
                                     GSD_COLOR_MANAGER_ERROR,
                                     GSD_COLOR_MANAGER_ERROR_FAILED,
                                     "failed to write copyright");
                goto out;
        }

        /* set model */
        data = gcm_edid_get_monitor_name (edid);
        if (data == NULL)
                data = gcm_dmi_get_name (priv->dmi);
        if (data == NULL)
                data = "Unknown monitor";
        ret = _cmsWriteTagTextAscii (lcms_profile,
                                     cmsSigDeviceModelDescTag,
                                     data);
        if (!ret) {
                g_set_error_literal (error,
                                     GSD_COLOR_MANAGER_ERROR,
                                     GSD_COLOR_MANAGER_ERROR_FAILED,
                                     "failed to write model");
                goto out;
        }

        /* write title */
        title = g_strdup_printf ("%s, %s",
                                 _("Default"),
                                 data);
        ret = _cmsWriteTagTextAscii (lcms_profile,
                                     cmsSigProfileDescriptionTag,
                                     title);
        if (!ret) {
                g_set_error_literal (error, GSD_COLOR_MANAGER_ERROR, GSD_COLOR_MANAGER_ERROR_FAILED, "failed to write description");
                goto out;
        }

        /* get manufacturer */
        data = gcm_edid_get_vendor_name (edid);
        if (data == NULL)
                data = gcm_dmi_get_vendor (priv->dmi);
        if (data == NULL)
                data = "Unknown vendor";
        ret = _cmsWriteTagTextAscii (lcms_profile,
                                     cmsSigDeviceMfgDescTag,
                                     data);
        if (!ret) {
                g_set_error_literal (error,
                                     GSD_COLOR_MANAGER_ERROR,
                                     GSD_COLOR_MANAGER_ERROR_FAILED,
                                     "failed to write manufacturer");
                goto out;
        }

#ifdef HAVE_NEW_LCMS
        /* just create a new dict */
        dict = cmsDictAlloc (NULL);

        /* set the framework creator metadata */
        _cmsDictAddEntryAscii (dict,
                               CD_PROFILE_METADATA_CMF_PRODUCT,
                               PACKAGE_NAME);
        _cmsDictAddEntryAscii (dict,
                               CD_PROFILE_METADATA_CMF_BINARY,
                               PACKAGE_NAME);
        _cmsDictAddEntryAscii (dict,
                               CD_PROFILE_METADATA_CMF_VERSION,
                               PACKAGE_VERSION);

        /* set the data source so we don't ever prompt the user to
         * recalibrate (as the EDID data won't have changed) */
        _cmsDictAddEntryAscii (dict,
                               CD_PROFILE_METADATA_DATA_SOURCE,
                               CD_PROFILE_METADATA_DATA_SOURCE_EDID);

        /* set 'ICC meta Tag for Monitor Profiles' data */
        _cmsDictAddEntryAscii (dict, "EDID_md5", gcm_edid_get_checksum (edid));
        data = gcm_edid_get_monitor_name (edid);
        if (data != NULL)
                _cmsDictAddEntryAscii (dict, "EDID_model", data);
        data = gcm_edid_get_serial_number (edid);
        if (data != NULL)
                _cmsDictAddEntryAscii (dict, "EDID_serial", data);
        data = gcm_edid_get_pnp_id (edid);
        if (data != NULL)
                _cmsDictAddEntryAscii (dict, "EDID_mnft", data);
        data = gcm_edid_get_vendor_name (edid);
        if (data != NULL)
                _cmsDictAddEntryAscii (dict, "EDID_manufacturer", data);
#endif /* HAVE_NEW_LCMS */

        /* write profile id */
        ret = cmsMD5computeID (lcms_profile);
        if (!ret) {
                g_set_error_literal (error,
                                     GSD_COLOR_MANAGER_ERROR,
                                     GSD_COLOR_MANAGER_ERROR_FAILED,
                                     "failed to write profile id");
                goto out;
        }

        /* save, TODO: get error */
        cmsSaveProfileToFile (lcms_profile, filename);
        ret = TRUE;
out:
        g_free (title);
#ifdef HAVE_NEW_LCMS
        if (dict != NULL)
                cmsDictFree (dict);
#endif
        if (*transfer_curve != NULL)
                cmsFreeToneCurve (*transfer_curve);
        return ret;
}

static GPtrArray *
gcm_session_generate_vcgt (CdProfile *profile, guint size)
{
        GnomeRROutputClutItem *tmp;
        GPtrArray *array = NULL;
        const cmsToneCurve **vcgt;
        cmsFloat32Number in;
        guint i;
        const gchar *filename;
        cmsHPROFILE lcms_profile = NULL;

        /* not an actual profile */
        filename = cd_profile_get_filename (profile);
        if (filename == NULL)
                goto out;

        /* open file */
        lcms_profile = cmsOpenProfileFromFile (filename, "r");
        if (lcms_profile == NULL)
                goto out;

        /* get tone curves from profile */
        vcgt = cmsReadTag (lcms_profile, cmsSigVcgtType);
        if (vcgt == NULL || vcgt[0] == NULL) {
                g_debug ("profile does not have any VCGT data");
                goto out;
        }

        /* create array */
        array = g_ptr_array_new_with_free_func (g_free);
        for (i = 0; i < size; i++) {
                in = (gdouble) i / (gdouble) (size - 1);
                tmp = g_new0 (GnomeRROutputClutItem, 1);
                tmp->red = cmsEvalToneCurveFloat(vcgt[0], in) * (gdouble) 0xffff;
                tmp->green = cmsEvalToneCurveFloat(vcgt[1], in) * (gdouble) 0xffff;
                tmp->blue = cmsEvalToneCurveFloat(vcgt[2], in) * (gdouble) 0xffff;
                g_ptr_array_add (array, tmp);
        }
out:
        if (lcms_profile != NULL)
                cmsCloseProfile (lcms_profile);
        return array;
}

static guint
gnome_rr_output_get_gamma_size (GnomeRROutput *output)
{
        GnomeRRCrtc *crtc;
        gint len = 0;

        crtc = gnome_rr_output_get_crtc (output);
        gnome_rr_crtc_get_gamma (crtc,
                                 &len,
                                 NULL, NULL, NULL);
        return (guint) len;
}

static gboolean
gcm_session_output_set_gamma (GnomeRROutput *output,
                              GPtrArray *array,
                              GError **error)
{
        gboolean ret = TRUE;
        guint16 *red = NULL;
        guint16 *green = NULL;
        guint16 *blue = NULL;
        guint i;
        GnomeRROutputClutItem *data;
        GnomeRRCrtc *crtc;

        /* no length? */
        if (array->len == 0) {
                ret = FALSE;
                g_set_error_literal (error,
                                     GSD_COLOR_MANAGER_ERROR,
                                     GSD_COLOR_MANAGER_ERROR_FAILED,
                                     "no data in the CLUT array");
                goto out;
        }

        /* convert to a type X understands */
        red = g_new (guint16, array->len);
        green = g_new (guint16, array->len);
        blue = g_new (guint16, array->len);
        for (i = 0; i < array->len; i++) {
                data = g_ptr_array_index (array, i);
                red[i] = data->red;
                green[i] = data->green;
                blue[i] = data->blue;
        }

        /* send to LUT */
        crtc = gnome_rr_output_get_crtc (output);
        gnome_rr_crtc_set_gamma (crtc, array->len,
                                 red, green, blue);
out:
        g_free (red);
        g_free (green);
        g_free (blue);
        return ret;
}

static gboolean
gcm_session_device_set_gamma (GnomeRROutput *output,
                              CdProfile *profile,
                              GError **error)
{
        gboolean ret;
        GPtrArray *clut = NULL;

        /* create a lookup table */
        clut = gcm_session_generate_vcgt (profile,
                                          gnome_rr_output_get_gamma_size (output));

        /* apply the vcgt to this output */
        ret = gcm_session_output_set_gamma (output, clut, error);
        if (!ret)
                goto out;
out:
        if (clut != NULL)
                g_ptr_array_unref (clut);
        return ret;
}

static gboolean
gcm_session_device_reset_gamma (GnomeRROutput *output,
                                GError **error)
{
        gboolean ret;
        guint i;
        guint size;
        guint32 value;
        GPtrArray *clut;
        GnomeRROutputClutItem *data;

        /* create a linear ramp */
        g_debug ("falling back to dummy ramp");
        clut = g_ptr_array_new_with_free_func (g_free);
        size = gnome_rr_output_get_gamma_size (output);
        for (i = 0; i < size; i++) {
                value = (i * 0xffff) / (size - 1);
                data = g_new0 (GnomeRROutputClutItem, 1);
                data->red = value;
                data->green = value;
                data->blue = value;
                g_ptr_array_add (clut, data);
        }

        /* apply the vcgt to this output */
        ret = gcm_session_output_set_gamma (output, clut, error);
        if (!ret)
                goto out;
out:
        g_ptr_array_unref (clut);
        return ret;
}

static GnomeRROutput *
gcm_session_get_x11_output_by_id (GsdColorManager *manager,
                                  const gchar *device_id,
                                  GError **error)
{
        gchar *output_id;
        GnomeRROutput *output = NULL;
        GnomeRROutput **outputs = NULL;
        guint i;
        GsdColorManagerPrivate *priv = manager->priv;

        /* search all X11 outputs for the device id */
        outputs = gnome_rr_screen_list_outputs (priv->x11_screen);
        if (outputs == NULL) {
                g_set_error_literal (error,
                                     GSD_COLOR_MANAGER_ERROR,
                                     GSD_COLOR_MANAGER_ERROR_FAILED,
                                     "Failed to get outputs");
                goto out;
        }
        for (i = 0; outputs[i] != NULL && output == NULL; i++) {
                if (!gnome_rr_output_is_connected (outputs[i]))
                        continue;
                output_id = gcm_session_get_output_id (manager, outputs[i]);
                if (g_strcmp0 (output_id, device_id) == 0)
                        output = outputs[i];
                g_free (output_id);
        }
        if (output == NULL) {
                g_set_error (error,
                             GSD_COLOR_MANAGER_ERROR,
                             GSD_COLOR_MANAGER_ERROR_FAILED,
                             "Failed to find output %s",
                             device_id);
        }
out:
        return output;
}

static void
gcm_session_device_assign_profile_connect_cb (GObject *object,
                                              GAsyncResult *res,
                                              gpointer user_data)
{
        CdProfile *profile = CD_PROFILE (object);
        const gchar *filename;
        gboolean ret;
        GError *error = NULL;
        GnomeRROutput *output;
        GcmSessionAsyncHelper *helper = (GcmSessionAsyncHelper *) user_data;
        GsdColorManager *manager = GSD_COLOR_MANAGER (helper->manager);

        /* get properties */
        ret = cd_profile_connect_finish (profile, res, &error);
        if (!ret) {
                g_warning ("failed to connect to profile: %s",
                           error->message);
                g_error_free (error);
                goto out;
        }

        /* get the filename */
        filename = cd_profile_get_filename (profile);
        g_assert (filename != NULL);

        /* get the output (can't save in helper as GnomeRROutput isn't
         * a GObject, just a pointer */
        output = gnome_rr_screen_get_output_by_id (manager->priv->x11_screen,
                                                   helper->output_id);
        if (output == NULL)
                goto out;

        /* set the _ICC_PROFILE atom */
        if (gnome_rr_output_get_is_primary (output)) {
                ret = gcm_session_screen_set_icc_profile (manager,
                                                          filename,
                                                          &error);
                if (!ret) {
                        g_warning ("failed to set screen _ICC_PROFILE: %s",
                                   error->message);
                        g_clear_error (&error);
                }
        }

        /* create a vcgt for this icc file */
        ret = cd_profile_get_has_vcgt (profile);
        if (ret) {
                ret = gcm_session_device_set_gamma (output,
                                                    profile,
                                                    &error);
                if (!ret) {
                        g_warning ("failed to set %s gamma tables: %s",
                                   cd_device_get_id (helper->device),
                                   error->message);
                        g_error_free (error);
                        goto out;
                }
        } else {
                ret = gcm_session_device_reset_gamma (output,
                                                      &error);
                if (!ret) {
                        g_warning ("failed to reset %s gamma tables: %s",
                                   cd_device_get_id (helper->device),
                                   error->message);
                        g_error_free (error);
                        goto out;
                }
        }
out:
        gcm_session_async_helper_free (helper);
}

static void
gcm_session_device_assign_connect_cb (GObject *object,
                                      GAsyncResult *res,
                                      gpointer user_data)
{
        CdDeviceKind kind;
        CdProfile *profile = NULL;
        gboolean ret;
        gchar *autogen_filename = NULL;
        gchar *autogen_path = NULL;
        GcmEdid *edid = NULL;
        GnomeRROutput *output = NULL;
        GError *error = NULL;
        const gchar *xrandr_id;
        GcmSessionAsyncHelper *helper;
        CdDevice *device = CD_DEVICE (object);
        GsdColorManager *manager = GSD_COLOR_MANAGER (user_data);
        GsdColorManagerPrivate *priv = manager->priv;

        /* get properties */
        ret = cd_device_connect_finish (device, res, &error);
        if (!ret) {
                g_warning ("failed to connect to device: %s",
                           error->message);
                g_error_free (error);
                goto out;
        }

        /* check we care */
        kind = cd_device_get_kind (device);
        if (kind != CD_DEVICE_KIND_DISPLAY)
                goto out;

        g_debug ("need to assign display device %s",
                 cd_device_get_id (device));

        /* get the GnomeRROutput for the device id */
        xrandr_id = cd_device_get_id (device);
        output = gcm_session_get_x11_output_by_id (manager,
                                                   xrandr_id,
                                                   &error);
        if (output == NULL) {
                g_warning ("no %s device found: %s",
                           cd_device_get_id (device),
                           error->message);
                g_error_free (error);
                goto out;
        }

        /* get the output EDID */
        edid = gcm_session_get_output_edid (manager, output, &error);
        if (edid == NULL) {
                g_warning ("unable to get EDID for %s: %s",
                           cd_device_get_id (device),
                           error->message);
                g_error_free (error);
                goto out;
        }

        /* create profile from device edid if it does not exist */
        autogen_filename = g_strdup_printf ("edid-%s.icc",
                                            gcm_edid_get_checksum (edid));
        autogen_path = g_build_filename (g_get_user_data_dir (),
                                         "icc", autogen_filename, NULL);

        if (g_file_test (autogen_path, G_FILE_TEST_EXISTS)) {
                g_debug ("auto-profile edid %s exists", autogen_path);
        } else {
                g_debug ("auto-profile edid does not exist, creating as %s",
                         autogen_path);
                ret = gcm_apply_create_icc_profile_for_edid (manager,
                                                             edid,
                                                             autogen_path,
                                                             &error);
                if (!ret) {
                        g_warning ("failed to create profile from EDID data: %s",
                                     error->message);
                        g_clear_error (&error);
                }
        }

        /* get the default profile for the device */
        profile = cd_device_get_default_profile (device);
        if (profile == NULL) {
                g_debug ("%s has no default profile to set",
                         cd_device_get_id (device));

                /* the default output? */
                if (gnome_rr_output_get_is_primary (output)) {
                        gdk_property_delete (priv->gdk_window,
                                             gdk_atom_intern_static_string ("_ICC_PROFILE"));
                        gdk_property_delete (priv->gdk_window,
                                             gdk_atom_intern_static_string ("_ICC_PROFILE_IN_X_VERSION"));
                }

                /* reset, as we want linear profiles for profiling */
                ret = gcm_session_device_reset_gamma (output,
                                                      &error);
                if (!ret) {
                        g_warning ("failed to reset %s gamma tables: %s",
                                   cd_device_get_id (device),
                                   error->message);
                        g_error_free (error);
                        goto out;
                }
                goto out;
        }

        /* get properties */
        helper = g_new0 (GcmSessionAsyncHelper, 1);
        helper->output_id = gnome_rr_output_get_id (output);
        helper->manager = g_object_ref (manager);
        helper->device = g_object_ref (device);
        cd_profile_connect (profile,
                            NULL,
                            gcm_session_device_assign_profile_connect_cb,
                            helper);
out:
        g_free (autogen_filename);
        g_free (autogen_path);
        if (edid != NULL)
                g_object_unref (edid);
        if (profile != NULL)
                g_object_unref (profile);
}

static void
gcm_session_device_assign (GsdColorManager *manager, CdDevice *device)
{
        cd_device_connect (device,
                           NULL,
                           gcm_session_device_assign_connect_cb,
                           manager);
}

static void
gcm_session_device_added_assign_cb (CdClient *client,
                                    CdDevice *device,
                                    GsdColorManager *manager)
{
        gcm_session_device_assign (manager, device);
}

static void
gcm_session_device_changed_assign_cb (CdClient *client,
                                      CdDevice *device,
                                      GsdColorManager *manager)
{
        g_debug ("%s changed", cd_device_get_object_path (device));
        gcm_session_device_assign (manager, device);
}

static void
gcm_session_create_device_cb (GObject *object,
                              GAsyncResult *res,
                              gpointer user_data)
{
        CdDevice *device;
        GError *error = NULL;

        device = cd_client_create_device_finish (CD_CLIENT (object),
                                                 res,
                                                 &error);
        if (device == NULL) {
                g_warning ("failed to create device: %s",
                           error->message);
                g_error_free (error);
                return;
        }
        g_object_unref (device);
}

static void
gcm_session_add_x11_output (GsdColorManager *manager, GnomeRROutput *output)
{
        const gchar *model;
        const gchar *serial;
        const gchar *vendor;
        gboolean ret;
        gchar *device_id = NULL;
        GcmEdid *edid;
        GError *error = NULL;
        GHashTable *device_props = NULL;
        GsdColorManagerPrivate *priv = manager->priv;

        /* get edid */
        edid = gcm_session_get_output_edid (manager, output, &error);
        if (edid == NULL) {
                g_warning ("failed to get edid: %s",
                           error->message);
                g_error_free (error);
                goto out;
        }

        /* is this an internal device? */
        ret = gnome_rr_output_is_laptop (output);
        if (ret) {
                model = gcm_dmi_get_name (priv->dmi);
                vendor = gcm_dmi_get_vendor (priv->dmi);
        } else {
                model = gcm_edid_get_monitor_name (edid);
                if (model == NULL)
                        model = gnome_rr_output_get_name (output);
                vendor = gcm_edid_get_vendor_name (edid);
        }

        /* get a serial number if one exists */
        serial = gcm_edid_get_serial_number (edid);
        if (serial == NULL)
                serial = "unknown";

        device_id = gcm_session_get_output_id (manager, output);
        g_debug ("output %s added", device_id);
        device_props = g_hash_table_new_full (g_str_hash, g_str_equal,
                                              NULL, NULL);
        g_hash_table_insert (device_props,
                             (gpointer) CD_DEVICE_PROPERTY_KIND,
                             (gpointer) cd_device_kind_to_string (CD_DEVICE_KIND_DISPLAY));
        g_hash_table_insert (device_props,
                             (gpointer) CD_DEVICE_PROPERTY_MODE,
                             (gpointer) cd_device_mode_to_string (CD_DEVICE_MODE_PHYSICAL));
        g_hash_table_insert (device_props,
                             (gpointer) CD_DEVICE_PROPERTY_COLORSPACE,
                             (gpointer) cd_colorspace_to_string (CD_COLORSPACE_RGB));
        g_hash_table_insert (device_props,
                             (gpointer) CD_DEVICE_PROPERTY_VENDOR,
                             (gpointer) vendor);
        g_hash_table_insert (device_props,
                             (gpointer) CD_DEVICE_PROPERTY_MODEL,
                             (gpointer) model);
        g_hash_table_insert (device_props,
                             (gpointer) CD_DEVICE_PROPERTY_SERIAL,
                             (gpointer) serial);
        g_hash_table_insert (device_props,
                             (gpointer) CD_DEVICE_METADATA_XRANDR_NAME,
                             (gpointer) gnome_rr_output_get_name (output));
        cd_client_create_device (priv->client,
                                 device_id,
                                 CD_OBJECT_SCOPE_TEMP,
                                 device_props,
                                 NULL,
                                 gcm_session_create_device_cb,
                                 manager);
out:
        g_free (device_id);
        if (device_props != NULL)
                g_hash_table_unref (device_props);
        if (edid != NULL)
                g_object_unref (edid);
}


static void
gnome_rr_screen_output_added_cb (GnomeRRScreen *screen,
                                GnomeRROutput *output,
                                GsdColorManager *manager)
{
        gcm_session_add_x11_output (manager, output);
}

static void
gcm_session_screen_removed_delete_device_cb (GObject *object, GAsyncResult *res, gpointer user_data)
{
        gboolean ret;
        GError *error = NULL;

        /* deleted device */
        ret = cd_client_delete_device_finish (CD_CLIENT (object),
                                              res,
                                              &error);
        if (!ret) {
                g_warning ("failed to delete device: %s",
                           error->message);
                g_error_free (error);
        }
}

static void
gcm_session_screen_removed_find_device_cb (GObject *object, GAsyncResult *res, gpointer user_data)
{
        GError *error = NULL;
        CdDevice *device;
        GsdColorManager *manager = GSD_COLOR_MANAGER (user_data);

        device = cd_client_find_device_finish (manager->priv->client,
                                               res,
                                               &error);
        if (device == NULL) {
                g_warning ("failed to find device: %s",
                           error->message);
                g_error_free (error);
                return;
        }
        g_debug ("output %s found, and will be removed",
                 cd_device_get_object_path (device));
        cd_client_delete_device (manager->priv->client,
                                 device,
                                 NULL,
                                 gcm_session_screen_removed_delete_device_cb,
                                 manager);
        g_object_unref (device);
}

static void
gnome_rr_screen_output_removed_cb (GnomeRRScreen *screen,
                                   GnomeRROutput *output,
                                   GsdColorManager *manager)
{
        g_debug ("output %s removed",
                 gnome_rr_output_get_name (output));
        g_hash_table_remove (manager->priv->edid_cache,
                             gnome_rr_output_get_name (output));
        cd_client_find_device_by_property (manager->priv->client,
                                           CD_DEVICE_METADATA_XRANDR_NAME,
                                           gnome_rr_output_get_name (output),
                                           NULL,
                                           gcm_session_screen_removed_find_device_cb,
                                           manager);
}

static void
gcm_session_get_devices_cb (GObject *object, GAsyncResult *res, gpointer user_data)
{
        CdDevice *device;
        GError *error = NULL;
        GPtrArray *array;
        guint i;
        GsdColorManager *manager = GSD_COLOR_MANAGER (user_data);

        array = cd_client_get_devices_finish (CD_CLIENT (object), res, &error);
        if (array == NULL) {
                g_warning ("failed to get devices: %s",
                           error->message);
                g_error_free (error);
                goto out;
        }
        for (i = 0; i < array->len; i++) {
                device = g_ptr_array_index (array, i);
                gcm_session_device_assign (manager, device);
        }
out:
        if (array != NULL)
                g_ptr_array_unref (array);
}

static void
gcm_session_client_connect_cb (GObject *source_object,
                               GAsyncResult *res,
                               gpointer user_data)
{
        gboolean ret;
        GError *error = NULL;
        GnomeRROutput **outputs;
        guint i;
        GsdColorManager *manager = GSD_COLOR_MANAGER (user_data);
        GsdColorManagerPrivate *priv = manager->priv;

        /* connected */
        g_debug ("connected to colord");
        ret = cd_client_connect_finish (manager->priv->client, res, &error);
        if (!ret) {
                g_warning ("failed to connect to colord: %s", error->message);
                g_error_free (error);
                return;
        }

        /* add profiles */
        gcm_profile_store_search (priv->profile_store);

        /* add screens */
        gnome_rr_screen_refresh (priv->x11_screen, &error);
        if (error != NULL) {
                g_warning ("failed to refresh: %s", error->message);
                g_error_free (error);
                goto out;
        }

        /* get X11 outputs */
        outputs = gnome_rr_screen_list_outputs (priv->x11_screen);
        if (outputs == NULL) {
                g_warning ("failed to get outputs");
                goto out;
        }
        for (i = 0; outputs[i] != NULL; i++) {
                if (gnome_rr_output_is_connected (outputs[i]))
                        gcm_session_add_x11_output (manager, outputs[i]);
        }

        /* only connect when colord is awake */
        g_signal_connect (priv->x11_screen, "output-connected",
                          G_CALLBACK (gnome_rr_screen_output_added_cb),
                          manager);
        g_signal_connect (priv->x11_screen, "output-disconnected",
                          G_CALLBACK (gnome_rr_screen_output_removed_cb),
                          manager);

        g_signal_connect (priv->client, "profile-added",
                          G_CALLBACK (gcm_session_profile_added_assign_cb),
                          manager);
        g_signal_connect (priv->client, "device-added",
                          G_CALLBACK (gcm_session_device_added_assign_cb),
                          manager);
        g_signal_connect (priv->client, "device-changed",
                          G_CALLBACK (gcm_session_device_changed_assign_cb),
                          manager);

        /* set for each device that already exist */
        cd_client_get_devices (priv->client, NULL,
                               gcm_session_get_devices_cb,
                               manager);
out:
        return;
}

gboolean
gsd_color_manager_start (GsdColorManager *manager,
                         GError          **error)
{
        GsdColorManagerPrivate *priv = manager->priv;
        gboolean ret = FALSE;

        g_debug ("Starting color manager");
        gnome_settings_profile_start (NULL);

        /* coldplug the list of screens */
        priv->x11_screen = gnome_rr_screen_new (gdk_screen_get_default (), error);
        if (priv->x11_screen == NULL)
                goto out;

        cd_client_connect (priv->client,
                           NULL,
                           gcm_session_client_connect_cb,
                           manager);

        /* success */
        ret = TRUE;
out:
        gnome_settings_profile_end (NULL);
        return ret;
}

void
gsd_color_manager_stop (GsdColorManager *manager)
{
        g_debug ("Stopping color manager");
}

static void
gcm_session_exec_control_center (GsdColorManager *manager)
{
        gboolean ret;
        GError *error = NULL;
        GAppInfo *app_info;
        GdkAppLaunchContext *launch_context;

        /* setup the launch context so the startup notification is correct */
        launch_context = gdk_display_get_app_launch_context (gdk_display_get_default ());
        app_info = g_app_info_create_from_commandline (BINDIR "/gnome-control-center color",
                                                       "gnome-control-center",
                                                       G_APP_INFO_CREATE_SUPPORTS_STARTUP_NOTIFICATION,
                                                       &error);
        if (app_info == NULL) {
                g_warning ("failed to create application info: %s",
                           error->message);
                g_error_free (error);
                goto out;
        }

        /* launch gnome-control-center */
        ret = g_app_info_launch (app_info,
                                 NULL,
                                 G_APP_LAUNCH_CONTEXT (launch_context),
                                 &error);
        if (!ret) {
                g_warning ("failed to launch gnome-control-center: %s",
                           error->message);
                g_error_free (error);
                goto out;
        }
out:
        g_object_unref (launch_context);
        if (app_info != NULL)
                g_object_unref (app_info);
}

static void
gcm_session_notify_cb (NotifyNotification *notification,
                       gchar *action,
                       gpointer user_data)
{
        GsdColorManager *manager = GSD_COLOR_MANAGER (user_data);
        GsdColorManagerPrivate *priv = manager->priv;

        if (g_strcmp0 (action, "display") == 0) {
                g_settings_set_uint (priv->settings,
                                     GCM_SETTINGS_RECALIBRATE_DISPLAY_THRESHOLD,
                                     0);
        } else if (g_strcmp0 (action, "printer") == 0) {
                g_settings_set_uint (priv->settings,
                                     GCM_SETTINGS_RECALIBRATE_PRINTER_THRESHOLD,
                                     0);
        } else if (g_strcmp0 (action, "recalibrate") == 0) {
                gcm_session_exec_control_center (manager);
        }
}

static gboolean
gcm_session_notify_recalibrate (GsdColorManager *manager,
                                const gchar *title,
                                const gchar *message,
                                CdDeviceKind kind)
{
        gboolean ret;
        GError *error = NULL;
        NotifyNotification *notification;
        GsdColorManagerPrivate *priv = manager->priv;

        /* show a bubble */
        notification = notify_notification_new (title, message, "preferences-color");
        notify_notification_set_timeout (notification, GCM_SESSION_NOTIFY_TIMEOUT);
        notify_notification_set_urgency (notification, NOTIFY_URGENCY_LOW);

        /* TRANSLATORS: button: this is to open GCM */
        notify_notification_add_action (notification,
                                        "recalibrate",
                                        _("Recalibrate now"),
                                        gcm_session_notify_cb,
                                        priv, NULL);

        /* TRANSLATORS: button: this is to ignore the recalibrate notifications */
        notify_notification_add_action (notification,
                                        cd_device_kind_to_string (kind),
                                        _("Ignore"),
                                        gcm_session_notify_cb,
                                        priv, NULL);

        ret = notify_notification_show (notification, &error);
        if (!ret) {
                g_warning ("failed to show notification: %s",
                           error->message);
                g_error_free (error);
        }
        return ret;
}

static gchar *
gcm_session_device_get_title (CdDevice *device)
{
        const gchar *vendor;
        const gchar *model;

        model = cd_device_get_model (device);
        vendor = cd_device_get_vendor (device);
        if (model != NULL && vendor != NULL)
                return g_strdup_printf ("%s - %s", vendor, model);
        if (vendor != NULL)
                return g_strdup (vendor);
        if (model != NULL)
                return g_strdup (model);
        return g_strdup (cd_device_get_id (device));
}

static void
gcm_session_notify_device (GsdColorManager *manager, CdDevice *device)
{
        CdDeviceKind kind;
        const gchar *title;
        gchar *device_title = NULL;
        gchar *message;
        gint threshold;
        glong since;
        GsdColorManagerPrivate *priv = manager->priv;

        /* TRANSLATORS: this is when the device has not been recalibrated in a while */
        title = _("Recalibration required");
        device_title = gcm_session_device_get_title (device);

        /* check we care */
        kind = cd_device_get_kind (device);
        if (kind == CD_DEVICE_KIND_DISPLAY) {

                /* get from GSettings */
                threshold = g_settings_get_int (priv->settings,
                                                GCM_SETTINGS_RECALIBRATE_DISPLAY_THRESHOLD);

                /* TRANSLATORS: this is when the display has not been recalibrated in a while */
                message = g_strdup_printf (_("The display '%s' should be recalibrated soon."),
                                           device_title);
        } else {

                /* get from GSettings */
                threshold = g_settings_get_int (priv->settings,
                                                GCM_SETTINGS_RECALIBRATE_PRINTER_THRESHOLD);

                /* TRANSLATORS: this is when the printer has not been recalibrated in a while */
                message = g_strdup_printf (_("The printer '%s' should be recalibrated soon."),
                                           device_title);
        }

        /* check if we need to notify */
        since = (g_get_real_time () / G_USEC_PER_SEC) - cd_device_get_modified (device);
        if (threshold > since)
                gcm_session_notify_recalibrate (manager, title, message, kind);
        g_free (device_title);
        g_free (message);
}

static void
gcm_session_profile_connect_cb (GObject *object,
                                GAsyncResult *res,
                                gpointer user_data)
{
        const gchar *filename;
        gboolean allow_notifications;
        gboolean ret;
        gchar *basename = NULL;
        const gchar *data_source;
        GError *error = NULL;
        CdProfile *profile = CD_PROFILE (object);
        GcmSessionAsyncHelper *helper = (GcmSessionAsyncHelper *) user_data;
        GsdColorManager *manager = GSD_COLOR_MANAGER (helper->manager);

        ret = cd_profile_connect_finish (profile,
                                         res,
                                         &error);
        if (!ret) {
                g_warning ("failed to connect to profile: %s",
                           error->message);
                g_error_free (error);
                goto out;
        }

        /* ensure it's a profile generated by us */
        data_source = cd_profile_get_metadata_item (profile,
                                                    CD_PROFILE_METADATA_DATA_SOURCE);
        if (data_source == NULL) {

                /* existing profiles from gnome-color-manager < 3.1
                 * won't have the extra metadata values added */
                filename = cd_profile_get_filename (profile);
                if (filename == NULL)
                        goto out;
                basename = g_path_get_basename (filename);
                if (!g_str_has_prefix (basename, "GCM")) {
                        g_debug ("not a GCM profile for %s: %s",
                                 cd_device_get_id (helper->device), filename);
                        goto out;
                }

        /* ensure it's been created from a calibration, rather than from
         * auto-EDID */
        } else if (g_strcmp0 (data_source,
                   CD_PROFILE_METADATA_DATA_SOURCE_CALIB) != 0) {
                g_debug ("not a calib profile for %s",
                         cd_device_get_id (helper->device));
                goto out;
        }

        /* do we allow notifications */
        allow_notifications = g_settings_get_boolean (manager->priv->settings,
                                                      GCM_SETTINGS_SHOW_NOTIFICATIONS);
        if (!allow_notifications)
                goto out;

        /* handle device */
        gcm_session_notify_device (manager, helper->device);
out:
        gcm_session_async_helper_free (helper);
        g_free (basename);
}

static void
gcm_session_device_connect_cb (GObject *object,
                               GAsyncResult *res,
                               gpointer user_data)
{
        gboolean ret;
        GError *error = NULL;
        CdDeviceKind kind;
        CdProfile *profile = NULL;
        CdDevice *device = CD_DEVICE (object);
        GsdColorManager *manager = GSD_COLOR_MANAGER (user_data);
        GcmSessionAsyncHelper *helper;

        ret = cd_device_connect_finish (device,
                                        res,
                                        &error);
        if (!ret) {
                g_warning ("failed to connect to device: %s",
                           error->message);
                g_error_free (error);
                goto out;
        }

        /* check we care */
        kind = cd_device_get_kind (device);
        if (kind != CD_DEVICE_KIND_DISPLAY &&
            kind != CD_DEVICE_KIND_PRINTER)
                goto out;

        /* ensure we have a profile */
        profile = cd_device_get_default_profile (device);
        if (profile == NULL) {
                g_debug ("no profile set for %s", cd_device_get_id (device));
                goto out;
        }

        /* connect to the profile */
        helper = g_new0 (GcmSessionAsyncHelper, 1);
        helper->manager = g_object_ref (manager);
        helper->device = g_object_ref (device);
        cd_profile_connect (profile,
                            NULL,
                            gcm_session_profile_connect_cb,
                            helper);
out:
        if (profile != NULL)
                g_object_unref (profile);
}

static void
gcm_session_device_added_notify_cb (CdClient *client,
                                    CdDevice *device,
                                    GsdColorManager *manager)
{
        /* connect to the device to get properties */
        cd_device_connect (device,
                           NULL,
                           gcm_session_device_connect_cb,
                           manager);
}

#ifdef HAVE_LCMS
static gchar *
gcm_session_get_precooked_md5 (cmsHPROFILE lcms_profile)
{
        cmsUInt8Number profile_id[16];
        gboolean md5_precooked = FALSE;
        guint i;
        gchar *md5 = NULL;

        /* check to see if we have a pre-cooked MD5 */
        cmsGetHeaderProfileID (lcms_profile, profile_id);
        for (i = 0; i < 16; i++) {
                if (profile_id[i] != 0) {
                        md5_precooked = TRUE;
                        break;
                }
        }
        if (!md5_precooked)
                goto out;

        /* convert to a hex string */
        md5 = g_new0 (gchar, 32 + 1);
        for (i = 0; i < 16; i++)
                g_snprintf (md5 + i*2, 3, "%02x", profile_id[i]);
out:
        return md5;
}
#endif

static gchar *
gcm_session_get_md5_for_filename (const gchar *filename,
                                  GError **error)
{
        gboolean ret;
        gchar *checksum = NULL;
        gchar *data = NULL;
        gsize length;

#ifdef HAVE_LCMS
        cmsHPROFILE lcms_profile = NULL;

        /* get the internal profile id, if it exists */
        lcms_profile = cmsOpenProfileFromFile (filename, "r");
        if (lcms_profile == NULL) {
                g_set_error_literal (error,
                                     GSD_COLOR_MANAGER_ERROR,
                                     GSD_COLOR_MANAGER_ERROR_FAILED,
                                     "failed to load: not an ICC profile");
                goto out;
        }
        checksum = gcm_session_get_precooked_md5 (lcms_profile);
        if (checksum != NULL)
                goto out;
#endif

        /* generate checksum */
        ret = g_file_get_contents (filename, &data, &length, error);
        if (!ret)
                goto out;
        checksum = g_compute_checksum_for_data (G_CHECKSUM_MD5,
                                                (const guchar *) data,
                                                length);
out:
        g_free (data);
#ifdef HAVE_LCMS
        if (lcms_profile != NULL)
                cmsCloseProfile (lcms_profile);
#endif
        return checksum;
}

static void
gcm_session_create_profile_cb (GObject *object,
                               GAsyncResult *res,
                               gpointer user_data)
{
        CdProfile *profile;
        GError *error = NULL;
        CdClient *client = CD_CLIENT (object);

        profile = cd_client_create_profile_finish (client, res, &error);
        if (profile == NULL) {
                g_warning ("%s", error->message);
                g_error_free (error);
                return;
        }
        g_object_unref (profile);
}

static void
gcm_session_profile_store_added_cb (GcmProfileStore *profile_store,
                                    const gchar *filename,
                                    GsdColorManager *manager)
{
        CdProfile *profile = NULL;
        gchar *checksum = NULL;
        gchar *profile_id = NULL;
        GError *error = NULL;
        GHashTable *profile_props = NULL;
        GsdColorManagerPrivate *priv = manager->priv;

        g_debug ("profile %s added", filename);

        /* generate ID */
        checksum = gcm_session_get_md5_for_filename (filename, &error);
        if (checksum == NULL) {
                g_warning ("failed to get profile checksum: %s",
                           error->message);
                g_error_free (error);
                goto out;
        }
        profile_id = g_strdup_printf ("icc-%s", checksum);
        profile_props = g_hash_table_new_full (g_str_hash, g_str_equal,
                                               NULL, NULL);
        g_hash_table_insert (profile_props,
                             CD_PROFILE_PROPERTY_FILENAME,
                             (gpointer) filename);
        g_hash_table_insert (profile_props,
                             CD_PROFILE_METADATA_FILE_CHECKSUM,
                             (gpointer) checksum);
        cd_client_create_profile (priv->client,
                                  profile_id,
                                  CD_OBJECT_SCOPE_TEMP,
                                  profile_props,
                                  NULL,
                                  gcm_session_create_profile_cb,
                                  manager);
out:
        g_free (checksum);
        g_free (profile_id);
        if (profile_props != NULL)
                g_hash_table_unref (profile_props);
        if (profile != NULL)
                g_object_unref (profile);
}

static void
gcm_session_delete_profile_cb (GObject *object,
                               GAsyncResult *res,
                               gpointer user_data)
{
        gboolean ret;
        GError *error = NULL;
        CdClient *client = CD_CLIENT (object);

        ret = cd_client_delete_profile_finish (client, res, &error);
        if (!ret) {
                g_warning ("%s", error->message);
                g_error_free (error);
        }
}

static void
gcm_session_find_profile_by_filename_cb (GObject *object,
                                         GAsyncResult *res,
                                         gpointer user_data)
{
        GError *error = NULL;
        CdProfile *profile;
        CdClient *client = CD_CLIENT (object);
        GsdColorManager *manager = GSD_COLOR_MANAGER (user_data);

        profile = cd_client_find_profile_by_filename_finish (client, res, &error);
        if (profile == NULL) {
                g_warning ("%s", error->message);
                g_error_free (error);
                goto out;
        }

        /* remove it from colord */
        g_debug ("profile %s removed", cd_profile_get_id (profile));
        cd_client_delete_profile (manager->priv->client,
                                  profile,
                                  NULL,
                                  gcm_session_delete_profile_cb,
                                  manager);
out:
        if (profile != NULL)
                g_object_unref (profile);
}

static void
gcm_session_profile_store_removed_cb (GcmProfileStore *profile_store,
                                      const gchar *filename,
                                      GsdColorManager *manager)
{
        /* find the ID for the filename */
        g_debug ("filename %s removed", filename);
        cd_client_find_profile_by_filename (manager->priv->client,
                                            filename,
                                            NULL,
                                            gcm_session_find_profile_by_filename_cb,
                                            manager);
}

static void
gcm_session_sensor_added_cb (CdClient *client,
                             CdSensor *sensor,
                             GsdColorManager *manager)
{
#ifdef HAVE_LIBCANBERRA
        ca_context_play (ca_gtk_context_get (), 0,
                         CA_PROP_EVENT_ID, "device-added",
                         /* TRANSLATORS: this is the application name */
                         CA_PROP_APPLICATION_NAME, _("GNOME Settings Daemon Color Plugin"),
                        /* TRANSLATORS: this is a sound description */
                         CA_PROP_EVENT_DESCRIPTION, _("Color calibration device added"), NULL);
#endif

        /* open up the color prefs window */
        gcm_session_exec_control_center (manager);
}

static void
gcm_session_sensor_removed_cb (CdClient *client,
                               CdSensor *sensor,
                               GsdColorManager *manager)
{
#ifdef HAVE_LIBCANBERRA
        ca_context_play (ca_gtk_context_get (), 0,
                         CA_PROP_EVENT_ID, "device-removed",
                         /* TRANSLATORS: this is the application name */
                         CA_PROP_APPLICATION_NAME, _("GNOME Settings Daemon Color Plugin"),
                        /* TRANSLATORS: this is a sound description */
                         CA_PROP_EVENT_DESCRIPTION, _("Color calibration device removed"), NULL);
#endif
}

static void
gsd_color_manager_set_property (GObject        *object,
                                guint           prop_id,
                                const GValue   *value,
                                GParamSpec     *pspec)
{
        switch (prop_id) {
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static void
gsd_color_manager_get_property (GObject        *object,
                               guint           prop_id,
                               GValue         *value,
                               GParamSpec     *pspec)
{
        switch (prop_id) {
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static void
gsd_color_manager_dispose (GObject *object)
{
        G_OBJECT_CLASS (gsd_color_manager_parent_class)->dispose (object);
}

static void
gsd_color_manager_class_init (GsdColorManagerClass *klass)
{
        GObjectClass   *object_class = G_OBJECT_CLASS (klass);

        object_class->get_property = gsd_color_manager_get_property;
        object_class->set_property = gsd_color_manager_set_property;
        object_class->dispose = gsd_color_manager_dispose;
        object_class->finalize = gsd_color_manager_finalize;

        g_type_class_add_private (klass, sizeof (GsdColorManagerPrivate));
}

static void
gsd_color_manager_init (GsdColorManager *manager)
{
        GsdColorManagerPrivate *priv;
        priv = manager->priv = GSD_COLOR_MANAGER_GET_PRIVATE (manager);

        /* set the _ICC_PROFILE atoms on the root screen */
        priv->gdk_window = gdk_screen_get_root_window (gdk_screen_get_default ());

        /* parsing the EDID is expensive */
        priv->edid_cache = g_hash_table_new_full (g_str_hash,
                                                  g_str_equal,
                                                  g_free,
                                                  g_object_unref);

        /* use DMI data for internal panels */
        priv->dmi = gcm_dmi_new ();

        priv->settings = g_settings_new ("org.gnome.settings-daemon.plugins.color");
        priv->client = cd_client_new ();
        g_signal_connect (priv->client, "device-added",
                          G_CALLBACK (gcm_session_device_added_notify_cb),
                          manager);
        g_signal_connect (priv->client, "sensor-added",
                          G_CALLBACK (gcm_session_sensor_added_cb),
                          manager);
        g_signal_connect (priv->client, "sensor-removed",
                          G_CALLBACK (gcm_session_sensor_removed_cb),
                          manager);

        /* have access to all user profiles */
        priv->profile_store = gcm_profile_store_new ();
        g_signal_connect (priv->profile_store, "added",
                          G_CALLBACK (gcm_session_profile_store_added_cb),
                          manager);
        g_signal_connect (priv->profile_store, "removed",
                          G_CALLBACK (gcm_session_profile_store_removed_cb),
                          manager);
}

static void
gsd_color_manager_finalize (GObject *object)
{
        GsdColorManager *manager;

        g_return_if_fail (object != NULL);
        g_return_if_fail (GSD_IS_COLOR_MANAGER (object));

        manager = GSD_COLOR_MANAGER (object);

        g_return_if_fail (manager->priv != NULL);

        g_object_unref (manager->priv->settings);
        g_object_unref (manager->priv->client);
        g_object_unref (manager->priv->profile_store);
        g_object_unref (manager->priv->dmi);
        g_hash_table_destroy (manager->priv->edid_cache);
        if (manager->priv->x11_screen != NULL);
                g_object_unref (manager->priv->x11_screen);

        G_OBJECT_CLASS (gsd_color_manager_parent_class)->finalize (object);
}

GsdColorManager *
gsd_color_manager_new (void)
{
        if (manager_object != NULL) {
                g_object_ref (manager_object);
        } else {
                manager_object = g_object_new (GSD_TYPE_COLOR_MANAGER, NULL);
                g_object_add_weak_pointer (manager_object,
                                           (gpointer *) &manager_object);
        }

        return GSD_COLOR_MANAGER (manager_object);
}
