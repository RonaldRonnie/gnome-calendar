/* gcal-search-button.c
 *
 * Copyright 2019 Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
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

#define G_LOG_DOMAIN "GcalSearchButton"

#include "gcal-context.h"
#include "gcal-debug.h"
#include "gcal-search-button.h"
#include "gcal-search-hit.h"
#include "gcal-search-hit-row.h"

#include <math.h>

#define MIN_WIDTH 450

struct _GcalSearchButton
{
  AdwBin               parent;

  GtkEditable         *entry;
  GtkWidget           *popover;
  GtkListBox          *results_listbox;
  GtkRevealer         *results_revealer;
  GtkStack            *stack;

  GCancellable        *cancellable;
  gint                 max_width_chars;
  GListModel          *model;

  GcalContext         *context;
};

G_DEFINE_TYPE (GcalSearchButton, gcal_search_button, ADW_TYPE_BIN)

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
show_suggestions (GcalSearchButton *self)
{
  gtk_popover_popup (GTK_POPOVER (self->popover));
  gtk_revealer_set_reveal_child (self->results_revealer, TRUE);
}

static void
hide_suggestions (GcalSearchButton *self)
{
  gtk_revealer_set_reveal_child (self->results_revealer, FALSE);
  gtk_editable_set_text (self->entry, "");
}

static GtkWidget *
create_widget_func (gpointer item,
                    gpointer user_data)
{
  return gcal_search_hit_row_new (item);
}

static void
set_model (GcalSearchButton *self,
           GListModel       *model)
{
  GCAL_ENTRY;

  gtk_list_box_bind_model (self->results_listbox,
                           model,
                           create_widget_func,
                           self,
                           NULL);

  if (model)
    show_suggestions (self);
  else
    hide_suggestions (self);

  GCAL_EXIT;
}


/*
 * Callbacks
 */

static void
on_button_clicked_cb (GtkButton        *button,
                      GcalSearchButton *self)
{
  gint max_width_chars;


  max_width_chars = gtk_editable_get_max_width_chars (self->entry);

  if (max_width_chars)
    self->max_width_chars = max_width_chars;

  gtk_editable_set_width_chars (self->entry, 1);
  gtk_editable_set_max_width_chars (self->entry, self->max_width_chars ?: 20);
  gtk_stack_set_visible_child_name (self->stack, "entry");
  gtk_widget_grab_focus (GTK_WIDGET (self->entry));
}

static void
on_focus_controller_leave_cb (GtkEventControllerFocus *focus_controller,
                              GcalSearchButton        *self)
{
  gtk_editable_set_width_chars (self->entry, 0);
  gtk_editable_set_max_width_chars (self->entry, 0);
  gtk_stack_set_visible_child_name (self->stack, "button");

  hide_suggestions (self);

  gtk_editable_set_text (self->entry, "");
}

static void
on_entry_icon_pressed_cb (GtkEntry             *entry,
                          GtkEntryIconPosition  position,
                          GcalSearchButton     *self)
{
  if (position == GTK_ENTRY_ICON_PRIMARY)
    gtk_stack_set_visible_child_name (self->stack, "button");
}

static void
on_search_finished_cb (GObject      *source_object,
                       GAsyncResult *result,
                       gpointer      user_data)
{
  g_autoptr (GListModel) model = NULL;
  g_autoptr (GError) error = NULL;
  GcalSearchButton *self;

  self = GCAL_SEARCH_BUTTON (user_data);
  model = gcal_search_engine_search_finish (GCAL_SEARCH_ENGINE (source_object), result, &error);

  set_model (self, model);
}

static void
on_entry_text_changed_cb (GtkEntry         *entry,
                          GParamSpec       *pspec,
                          GcalSearchButton *self)
{
  g_autofree gchar *sexp_query = NULL;
  GcalSearchEngine *search_engine;
  const gchar *text;

  text = gtk_editable_get_text (self->entry);

  g_cancellable_cancel (self->cancellable);

  if (!text || *text == '\0')
    {
      set_model (self, NULL);
      return;
    }

  sexp_query = g_strdup_printf ("(contains? \"summary\" \"%s\")", text);
  search_engine = gcal_context_get_search_engine (self->context);
  gcal_search_engine_search (search_engine,
                             sexp_query,
                             50,
                             self->cancellable,
                             on_search_finished_cb,
                             g_object_ref (self));
}

static void
on_popover_closed_cb (GtkPopover       *popover,
                      GcalSearchButton *self)
{
  gtk_editable_set_width_chars (self->entry, 0);
  gtk_editable_set_max_width_chars (self->entry, 0);
  gtk_editable_set_text (self->entry, "");
  gtk_stack_set_visible_child_name (self->stack, "button");
}

static void
on_results_listbox_row_activated_cb (GtkListBox       *listbox,
                                     GcalSearchHitRow *row,
                                     GcalSearchButton *self)
{
  GcalSearchHit *search_hit;

  search_hit = gcal_search_hit_row_get_search_hit (row);
  gcal_search_hit_activate (search_hit, GTK_WIDGET (self));

  hide_suggestions (self);
}

static void
on_results_revealer_child_reveal_state_changed_cb (GtkRevealer      *revealer,
                                                   GParamSpec       *pspec,
                                                   GcalSearchButton *self)
{
  if (!gtk_revealer_get_child_revealed (revealer) && !gtk_revealer_get_reveal_child (revealer))
    gtk_popover_popdown (GTK_POPOVER (self->popover));
}


/*
 * GtkWidget overrides
 */

static gboolean
gcal_search_button_focus (GtkWidget        *widget,
                          GtkDirectionType  direction)
{
  GcalSearchButton *self = GCAL_SEARCH_BUTTON (widget);

  if (!gtk_widget_get_visible (GTK_WIDGET (self->popover)))
    return gtk_widget_child_focus (GTK_WIDGET (self->stack), direction);

  if (direction == GTK_DIR_DOWN)
    {
      GtkListBoxRow *first_row = gtk_list_box_get_row_at_index (self->results_listbox, 0);

      if (!first_row)
        return gtk_widget_child_focus (GTK_WIDGET (self->stack), direction);

      gtk_widget_grab_focus (GTK_WIDGET (first_row));
      return TRUE;
    }

  return gtk_widget_child_focus (GTK_WIDGET (self->stack), direction);
}


/*
 * GObject overrides
 */

static void
gcal_search_button_dispose (GObject *object)
{
  GcalSearchButton *self = (GcalSearchButton *)object;

  g_clear_pointer (&self->popover, gtk_widget_unparent);

  G_OBJECT_CLASS (gcal_search_button_parent_class)->dispose (object);
}

static void
gcal_search_button_finalize (GObject *object)
{
  GcalSearchButton *self = (GcalSearchButton *)object;

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->context);

  G_OBJECT_CLASS (gcal_search_button_parent_class)->finalize (object);
}

static void
gcal_search_button_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  GcalSearchButton *self = GCAL_SEARCH_BUTTON (object);

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
gcal_search_button_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  GcalSearchButton *self = GCAL_SEARCH_BUTTON (object);

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
gcal_search_button_class_init (GcalSearchButtonClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = gcal_search_button_dispose;
  object_class->finalize = gcal_search_button_finalize;
  object_class->get_property = gcal_search_button_get_property;
  object_class->set_property = gcal_search_button_set_property;

  widget_class->focus = gcal_search_button_focus;

  /**
   * GcalSearchButton::context:
   *
   * The #GcalContext of the application.
   */
  properties[PROP_CONTEXT] = g_param_spec_object ("context",
                                                  "Context of the application",
                                                  "The context of the application",
                                                  GCAL_TYPE_CONTEXT,
                                                  G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, properties);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/calendar/ui/gui/gcal-search-button.ui");

  gtk_widget_class_bind_template_child (widget_class, GcalSearchButton, entry);
  gtk_widget_class_bind_template_child (widget_class, GcalSearchButton, popover);
  gtk_widget_class_bind_template_child (widget_class, GcalSearchButton, results_listbox);
  gtk_widget_class_bind_template_child (widget_class, GcalSearchButton, results_revealer);
  gtk_widget_class_bind_template_child (widget_class, GcalSearchButton, stack);

  gtk_widget_class_bind_template_callback (widget_class, on_button_clicked_cb);
  gtk_widget_class_bind_template_callback (widget_class, on_focus_controller_leave_cb);
  gtk_widget_class_bind_template_callback (widget_class, on_entry_icon_pressed_cb);
  gtk_widget_class_bind_template_callback (widget_class, on_entry_text_changed_cb);
  gtk_widget_class_bind_template_callback (widget_class, on_popover_closed_cb);
  gtk_widget_class_bind_template_callback (widget_class, on_results_listbox_row_activated_cb);
  gtk_widget_class_bind_template_callback (widget_class, on_results_revealer_child_reveal_state_changed_cb);

  gtk_widget_class_set_css_name (widget_class, "searchbutton");
}

static void
gcal_search_button_init (GcalSearchButton *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  gtk_widget_set_parent (GTK_WIDGET (self->popover), GTK_WIDGET (self));
}

void
gcal_search_button_search (GcalSearchButton *self,
                           const gchar      *search_text)
{
  g_return_if_fail (GCAL_IS_SEARCH_BUTTON (self));

  gtk_widget_grab_focus (GTK_WIDGET (self));
  gtk_editable_set_text (self->entry, search_text);
}
