#include <epoxy/gl.h>
#include <gtk/gtk.h>
#include <GL/glu.h>

#include "gtkeglimagewidget.h"

#define APP_NAME "org.example.EglImageWidgetGL2Example"

#define EXAMPLE_TYPE_GL2_CUBE (example_gl2_cube_get_type ())
G_DECLARE_FINAL_TYPE (ExampleGl2Cube, example_gl2_cube, EXAMPLE, GL2_CUBE, GtkEglImageWidget)

struct _ExampleGl2Cube
{
  GtkEglImageWidget parent_instance;

  EGLDisplay display;
  EGLContext context;
  GLuint fb;
  GLuint rb;
  gint64 start_time;
};

G_DEFINE_TYPE (ExampleGl2Cube, example_gl2_cube, GTK_TYPE_EGL_IMAGE_WIDGET);

static gboolean
tick (GtkWidget *widget, GdkFrameClock *frame_clock, gpointer user_data)
{
  gtk_widget_queue_draw (widget);
  return TRUE;
}

static void
example_gl2_cube_init (ExampleGl2Cube *cube)
{
  gtk_widget_add_tick_callback (GTK_WIDGET (cube), tick, NULL, NULL);
}

static void
example_gl2_cube_resize (GtkEglImageWidget *ewidget, int width, int height)
{
  ExampleGl2Cube *cube = EXAMPLE_GL2_CUBE (ewidget);

  if (!eglMakeCurrent (cube->display, EGL_NO_SURFACE, EGL_NO_SURFACE, cube->context))
    return;
  if (!eglBindAPI (EGL_OPENGL_API))
    return;

  glViewport (0, 0, width, height);
  glMatrixMode (GL_PROJECTION);
  glLoadIdentity ();
  gluPerspective (45.0f, width / (float) height, 0.1f, 100.0f);
  glMatrixMode (GL_MODELVIEW);
}

static EGLImage
example_gl2_cube_render (GtkEglImageWidget *ewidget)
{
  ExampleGl2Cube *cube = EXAMPLE_GL2_CUBE (ewidget);
  GLuint tex;
  int width, height;
  EGLImage image;
  gint64 cur_time;

  if (!eglMakeCurrent (cube->display, EGL_NO_SURFACE, EGL_NO_SURFACE, cube->context))
    return EGL_NO_IMAGE;
  if (!eglBindAPI (EGL_OPENGL_API))
    return EGL_NO_IMAGE;

  width = gtk_widget_get_width (GTK_WIDGET (GTK_EGL_IMAGE_WIDGET (cube)));
  height = gtk_widget_get_height (GTK_WIDGET (GTK_EGL_IMAGE_WIDGET (cube)));

  glGenTextures (1, &tex);
  glBindTexture (GL_TEXTURE_2D, tex);
  glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glRenderbufferStorage (GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, width, height);
  glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

  cur_time = g_get_monotonic_time ();
  if (cube->start_time < 0)
    cube->start_time = cur_time;

  glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glLoadIdentity ();
  glTranslatef (0.f, 0.f, -5.0f);
  glRotatef (((cur_time - cube->start_time) % (1000000 * 10)) / (1000000. * 10.) * 360.,
             1.f, 1.f, 1.f);

  glBegin (GL_QUADS);
    glColor3f (0.f, 1.f, 0.f);
    glVertex3f ( 1.f, 1.f,-1.f);
    glVertex3f (-1.f, 1.f,-1.f);
    glVertex3f (-1.f, 1.f, 1.f);
    glVertex3f ( 1.f, 1.f, 1.f);

    glColor3f (1.f, .5f, 0.f);
    glVertex3f ( 1.f,-1.f, 1.f);
    glVertex3f (-1.f,-1.f, 1.f);
    glVertex3f (-1.f,-1.f,-1.f);
    glVertex3f ( 1.f,-1.f,-1.f);

    glColor3f (1.f, 0.f, 0.f);
    glVertex3f ( 1.f, 1.f, 1.f);
    glVertex3f (-1.f, 1.f, 1.f);
    glVertex3f (-1.f,-1.f, 1.f);
    glVertex3f ( 1.f,-1.f, 1.f);

    glColor3f (1.f, 1.f, 0.f);
    glVertex3f ( 1.f,-1.f,-1.f);
    glVertex3f (-1.f,-1.f,-1.f);
    glVertex3f (-1.f, 1.f,-1.f);
    glVertex3f ( 1.f, 1.f,-1.f);

    glColor3f (0.f, 0.f, 1.f);
    glVertex3f (-1.f, 1.f, 1.f);
    glVertex3f (-1.f, 1.f,-1.f);
    glVertex3f (-1.f,-1.f,-1.f);
    glVertex3f (-1.f,-1.f, 1.f);

    glColor3f (1.f, 0.f, 1.f);
    glVertex3f ( 1.f, 1.f,-1.f);
    glVertex3f ( 1.f, 1.f, 1.f);
    glVertex3f ( 1.f,-1.f, 1.f);
    glVertex3f ( 1.f,-1.f,-1.f);
  glEnd ();
  glFinish ();

  image = eglCreateImage (cube->display,
                          cube->context,
                          EGL_GL_TEXTURE_2D,
                          (EGLClientBuffer) (GLintptr) tex,
                          NULL);

  glBindTexture (GL_TEXTURE_2D, 0);
  glDeleteTextures (1, &tex);

  if (image == EGL_NO_IMAGE)
    gtk_egl_image_widget_set_last_egl_error (GTK_EGL_IMAGE_WIDGET (cube), "eglCreateImage");

  return image;
}

static void
example_gl2_cube_realize (GtkWidget *widget)
{
  ExampleGl2Cube *cube = EXAMPLE_GL2_CUBE (widget);
  EGLConfig config;
  EGLint num_configs;

  const EGLint config_attribs[] = {
    EGL_RED_SIZE,             8,
    EGL_GREEN_SIZE,           8,
    EGL_BLUE_SIZE,            8,
    EGL_ALPHA_SIZE,           8,
    EGL_DEPTH_SIZE,           0,
    EGL_CONFORMANT,           EGL_OPENGL_BIT,
    EGL_RENDERABLE_TYPE,      EGL_OPENGL_BIT,
    EGL_NONE,
  };
  const EGLint ctx_attribs[] = {
    EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,
    EGL_CONTEXT_MAJOR_VERSION, 2,
    EGL_CONTEXT_MINOR_VERSION, 1,
    EGL_NONE,
  };

  GTK_WIDGET_CLASS (example_gl2_cube_parent_class)->realize (widget);

  cube->display = gtk_egl_image_widget_get_egl_display (GTK_EGL_IMAGE_WIDGET (cube));
  if (!cube->display)
    return;
  if (!eglBindAPI (EGL_OPENGL_API))
    {
      gtk_egl_image_widget_set_last_egl_error (GTK_EGL_IMAGE_WIDGET (cube), "Main eglBindAPI");
      return;
    }
  if (!eglChooseConfig (cube->display, config_attribs, &config, 1, &num_configs))
    {
      gtk_egl_image_widget_set_last_egl_error (GTK_EGL_IMAGE_WIDGET (cube), "Main eglChooseConfig");
      return;
    }
  if (num_configs < 1)
    {
      gtk_egl_image_widget_set_error_literal (GTK_EGL_IMAGE_WIDGET (cube), "Main no valid EGL configs");
      return;
    }
  cube->context = eglCreateContext (cube->display, config, EGL_NO_CONTEXT, ctx_attribs);
  if (cube->context == EGL_NO_CONTEXT)
    {
      gtk_egl_image_widget_set_last_egl_error (GTK_EGL_IMAGE_WIDGET (cube), "Main eglCreateContext");
      return;
    }
  if (!eglMakeCurrent (cube->display, EGL_NO_SURFACE, EGL_NO_SURFACE, cube->context))
    {
      gtk_egl_image_widget_set_last_egl_error (GTK_EGL_IMAGE_WIDGET (cube), "Main eglMakeCurrent");
      return;
    }

  glGenFramebuffers (1, &cube->fb);
  glGenRenderbuffers (1, &cube->rb);

  glBindFramebuffer (GL_FRAMEBUFFER, cube->fb);
  glBindRenderbuffer (GL_RENDERBUFFER, cube->rb);
  glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, cube->rb);

  glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
  glClearDepth (1.0);
  glDepthFunc (GL_LESS);
  glEnable (GL_DEPTH_TEST);
  glShadeModel (GL_SMOOTH);
}

static void
example_gl2_cube_unrealize (GtkWidget *widget)
{
  ExampleGl2Cube *cube = EXAMPLE_GL2_CUBE (widget);

  if (eglMakeCurrent (cube->display, EGL_NO_SURFACE, EGL_NO_SURFACE, cube->context))
    {
      if (cube->fb)
        glDeleteFramebuffers (1, &cube->fb);
      if (cube->rb)
        glDeleteRenderbuffers (1, &cube->rb);
    }
  if (cube->context != EGL_NO_CONTEXT)
    eglDestroyContext (cube->display, cube->context);

  GTK_WIDGET_CLASS (example_gl2_cube_parent_class)->unrealize (widget);
}

static void
example_gl2_cube_class_init (ExampleGl2CubeClass *class)
{
  GtkEglImageWidgetClass *ei_class = GTK_EGL_IMAGE_WIDGET_CLASS (class);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (class);

  ei_class->render = example_gl2_cube_render;
  ei_class->resize = example_gl2_cube_resize;

  widget_class->realize = example_gl2_cube_realize;
  widget_class->unrealize = example_gl2_cube_unrealize;
}

static void
build_ui (GtkApplication *app)
{
  GtkWidget *window;
  GtkWidget *cube;

  if (gtk_application_get_windows (app) != NULL)
    return;

  window = gtk_application_window_new (app);
  gtk_window_set_default_size (GTK_WINDOW (window), 400, 400);
  cube = g_object_new (EXAMPLE_TYPE_GL2_CUBE, NULL);
  gtk_window_set_child (GTK_WINDOW (window), cube);
  gtk_window_present (GTK_WINDOW (window));
}

int
main (int argc, char *argv[])
{
  g_autoptr (GtkApplication) app = NULL;

  app = gtk_application_new (APP_NAME, G_APPLICATION_FLAGS_NONE);
  g_signal_connect (app, "activate", G_CALLBACK (build_ui), NULL);
  return g_application_run (G_APPLICATION (app), argc, argv);
}
