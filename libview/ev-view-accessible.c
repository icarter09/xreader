/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8; c-indent-level: 8 -*- */
/* this file is part of xreader, a generic document viewer
 *
 *  Copyright (C) 2004 Red Hat, Inc
 *
 * Xreader is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Xreader is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <math.h>
#include <config.h>
#include <glib/gi18n-lib.h>
#include <gtk/gtk.h>
#include <libgail-util/gail-util.h>

#include "ev-selection.h"
#include "ev-page-cache.h"
#include "ev-view-accessible.h"
#include "ev-link-accessible.h"
#include "ev-view-private.h"

static void ev_view_accessible_text_iface_init      (AtkTextIface      *iface);
static void ev_view_accessible_action_iface_init    (AtkActionIface    *iface);
static void ev_view_accessible_hypertext_iface_init (AtkHypertextIface *iface);

enum {
	ACTION_SCROLL_UP,
	ACTION_SCROLL_DOWN,
	LAST_ACTION
};

static const gchar *const ev_view_accessible_action_names[] =
{
	N_("Scroll Up"),
	N_("Scroll Down"),
	NULL
};

static const gchar *const ev_view_accessible_action_descriptions[] =
{
	N_("Scroll View Up"),
	N_("Scroll View Down"),
	NULL
};

struct _EvViewAccessiblePrivate {
	guint         current_page;

	/* AtkAction */
	gchar        *action_descriptions[LAST_ACTION];
	guint         action_idle_handler;
	GtkScrollType idle_scroll;

	/* AtkText */
	GtkTextBuffer *buffer;

	/* AtkHypertext */
	GHashTable    *links;
};

G_DEFINE_TYPE_WITH_CODE (EvViewAccessible, ev_view_accessible, GTK_TYPE_CONTAINER_ACCESSIBLE,
			 G_IMPLEMENT_INTERFACE (ATK_TYPE_TEXT, ev_view_accessible_text_iface_init)
			 G_IMPLEMENT_INTERFACE (ATK_TYPE_ACTION, ev_view_accessible_action_iface_init)
			 G_IMPLEMENT_INTERFACE (ATK_TYPE_HYPERTEXT, ev_view_accessible_hypertext_iface_init)
	)

static void
ev_view_accessible_finalize (GObject *object)
{
	EvViewAccessiblePrivate *priv = EV_VIEW_ACCESSIBLE (object)->priv;
	int i;

	if (priv->action_idle_handler)
		g_source_remove (priv->action_idle_handler);
	for (i = 0; i < LAST_ACTION; i++)
		g_free (priv->action_descriptions [i]);
	if (priv->buffer)
		g_object_unref (priv->buffer);
	if (priv->links)
		g_hash_table_destroy (priv->links);

	G_OBJECT_CLASS (ev_view_accessible_parent_class)->finalize (object);
}

static void
ev_view_accessible_initialize (AtkObject *obj,
			       gpointer   data)
{
	if (ATK_OBJECT_CLASS (ev_view_accessible_parent_class)->initialize != NULL)
		ATK_OBJECT_CLASS (ev_view_accessible_parent_class)->initialize (obj, data);

	gtk_accessible_set_widget (GTK_ACCESSIBLE (obj), GTK_WIDGET (data));

	atk_object_set_name (obj, _("Document View"));
	atk_object_set_role (obj, ATK_ROLE_DOCUMENT_FRAME);
}

static void
ev_view_accessible_class_init (EvViewAccessibleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	AtkObjectClass *atk_class = ATK_OBJECT_CLASS (klass);

	object_class->finalize = ev_view_accessible_finalize;
	atk_class->initialize = ev_view_accessible_initialize;

	g_type_class_add_private (klass, sizeof (EvViewAccessiblePrivate));
}

static void
ev_view_accessible_init (EvViewAccessible *accessible)
{
	accessible->priv = G_TYPE_INSTANCE_GET_PRIVATE (accessible, EV_TYPE_VIEW_ACCESSIBLE, EvViewAccessiblePrivate);
}

static GtkTextBuffer *
ev_view_accessible_get_text_buffer (EvViewAccessible *accessible, EvView *view)
{
	EvPageCache *page_cache;
	const gchar *retval = NULL;
	EvViewAccessiblePrivate* priv = accessible->priv;

	page_cache = view->page_cache;
	if (!page_cache) {
		return NULL;
	}

	if (view->current_page == priv->current_page && priv->buffer) {
		return priv->buffer;
	}

	priv->current_page = view->current_page;

	if (!priv->buffer) {
		priv->buffer = gtk_text_buffer_new (NULL);
	}

	retval = ev_page_cache_get_text (page_cache, view->current_page);
	if (retval)
		gtk_text_buffer_set_text (priv->buffer, retval, -1);

	return priv->buffer;
}

static gchar *
ev_view_accessible_get_text (AtkText *text,
			     gint     start_pos,
			     gint     end_pos)
{
	GtkWidget *widget;
	GtkTextIter start, end;
	GtkTextBuffer *buffer;
	gchar *retval;

	widget = gtk_accessible_get_widget (GTK_ACCESSIBLE (text));
	if (widget == NULL)
		/* State is defunct */
		return NULL;

	buffer = ev_view_accessible_get_text_buffer (EV_VIEW_ACCESSIBLE (text), EV_VIEW (widget));
	if (!buffer)
		return NULL;

	gtk_text_buffer_get_iter_at_offset (buffer, &start, start_pos);
	gtk_text_buffer_get_iter_at_offset (buffer, &end, end_pos);
	retval = gtk_text_buffer_get_text (buffer, &start, &end, FALSE);

	return retval;
}

static gunichar
ev_view_accessible_get_character_at_offset (AtkText *text,
					    gint     offset)
{
	GtkWidget *widget;
	GtkTextIter start, end;
	GtkTextBuffer *buffer;
	gchar *string;
	gunichar unichar;

	widget = gtk_accessible_get_widget (GTK_ACCESSIBLE (text));
	if (widget == NULL)
		/* State is defunct */
		return '\0';

	buffer = ev_view_accessible_get_text_buffer (EV_VIEW_ACCESSIBLE (text), EV_VIEW (widget));
	if (!buffer)
		return '\0';

	if (offset >= gtk_text_buffer_get_char_count (buffer))
		return '\0';

	gtk_text_buffer_get_iter_at_offset (buffer, &start, offset);
	end = start;
	gtk_text_iter_forward_char (&end);
	string = gtk_text_buffer_get_slice (buffer, &start, &end, FALSE);
	unichar = g_utf8_get_char (string);
	g_free(string);

	return unichar;
}

static gchar *
ev_view_accessible_get_text_for_offset (EvViewAccessible *view_accessible,
                                        gint              offset,
                                        GailOffsetType    offset_type,
                                        AtkTextBoundary   boundary_type,
                                        gint             *start_offset,
                                        gint             *end_offset)
{
        GtkWidget     *widget;
        GtkTextBuffer *buffer;
        GailTextUtil  *gail_text;
        gchar         *retval;

        widget = gtk_accessible_get_widget (GTK_ACCESSIBLE (view_accessible));
        if (widget == NULL)
                /* State is defunct */
                return NULL;

        buffer = ev_view_accessible_get_text_buffer (view_accessible, EV_VIEW (widget));
        if (!buffer)
                return NULL;

        gail_text = gail_text_util_new ();
        gail_text_util_buffer_setup (gail_text, buffer);
        retval = gail_text_util_get_text (gail_text, NULL, offset_type, boundary_type,
                                          offset, start_offset, end_offset);
        g_object_unref (gail_text);

        return retval;
}

static gchar *
ev_view_accessible_get_text_before_offset (AtkText        *text,
                                           gint            offset,
                                           AtkTextBoundary boundary_type,
                                           gint           *start_offset,
                                           gint           *end_offset)
{
        return ev_view_accessible_get_text_for_offset (EV_VIEW_ACCESSIBLE (text),
                                                       offset, GAIL_BEFORE_OFFSET,
                                                       boundary_type,
                                                       start_offset, end_offset);
}

static gchar *
ev_view_accessible_get_text_at_offset (AtkText        *text,
                                       gint            offset,
                                       AtkTextBoundary boundary_type,
                                       gint           *start_offset,
                                       gint           *end_offset)
{
        return ev_view_accessible_get_text_for_offset (EV_VIEW_ACCESSIBLE (text),
                                                       offset, GAIL_AT_OFFSET,
                                                       boundary_type,
                                                       start_offset, end_offset);
}

static gchar *
ev_view_accessible_get_text_after_offset (AtkText        *text,
                                          gint            offset,
                                          AtkTextBoundary boundary_type,
                                          gint            *start_offset,
                                          gint           *end_offset)
{
        return ev_view_accessible_get_text_for_offset (EV_VIEW_ACCESSIBLE (text),
                                                       offset, GAIL_AFTER_OFFSET,
                                                       boundary_type,
                                                       start_offset, end_offset);
}

static gint
ev_view_accessible_get_character_count (AtkText *text)
{
	GtkWidget *widget;
	GtkTextBuffer *buffer;
	gint retval;

	widget = gtk_accessible_get_widget (GTK_ACCESSIBLE (text));
	if (widget == NULL)
		/* State is defunct */
		return 0;

	buffer = ev_view_accessible_get_text_buffer (EV_VIEW_ACCESSIBLE (text), EV_VIEW (widget));
	if (!buffer)
		return 0;

	retval = gtk_text_buffer_get_char_count (buffer);

	return retval;
}

static gint
ev_view_accessible_get_caret_offset (AtkText *text)
{
	GtkWidget *widget;
	GtkTextBuffer *buffer;
	GtkTextMark *cursor_mark;
	GtkTextIter cursor_itr;
	gint retval;

	widget = gtk_accessible_get_widget (GTK_ACCESSIBLE (text));
	if (widget == NULL)
		/* State is defunct */
		return 0;

	buffer = ev_view_accessible_get_text_buffer (EV_VIEW_ACCESSIBLE (text), EV_VIEW (widget));
	if (!buffer)
		return 0;

	cursor_mark = gtk_text_buffer_get_insert (buffer);
	gtk_text_buffer_get_iter_at_mark (buffer, &cursor_itr, cursor_mark);
	retval = gtk_text_iter_get_offset (&cursor_itr);

	return retval;
}

static gboolean
ev_view_accessible_set_caret_offset (AtkText *text, gint offset)
{
	GtkWidget *widget;
	GtkTextBuffer *buffer;
	GtkTextIter pos_itr;

	widget = gtk_accessible_get_widget (GTK_ACCESSIBLE (text));
	if (widget == NULL)
		/* State is defunct */
		return FALSE;

	buffer = ev_view_accessible_get_text_buffer (EV_VIEW_ACCESSIBLE (text), EV_VIEW (widget));
	if (!buffer)
		return FALSE;

	gtk_text_buffer_get_iter_at_offset (buffer, &pos_itr, offset);
	gtk_text_buffer_place_cursor (buffer, &pos_itr);

	return TRUE;
}

static AtkAttributeSet*
ev_view_accessible_get_run_attributes (AtkText *text,
				       gint    offset,
				       gint    *start_offset,
				       gint    *end_offset)
{
	GtkWidget *widget;
	GtkTextBuffer *buffer;
	AtkAttributeSet *retval;

	widget = gtk_accessible_get_widget (GTK_ACCESSIBLE (text));
	if (widget == NULL)
		/* State is defunct */
		return NULL;

	buffer = ev_view_accessible_get_text_buffer (EV_VIEW_ACCESSIBLE (text), EV_VIEW (widget));
	if (!buffer)
		return NULL;

	retval = gail_misc_buffer_get_run_attributes (buffer, offset,
	                                              start_offset, end_offset);

	return retval;
}

static AtkAttributeSet*
ev_view_accessible_get_default_attributes (AtkText *text)
{
	GtkWidget *widget;

	widget = gtk_accessible_get_widget (GTK_ACCESSIBLE (text));
	if (widget == NULL)
		/* State is defunct */
		return NULL;
	return NULL;
}

static void
ev_view_accessible_get_character_extents (AtkText      *text,
					  gint         offset,
					  gint         *x,
					  gint         *y,
					  gint         *width,
					  gint         *height,
					  AtkCoordType coords)
{
	GtkWidget *widget, *toplevel;
	EvView *view;
	EvRectangle *areas = NULL;
	EvRectangle *doc_rect;
	guint n_areas = 0;
	gint x_widget, y_widget;
	GdkRectangle view_rect;

	widget = gtk_accessible_get_widget (GTK_ACCESSIBLE (text));
	if (widget == NULL)
		/* State is defunct */
		return;

	view = EV_VIEW (widget);
	if (!view->page_cache)
		return;

	ev_page_cache_get_text_layout (view->page_cache, view->current_page, &areas, &n_areas);
	if (!areas || offset >= n_areas)
		return;

	doc_rect = areas + offset;
	_ev_view_transform_doc_rect_to_view_rect (view, view->current_page, doc_rect, &view_rect);
	view_rect.x -= view->scroll_x;
	view_rect.y -= view->scroll_y;

	toplevel = gtk_widget_get_toplevel (widget);
	gtk_widget_translate_coordinates (widget, toplevel, 0, 0, &x_widget, &y_widget);
	view_rect.x += x_widget;
	view_rect.y += y_widget;

	if (coords == ATK_XY_SCREEN) {
		gint x_window, y_window;

		gdk_window_get_origin (gtk_widget_get_window (toplevel), &x_window, &y_window);
		view_rect.x += x_window;
		view_rect.y += y_window;
	}

	*x = view_rect.x;
	*y = view_rect.y;
	*width = view_rect.width;
	*height = view_rect.height;
}

static gint
ev_view_accessible_get_offset_at_point (AtkText      *text,
					gint         x,
					gint         y,
					AtkCoordType coords)
{
	GtkWidget *widget, *toplevel;
	EvView *view;
	EvRectangle *areas = NULL;
	EvRectangle *rect = NULL;
	guint n_areas = 0;
	guint i;
	gint x_widget, y_widget;
	gint offset=-1;
	GdkPoint view_point;
	gdouble doc_x, doc_y;
	GtkBorder border;
	GdkRectangle page_area;

	widget = gtk_accessible_get_widget (GTK_ACCESSIBLE (text));
	if (widget == NULL)
		/* State is defunct */
		return -1;

	view = EV_VIEW (widget);
	if (!view->page_cache)
		return -1;

	ev_page_cache_get_text_layout (view->page_cache, view->current_page, &areas, &n_areas);
	if (!areas)
		return -1;

	view_point.x = x;
	view_point.y = y;
	toplevel = gtk_widget_get_toplevel (widget);
	gtk_widget_translate_coordinates (widget, toplevel, 0, 0, &x_widget, &y_widget);
	view_point.x -= x_widget;
	view_point.y -= y_widget;

	if (coords == ATK_XY_SCREEN) {
		gint x_window, y_window;

		gdk_window_get_origin (gtk_widget_get_window (toplevel), &x_window, &y_window);
		view_point.x -= x_window;
		view_point.y -= y_window;
	}

	ev_view_get_page_extents (view, view->current_page, &page_area, &border);
	_ev_view_transform_view_point_to_doc_point (view, &view_point, &page_area, &border, &doc_x, &doc_y);

	for (i = 0; i < n_areas; i++) {
		rect = areas + i;
		if (doc_x >= rect->x1 && doc_x <= rect->x2 &&
		    doc_y >= rect->y1 && doc_y <= rect->y2)
			offset = i;
	}

	return offset;
}

static gint
ev_view_accessible_get_n_selections (AtkText *text)
{
	GtkWidget *widget;
	GtkTextBuffer *buffer;
	GtkTextIter start, end;
	gint select_start, select_end;

	widget = gtk_accessible_get_widget (GTK_ACCESSIBLE (text));
	if (widget == NULL)
		/* State is defunct */
		return -1;

	buffer = ev_view_accessible_get_text_buffer (EV_VIEW_ACCESSIBLE (text), EV_VIEW (widget));
	if (!buffer)
		return -1;

	gtk_text_buffer_get_selection_bounds (buffer, &start, &end);
	select_start = gtk_text_iter_get_offset (&start);
	select_end = gtk_text_iter_get_offset (&end);

	if (select_start != select_end)
		return 1;
	else
		return 0;
}

static gchar *
ev_view_accessible_get_selection (AtkText *text,
				  gint     selection_num,
				  gint    *start_pos,
				  gint    *end_pos)
{
	GtkWidget *widget;
	GtkTextBuffer *buffer;
	GtkTextIter start, end;
	gchar *retval = NULL;

	widget = gtk_accessible_get_widget (GTK_ACCESSIBLE (text));
	if (widget == NULL)
		/* State is defunct */
		return NULL;

	if (selection_num != 0)
		return NULL;

	buffer = ev_view_accessible_get_text_buffer (EV_VIEW_ACCESSIBLE (text), EV_VIEW (widget));
	if (!buffer)
		return NULL;

	gtk_text_buffer_get_selection_bounds (buffer, &start, &end);
	*start_pos = gtk_text_iter_get_offset (&start);
	*end_pos = gtk_text_iter_get_offset (&end);

	if (*start_pos != *end_pos)
		retval = gtk_text_buffer_get_text (buffer, &start, &end, FALSE);

	return retval;
}

static gboolean
ev_view_accessible_add_selection (AtkText *text,
				  gint     start_pos,
				  gint     end_pos)
{
	GtkWidget *widget;
	GtkTextBuffer *buffer;
	GtkTextIter pos_itr;
	GtkTextIter start, end;
	gint select_start, select_end;
	gboolean retval = FALSE;

	widget = gtk_accessible_get_widget (GTK_ACCESSIBLE (text));
	if (widget == NULL)
		/* State is defunct */
		return FALSE;

	buffer = ev_view_accessible_get_text_buffer (EV_VIEW_ACCESSIBLE (text), EV_VIEW (widget));
	if (!buffer)
		return FALSE;

	gtk_text_buffer_get_selection_bounds (buffer, &start, &end);
	select_start = gtk_text_iter_get_offset (&start);
	select_end = gtk_text_iter_get_offset (&end);

	/* If there is already a selection, then don't allow
	 * another to be added
	 */
	if (select_start == select_end) {
		gtk_text_buffer_get_iter_at_offset (buffer, &pos_itr, start_pos);
		gtk_text_buffer_move_mark_by_name (buffer, "selection_bound", &pos_itr);
		gtk_text_buffer_get_iter_at_offset (buffer, &pos_itr, end_pos);
		gtk_text_buffer_move_mark_by_name (buffer, "insert", &pos_itr);

		retval = TRUE;
	}

	return retval;
}

static gboolean
ev_view_accessible_remove_selection (AtkText *text,
				     gint     selection_num)
{
	GtkWidget *widget;
	GtkTextBuffer *buffer;
	GtkTextMark *cursor_mark;
	GtkTextIter cursor_itr;
	GtkTextIter start, end;
	gint select_start, select_end;
	gboolean retval = FALSE;

	widget = gtk_accessible_get_widget (GTK_ACCESSIBLE (text));
	if (widget == NULL)
		/* State is defunct */
		return FALSE;

	buffer = ev_view_accessible_get_text_buffer (EV_VIEW_ACCESSIBLE (text), EV_VIEW (widget));
	if (!buffer)
		return FALSE;

	gtk_text_buffer_get_selection_bounds(buffer, &start, &end);
	select_start = gtk_text_iter_get_offset(&start);
	select_end = gtk_text_iter_get_offset(&end);

	if (select_start != select_end) {
		/* Setting the start & end of the selected region
		 * to the caret position turns off the selection.
		 */
		cursor_mark = gtk_text_buffer_get_insert (buffer);
		gtk_text_buffer_get_iter_at_mark (buffer, &cursor_itr, cursor_mark);
		gtk_text_buffer_move_mark_by_name (buffer, "selection_bound", &cursor_itr);

		retval = TRUE;
	}

	return retval;
}

static gboolean
ev_view_accessible_set_selection (AtkText *text,
				  gint	   selection_num,
				  gint     start_pos,
				  gint     end_pos)
{
	GtkWidget *widget;
	GtkTextBuffer *buffer;
	GtkTextIter pos_itr;
	GtkTextIter start, end;
	gint select_start, select_end;
	gboolean retval = FALSE;

	widget = gtk_accessible_get_widget (GTK_ACCESSIBLE (text));
	if (widget == NULL)
		/* State is defunct */
		return FALSE;

	buffer = ev_view_accessible_get_text_buffer (EV_VIEW_ACCESSIBLE (text), EV_VIEW (widget));
	if (!buffer)
		return FALSE;

	gtk_text_buffer_get_selection_bounds(buffer, &start, &end);
	select_start = gtk_text_iter_get_offset(&start);
	select_end = gtk_text_iter_get_offset(&end);

	if (select_start != select_end) {
		gtk_text_buffer_get_iter_at_offset (buffer, &pos_itr, start_pos);
		gtk_text_buffer_move_mark_by_name (buffer, "selection_bound", &pos_itr);
		gtk_text_buffer_get_iter_at_offset (buffer, &pos_itr, end_pos);
		gtk_text_buffer_move_mark_by_name (buffer, "insert", &pos_itr);

		retval = TRUE;
	}

	return retval;
}

static void
ev_view_accessible_text_iface_init (AtkTextIface * iface)
{
	iface->get_text = ev_view_accessible_get_text;
	iface->get_character_at_offset = ev_view_accessible_get_character_at_offset;
	iface->get_text_before_offset = ev_view_accessible_get_text_before_offset;
	iface->get_text_at_offset = ev_view_accessible_get_text_at_offset;
	iface->get_text_after_offset = ev_view_accessible_get_text_after_offset;
	iface->get_caret_offset = ev_view_accessible_get_caret_offset;
	iface->set_caret_offset = ev_view_accessible_set_caret_offset;
	iface->get_character_count = ev_view_accessible_get_character_count;
	iface->get_n_selections = ev_view_accessible_get_n_selections;
	iface->get_selection = ev_view_accessible_get_selection;
	iface->add_selection = ev_view_accessible_add_selection;
	iface->remove_selection = ev_view_accessible_remove_selection;
	iface->set_selection = ev_view_accessible_set_selection;
	iface->get_run_attributes = ev_view_accessible_get_run_attributes;
	iface->get_default_attributes = ev_view_accessible_get_default_attributes;
	iface->get_character_extents = ev_view_accessible_get_character_extents;
	iface->get_offset_at_point = ev_view_accessible_get_offset_at_point;
}

static gboolean
ev_view_accessible_idle_do_action (gpointer data)
{
	EvViewAccessiblePrivate* priv = EV_VIEW_ACCESSIBLE (data)->priv;

	ev_view_scroll (EV_VIEW (gtk_accessible_get_widget (GTK_ACCESSIBLE (data))),
	                priv->idle_scroll,
	                FALSE);
	priv->action_idle_handler = 0;
	return FALSE;
}

static gboolean
ev_view_accessible_action_do_action (AtkAction *action,
				     gint       i)
{
	EvViewAccessiblePrivate* priv = EV_VIEW_ACCESSIBLE (action)->priv;

	if (gtk_accessible_get_widget (GTK_ACCESSIBLE (action)) == NULL)
		return FALSE;

	if (priv->action_idle_handler)
		return FALSE;

	switch (i) {
	case ACTION_SCROLL_UP:
		priv->idle_scroll = GTK_SCROLL_PAGE_BACKWARD;
		break;
	case ACTION_SCROLL_DOWN:
		priv->idle_scroll = GTK_SCROLL_PAGE_FORWARD;
		break;
	default:
		return FALSE;
	}
	priv->action_idle_handler = g_idle_add (ev_view_accessible_idle_do_action,
	                                        action);
	return TRUE;
}

static gint
ev_view_accessible_action_get_n_actions (AtkAction *action)
{
	return LAST_ACTION;
}

static const gchar *
ev_view_accessible_action_get_description (AtkAction *action,
					   gint       i)
{
	EvViewAccessiblePrivate* priv = EV_VIEW_ACCESSIBLE (action)->priv;

	if (i < 0 || i >= LAST_ACTION)
		return NULL;

	if (priv->action_descriptions[i])
		return priv->action_descriptions[i];
	else
		return ev_view_accessible_action_descriptions[i];
}

static const gchar *
ev_view_accessible_action_get_name (AtkAction *action,
				    gint       i)
{
	if (i < 0 || i >= LAST_ACTION)
		return NULL;

	return ev_view_accessible_action_names[i];
}

static gboolean
ev_view_accessible_action_set_description (AtkAction   *action,
					   gint         i,
					   const gchar *description)
{
	EvViewAccessiblePrivate* priv = EV_VIEW_ACCESSIBLE (action)->priv;
	gchar *old_description;

	if (i < 0 || i >= LAST_ACTION)
		return FALSE;

	old_description = priv->action_descriptions[i];
	priv->action_descriptions[i] = g_strdup (description);
	g_free (old_description);

	return TRUE;
}

static void
ev_view_accessible_action_iface_init (AtkActionIface * iface)
{
	iface->do_action = ev_view_accessible_action_do_action;
	iface->get_n_actions = ev_view_accessible_action_get_n_actions;
	iface->get_description = ev_view_accessible_action_get_description;
	iface->get_name = ev_view_accessible_action_get_name;
	iface->set_description = ev_view_accessible_action_set_description;
}

static GHashTable *
ev_view_accessible_get_links (EvViewAccessible *accessible,
			      EvView           *view)
{
	EvViewAccessiblePrivate* priv = accessible->priv;

	if (view->current_page == priv->current_page && priv->links)
		return priv->links;

	priv->current_page = view->current_page;

	if (priv->links)
		g_hash_table_destroy (priv->links);
	priv->links = g_hash_table_new_full (g_direct_hash,
					     g_direct_equal,
					     NULL,
					     (GDestroyNotify)g_object_unref);
	return priv->links;
}

static AtkHyperlink *
ev_view_accessible_get_link (AtkHypertext *hypertext,
			     gint          link_index)
{
	GtkWidget        *widget;
	EvView           *view;
	GHashTable       *links;
	EvMappingList    *link_mapping;
	gint              n_links;
	EvMapping        *mapping;
	EvLinkAccessible *atk_link;

	widget = gtk_accessible_get_widget (GTK_ACCESSIBLE (hypertext));
	if (widget == NULL)
		/* State is defunct */
		return NULL;

	view = EV_VIEW (widget);
	if (!EV_IS_DOCUMENT_LINKS (view->document))
		return NULL;

	links = ev_view_accessible_get_links (EV_VIEW_ACCESSIBLE (hypertext), view);

	atk_link = g_hash_table_lookup (links, GINT_TO_POINTER (link_index));
	if (atk_link)
		return atk_hyperlink_impl_get_hyperlink (ATK_HYPERLINK_IMPL (atk_link));

	link_mapping = ev_page_cache_get_link_mapping (view->page_cache, view->current_page);
	if (!link_mapping)
		return NULL;

	n_links = ev_mapping_list_length (link_mapping);
	mapping = ev_mapping_list_nth (link_mapping, n_links - link_index - 1);
	atk_link = ev_link_accessible_new (EV_VIEW_ACCESSIBLE (hypertext),
					   EV_LINK (mapping->data),
					   &mapping->area);
	g_hash_table_insert (links, GINT_TO_POINTER (link_index), atk_link);

	return atk_hyperlink_impl_get_hyperlink (ATK_HYPERLINK_IMPL (atk_link));
}

static gint
ev_view_accessible_get_n_links (AtkHypertext *hypertext)
{
	GtkWidget     *widget;
	EvView        *view;
	EvMappingList *link_mapping;

	widget = gtk_accessible_get_widget (GTK_ACCESSIBLE (hypertext));
	if (widget == NULL)
		/* State is defunct */
		return 0;

	view = EV_VIEW (widget);
	if (!EV_IS_DOCUMENT_LINKS (view->document))
		return 0;

	link_mapping = ev_page_cache_get_link_mapping (view->page_cache, view->current_page);

	return link_mapping ? ev_mapping_list_length (link_mapping) : 0;
}

static gint
ev_view_accessible_get_link_index (AtkHypertext *hypertext,
				   gint          offset)
{
	guint i;

	for (i = 0; i < ev_view_accessible_get_n_links (hypertext); i++) {
		AtkHyperlink *hyperlink;
		gint          start_index, end_index;

		hyperlink = ev_view_accessible_get_link (hypertext, i);
		start_index = atk_hyperlink_get_start_index (hyperlink);
		end_index = atk_hyperlink_get_end_index (hyperlink);

		if (start_index <= offset && end_index >= offset)
			return i;
	}

	return -1;
}

static void
ev_view_accessible_hypertext_iface_init (AtkHypertextIface *iface)
{
	iface->get_link = ev_view_accessible_get_link;
	iface->get_n_links = ev_view_accessible_get_n_links;
	iface->get_link_index = ev_view_accessible_get_link_index;
}

AtkObject *
ev_view_accessible_new (GtkWidget *widget)
{
	AtkObject *accessible;

	g_return_val_if_fail (EV_IS_VIEW (widget), NULL);

	accessible = g_object_new (EV_TYPE_VIEW_ACCESSIBLE, NULL);
	atk_object_initialize (accessible, widget);

	return accessible;
}

