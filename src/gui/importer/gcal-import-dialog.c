/* gcal-import-dialog.c
 *
 * Copyright 2021 Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
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
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "GcalImportDialog"

#include "gcal-import-dialog.h"

#include "config.h"
#include "gcal-debug.h"
#include "gcal-import-file-row.h"
#include "gcal-utils.h"

#include <adwaita.h>
#include <glib/gi18n.h>

typedef struct
{
  ECalClient         *client;
  GSList             *components;
  GSList             *zones;
} ImportData;

struct _GcalImportDialog
{
  AdwDialog           parent;

  GtkBox             *calendars_box;
  GtkWidget          *cancel_button;
  AdwComboRow        *calendar_combo_row;
  AdwPreferencesGroup *files_group;
  AdwHeaderBar       *headerbar;
  GtkWidget          *import_button;
  GtkWidget          *placeholder_spinner;
  GtkSizeGroup       *title_sizegroup;
  AdwToastOverlay    *toast_overlay;

  GList              *rows;
  GcalCalendar       *selected_calendar;

  GCancellable       *cancellable;
  GcalContext        *context;
  gint                n_events;
  gint                n_files;
};


static void          on_import_row_file_loaded_cb                 (GcalImportFileRow *row,
                                                                   GPtrArray         *events,
                                                                   GcalImportDialog  *self);

G_DEFINE_TYPE (GcalImportDialog, gcal_import_dialog, ADW_TYPE_DIALOG)

enum
{
  PROP_0,
  PROP_CONTEXT,
  N_PROPS
};

static GParamSpec *properties [N_PROPS];


/*
 * Auxiliary methods
 */

static void
import_data_free (gpointer data)
{
  ImportData *import_data = data;

  if (!import_data)
    return;

  g_clear_object (&import_data->client);
  g_slist_free_full (import_data->components, g_object_unref);
  g_slist_free_full (import_data->zones, g_object_unref);
  g_free (import_data);
}

static gboolean
find_calendar (GListModel   *model,
               GcalCalendar *calendar,
               guint        *out_position)
{
  g_assert (out_position != NULL);
  g_assert (G_IS_LIST_MODEL (model));
  g_assert (GCAL_IS_CALENDAR (calendar));

  for (guint i = 0; i < g_list_model_get_n_items (model); i++)
    {
      GcalCalendar *aux = g_list_model_get_item (model, i);
      g_assert (GCAL_IS_CALENDAR (aux));
      if (aux == calendar)
        {
          *out_position = i;
          return TRUE;
        }
    }
  return FALSE;
}

static void
update_default_calendar (GcalImportDialog *self)
{
  g_autoptr (GListModel) calendars = NULL;
  GcalCalendar *default_calendar;
  GcalManager *manager;
  guint position;

  manager = gcal_context_get_manager (self->context);
  calendars = gcal_create_writable_calendars_model (manager);
  default_calendar = gcal_manager_get_default_calendar (manager);

  if (default_calendar && find_calendar (calendars, default_calendar, &position))
    adw_combo_row_set_selected (self->calendar_combo_row, position);
}

static void
setup_calendars (GcalImportDialog *self)
{
  g_autoptr (GListModel) calendars = NULL;
  GcalManager *manager;

  g_assert (self->context != NULL);

  manager = gcal_context_get_manager (self->context);
  calendars = gcal_create_writable_calendars_model (manager);

  // TODO: sort model

  adw_combo_row_set_model (self->calendar_combo_row, calendars);
  update_default_calendar (self);

  g_signal_connect_object (manager, "notify::default-calendar", G_CALLBACK (update_default_calendar), self, G_CONNECT_SWAPPED);
}

static void
add_file (GcalImportDialog *self,
          GFile            *file,
          gboolean          multiple_files)
{
  AdwPreferencesGroup *group;
  GtkWidget *row;
  g_autofree gchar *basename = NULL;

  GCAL_ENTRY;

  row = gcal_import_file_row_new (self->context, file, self->title_sizegroup);
  g_signal_connect (row, "file-loaded", G_CALLBACK (on_import_row_file_loaded_cb), self);

  group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_add (group, row);

  if (multiple_files)
    {
      basename = g_file_get_basename (file);
      adw_preferences_group_set_title (group, basename);
    }

  gtk_box_append (self->calendars_box, GTK_WIDGET (group));
  self->rows = g_list_prepend (self->rows, row);

  gtk_widget_set_visible (self->placeholder_spinner, FALSE);

  GCAL_EXIT;
}

static void
setup_files (GcalImportDialog  *self,
             GFile            **files,
             gint               n_files)
{
  gint i;

  GCAL_ENTRY;

  self->n_files = n_files;
  for (i = 0; i < n_files; i++)
    add_file (self, files[i], n_files > 1);

  GCAL_EXIT;
}

static void
setup_files_list (GcalImportDialog *self,
                  GList            *list)
{
  gint n_files = 0;
  gboolean has_multiple = list->next != NULL;

  GCAL_ENTRY;

  has_multiple = list->next != NULL;
  for (GList *iter = list; iter != NULL; iter = iter->next)
    {
      n_files++;
      add_file (self, G_FILE (iter->data), has_multiple);
    }

  self->n_files = n_files;

  GCAL_EXIT;
}


/*
 * Callbacks
 */

static void
on_events_created_cb (GObject      *source_object,
                      GAsyncResult *result,
                      gpointer      user_data)
{
  g_autoptr (GError) error = NULL;
  GcalImportDialog *self;

  GCAL_ENTRY;

  self = GCAL_IMPORT_DIALOG (user_data);

  g_task_propagate_boolean (G_TASK (result), &error);
  if (error)
    g_warning ("Error creating events: %s", error->message);

  adw_dialog_close (ADW_DIALOG (self));

  GCAL_EXIT;
}

static void
on_calendar_combo_row_selected_item_changed_cb (AdwComboRow      *combo_row,
                                                GParamSpec       *pspec,
                                                GcalImportDialog *self)
{
  GCAL_ENTRY;

  self->selected_calendar = adw_combo_row_get_selected_item (combo_row);

  GCAL_EXIT;
}

static void
on_combo_row_signal_factory_setup_cb (GtkSignalListItemFactory *factory,
                                      GtkListItem              *item,
                                      GcalImportDialog         *self)
{
  GtkWidget *label;
  GtkWidget *color_icon;
  GtkWidget *visibility_status_icon;
  GtkWidget *grid;

  grid = gtk_grid_new ();
  gtk_grid_set_column_spacing (GTK_GRID (grid), 6);

  color_icon = gtk_image_new ();
  gtk_widget_add_css_class (color_icon, "calendar-color-image");
  g_object_set_data (G_OBJECT (grid), "color_icon", color_icon);
  gtk_grid_attach (GTK_GRID (grid), color_icon, 0, 0, 1, 2);

  label = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  g_object_set_data (G_OBJECT (grid), "title", label);
  gtk_grid_attach (GTK_GRID (grid), label, 1, 0, 1, 1);

  label = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_widget_add_css_class (label, "caption");
  gtk_widget_add_css_class (label, "dim-label");
  g_object_set_data (G_OBJECT (grid), "subtitle", label);
  gtk_grid_attach (GTK_GRID (grid), label, 1, 1, 1, 1);

  visibility_status_icon = gtk_image_new ();

  g_object_set_data (G_OBJECT (grid), "visibility_status_icon", visibility_status_icon);
  gtk_widget_set_hexpand (visibility_status_icon, TRUE);
  gtk_widget_set_halign (visibility_status_icon, GTK_ALIGN_END);
  gtk_grid_attach (GTK_GRID (grid), visibility_status_icon, 2, 0, 1, 2);

  gtk_list_item_set_child (item, grid);
}

static void
on_combo_row_signal_factory_bind_cb (GtkSignalListItemFactory *factory,
                                     GtkListItem              *item,
                                     GcalImportDialog         *self)
{
  g_autoptr (GdkPaintable) color_paintable = NULL;
  g_autofree gchar *parent_name = NULL;
  GcalCalendar *calendar;
  GtkWidget *label;
  GtkWidget *color_icon_widget;
  GtkWidget *grid;

  calendar = gtk_list_item_get_item (item);

  grid = gtk_list_item_get_child (item);

  color_icon_widget = g_object_get_data (G_OBJECT (grid), "color_icon");
  color_paintable = get_circle_paintable_from_color (gcal_calendar_get_color (calendar), 16);
  gtk_image_set_from_paintable (GTK_IMAGE (color_icon_widget), color_paintable);

  /* Show indicator icons for calendars that are not shown in the views */
  if (!gcal_calendar_get_visible (calendar))
    {
      g_autoptr (GIcon) visibility_icon = NULL;
      GtkWidget *visibility_status_icon_widget;

      visibility_icon = g_themed_icon_new ("eye-not-looking-symbolic");
      visibility_status_icon_widget = g_object_get_data (G_OBJECT (grid), "visibility_status_icon");
      gtk_image_set_from_gicon (GTK_IMAGE (visibility_status_icon_widget), visibility_icon);
    }

  label = g_object_get_data (G_OBJECT (grid), "title");
  gtk_label_set_label (GTK_LABEL (label), gcal_calendar_get_name (calendar));

  /* Get a colored version of the calendar source's name, to use as a subtitle */
  get_source_parent_name_color (gcal_context_get_manager (self->context),
                                gcal_calendar_get_source (calendar),
                                &parent_name,
                                NULL);

  label = g_object_get_data (G_OBJECT (grid), "subtitle");
  gtk_label_set_label (GTK_LABEL (label), parent_name);

  if (gtk_widget_get_ancestor (grid, GTK_TYPE_POPOVER) != NULL)
    gtk_widget_set_size_request (grid, 250, -1);
  else
    gtk_widget_set_size_request (grid, -1, -1);
}

static void
import_data_thread (GTask        *task,
                    gpointer      source_object,
                    gpointer      task_data,
                    GCancellable *cancellable)
{
  ImportData *id = task_data;
  GError *error = NULL;
  GSList *uids = NULL;
  GSList *l = NULL;

  for (l = id->zones; l && !g_cancellable_is_cancelled (cancellable); l = l->next)
    {
      g_autoptr (GError) local_error = NULL;
      ICalTimezone *zone = l->data;

      e_cal_client_add_timezone_sync (id->client, zone, cancellable, &local_error);

      if (local_error)
        g_warning ("Import: Failed to add timezone: %s", local_error->message);
    }

  e_cal_client_create_objects_sync (id->client,
                                    id->components,
                                    E_CAL_OPERATION_FLAG_NONE,
                                    &uids,
                                    cancellable,
                                    &error);

  g_slist_free_full (uids, g_free);

  if (error)
    g_task_return_error (task, error);
  else
    g_task_return_boolean (task, TRUE);
}

static void
on_import_button_clicked_cb (GtkButton        *button,
                             GcalImportDialog *self)
{
  g_autoptr (GTask) task = NULL;
  ImportData *import_data;
  ECalClient *client;
  GSList *slist = NULL;
  GSList *zones = NULL;
  GList *l;

  GCAL_ENTRY;

  g_assert (self->selected_calendar != NULL);

  slist = NULL;
  for (l = self->rows; l; l = l->next)
    {
      GcalImportFileRow *row = GCAL_IMPORT_FILE_ROW (l->data);
      GPtrArray *ical_components;
      GPtrArray *ical_timezones;
      guint i;

      ical_components = gcal_import_file_row_get_ical_components (row);
      if (!ical_components)
        continue;

      for (i = 0; i < ical_components->len; i++)
        {
          ICalComponent *comp = g_ptr_array_index (ical_components, i);
          slist = g_slist_prepend (slist, g_object_ref (comp));
        }

      ical_timezones = gcal_import_file_row_get_timezones (row);
      if (!ical_timezones)
        continue;

      for (i = 0; i < ical_timezones->len; i++)
        {
          ICalTimezone *zone = g_ptr_array_index (ical_timezones, i);
          zones = g_slist_prepend (zones, g_object_ref (zone));
        }
    }

  if (!slist)
    GCAL_RETURN ();

  self->cancellable = g_cancellable_new ();

  gtk_widget_set_sensitive (GTK_WIDGET (self), FALSE);

  client = gcal_calendar_get_client (self->selected_calendar);

  import_data = g_new0 (ImportData, 1);
  import_data->client = g_object_ref (client);
  import_data->components = g_slist_reverse (slist);
  import_data->zones = g_slist_reverse (zones);

  task = g_task_new (NULL, self->cancellable, on_events_created_cb, self);
  g_task_set_task_data (task, import_data, import_data_free);
  g_task_set_source_tag (task, on_import_button_clicked_cb);
  g_task_run_in_thread (task, import_data_thread);

  GCAL_EXIT;
}

static void
on_import_row_file_loaded_cb (GcalImportFileRow *row,
                              GPtrArray         *events,
                              GcalImportDialog  *self)
{
  g_autofree gchar *title = NULL;

  GCAL_ENTRY;

  self->n_events += events ? events->len : 0;

  title = g_strdup_printf (g_dngettext (GETTEXT_PACKAGE,
                                        "Import %d event",
                                        "Import %d events",
                                        self->n_events),
                           self->n_events);
  adw_dialog_set_title (ADW_DIALOG (self), title);

  gtk_widget_set_visible (GTK_WIDGET (row), TRUE);

  GCAL_EXIT;
}


/*
 * GObject overrides
 */

static void
gcal_import_dialog_constructed (GObject *object)
{
  GcalImportDialog *self = (GcalImportDialog *)object;

  G_OBJECT_CLASS (gcal_import_dialog_parent_class)->constructed (object);

  setup_calendars (self);
}

static void
gcal_import_dialog_finalize (GObject *object)
{
  GcalImportDialog *self = (GcalImportDialog *)object;

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->context);

  g_clear_pointer (&self->rows, g_list_free);

  G_OBJECT_CLASS (gcal_import_dialog_parent_class)->finalize (object);
}

static void
gcal_import_dialog_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  GcalImportDialog *self = GCAL_IMPORT_DIALOG (object);

  switch (prop_id)
    {
    case PROP_CONTEXT:
      g_value_set_object (value, self->context);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gcal_import_dialog_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  GcalImportDialog *self = GCAL_IMPORT_DIALOG (object);

  switch (prop_id)
    {
    case PROP_CONTEXT:
      g_assert (self->context == NULL);
      self->context = g_value_dup_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gcal_import_dialog_class_init (GcalImportDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->constructed = gcal_import_dialog_constructed;
  object_class->finalize = gcal_import_dialog_finalize;
  object_class->get_property = gcal_import_dialog_get_property;
  object_class->set_property = gcal_import_dialog_set_property;

  /**
   * GcalEventPopover::context:
   *
   * The context of the import dialog.
   */
  properties[PROP_CONTEXT] = g_param_spec_object ("context",
                                                  "Context",
                                                  "Context",
                                                  GCAL_TYPE_CONTEXT,
                                                  G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, properties);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/calendar/ui/gui/importer/gcal-import-dialog.ui");

  gtk_widget_class_bind_template_child (widget_class, GcalImportDialog, calendar_combo_row);
  gtk_widget_class_bind_template_child (widget_class, GcalImportDialog, cancel_button);
  gtk_widget_class_bind_template_child (widget_class, GcalImportDialog, files_group);
  gtk_widget_class_bind_template_child (widget_class, GcalImportDialog, headerbar);
  gtk_widget_class_bind_template_child (widget_class, GcalImportDialog, import_button);
  gtk_widget_class_bind_template_child (widget_class, GcalImportDialog, placeholder_spinner);
  gtk_widget_class_bind_template_child (widget_class, GcalImportDialog, calendars_box);
  gtk_widget_class_bind_template_child (widget_class, GcalImportDialog, title_sizegroup);
  gtk_widget_class_bind_template_child (widget_class, GcalImportDialog, toast_overlay);

  gtk_widget_class_bind_template_callback (widget_class, on_calendar_combo_row_selected_item_changed_cb);
  gtk_widget_class_bind_template_callback (widget_class, on_import_button_clicked_cb);
  gtk_widget_class_bind_template_callback (widget_class, on_combo_row_signal_factory_bind_cb);
  gtk_widget_class_bind_template_callback (widget_class, on_combo_row_signal_factory_setup_cb);
}

static void
gcal_import_dialog_init (GcalImportDialog *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));
}

GtkWidget*
gcal_import_dialog_new_for_files (GcalContext  *context,
                                  GFile       **files,
                                  gint          n_files)
{
  GcalImportDialog *self;

  self =  g_object_new (GCAL_TYPE_IMPORT_DIALOG,
                        "context", context,
                        NULL);

  setup_files (self, files, n_files);

  return GTK_WIDGET (self);
}

GtkWidget*
gcal_import_dialog_new_for_file_list (GcalContext *context,
                                      GList       *file_list)
{
  GcalImportDialog *self;

  self =  g_object_new (GCAL_TYPE_IMPORT_DIALOG,
                        "context", context,
                        NULL);

  setup_files_list (self, file_list);

  return GTK_WIDGET (self);
}

void
gcal_import_dialog_add_toast (GcalImportDialog *self,
                              AdwToast         *toast)
{
  g_return_if_fail (GCAL_IS_IMPORT_DIALOG (self));
  g_return_if_fail (ADW_IS_TOAST (toast));

  adw_toast_overlay_add_toast (self->toast_overlay, toast);
}
