// gcc -o test-media-window-clutter `pkg-config --libs --cflags clutter-1.0 gtk+-3.0 cairo` -I../../ -lm test-media-window-clutter.c gsd-osd-window.c

#include <stdlib.h>

#include <gtk/gtk.h>
#include <clutter/clutter.h>

#include "gsd-osd-window.h"
#include "gsd-osd-window-private.h"

static gboolean
draw_box (ClutterCanvas     *canvas,
          cairo_t           *cr,
          int                width,
          int                height,
          GsdOsdDrawContext *ctx)
{
	cairo_save (cr);
	cairo_set_operator (cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint (cr);

	cairo_restore (cr);
	cairo_set_operator (cr, CAIRO_OPERATOR_OVER);

	ctx->size = MIN(width, height);

	ctx->action = GSD_OSD_WINDOW_ACTION_CUSTOM;
	ctx->icon_name = "touchpad-disabled-symbolic";

	gsd_osd_window_draw (ctx, cr);

	return FALSE;
}

static void
test_window (void)
{
  GsdOsdDrawContext ctx;
  ClutterActor *stage, *actor;
  ClutterContent *canvas;
  GtkWidget *widget;

  /* create a resizable stage */
  stage = clutter_stage_new ();
  clutter_stage_set_title (CLUTTER_STAGE (stage), "OSD Test");
  clutter_stage_set_user_resizable (CLUTTER_STAGE (stage), TRUE);
  clutter_actor_set_background_color (stage, CLUTTER_COLOR_Red);
  clutter_actor_set_size (stage, 300, 300);
  clutter_actor_show (stage);

  /* box canvas */
  canvas = clutter_canvas_new ();
  clutter_canvas_set_size (CLUTTER_CANVAS (canvas), 300, 300);

  actor = clutter_actor_new ();
  clutter_actor_add_constraint (actor, clutter_bind_constraint_new (stage, CLUTTER_BIND_SIZE, 0));
  clutter_actor_set_content (actor, canvas);
  g_object_unref (canvas);

  clutter_actor_add_child (stage, actor);

  memset (&ctx, 0, sizeof(ctx));
  widget = gsd_osd_window_new ();
  ctx.style = gtk_widget_get_style_context (widget);
  ctx.direction = gtk_widget_get_direction (GTK_WIDGET (widget));;
  ctx.theme = gtk_icon_theme_get_default ();

  g_signal_connect (canvas, "draw", G_CALLBACK (draw_box), &ctx);
  clutter_content_invalidate (canvas);

  g_signal_connect (stage, "destroy", G_CALLBACK (gtk_main_quit), NULL);

  clutter_content_invalidate (canvas);
}

int
main (int    argc,
      char **argv)
{
        GError *error = NULL;

        if (! gtk_init_with_args (&argc, &argv, NULL, NULL, NULL, &error)) {
                fprintf (stderr, "%s", error->message);
                g_error_free (error);
                exit (1);
        }
	if (clutter_init (&argc, &argv) != CLUTTER_INIT_SUCCESS)
		return EXIT_FAILURE;

        test_window ();

        gtk_main ();

        return 0;
}
