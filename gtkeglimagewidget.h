#pragma once

#include <epoxy/egl.h>
#include <gtk/gtk.h>

#define GTK_TYPE_EGL_IMAGE_WIDGET (gtk_egl_image_widget_get_type ())
G_DECLARE_DERIVABLE_TYPE (GtkEglImageWidget, gtk_egl_image_widget, GTK, EGL_IMAGE_WIDGET, GtkWidget)

struct _GtkEglImageWidgetClass
{
  GtkWidgetClass parent_class;

  EGLImage (* render) (GtkEglImageWidget *ewidget);
  void     (* resize) (GtkEglImageWidget *ewidget,
                       int                width,
                       int                height);
};

GtkWidget *gtk_egl_image_widget_new                (void);
EGLDisplay gtk_egl_image_widget_get_egl_display    (GtkEglImageWidget *ewidget);
gboolean   gtk_egl_image_widget_get_auto_render    (GtkEglImageWidget *ewidget);
void       gtk_egl_image_widget_set_auto_render    (GtkEglImageWidget *ewidget,
                                                    gboolean        auto_render);
void       gtk_egl_image_widget_queue_render       (GtkEglImageWidget *ewidget);
void       gtk_egl_image_widget_set_error          (GtkEglImageWidget *ewidget,
                                                    const GError   *error);
void       gtk_egl_image_widget_set_error_literal  (GtkEglImageWidget *ewidget,
                                                    const char *fmt,
                                                    ...) G_GNUC_PRINTF (2, 3);
void       gtk_egl_image_widget_set_last_egl_error (GtkEglImageWidget *ewidget,
                                                    const char *prefix);
GError *   gtk_egl_image_widget_get_error          (GtkEglImageWidget *ewidget);
