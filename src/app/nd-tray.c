/* nd-tray.c
 *
 * Copyright 2024 GNOME Network Displays contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "nd-tray.h"
#include <gio/gio.h>

/* StatusNotifierItem D-Bus interface */
static const gchar sni_introspection_xml[] =
  "<node>"
  "  <interface name='org.kde.StatusNotifierItem'>"
  "    <method name='Activate'>"
  "      <arg type='i' name='x' direction='in'/>"
  "      <arg type='i' name='y' direction='in'/>"
  "    </method>"
  "    <method name='SecondaryActivate'>"
  "      <arg type='i' name='x' direction='in'/>"
  "      <arg type='i' name='y' direction='in'/>"
  "    </method>"
  "    <method name='ContextMenu'>"
  "      <arg type='i' name='x' direction='in'/>"
  "      <arg type='i' name='y' direction='in'/>"
  "    </method>"
  "    <signal name='NewStatus'>"
  "      <arg type='s' name='status'/>"
  "    </signal>"
  "    <property name='Category' type='s' access='read'/>"
  "    <property name='Id' type='s' access='read'/>"
  "    <property name='Title' type='s' access='read'/>"
  "    <property name='Status' type='s' access='read'/>"
  "    <property name='IconName' type='s' access='read'/>"
  "    <property name='Menu' type='o' access='read'/>"
  "    <property name='ItemIsMenu' type='b' access='read'/>"
  "  </interface>"
  "</node>";

/* com.canonical.dbusmenu D-Bus interface */
static const gchar dbusmenu_introspection_xml[] =
  "<node>"
  "  <interface name='com.canonical.dbusmenu'>"
  "    <method name='GetLayout'>"
  "      <arg type='i' name='parentId' direction='in'/>"
  "      <arg type='i' name='recursionDepth' direction='in'/>"
  "      <arg type='as' name='propertyNames' direction='in'/>"
  "      <arg type='u' name='revision' direction='out'/>"
  "      <arg type='(ia{sv}av)' name='layout' direction='out'/>"
  "    </method>"
  "    <method name='Event'>"
  "      <arg type='i' name='id' direction='in'/>"
  "      <arg type='s' name='eventId' direction='in'/>"
  "      <arg type='v' name='data' direction='in'/>"
  "      <arg type='u' name='timestamp' direction='in'/>"
  "    </method>"
  "    <method name='GetGroupProperties'>"
  "      <arg type='ai' name='ids' direction='in'/>"
  "      <arg type='as' name='propertyNames' direction='in'/>"
  "      <arg type='a(ia{sv})' name='properties' direction='out'/>"
  "    </method>"
  "    <method name='AboutToShow'>"
  "      <arg type='i' name='id' direction='in'/>"
  "      <arg type='b' name='needUpdate' direction='out'/>"
  "    </method>"
  "    <signal name='LayoutUpdated'>"
  "      <arg type='u' name='revision'/>"
  "      <arg type='i' name='parent'/>"
  "    </signal>"
  "    <property name='Version' type='u' access='read'/>"
  "    <property name='Status' type='s' access='read'/>"
  "  </interface>"
  "</node>";

struct _NdTray
{
  GApplication *app;
  gboolean      streaming;

  guint         sni_bus_name_id;
  guint         sni_registration_id;
  guint         dbusmenu_registration_id;
  GDBusConnection *connection;

  GDBusNodeInfo *sni_node_info;
  GDBusNodeInfo *dbusmenu_node_info;

  guint         layout_revision;
};

static void nd_tray_register_with_watcher (NdTray *self);

/* --- SNI interface --- */

static void
sni_method_call (GDBusConnection       *connection,
                 const gchar           *sender,
                 const gchar           *object_path,
                 const gchar           *interface_name,
                 const gchar           *method_name,
                 GVariant              *parameters,
                 GDBusMethodInvocation *invocation,
                 gpointer               user_data)
{
  NdTray *self = user_data;

  if (g_strcmp0 (method_name, "Activate") == 0 ||
      g_strcmp0 (method_name, "SecondaryActivate") == 0)
    {
      g_application_activate (self->app);
      g_dbus_method_invocation_return_value (invocation, NULL);
    }
  else if (g_strcmp0 (method_name, "ContextMenu") == 0)
    {
      /* Context menu is handled via dbusmenu, just acknowledge */
      g_dbus_method_invocation_return_value (invocation, NULL);
    }
  else
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_UNKNOWN_METHOD,
                                             "Unknown method: %s",
                                             method_name);
    }
}

static GVariant *
sni_get_property (GDBusConnection *connection,
                  const gchar     *sender,
                  const gchar     *object_path,
                  const gchar     *interface_name,
                  const gchar     *property_name,
                  GError         **error,
                  gpointer         user_data)
{
  NdTray *self = user_data;

  if (g_strcmp0 (property_name, "Category") == 0)
    return g_variant_new_string ("ApplicationStatus");
  if (g_strcmp0 (property_name, "Id") == 0)
    return g_variant_new_string ("gnome-network-displays");
  if (g_strcmp0 (property_name, "Title") == 0)
    return g_variant_new_string ("GNOME Network Displays");
  if (g_strcmp0 (property_name, "Status") == 0)
    return g_variant_new_string (self->streaming ? "Active" : "Passive");
  if (g_strcmp0 (property_name, "IconName") == 0)
    return g_variant_new_string ("org.gnome.NetworkDisplays");
  if (g_strcmp0 (property_name, "Menu") == 0)
    return g_variant_new_object_path ("/MenuBar");
  if (g_strcmp0 (property_name, "ItemIsMenu") == 0)
    return g_variant_new_boolean (FALSE);

  g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
               "Unknown property: %s", property_name);
  return NULL;
}

static const GDBusInterfaceVTable sni_vtable = {
  sni_method_call,
  sni_get_property,
  NULL,
};

/* --- dbusmenu interface --- */

static GVariant *
build_menu_layout (NdTray *self)
{
  GVariantBuilder root_props;
  GVariantBuilder children;

  g_variant_builder_init (&root_props, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&root_props, "{sv}", "children-display",
                         g_variant_new_string ("submenu"));

  g_variant_builder_init (&children, G_VARIANT_TYPE ("av"));

  /* Item 1: Show Window */
  {
    GVariantBuilder item_props;
    g_variant_builder_init (&item_props, G_VARIANT_TYPE ("a{sv}"));
    g_variant_builder_add (&item_props, "{sv}", "label",
                           g_variant_new_string ("Show Window"));
    g_variant_builder_add (&item_props, "{sv}", "enabled",
                           g_variant_new_boolean (TRUE));

    GVariantBuilder no_children;
    g_variant_builder_init (&no_children, G_VARIANT_TYPE ("av"));

    g_variant_builder_add (&children, "v",
                           g_variant_new ("(ia{sv}av)", 1,
                                          &item_props, &no_children));
  }

  /* Item 2: Disconnect */
  {
    GVariantBuilder item_props;
    g_variant_builder_init (&item_props, G_VARIANT_TYPE ("a{sv}"));
    g_variant_builder_add (&item_props, "{sv}", "label",
                           g_variant_new_string ("Disconnect"));
    g_variant_builder_add (&item_props, "{sv}", "enabled",
                           g_variant_new_boolean (self->streaming));

    GVariantBuilder no_children;
    g_variant_builder_init (&no_children, G_VARIANT_TYPE ("av"));

    g_variant_builder_add (&children, "v",
                           g_variant_new ("(ia{sv}av)", 2,
                                          &item_props, &no_children));
  }

  /* Item 3: Quit */
  {
    GVariantBuilder item_props;
    g_variant_builder_init (&item_props, G_VARIANT_TYPE ("a{sv}"));
    g_variant_builder_add (&item_props, "{sv}", "label",
                           g_variant_new_string ("Quit"));
    g_variant_builder_add (&item_props, "{sv}", "enabled",
                           g_variant_new_boolean (TRUE));

    GVariantBuilder no_children;
    g_variant_builder_init (&no_children, G_VARIANT_TYPE ("av"));

    g_variant_builder_add (&children, "v",
                           g_variant_new ("(ia{sv}av)", 3,
                                          &item_props, &no_children));
  }

  /* Root: (u(ia{sv}av)) */
  return g_variant_new ("(u(ia{sv}av))",
                        self->layout_revision,
                        0, &root_props, &children);
}

static void
dbusmenu_method_call (GDBusConnection       *connection,
                      const gchar           *sender,
                      const gchar           *object_path,
                      const gchar           *interface_name,
                      const gchar           *method_name,
                      GVariant              *parameters,
                      GDBusMethodInvocation *invocation,
                      gpointer               user_data)
{
  NdTray *self = user_data;

  if (g_strcmp0 (method_name, "GetLayout") == 0)
    {
      g_dbus_method_invocation_return_value (invocation,
                                             build_menu_layout (self));
    }
  else if (g_strcmp0 (method_name, "Event") == 0)
    {
      gint32 id;
      const gchar *event_id;
      g_autoptr(GVariant) data = NULL;
      guint32 timestamp;

      g_variant_get (parameters, "(is@vu)", &id, &event_id, &data, &timestamp);

      if (g_strcmp0 (event_id, "clicked") == 0)
        {
          switch (id)
            {
            case 1: /* Show Window */
              g_application_activate (self->app);
              break;

            case 2: /* Disconnect */
              {
                GAction *action;
                action = g_action_map_lookup_action (G_ACTION_MAP (self->app),
                                                     "disconnect");
                if (action)
                  g_action_activate (action, NULL);
              }
              break;

            case 3: /* Quit */
              g_application_quit (self->app);
              break;
            }
        }

      g_dbus_method_invocation_return_value (invocation, NULL);
    }
  else if (g_strcmp0 (method_name, "GetGroupProperties") == 0)
    {
      GVariantBuilder builder;
      g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ia{sv})"));
      g_dbus_method_invocation_return_value (invocation,
                                             g_variant_new ("(a(ia{sv}))",
                                                            &builder));
    }
  else if (g_strcmp0 (method_name, "AboutToShow") == 0)
    {
      g_dbus_method_invocation_return_value (invocation,
                                             g_variant_new ("(b)", FALSE));
    }
  else
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_UNKNOWN_METHOD,
                                             "Unknown method: %s",
                                             method_name);
    }
}

static GVariant *
dbusmenu_get_property (GDBusConnection *connection,
                       const gchar     *sender,
                       const gchar     *object_path,
                       const gchar     *interface_name,
                       const gchar     *property_name,
                       GError         **error,
                       gpointer         user_data)
{
  if (g_strcmp0 (property_name, "Version") == 0)
    return g_variant_new_uint32 (3);
  if (g_strcmp0 (property_name, "Status") == 0)
    return g_variant_new_string ("normal");

  g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
               "Unknown property: %s", property_name);
  return NULL;
}

static const GDBusInterfaceVTable dbusmenu_vtable = {
  dbusmenu_method_call,
  dbusmenu_get_property,
  NULL,
};

/* --- Bus name callbacks --- */

static void
on_bus_name_acquired (GDBusConnection *connection,
                      const gchar     *name,
                      gpointer         user_data)
{
  NdTray *self = user_data;

  g_debug ("NdTray: Acquired bus name %s", name);
  self->connection = connection;

  /* Register SNI object */
  g_autoptr(GError) error = NULL;
  self->sni_registration_id =
    g_dbus_connection_register_object (connection,
                                       "/StatusNotifierItem",
                                       self->sni_node_info->interfaces[0],
                                       &sni_vtable,
                                       self,
                                       NULL,
                                       &error);
  if (error)
    {
      g_warning ("NdTray: Failed to register SNI object: %s", error->message);
      return;
    }

  /* Register dbusmenu object */
  self->dbusmenu_registration_id =
    g_dbus_connection_register_object (connection,
                                       "/MenuBar",
                                       self->dbusmenu_node_info->interfaces[0],
                                       &dbusmenu_vtable,
                                       self,
                                       NULL,
                                       &error);
  if (error)
    {
      g_warning ("NdTray: Failed to register dbusmenu object: %s", error->message);
      return;
    }

  nd_tray_register_with_watcher (self);
}

static void
on_bus_name_lost (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
  g_debug ("NdTray: Lost bus name %s", name);
}

/* --- Register with StatusNotifierWatcher --- */

static void
register_cb (GObject      *source_object,
             GAsyncResult *result,
             gpointer      user_data)
{
  g_autoptr(GVariant) ret = NULL;
  g_autoptr(GError) error = NULL;

  ret = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source_object),
                                       result, &error);
  if (error)
    g_debug ("NdTray: Failed to register with StatusNotifierWatcher: %s",
             error->message);
  else
    g_debug ("NdTray: Registered with StatusNotifierWatcher");
}

static void
nd_tray_register_with_watcher (NdTray *self)
{
  g_autofree gchar *bus_name = NULL;

  bus_name = g_strdup_printf ("org.kde.StatusNotifierItem-%d-1", getpid ());

  g_dbus_connection_call (self->connection,
                          "org.kde.StatusNotifierWatcher",
                          "/StatusNotifierWatcher",
                          "org.kde.StatusNotifierWatcher",
                          "RegisterStatusNotifierItem",
                          g_variant_new ("(s)", bus_name),
                          NULL,
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          NULL,
                          register_cb,
                          self);
}

/* --- Public API --- */

NdTray *
nd_tray_new (GApplication *app)
{
  NdTray *self;
  g_autofree gchar *bus_name = NULL;

  g_return_val_if_fail (G_IS_APPLICATION (app), NULL);

  self = g_new0 (NdTray, 1);
  self->app = app;
  self->streaming = FALSE;
  self->layout_revision = 1;

  self->sni_node_info = g_dbus_node_info_new_for_xml (sni_introspection_xml, NULL);
  self->dbusmenu_node_info = g_dbus_node_info_new_for_xml (dbusmenu_introspection_xml, NULL);

  if (!self->sni_node_info || !self->dbusmenu_node_info)
    {
      g_warning ("NdTray: Failed to parse introspection XML");
      g_free (self);
      return NULL;
    }

  bus_name = g_strdup_printf ("org.kde.StatusNotifierItem-%d-1", getpid ());

  self->sni_bus_name_id =
    g_bus_own_name (G_BUS_TYPE_SESSION,
                    bus_name,
                    G_BUS_NAME_OWNER_FLAGS_NONE,
                    on_bus_name_acquired,
                    NULL,
                    on_bus_name_lost,
                    self,
                    NULL);

  return self;
}

void
nd_tray_set_streaming (NdTray  *self,
                       gboolean streaming)
{
  g_return_if_fail (self != NULL);

  if (self->streaming == streaming)
    return;

  self->streaming = streaming;
  self->layout_revision++;

  if (!self->connection)
    return;

  /* Emit NewStatus signal on SNI */
  g_dbus_connection_emit_signal (self->connection,
                                 NULL,
                                 "/StatusNotifierItem",
                                 "org.kde.StatusNotifierItem",
                                 "NewStatus",
                                 g_variant_new ("(s)",
                                                streaming ? "Active" : "Passive"),
                                 NULL);

  /* Emit LayoutUpdated signal on dbusmenu */
  g_dbus_connection_emit_signal (self->connection,
                                 NULL,
                                 "/MenuBar",
                                 "com.canonical.dbusmenu",
                                 "LayoutUpdated",
                                 g_variant_new ("(ui)",
                                                self->layout_revision, 0),
                                 NULL);
}

void
nd_tray_destroy (NdTray *self)
{
  if (self == NULL)
    return;

  if (self->connection)
    {
      if (self->sni_registration_id > 0)
        g_dbus_connection_unregister_object (self->connection,
                                             self->sni_registration_id);
      if (self->dbusmenu_registration_id > 0)
        g_dbus_connection_unregister_object (self->connection,
                                             self->dbusmenu_registration_id);
    }

  if (self->sni_bus_name_id > 0)
    g_bus_unown_name (self->sni_bus_name_id);

  g_clear_pointer (&self->sni_node_info, g_dbus_node_info_unref);
  g_clear_pointer (&self->dbusmenu_node_info, g_dbus_node_info_unref);

  g_free (self);
}
