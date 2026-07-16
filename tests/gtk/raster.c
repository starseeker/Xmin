#include <gtk/gtk.h>

#include <stdio.h>

static gboolean painted;

static gboolean
paint_green(GtkWidget *widget, cairo_t *graphics, gpointer data)
{
    (void) widget;
    (void) data;
    cairo_set_source_rgb(graphics, 0.0, 1.0, 0.0);
    cairo_paint(graphics);
    painted = TRUE;
    return FALSE;
}

int
main(int argc, char **argv)
{
    GtkWidget *window;
    GtkWidget *drawing_area;
    GdkPixbuf *capture = NULL;
    gint64 deadline;
    int result = 1;

    if (!gtk_init_check(&argc, &argv)) {
        fprintf(stderr, "GTK 3 could not open the Xmin display\n");
        return 2;
    }
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(drawing_area, 64, 64);
    gtk_container_add(GTK_CONTAINER(window), drawing_area);
    g_signal_connect(drawing_area, "draw", G_CALLBACK(paint_green), NULL);
    gtk_widget_show_all(window);
    gtk_widget_queue_draw(drawing_area);

    deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while ((!painted || gtk_events_pending()) &&
           g_get_monotonic_time() < deadline) {
        gtk_main_iteration_do(FALSE);
        g_usleep(1000);
    }
    if (!painted || gtk_widget_get_window(drawing_area) == NULL)
        goto cleanup;

    capture = gdk_pixbuf_get_from_window(gtk_widget_get_window(drawing_area),
                                          32, 32, 1, 1);
    if (capture == NULL || gdk_pixbuf_get_n_channels(capture) < 3)
        goto cleanup;
    {
        const guchar *pixel = gdk_pixbuf_get_pixels(capture);
        if (pixel[1] < 200 || pixel[0] > 40 || pixel[2] > 40)
            goto cleanup;
    }
    result = 0;

cleanup:
    if (capture != NULL)
        g_object_unref(capture);
    gtk_widget_destroy(window);
    while (gtk_events_pending())
        gtk_main_iteration_do(FALSE);
    return result;
}
