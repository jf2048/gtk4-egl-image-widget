#include <X11/Xlib-xcb.h>
#include <drm_fourcc.h>
#include <epoxy/gl.h>
#include <epoxy/glx.h>
#include <gdk/wayland/gdkwayland.h>
#include <gdk/x11/gdkx.h>
#include <gtk/gtk.h>
#include <xcb/dri3.h>
#include <xcb/glx.h>

#include "gtkeglimagewidget.h"

typedef struct
{
  EGLDisplay     display;
  EGLint         platform;
  EGLContext     egl_context;
  struct {
    Display     *display;
    GLXFBConfig  fb_config;
    int          screen;
    int          fb_depth;
  } x11;
  GdkGLContext  *gdk_context;
  EGLenum        gdk_api;
  GdkTexture    *texture;
  GError        *error;
  GtkWidget     *label;
  GskGLShader   *swap_shader;
  gboolean       needs_resize: 1;
  gboolean       needs_render: 1;
  gboolean       auto_render: 1;
  gboolean       swap_rb: 1;
  gboolean       is_glx: 1;
  gboolean       owned_display: 1;
} GtkEglImageWidgetPrivate;

enum {
  PROP_0,
  PROP_AUTO_RENDER,
  LAST_PROP
};

static GParamSpec *props[LAST_PROP] = { NULL, };

enum {
  RENDER,
  RESIZE,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

G_DEFINE_TYPE_WITH_PRIVATE (GtkEglImageWidget, gtk_egl_image_widget, GTK_TYPE_WIDGET);

static const char *
egl_error_str (void);

static void
gtk_egl_image_widget_init (GtkEglImageWidget *ewidget)
{
  GtkEglImageWidgetPrivate *priv = gtk_egl_image_widget_get_instance_private (ewidget);

  priv->auto_render = TRUE;
  priv->needs_render = TRUE;
}

typedef struct
{
  EGLDisplay display;
  EGLContext context;
  EGLImage   image;
} EGLTextureData;

static void
free_egl_texture_data (gpointer data)
{
  EGLTextureData *tdata = data;

  if (tdata->image != EGL_NO_IMAGE
      && (tdata->context == EGL_NO_CONTEXT
        || eglMakeCurrent (tdata->display, EGL_NO_SURFACE, EGL_NO_SURFACE, tdata->context)))
    eglDestroyImage (tdata->display, tdata->image);
  g_free (tdata);
}

static inline EGLDisplay
get_egl_display (EGLenum platform, gpointer native_display)
{
  if (epoxy_has_egl_extension (NULL, "EGL_EXT_platform_base"))
    return eglGetPlatformDisplayEXT (platform, native_display, NULL);
  if (epoxy_has_egl_extension (NULL, "EGL_KHR_platform_base"))
    return eglGetPlatformDisplay (platform, native_display, NULL);
  return eglGetDisplay (native_display);
}

static inline Bool
check_dri3_version (Display *disp)
{
  int op, ev, err;
  xcb_connection_t *conn;
  g_autofree xcb_dri3_query_version_reply_t *reply = NULL;

  if (!XQueryExtension (disp, "DRI3", &op, &ev, &err))
    return False;

  conn = XGetXCBConnection (disp);
  reply = xcb_dri3_query_version_reply (
      conn, xcb_dri3_query_version_unchecked (conn, 1, 2), NULL);
  if (!reply || reply->major_version < 1
      || (reply->major_version == 1 && reply->minor_version < 2))
    return False;

  return True;
}

static const char SWAP_SRC[] = "\
uniform sampler2D u_texture1;\n\
void mainImage(out vec4 fragColor, in vec2 fragCoord, in vec2 resolution, in vec2 uv)\n\
{\n\
  fragColor = GskTexture(u_texture1, uv).bgra;\n\
}\
";

static inline void
find_display (GtkEglImageWidget *ewidget)
{
  GtkEglImageWidgetPrivate *priv = gtk_egl_image_widget_get_instance_private (ewidget);
  GdkDisplay *gdk_display = gtk_widget_get_display (GTK_WIDGET (ewidget));
  EGLDisplay tmp_display;
  int major, minor;

  if (GDK_IS_WAYLAND_DISPLAY (gdk_display))
    {
      priv->display = gdk_wayland_display_get_egl_display (gdk_display);
      if (priv->display == EGL_NO_DISPLAY
          && (epoxy_has_egl_extension (NULL, "EGL_EXT_platform_wayland")
            || epoxy_has_egl_extension (NULL, "EGL_KHR_platform_wayland")))
        {
          tmp_display = get_egl_display (EGL_PLATFORM_WAYLAND_EXT,
                                         gdk_wayland_display_get_wl_display (gdk_display));
          if (tmp_display && eglInitialize (tmp_display, &major, &minor)
              && (major > 1 || (major == 1 && minor >= 4)))
            {
              priv->display = tmp_display;
              priv->owned_display = TRUE;
            }
        }
      if (priv->display)
        priv->platform = EGL_PLATFORM_WAYLAND_EXT;
    }
  else if (GDK_IS_X11_DISPLAY (gdk_display))
    {
      priv->display = gdk_x11_display_get_egl_display (gdk_display);
      if (priv->display == EGL_NO_DISPLAY)
        {
          Display *x11_display = gdk_x11_display_get_xdisplay (gdk_display);

          if (epoxy_has_egl_extension (NULL, "EGL_EXT_platform_x11")
              || epoxy_has_egl_extension (NULL, "EGL_KHR_platform_x11"))
            {
              tmp_display = get_egl_display (EGL_PLATFORM_X11_EXT, x11_display);
              if (tmp_display && eglInitialize (tmp_display, &major, &minor)
                  && (major > 1 || (major == 1 && minor >= 4)))
                {
                  priv->display = tmp_display;
                  priv->owned_display = TRUE;
                }
            }
        }
      if (priv->display != EGL_NO_DISPLAY)
        priv->platform = EGL_PLATFORM_X11_EXT;
    }

  if (priv->display == EGL_NO_DISPLAY)
    {
      if (epoxy_has_egl_extension (NULL, "EGL_MESA_platform_surfaceless"))
        {
          tmp_display = eglGetPlatformDisplay (EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);
          if (tmp_display && eglInitialize (tmp_display, &major, &minor)
              && (major > 1 || (major == 1 && minor >= 4)))
            {
              priv->display = tmp_display;
              priv->platform = EGL_PLATFORM_SURFACELESS_MESA;
              priv->owned_display = TRUE;
            }
        }
      else if (epoxy_has_egl_extension (NULL, "EGL_KHR_platform_gbm")
          || epoxy_has_egl_extension (NULL, "EGL_MESA_platform_gbm"))
        {
          tmp_display = eglGetPlatformDisplayEXT (EGL_PLATFORM_GBM_KHR, NULL, NULL);
          if (tmp_display && eglInitialize (tmp_display, &major, &minor)
              && (major > 1 || (major == 1 && minor >= 4)))
            {
              priv->display = tmp_display;
              priv->platform = EGL_PLATFORM_GBM_KHR;
              priv->owned_display = TRUE;
            }
        }
    }

  if (priv->display != EGL_NO_DISPLAY && GDK_IS_X11_DISPLAY (gdk_display)
      && priv->owned_display)
    {
      Display *x11_display = gdk_x11_display_get_xdisplay (gdk_display);
      GdkX11Screen *screen = gdk_x11_display_get_screen (gdk_display);
      int screen_num = gdk_x11_screen_get_screen_number (screen);

      if (gdk_x11_display_get_glx_version (gdk_display, &major, &minor)
          && (major > 1 || (major == 1 && minor >= 3))
          && epoxy_has_egl_extension (priv->display, "EGL_MESA_image_dma_buf_export")
          && epoxy_has_glx_extension (x11_display, screen_num,
                                      "GLX_EXT_texture_from_pixmap")
          && check_dri3_version (x11_display))
        {
          g_autoptr (GBytes) bytes = g_bytes_new_static (SWAP_SRC, sizeof SWAP_SRC);

          priv->is_glx = TRUE;
          priv->x11.display = x11_display;
          priv->x11.screen = screen_num;
          priv->swap_shader = gsk_gl_shader_new_from_bytes (bytes);
        }
      else
        g_clear_object (&priv->gdk_context);
    }
}

static inline void
clear_current_internal (GtkEglImageWidget *ewidget)
{
  GtkEglImageWidgetPrivate *priv = gtk_egl_image_widget_get_instance_private (ewidget);

  if (priv->gdk_context)
      gdk_gl_context_clear_current ();
    eglMakeCurrent (priv->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (priv->gdk_api)
      eglBindAPI (priv->gdk_api);
}

static inline EGLBoolean
make_current_internal (GtkEglImageWidget *ewidget)
{
  GtkEglImageWidgetPrivate *priv = gtk_egl_image_widget_get_instance_private (ewidget);

  g_assert (priv->gdk_context || priv->egl_context != EGL_NO_CONTEXT);

  clear_current_internal (ewidget);

  if (priv->gdk_context && !priv->is_glx)
    {
      if (priv->gdk_api)
        eglBindAPI (priv->gdk_api);
      gdk_gl_context_make_current (priv->gdk_context);
      return EGL_TRUE;
    }

  if (!eglMakeCurrent (priv->display, EGL_NO_SURFACE, EGL_NO_SURFACE, priv->egl_context))
    {
      gtk_egl_image_widget_set_last_egl_error (ewidget, "eglMakeCurrent");
      return EGL_FALSE;
    }

  if (!eglBindAPI (EGL_OPENGL_API))
    {
      gtk_egl_image_widget_set_last_egl_error (ewidget, "eglBindAPI");
      return EGL_FALSE;
    }

  return EGL_TRUE;
}

static inline EGLContext
create_rgba_context (GtkEglImageWidget *ewidget)
{
  const GtkEglImageWidgetPrivate *priv = gtk_egl_image_widget_get_instance_private (ewidget);
  EGLConfig config;
  EGLint num_configs;
  EGLContext context = EGL_NO_CONTEXT;
  const EGLint config_attribs[] = {
    EGL_RED_SIZE,             8,
    EGL_GREEN_SIZE,           8,
    EGL_BLUE_SIZE,            8,
    EGL_ALPHA_SIZE,           8,
    EGL_NONE,
  };
  const EGLint ctx_attribs[] = {
    EGL_CONTEXT_CLIENT_VERSION, 3,
    EGL_NONE,
  };

  g_assert (priv->display != EGL_NO_DISPLAY);

  if (!eglChooseConfig (priv->display, config_attribs, &config, 1, &num_configs))
    {
      gtk_egl_image_widget_set_last_egl_error (ewidget, "eglGetConfigs");
      return EGL_NO_CONTEXT;
    }
  if (num_configs < 1)
    {
      gtk_egl_image_widget_set_error_literal (ewidget, "No valid EGL configs");
      return EGL_NO_CONTEXT;
    }
  context = eglCreateContext (priv->display, config, EGL_NO_CONTEXT, ctx_attribs);
  if (context == EGL_NO_CONTEXT)
    {
      gtk_egl_image_widget_set_last_egl_error (ewidget, "eglCreateContext");
      return EGL_NO_CONTEXT;
    }
  return context;
}

static void
gtk_egl_image_widget_realize (GtkWidget *widget)
{
  GtkEglImageWidget *ewidget = GTK_EGL_IMAGE_WIDGET (widget);
  GtkEglImageWidgetPrivate *priv = gtk_egl_image_widget_get_instance_private (ewidget);
  GdkSurface *surface;
  gboolean has_oes_egl_image;

  g_clear_error (&priv->error);

  GTK_WIDGET_CLASS (gtk_egl_image_widget_parent_class)->realize (widget);

  surface = gtk_native_get_surface (GTK_NATIVE (gtk_widget_get_root (widget)));
  g_set_object (&priv->gdk_context, gdk_surface_create_gl_context (surface, NULL));

  if (priv->gdk_context)
    {
      gdk_gl_context_set_forward_compatible (priv->gdk_context, GL_TRUE);
      if (!gdk_gl_context_realize (priv->gdk_context, NULL))
        goto error;
      priv->gdk_api = eglQueryAPI ();
    }

  find_display (ewidget);

  if (priv->display == EGL_NO_DISPLAY)
    {
      gtk_egl_image_widget_set_error_literal (ewidget,
                                          "Could not create EGLDisplay for %s",
                                          G_OBJECT_TYPE_NAME (gtk_widget_get_display (widget)));
      goto error;
    }

  if (!priv->gdk_context || priv->is_glx)
    {
      priv->egl_context = create_rgba_context (ewidget);
      if (priv->egl_context == EGL_NO_CONTEXT)
        goto error;
    }

  if (!make_current_internal (ewidget))
    goto error;

  has_oes_egl_image = epoxy_has_gl_extension ("GL_OES_EGL_image");

  clear_current_internal (ewidget);

  if (!has_oes_egl_image)
    {
      gtk_egl_image_widget_set_error_literal (ewidget, "Missing extension: GL_OES_EGL_image");
      goto error;
    }

  priv->needs_resize = TRUE;

  return;
error:
  g_signal_stop_emission_by_name (ewidget, "realize");
}

static void
gtk_egl_image_widget_unrealize (GtkWidget *widget)
{
  GtkEglImageWidget *ewidget = GTK_EGL_IMAGE_WIDGET (widget);
  GtkEglImageWidgetPrivate *priv = gtk_egl_image_widget_get_instance_private (ewidget);
  GtkWidget *child;

  g_clear_object (&priv->gdk_context);
  g_clear_object (&priv->texture);
  g_clear_object (&priv->swap_shader);
  g_clear_error (&priv->error);
  priv->platform = EGL_FALSE;
  priv->gdk_api = EGL_FALSE;
  priv->label = NULL;
  priv->swap_rb = FALSE;
  priv->is_glx = FALSE;
  priv->owned_display = FALSE;

  while ((child = gtk_widget_get_first_child (widget)) != NULL)
    gtk_widget_unparent (child);

  GTK_WIDGET_CLASS (gtk_egl_image_widget_parent_class)->unrealize (widget);

  if (priv->display)
    {
      if (priv->egl_context != EGL_NO_CONTEXT)
        {
          eglDestroyContext (priv->display, priv->egl_context);
          priv->egl_context = EGL_NO_CONTEXT;
        }
      if (priv->owned_display)
        eglTerminate (priv->display);
      if (priv->platform == EGL_PLATFORM_X11_EXT)
        memset (&priv->x11, 0, sizeof priv->x11);
    }
  priv->display = EGL_NO_DISPLAY;
}

static void
gtk_egl_image_widget_size_allocate (GtkWidget *widget,
                                    int        width,
                                    int        height,
                                    int        baseline)
{
  GtkEglImageWidget *ewidget = GTK_EGL_IMAGE_WIDGET (widget);
  GtkEglImageWidgetPrivate *priv = gtk_egl_image_widget_get_instance_private (ewidget);
  GtkWidget *child = gtk_widget_get_first_child (widget);

  if (child)
    {
      gtk_widget_measure (child, GTK_ORIENTATION_HORIZONTAL, width, NULL, NULL, NULL, NULL);
      gtk_widget_size_allocate (child, &(GtkAllocation) { 0, 0, width, height }, baseline);
    }

  if (gtk_widget_get_realized (widget))
    priv->needs_resize = TRUE;
}

static gboolean
queue_alloc (gpointer user_data)
{
  gtk_widget_queue_allocate (GTK_WIDGET (user_data));
  return FALSE;
}

typedef struct
{
  xcb_connection_t *conn;
  Display *display;
  Pixmap pixmap;
  GLXPixmap glxpixmap;
} GLXTextureData;

static void
free_glx_texture_data (gpointer data)
{
  GLXTextureData *tdata = data;

  glXReleaseTexImageEXT (tdata->display, tdata->glxpixmap, GLX_FRONT_LEFT_EXT);
  glXDestroyGLXPixmap (tdata->display, tdata->glxpixmap);
  xcb_free_pixmap (tdata->conn, tdata->pixmap);
  g_free (tdata);
}

static uint32_t
depth_for_format (uint32_t format)
{
  switch (format)
    {
    case DRM_FORMAT_XRGB1555:
    case DRM_FORMAT_XBGR1555:
      return 15;
    case DRM_FORMAT_RGB565:
    case DRM_FORMAT_BGR565:
      return 16;
    case DRM_FORMAT_XRGB8888:
    case DRM_FORMAT_XBGR8888:
      return 24;
    case DRM_FORMAT_XRGB2101010:
    case DRM_FORMAT_XBGR2101010:
      return 30;
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_ABGR8888:
      return 32;
    case DRM_FORMAT_ARGB16161616F:
    case DRM_FORMAT_ABGR16161616F:
      return 64;
    default:
      return 0;
    }
}

static uint32_t
bpp_for_format (uint32_t format) {
  switch (format)
    {
    case DRM_FORMAT_XRGB1555:
    case DRM_FORMAT_XBGR1555:
    case DRM_FORMAT_RGB565:
    case DRM_FORMAT_BGR565:
      return 2;
    case DRM_FORMAT_XRGB8888:
    case DRM_FORMAT_XBGR8888:
    case DRM_FORMAT_XRGB2101010:
    case DRM_FORMAT_XBGR2101010:
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_ABGR8888:
      return 4;
    case DRM_FORMAT_ARGB16161616F:
    case DRM_FORMAT_ABGR16161616F:
      return 8;
    default:
      return 0;
    }
}

static gboolean
swapped_for_format (uint32_t format)
{
  switch (format)
    {
    case DRM_FORMAT_XBGR1555:
    case DRM_FORMAT_BGR565:
    case DRM_FORMAT_XBGR8888:
    case DRM_FORMAT_XBGR2101010:
    case DRM_FORMAT_ABGR8888:
    case DRM_FORMAT_ABGR16161616F:
      return TRUE;
    default:
      return FALSE;
    }
}

static void
gtk_egl_image_widget_update_image_glx (GtkEglImageWidget *ewidget, EGLImage image)
{
  GtkEglImageWidgetPrivate *priv = gtk_egl_image_widget_get_instance_private (ewidget);
  int width = gtk_widget_get_width (GTK_WIDGET (ewidget));
  int height = gtk_widget_get_height (GTK_WIDGET (ewidget));
  int fourcc, num_planes;
  EGLuint64KHR modifiers;
  int fds[4] = { -1, -1, -1, -1 };
  EGLint strides[4] = { 0, };
  EGLint offsets[4] = { 0, };
  xcb_connection_t *conn;
  GtkRoot *root;
  GdkSurface *surface;
  Window win;
  guint depth, bpp;
  Pixmap pixmap;
  xcb_void_cookie_t cookie;
  GLXPixmap glxpixmap;
  GLXTextureData *texdata;
  GLuint texid;
  g_autoptr (GdkTexture) texture = NULL;
  static const int pixmap_attribs[] = {
    GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT,
    GLX_TEXTURE_FORMAT_EXT, GLX_TEXTURE_FORMAT_RGBA_EXT,
    None
  };

  if (!make_current_internal (ewidget))
    return;

  if (!eglExportDMABUFImageQueryMESA (priv->display, image, &fourcc, &num_planes, &modifiers))
    {
      gtk_egl_image_widget_set_last_egl_error (ewidget, "eglExportDMABUFImageQueryMESA");
      return;
    }
  g_assert (num_planes >= 1 && num_planes <= 4);
  depth = depth_for_format (fourcc);
  bpp = bpp_for_format (fourcc) * 8;

  if (!depth || !bpp)
    {
      gtk_egl_image_widget_set_error_literal (
          ewidget, "Unsupported DMABUF format 0x%08x for GLX import", fourcc);
      return;
    }

  if (!eglExportDMABUFImageMESA (priv->display, image, fds, strides, offsets))
    {
      gtk_egl_image_widget_set_last_egl_error (ewidget, "eglExportDMABUFImageMESA");
      return;
    }

  clear_current_internal (ewidget);
  gdk_gl_context_make_current (priv->gdk_context);

  conn = XGetXCBConnection (priv->x11.display);
  root = gtk_widget_get_root (GTK_WIDGET (ewidget));
  surface = gtk_native_get_surface (GTK_NATIVE (root));
  win = gdk_x11_surface_get_xid (surface);

  if (priv->x11.fb_config == NULL || depth != priv->x11.fb_depth)
    {
      GLXFBConfig *configs;
      int num_configs;
      GLXFBConfig config = NULL;
      const int config_attribs[] = {
          GLX_BIND_TO_TEXTURE_RGBA_EXT,    GL_TRUE,
          GLX_BIND_TO_TEXTURE_TARGETS_EXT, GLX_TEXTURE_2D_BIT_EXT,
          GLX_DOUBLEBUFFER,                GL_FALSE,
          GLX_DRAWABLE_TYPE,               GLX_PIXMAP_BIT,
          None
      };

      configs = glXChooseFBConfig (priv->x11.display, priv->x11.screen,
                                   config_attribs, &num_configs);

      for (int i = 0; i < num_configs; i++)
        {
          GLXFBConfig c = configs[i];
          XVisualInfo *visual = glXGetVisualFromFBConfig (priv->x11.display, c);
          gboolean found = visual && visual->depth == depth;
          XFree (visual);

          if (!found)
            continue;
          config = c;
          break;
        }

      XFree (configs);

      if (config == NULL)
        {
          gtk_egl_image_widget_set_error_literal (
              ewidget, "No compatible GLXFBConfig found for depth %d", depth);
          for (int i = 0; i < num_planes; i++)
            if (fds[i] != -1)
              close (fds[i]);
          gdk_gl_context_clear_current ();
          return;
        }

      priv->x11.fb_config = config;
      priv->x11.fb_depth = depth;
    }

  pixmap = xcb_generate_id (conn);

  if ((modifiers != DRM_FORMAT_MOD_INVALID && modifiers != DRM_FORMAT_MOD_LINEAR) || num_planes > 1)
    cookie =
       xcb_dri3_pixmap_from_buffers_checked (conn, pixmap, win, num_planes,
                                             width, height,
                                             strides[0], offsets[0],
                                             strides[1], offsets[1],
                                             strides[2], offsets[2],
                                             strides[3], offsets[3],
                                             depth, bpp, modifiers, fds);
  else
    cookie =
       xcb_dri3_pixmap_from_buffer_checked (conn, pixmap, win,
                                            height * strides[0],
                                            width, height, strides[0],
                                            depth, bpp, fds[0]);

  xcb_discard_reply (conn, cookie.sequence);

  glxpixmap = glXCreatePixmap (priv->x11.display, priv->x11.fb_config,
                               pixmap, pixmap_attribs);

  texdata = g_new0 (GLXTextureData, 1);
  texdata->conn = conn;
  texdata->display = priv->x11.display;
  texdata->pixmap = pixmap;
  texdata->glxpixmap = glxpixmap;

  glGenTextures (1, &texid);
  glBindTexture (GL_TEXTURE_2D, texid);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glXBindTexImageEXT (priv->x11.display, texdata->glxpixmap, GLX_FRONT_LEFT_EXT, NULL);
  glBindTexture (GL_TEXTURE_2D, 0);

  texture = gdk_gl_texture_new (priv->gdk_context, texid, width, height,
                                free_glx_texture_data, texdata);
  g_set_object (&priv->texture, texture);
  priv->swap_rb = swapped_for_format (fourcc);
  gdk_gl_context_clear_current ();
}

static void
gtk_egl_image_widget_update_image (GtkEglImageWidget *ewidget)
{
  GtkEglImageWidgetPrivate *priv = gtk_egl_image_widget_get_instance_private (ewidget);
  int width = gtk_widget_get_width (GTK_WIDGET (ewidget));
  int height = gtk_widget_get_height (GTK_WIDGET (ewidget));
  EGLImage image = EGL_NO_IMAGE;
  GLuint fbid, texid;
  g_autoptr (GdkTexture) texture = NULL;

  clear_current_internal (ewidget);

  g_signal_emit (ewidget, signals[RENDER], 0, &image);

  if (image == EGL_NO_IMAGE)
    return;
  if (priv->is_glx)
    {
      gtk_egl_image_widget_update_image_glx (ewidget, image);
      if (!make_current_internal (ewidget))
        return;
      eglDestroyImage (priv->display, image);
      clear_current_internal (ewidget);
      return;
    }
  if (!make_current_internal (ewidget))
    return;

  glGenTextures (1, &texid);
  if (priv->gdk_context)
    {
      EGLTextureData *texdata = g_new0 (EGLTextureData, 1);

      texdata->display = priv->display;
      texdata->context = priv->egl_context;
      texdata->image = image;

      glBindTexture (GL_TEXTURE_2D, texid);
      glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glEGLImageTargetTexture2DOES (GL_TEXTURE_2D, image);
      glBindTexture (GL_TEXTURE_2D, 0);
      texture = gdk_gl_texture_new (priv->gdk_context, texid, width, height,
                                    free_egl_texture_data, texdata);
    }
  else
    {
      const gsize size = width * height * 4;
      gpointer data = g_malloc (size);
      g_autoptr (GBytes) bytes = NULL;
      GLint old_align;

      glBindTexture (GL_TEXTURE_2D, texid);
      glEGLImageTargetTexture2DOES (GL_TEXTURE_2D, image);
      glBindTexture (GL_TEXTURE_2D, 0);

      glGenFramebuffers (1, &fbid);
      glBindFramebuffer (GL_READ_FRAMEBUFFER, fbid);
      glFramebufferTexture2D (GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texid, 0);

      glGetIntegerv (GL_PACK_ALIGNMENT, &old_align);
      glPixelStorei (GL_PACK_ALIGNMENT, 4);
      glReadPixels (0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, data);
      glPixelStorei (GL_PACK_ALIGNMENT, old_align);

      glBindFramebuffer (GL_READ_FRAMEBUFFER, 0);

      glDeleteTextures (1, &texid);
      glDeleteFramebuffers (1, &fbid);

      bytes = g_bytes_new_take (data, size);
      texture = gdk_memory_texture_new (width, height, GDK_MEMORY_R8G8B8A8, bytes, width * 4);
    }

  g_set_object (&priv->texture, texture);
  clear_current_internal (ewidget);
}

static void
gtk_egl_image_widget_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  GtkEglImageWidget *ewidget = GTK_EGL_IMAGE_WIDGET (widget);
  GtkEglImageWidgetPrivate *priv = gtk_egl_image_widget_get_instance_private (ewidget);
  int width = gtk_widget_get_width (widget);
  int height = gtk_widget_get_height (widget);

  if (priv->error)
    {
      GTK_WIDGET_CLASS (gtk_egl_image_widget_parent_class)->snapshot (widget, snapshot);
      return;
    }

  if (!priv->error && (priv->needs_render || priv->auto_render))
    {
      if (priv->needs_resize)
        {
          clear_current_internal (ewidget);
          g_signal_emit (ewidget, signals[RESIZE], 0, width, height);
          priv->needs_resize = FALSE;
        }

      gtk_egl_image_widget_update_image (ewidget);

      if (priv->error)
        g_idle_add_full (G_PRIORITY_DEFAULT, queue_alloc, g_object_ref (widget), g_object_unref);
    }

  priv->needs_render = FALSE;

  if (priv->texture)
    {
      const graphene_rect_t bounds = GRAPHENE_RECT_INIT (0.f, 0.f, width, height);
      const gboolean needs_swap_rb = priv->swap_rb && (priv->is_glx || priv->gdk_context);

      if (needs_swap_rb)
        gtk_snapshot_push_gl_shader (snapshot, priv->swap_shader, &bounds,
                                     g_bytes_new (NULL, 0));

      gtk_snapshot_append_texture (snapshot, priv->texture, &bounds);

      if (needs_swap_rb)
        {
          gtk_snapshot_gl_shader_pop_texture (snapshot);
          gtk_snapshot_pop (snapshot);
        }
    }
}

static void
gtk_egl_image_widget_set_property (GObject      *object,
                                   guint         prop_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{
  GtkEglImageWidget *ewidget = GTK_EGL_IMAGE_WIDGET (object);

  switch (prop_id)
    {
    case PROP_AUTO_RENDER:
      gtk_egl_image_widget_set_auto_render (ewidget, g_value_get_boolean (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gtk_egl_image_widget_get_property (GObject    *object,
                                   guint       prop_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
  GtkEglImageWidget *ewidget = GTK_EGL_IMAGE_WIDGET (object);
  GtkEglImageWidgetPrivate *priv = gtk_egl_image_widget_get_instance_private (ewidget);

  switch (prop_id)
    {
    case PROP_AUTO_RENDER:
      g_value_set_boolean (value, priv->auto_render);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gtk_egl_image_widget_notify (GObject *object, GParamSpec *pspec)
{
  if (g_strcmp0 (pspec->name, "scale-factor") == 0)
    {
      GtkEglImageWidget *ewidget = GTK_EGL_IMAGE_WIDGET (object);
      GtkEglImageWidgetPrivate *priv = gtk_egl_image_widget_get_instance_private (ewidget);

      priv->needs_resize = TRUE;
    }

  if (G_OBJECT_CLASS (gtk_egl_image_widget_parent_class)->notify)
    G_OBJECT_CLASS (gtk_egl_image_widget_parent_class)->notify (object, pspec);
}

static void
gtk_egl_image_widget_class_init (GtkEglImageWidgetClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (class);

  widget_class->realize = gtk_egl_image_widget_realize;
  widget_class->unrealize = gtk_egl_image_widget_unrealize;
  widget_class->size_allocate = gtk_egl_image_widget_size_allocate;
  widget_class->snapshot = gtk_egl_image_widget_snapshot;

  object_class->set_property = gtk_egl_image_widget_set_property;
  object_class->get_property = gtk_egl_image_widget_get_property;
  object_class->notify = gtk_egl_image_widget_notify;

  props[PROP_AUTO_RENDER]
    = g_param_spec_boolean ("auto-render", NULL, NULL,
                            TRUE,
                            G_PARAM_READWRITE |
                            G_PARAM_STATIC_STRINGS |
                            G_PARAM_EXPLICIT_NOTIFY);

  g_object_class_install_properties (object_class, LAST_PROP, props);

  signals[RENDER]
    = g_signal_new ("render",
                    G_TYPE_FROM_CLASS (class),
                    G_SIGNAL_RUN_LAST,
                    G_STRUCT_OFFSET (GtkEglImageWidgetClass, render),
                    g_signal_accumulator_first_wins, NULL,
                    NULL,
                    G_TYPE_POINTER, 0);
  signals[RESIZE]
    = g_signal_new ("resize",
                    G_TYPE_FROM_CLASS (class),
                    G_SIGNAL_RUN_LAST,
                    G_STRUCT_OFFSET (GtkEglImageWidgetClass, resize),
                    NULL, NULL,
                    NULL,
                    G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_INT);
}

GtkWidget *
gtk_egl_image_widget_new (void)
{
  return g_object_new (GTK_TYPE_EGL_IMAGE_WIDGET, NULL);
}

EGLDisplay
gtk_egl_image_widget_get_egl_display (GtkEglImageWidget *ewidget)
{
  GtkEglImageWidgetPrivate *priv = gtk_egl_image_widget_get_instance_private (ewidget);

  g_return_val_if_fail (GTK_IS_EGL_IMAGE_WIDGET (ewidget), EGL_NO_IMAGE);
  g_return_val_if_fail (gtk_widget_get_realized (GTK_WIDGET (ewidget)), NULL);

  return priv->display;
}

gboolean
gtk_egl_image_widget_get_auto_render (GtkEglImageWidget *ewidget)
{
  GtkEglImageWidgetPrivate *priv = gtk_egl_image_widget_get_instance_private (ewidget);

  g_return_val_if_fail (GTK_IS_EGL_IMAGE_WIDGET (ewidget), FALSE);

  return priv->auto_render;
}

void
gtk_egl_image_widget_set_auto_render (GtkEglImageWidget *ewidget, gboolean auto_render)
{
  GtkEglImageWidgetPrivate *priv = gtk_egl_image_widget_get_instance_private (ewidget);

  g_return_if_fail (GTK_IS_EGL_IMAGE_WIDGET (ewidget));

  auto_render = !!auto_render;
  if (priv->auto_render != auto_render)
    {
      priv->auto_render = auto_render;
      g_object_notify_by_pspec (G_OBJECT (ewidget), props[PROP_AUTO_RENDER]);
      if (auto_render)
        gtk_widget_queue_draw (GTK_WIDGET (ewidget));
    }
}

void
gtk_egl_image_widget_queue_render (GtkEglImageWidget *ewidget)
{
  GtkEglImageWidgetPrivate *priv = gtk_egl_image_widget_get_instance_private (ewidget);

  g_return_if_fail (GTK_IS_EGL_IMAGE_WIDGET (ewidget));

  priv->needs_render = TRUE;
  gtk_widget_queue_draw (GTK_WIDGET (ewidget));
}

void
gtk_egl_image_widget_set_error (GtkEglImageWidget *ewidget, const GError *error)
{
  GtkEglImageWidgetPrivate *priv = gtk_egl_image_widget_get_instance_private (ewidget);

  g_return_if_fail (GTK_IS_EGL_IMAGE_WIDGET (ewidget));

  g_clear_error (&priv->error);
  if (error)
    {
      priv->error = g_error_copy (error);
      g_critical ("%s", error->message);

      if (!priv->label)
        {
          GtkWidget *scroll = gtk_scrolled_window_new ();
          GtkWidget *child;

          while ((child = gtk_widget_get_first_child (GTK_WIDGET (ewidget))) != NULL)
            gtk_widget_unparent (child);

          priv->label = gtk_label_new (NULL);
          gtk_label_set_justify (GTK_LABEL (priv->label), GTK_JUSTIFY_CENTER);
          gtk_label_set_selectable (GTK_LABEL (priv->label), TRUE);
          gtk_label_set_wrap (GTK_LABEL (priv->label), TRUE);
          gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), priv->label);
          gtk_widget_set_parent (scroll, GTK_WIDGET (ewidget));
        }

      gtk_label_set_label (GTK_LABEL (priv->label), priv->error->message);
    }
  else
    {
      GtkWidget *child;

      while ((child = gtk_widget_get_first_child (GTK_WIDGET (ewidget))) != NULL)
        gtk_widget_unparent (child);
      priv->label = NULL;
    }

  gtk_widget_queue_draw (GTK_WIDGET (ewidget));
}

void
gtk_egl_image_widget_set_error_literal (GtkEglImageWidget *ewidget, const char *fmt, ...)
{
  va_list args;
  g_autoptr (GError) error = NULL;

  va_start (args, fmt);
  error = g_error_new_valist (GDK_GL_ERROR, GDK_GL_ERROR_NOT_AVAILABLE, fmt, args);
  gtk_egl_image_widget_set_error (ewidget, error);
  va_end (args);
}

void
gtk_egl_image_widget_set_last_egl_error (GtkEglImageWidget *ewidget, const char *prefix)
{
  if (prefix != NULL)
    gtk_egl_image_widget_set_error_literal (ewidget, "%s: %s", prefix, egl_error_str ());
  else
    gtk_egl_image_widget_set_error_literal (ewidget, "%s", egl_error_str ());
}

GError *
gtk_egl_image_widget_get_error (GtkEglImageWidget *ewidget)
{
  GtkEglImageWidgetPrivate *priv = gtk_egl_image_widget_get_instance_private (ewidget);

  g_return_val_if_fail (GTK_IS_EGL_IMAGE_WIDGET (ewidget), NULL);

  return priv->error;
}

static const char *
egl_error_str (void)
{
  GLint e = eglGetError ();
#define CHECK(_err) \
  do \
    if (e == _err) \
      return #_err; \
  while (0)

  CHECK (EGL_SUCCESS);
  CHECK (EGL_NOT_INITIALIZED);
  CHECK (EGL_BAD_ACCESS);
  CHECK (EGL_BAD_ALLOC);
  CHECK (EGL_BAD_ATTRIBUTE);
  CHECK (EGL_BAD_CONTEXT);
  CHECK (EGL_BAD_CONFIG);
  CHECK (EGL_BAD_CURRENT_SURFACE);
  CHECK (EGL_BAD_DISPLAY);
  CHECK (EGL_BAD_SURFACE);
  CHECK (EGL_BAD_MATCH);
  CHECK (EGL_BAD_PARAMETER);
  CHECK (EGL_BAD_NATIVE_PIXMAP);
  CHECK (EGL_BAD_NATIVE_WINDOW);
  CHECK (EGL_CONTEXT_LOST);
  CHECK (EGL_BAD_STREAM_KHR);
  CHECK (EGL_BAD_STATE_KHR);
  CHECK (EGL_BAD_DEVICE_EXT);
  CHECK (EGL_BAD_OUTPUT_LAYER_EXT);
  CHECK (EGL_BAD_OUTPUT_PORT_EXT);

#undef CHECK
  static char buf[64];
  g_snprintf (buf, sizeof buf, "Unknown error 0x%08x", e);
  return buf;
}
