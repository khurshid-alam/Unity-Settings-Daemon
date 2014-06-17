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

#include "fcitx.h"
#include "fcitx-keyboard.h"
#include "fcitx-inputmethod.h"
/* #include <fcitx/module/dbus/fcitx-dbus.h> */
#include <fcitx/module/dbus/dbusstuff.h>
#include <fcitx/module/xkb/fcitx-xkb.h>
#include <fcitx/module/ipc/ipc.h>

static guint watch_id;
static FcitxKeyboard *keyboard_proxy;
static FcitxInputMethod *input_method_proxy;
static gchar **input_method_ids;
static GHashTable *input_methods;

typedef struct
{
  gchar *id;
  gchar *name;
  gchar *layout;
  gchar *variant;
}
InputMethod;

static InputMethod *
input_method_new (const gchar *id,
                  const gchar *name,
                  const gchar *layout,
                  const gchar *variant)
{
  InputMethod *input_method = g_new (InputMethod, 1);

  input_method->id = g_strdup (id);
  input_method->name = g_strdup (name);
  input_method->layout = g_strdup (layout);
  input_method->variant = g_strdup (variant);

  return input_method;
}

static void
input_method_free (gpointer data)
{
  if (data != NULL)
    {
      InputMethod *input_method = data;

      g_free (input_method->variant);
      g_free (input_method->layout);
      g_free (input_method->name);
      g_free (input_method->id);
      g_free (input_method);
    }
}

static void
update_input_methods (void)
{
  if (input_methods != NULL)
    {
      g_hash_table_unref (input_methods);
      input_methods = NULL;
    }

  g_strfreev (input_method_ids);
  input_method_ids = NULL;

  if (input_method_proxy != NULL)
    {
      GVariant *variant = fcitx_input_method_get_imlist (input_method_proxy);
      GVariantIter iter;
      const gchar *id;
      const gchar *name;
      guint i;

      input_method_ids = g_new (gchar *, g_variant_n_children (variant) + 1);
      input_methods = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, input_method_free);

      g_variant_iter_init (&iter, variant);
      for (i = 0; g_variant_iter_next (&iter, "(&s&s&sb)", &name, &id, NULL, NULL); i++)
        {
          input_method_ids[i] = g_strdup (id);
          g_hash_table_insert (input_methods, g_strdup (id), input_method_new (id, name, NULL, NULL));
        }

      input_method_ids[i] = NULL;
    }
}

static void
keyboard_proxy_ready_cb (GObject      *source_object,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  GTask *task = user_data;
  GError *error = NULL;

  keyboard_proxy = fcitx_keyboard_proxy_new_for_bus_finish (result, &error);

  if (!g_task_had_error (task))
    {
      if (error != NULL)
        g_task_return_error (task, error);
      else if (input_method_proxy != NULL)
        g_task_return_boolean (task, input_methods != NULL);
    }

  g_object_unref (task);
}

static void
input_method_proxy_ready_cb (GObject      *source_object,
                             GAsyncResult *result,
                             gpointer      user_data)
{
  GTask *task = user_data;
  GError *error = NULL;

  input_method_proxy = fcitx_input_method_proxy_new_for_bus_finish (result, &error);

  update_input_methods ();

  if (!g_task_had_error (task))
    {
      if (error != NULL)
        g_task_return_error (task, error);
      else if (keyboard_proxy != NULL)
        g_task_return_boolean (task, input_methods != NULL);
    }

  g_object_unref (task);
}

static void
name_appeared_cb (GDBusConnection *connection,
                  const gchar     *name,
                  const gchar     *name_owner,
                  gpointer         user_data)
{
  if (keyboard_proxy == NULL)
    fcitx_keyboard_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                      G_DBUS_PROXY_FLAGS_NONE,
                                      name,
                                      FCITX_XKB_PATH,
                                      NULL,
                                      keyboard_proxy_ready_cb,
                                      g_object_ref (user_data));

  if (input_method_proxy == NULL)
    fcitx_input_method_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                          G_DBUS_PROXY_FLAGS_NONE,
                                          name,
                                          FCITX_IM_DBUS_PATH,
                                          NULL,
                                          input_method_proxy_ready_cb,
                                          g_object_ref (user_data));
}

void
fcitx_start_async (GCancellable        *cancellable,
                   GAsyncReadyCallback  callback,
                   gpointer             user_data)
{
  GTask *task = g_task_new (NULL, cancellable, callback, user_data);

  if (watch_id)
    {
      g_task_return_boolean (task, input_methods != NULL);
      g_object_unref (task);
    }
  else
    watch_id = g_bus_watch_name (G_BUS_TYPE_SESSION,
                                 FCITX_DBUS_SERVICE,
                                 G_BUS_NAME_WATCHER_FLAGS_AUTO_START,
                                 name_appeared_cb,
                                 NULL,
                                 task,
                                 g_object_unref);
}

gboolean
fcitx_start_finish (GAsyncResult  *result,
                    GError       **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}

void
fcitx_stop (void)
{
  if (watch_id)
    {
      g_bus_unwatch_name (watch_id);
      watch_id = 0;
    }

  if (input_method_proxy != NULL)
    g_clear_object (&input_method_proxy);

  if (keyboard_proxy != NULL)
    g_clear_object (&keyboard_proxy);

  update_input_methods ();
}

gchar **
fcitx_get_input_method_ids (void)
{
  return g_strdupv (input_method_ids);
}

const gchar *
fcitx_get_input_method_name (const gchar *id)
{
  if (input_methods != NULL)
    {
      InputMethod *input_method = g_hash_table_lookup (input_methods, id);

      if (input_method != NULL)
        return input_method->name;
    }

  return NULL;
}

const gchar *
fcitx_get_input_method_layout (const gchar *id)
{
  if (input_methods != NULL)
    {
      InputMethod *input_method = g_hash_table_lookup (input_methods, id);

      if (input_method != NULL)
        {
          if (input_method->layout == NULL && keyboard_proxy != NULL)
            fcitx_keyboard_call_get_layout_for_im_sync (keyboard_proxy,
                                                        id,
                                                        &input_method->layout,
                                                        &input_method->variant,
                                                        NULL,
                                                        NULL);

          return input_method->layout != NULL && input_method->layout[0] != '\0' ? input_method->layout : NULL;
        }
    }

  return NULL;
}

const gchar *
fcitx_get_input_method_variant (const gchar *id)
{
  if (input_methods != NULL)
    {
      InputMethod *input_method = g_hash_table_lookup (input_methods, id);

      if (input_method != NULL)
        {
          if (input_method->layout == NULL && keyboard_proxy != NULL)
            fcitx_keyboard_call_get_layout_for_im_sync (keyboard_proxy,
                                                        id,
                                                        &input_method->layout,
                                                        &input_method->variant,
                                                        NULL,
                                                        NULL);

          return input_method->variant != NULL && input_method->variant[0] != '\0' ? input_method->variant : NULL;
        }
    }

  return NULL;
}

void
fcitx_configure_input_method (const gchar *id)
{
  if (input_method_proxy != NULL)
    fcitx_input_method_call_configure_im (input_method_proxy, id, NULL, NULL, NULL);
}

void
fcitx_activate_input_method (const gchar *id)
{
  if (input_method_proxy != NULL)
    {
      GVariant *list = fcitx_input_method_dup_imlist (input_method_proxy);
      GVariantIter iter;
      const gchar *display_name;
      const gchar *name;
      const gchar *language_code;
      gboolean enabled;

      if (list != NULL)
        {
          g_variant_iter_init (&iter, list);
          while (g_variant_iter_next (&iter, "(&s&s&sb)", NULL, &name, NULL, &enabled))
            {
              if (g_str_equal (name, id))
                {
                  if (!enabled)
                    {
                      GVariantBuilder builder;

                      g_variant_builder_init (&builder, g_variant_get_type (list));

                      g_variant_iter_init (&iter, list);
                      while (g_variant_iter_next (&iter, "(&s&s&sb)", &display_name, &name, &language_code, &enabled))
                        g_variant_builder_add (&builder, "(sssb)", display_name, name, language_code, enabled || g_str_equal (name, id));

                      g_variant_unref (list);
                      list = g_variant_ref_sink (g_variant_builder_end (&builder));
                      fcitx_input_method_set_imlist (input_method_proxy, list);
                    }

                  break;
                }
            }
        }

      g_variant_unref (list);

      fcitx_input_method_call_activate_im (input_method_proxy, NULL, NULL, NULL);

      if (id != NULL)
        fcitx_input_method_call_set_current_im (input_method_proxy, id, NULL, NULL, NULL);
    }
}

void
fcitx_deactivate_input_method (void)
{
  if (input_method_proxy != NULL)
    fcitx_input_method_call_inactivate_im (input_method_proxy, NULL, NULL, NULL);
}
