/**
 * ClutterSettings:
 *
 * Settings configuration
 *
 * Clutter depends on some settings to perform operations like detecting
 * multiple button press events, or font options to render text.
 *
 * Usually, Clutter will strive to use the platform's settings in order
 * to be as much integrated as possible. It is, however, possible to
 * change these settings on a per-application basis, by using the
 * #ClutterSettings singleton object and setting its properties. It is
 * also possible, for toolkit developers, to retrieve the settings from
 * the #ClutterSettings properties when implementing new UI elements,
 * for instance the default font name.
 */

#include "config.h"

#include "clutter/clutter-settings.h"

#include "clutter/clutter-context-private.h"
#include "clutter/clutter-debug.h"
#include "clutter/clutter-settings-private.h"
#include "clutter/clutter-stage-private.h"
#include "clutter/clutter-private.h"

#include <gdesktop-enums.h>
#include <stdlib.h>

#define DEFAULT_FONT_NAME       "Sans 12"

typedef struct
{
  cairo_antialias_t cairo_antialias;
  gint clutter_font_antialias;

  cairo_hint_style_t cairo_hint_style;
  const char *clutter_font_hint_style;

  cairo_subpixel_order_t cairo_subpixel_order;
  const char *clutter_font_subpixel_order;
} FontSettings;

struct _ClutterSettings
{
  GObject parent_instance;

  ClutterBackend *backend;
  GSettings *font_settings;
  GSettings *mouse_settings;
  GSettings *mouse_a11y_settings;

  gint double_click_time;
  gint double_click_distance;

  gint dnd_drag_threshold;

  gdouble resolution;

  gchar *font_name;
  gint font_dpi;

  gint xft_hinting;
  gint xft_antialias;
  gchar *xft_hint_style;
  gchar *xft_rgba;

  gint long_press_duration;

  guint last_fontconfig_timestamp;

  guint password_hint_time;

  gint unscaled_font_dpi;
};

enum
{
  PROP_0,

  PROP_DOUBLE_CLICK_TIME,
  PROP_DOUBLE_CLICK_DISTANCE,

  PROP_DND_DRAG_THRESHOLD,

  PROP_FONT_NAME,

  PROP_FONT_ANTIALIAS,
  PROP_FONT_DPI,
  PROP_FONT_HINTING,
  PROP_FONT_HINT_STYLE,
  PROP_FONT_RGBA,

  PROP_LONG_PRESS_DURATION,

  PROP_PASSWORD_HINT_TIME,

  PROP_UNSCALED_FONT_DPI,

  PROP_LAST
};

static GParamSpec *obj_props[PROP_LAST];

G_DEFINE_FINAL_TYPE (ClutterSettings, clutter_settings, G_TYPE_OBJECT);

static inline void
settings_update_font_options (ClutterSettings *self)
{
  cairo_hint_style_t hint_style = CAIRO_HINT_STYLE_NONE;
  cairo_antialias_t antialias_mode = CAIRO_ANTIALIAS_GRAY;
  cairo_subpixel_order_t subpixel_order = CAIRO_SUBPIXEL_ORDER_DEFAULT;
  cairo_font_options_t *options;

  if (self->backend == NULL)
    return;

  options = cairo_font_options_create ();

  cairo_font_options_set_hint_metrics (options, CAIRO_HINT_METRICS_ON);

  if (self->xft_hinting >= 0 &&
      self->xft_hint_style == NULL)
    {
      hint_style = CAIRO_HINT_STYLE_NONE;
    }
  else if (self->xft_hint_style != NULL)
    {
      if (strcmp (self->xft_hint_style, "hintnone") == 0)
        hint_style = CAIRO_HINT_STYLE_NONE;
      else if (strcmp (self->xft_hint_style, "hintslight") == 0)
        hint_style = CAIRO_HINT_STYLE_SLIGHT;
      else if (strcmp (self->xft_hint_style, "hintmedium") == 0)
        hint_style = CAIRO_HINT_STYLE_MEDIUM;
      else if (strcmp (self->xft_hint_style, "hintfull") == 0)
        hint_style = CAIRO_HINT_STYLE_FULL;
    }

  cairo_font_options_set_hint_style (options, hint_style);

  if (self->xft_rgba)
    {
      if (strcmp (self->xft_rgba, "rgb") == 0)
        subpixel_order = CAIRO_SUBPIXEL_ORDER_RGB;
      else if (strcmp (self->xft_rgba, "bgr") == 0)
        subpixel_order = CAIRO_SUBPIXEL_ORDER_BGR;
      else if (strcmp (self->xft_rgba, "vrgb") == 0)
        subpixel_order = CAIRO_SUBPIXEL_ORDER_VRGB;
      else if (strcmp (self->xft_rgba, "vbgr") == 0)
        subpixel_order = CAIRO_SUBPIXEL_ORDER_VBGR;
    }

  cairo_font_options_set_subpixel_order (options, subpixel_order);

  if (self->xft_antialias >= 0 && !self->xft_antialias)
    antialias_mode = CAIRO_ANTIALIAS_NONE;
  else if (subpixel_order != CAIRO_SUBPIXEL_ORDER_DEFAULT)
    antialias_mode = CAIRO_ANTIALIAS_SUBPIXEL;
  else if (self->xft_antialias >= 0)
    antialias_mode = CAIRO_ANTIALIAS_GRAY;

  cairo_font_options_set_antialias (options, antialias_mode);

  CLUTTER_NOTE (BACKEND, "New font options:\n"
                " - font-name:  %s\n"
                " - antialias:  %d\n"
                " - hinting:    %d\n"
                " - hint-style: %s\n"
                " - rgba:       %s\n",
                self->font_name != NULL ? self->font_name : DEFAULT_FONT_NAME,
                self->xft_antialias,
                self->xft_hinting,
                self->xft_hint_style != NULL ? self->xft_hint_style : "<null>",
                self->xft_rgba != NULL ? self->xft_rgba : "<null>");

  clutter_backend_set_font_options (self->backend, options);
  cairo_font_options_destroy (options);
}

static void
settings_update_font_name (ClutterSettings *self)
{
  CLUTTER_NOTE (BACKEND, "New font-name: %s", self->font_name);

  if (self->backend != NULL)
    g_signal_emit_by_name (self->backend, "font-changed");
}

static void
settings_update_resolution (ClutterSettings *self)
{
  const char *scale_env = NULL;

  if (self->font_dpi > 0)
    self->resolution = (gdouble) self->font_dpi / 1024.0;
  else
    self->resolution = 96.0;

  scale_env = g_getenv ("GDK_DPI_SCALE");
  if (scale_env != NULL)
    {
      double scale = g_ascii_strtod (scale_env, NULL);
      if (scale != 0 && self->resolution > 0)
        self->resolution *= scale;
    }

  CLUTTER_NOTE (BACKEND, "New resolution: %.2f (%s)",
                self->resolution,
                self->unscaled_font_dpi > 0 ? "unscaled" : "scaled");

  if (self->backend != NULL)
    g_signal_emit_by_name (self->backend, "resolution-changed");
}

static void
get_font_gsettings (GSettings    *settings,
                    FontSettings *output)
{
  /* org.gnome.desktop.GDesktopFontAntialiasingMode */
  static const struct
  {
    cairo_antialias_t cairo_antialias;
    gint clutter_font_antialias;
  }
  antialiasings[] =
  {
    /* none=0      */ {CAIRO_ANTIALIAS_NONE,     0},
    /* grayscale=1 */ {CAIRO_ANTIALIAS_GRAY,     1},
    /* rgba=2      */ {CAIRO_ANTIALIAS_SUBPIXEL, 1},
  };

  /* org.gnome.desktop.GDesktopFontHinting */
  static const struct
  {
    cairo_hint_style_t cairo_hint_style;
    const char *clutter_font_hint_style;
  }
  hintings[] =
  {
    /* none=0   */ {CAIRO_HINT_STYLE_NONE,   "hintnone"},
    /* slight=1 */ {CAIRO_HINT_STYLE_SLIGHT, "hintslight"},
    /* medium=2 */ {CAIRO_HINT_STYLE_MEDIUM, "hintmedium"},
    /* full=3   */ {CAIRO_HINT_STYLE_FULL,   "hintfull"},
  };

  /* org.gnome.desktop.GDesktopFontRgbaOrder */
  static const struct
  {
    cairo_subpixel_order_t cairo_subpixel_order;
    const char *clutter_font_subpixel_order;
  }
  rgba_orders[] =
  {
    /* rgba=0 */ {CAIRO_SUBPIXEL_ORDER_RGB,  "rgb"}, /* XXX what is 'rgba'? */
    /* rgb=1  */ {CAIRO_SUBPIXEL_ORDER_RGB,  "rgb"},
    /* bgr=2  */ {CAIRO_SUBPIXEL_ORDER_BGR,  "bgr"},
    /* vrgb=3 */ {CAIRO_SUBPIXEL_ORDER_VRGB, "vrgb"},
    /* vbgr=4 */ {CAIRO_SUBPIXEL_ORDER_VBGR, "vbgr"},
  };
  guint i;

  i = g_settings_get_enum (settings, "font-hinting");
  if (i < G_N_ELEMENTS (hintings))
    {
      output->cairo_hint_style = hintings[i].cairo_hint_style;
      output->clutter_font_hint_style = hintings[i].clutter_font_hint_style;
    }
  else
    {
      output->cairo_hint_style = CAIRO_HINT_STYLE_DEFAULT;
      output->clutter_font_hint_style = NULL;
    }

  i = g_settings_get_enum (settings, "font-antialiasing");
  if (i < G_N_ELEMENTS (antialiasings))
    {
      output->cairo_antialias = antialiasings[i].cairo_antialias;
      output->clutter_font_antialias = antialiasings[i].clutter_font_antialias;
    }
  else
    {
      output->cairo_antialias = CAIRO_ANTIALIAS_DEFAULT;
      output->clutter_font_antialias = -1;
    }

  i = g_settings_get_enum (settings, "font-rgba-order");
  if (i < G_N_ELEMENTS (rgba_orders))
    {
      output->cairo_subpixel_order = rgba_orders[i].cairo_subpixel_order;
      output->clutter_font_subpixel_order = rgba_orders[i].clutter_font_subpixel_order;
    }
  else
    {
      output->cairo_subpixel_order = CAIRO_SUBPIXEL_ORDER_DEFAULT;
      output->clutter_font_subpixel_order = NULL;
    }

  if (output->cairo_antialias == CAIRO_ANTIALIAS_GRAY)
    output->clutter_font_subpixel_order = "none";
}

static void
init_font_options (ClutterSettings *self)
{
  GSettings *settings = self->font_settings;
  cairo_font_options_t *options = cairo_font_options_create ();
  FontSettings fs;

  get_font_gsettings (settings, &fs);

  cairo_font_options_set_hint_style (options, fs.cairo_hint_style);
  cairo_font_options_set_antialias (options, fs.cairo_antialias);
  cairo_font_options_set_subpixel_order (options, fs.cairo_subpixel_order);

  clutter_backend_set_font_options (self->backend, options);

  cairo_font_options_destroy (options);
}

static void
sync_mouse_options (ClutterSettings *self)
{
  int double_click;
  int drag_threshold;

  double_click = g_settings_get_int (self->mouse_settings, "double-click");
  drag_threshold = g_settings_get_int (self->mouse_settings, "drag-threshold");

  g_object_set (self,
		"double-click-time", double_click,
		"dnd-drag-threshold", drag_threshold,
                NULL);
}

static gboolean
on_font_settings_change_event (GSettings *settings,
			       gpointer   keys,
			       gint       n_keys,
			       gpointer   user_data)
{
  ClutterSettings *self = CLUTTER_SETTINGS (user_data);
  FontSettings fs;
  gint hinting;

  get_font_gsettings (settings, &fs);
  hinting = fs.cairo_hint_style == CAIRO_HINT_STYLE_NONE ? 0 : 1;
  g_object_set (self,
                "font-hinting",        hinting,
                "font-hint-style",     fs.clutter_font_hint_style,
                "font-antialias",      fs.clutter_font_antialias,
                "font-subpixel-order", fs.clutter_font_subpixel_order,
                NULL);

  return FALSE;
}

static gboolean
on_mouse_settings_change_event (GSettings *settings,
				gpointer   keys,
				gint       n_keys,
				gpointer   user_data)
{
  ClutterSettings *self = CLUTTER_SETTINGS (user_data);

  sync_mouse_options (self);

  return FALSE;
}

struct _pointer_a11y_settings_flags_pair {
  const char *name;
  ClutterPointerA11yFlags flag;
} pointer_a11y_settings_flags_pair[] = {
  { "secondary-click-enabled", CLUTTER_A11Y_SECONDARY_CLICK_ENABLED },
  { "dwell-click-enabled",     CLUTTER_A11Y_DWELL_ENABLED },
};

static ClutterPointerA11yDwellDirection
pointer_a11y_dwell_direction_from_setting (ClutterSettings *self,
                                           const char      *key)
{
  GDesktopMouseDwellDirection dwell_gesture_direction;

  dwell_gesture_direction = g_settings_get_enum (self->mouse_a11y_settings,
                                                 key);
  switch (dwell_gesture_direction)
    {
    case G_DESKTOP_MOUSE_DWELL_DIRECTION_LEFT:
      return CLUTTER_A11Y_DWELL_DIRECTION_LEFT;
      break;
    case G_DESKTOP_MOUSE_DWELL_DIRECTION_RIGHT:
      return CLUTTER_A11Y_DWELL_DIRECTION_RIGHT;
      break;
    case G_DESKTOP_MOUSE_DWELL_DIRECTION_UP:
      return CLUTTER_A11Y_DWELL_DIRECTION_UP;
      break;
    case G_DESKTOP_MOUSE_DWELL_DIRECTION_DOWN:
      return CLUTTER_A11Y_DWELL_DIRECTION_DOWN;
      break;
    default:
      break;
    }
  return CLUTTER_A11Y_DWELL_DIRECTION_NONE;
}

static void
sync_pointer_a11y_settings (ClutterSettings *self,
                            ClutterSeat     *seat)
{
  ClutterPointerA11ySettings pointer_a11y_settings;
  GDesktopMouseDwellMode dwell_mode;
  int i;

  clutter_seat_get_pointer_a11y_settings (seat, &pointer_a11y_settings);
  pointer_a11y_settings.controls = 0;
  for (i = 0; i < G_N_ELEMENTS (pointer_a11y_settings_flags_pair); i++)
    {
      if (!g_settings_get_boolean (self->mouse_a11y_settings,
                                   pointer_a11y_settings_flags_pair[i].name))
        continue;

      pointer_a11y_settings.controls |=
        pointer_a11y_settings_flags_pair[i].flag;
    }

  /* "secondary-click-time" is expressed in seconds */
  pointer_a11y_settings.secondary_click_delay =
    (int) (1000 * g_settings_get_double (self->mouse_a11y_settings,
                                         "secondary-click-time"));
  /* "dwell-time" is expressed in seconds */
  pointer_a11y_settings.dwell_delay =
    (int) (1000 * g_settings_get_double (self->mouse_a11y_settings,
                                         "dwell-time"));
  pointer_a11y_settings.dwell_threshold =
    g_settings_get_int (self->mouse_a11y_settings, "dwell-threshold");

  dwell_mode = g_settings_get_enum (self->mouse_a11y_settings, "dwell-mode");
  if (dwell_mode == G_DESKTOP_MOUSE_DWELL_MODE_WINDOW)
    pointer_a11y_settings.dwell_mode = CLUTTER_A11Y_DWELL_MODE_WINDOW;
  else
    pointer_a11y_settings.dwell_mode = CLUTTER_A11Y_DWELL_MODE_GESTURE;

  pointer_a11y_settings.dwell_gesture_single =
    pointer_a11y_dwell_direction_from_setting (self, "dwell-gesture-single");
  pointer_a11y_settings.dwell_gesture_double =
    pointer_a11y_dwell_direction_from_setting (self, "dwell-gesture-double");
  pointer_a11y_settings.dwell_gesture_drag =
    pointer_a11y_dwell_direction_from_setting (self, "dwell-gesture-drag");
  pointer_a11y_settings.dwell_gesture_secondary =
    pointer_a11y_dwell_direction_from_setting (self, "dwell-gesture-secondary");

  clutter_seat_set_pointer_a11y_settings (seat, &pointer_a11y_settings);
}

static gboolean
on_mouse_a11y_settings_change_event (GSettings *settings,
                                     gpointer   keys,
                                     int        n_keys,
                                     gpointer   user_data)
{
  ClutterSettings *self = CLUTTER_SETTINGS (user_data);
  ClutterSeat *seat = clutter_backend_get_default_seat (self->backend);

  sync_pointer_a11y_settings (self, seat);

  return FALSE;
}

static void
load_initial_settings (ClutterSettings *self)
{
  static const gchar *font_settings_path = "org.gnome.desktop.interface";
  static const gchar *mouse_settings_path = "org.gnome.desktop.peripherals.mouse";
  static const char *mouse_a11y_settings_path = "org.gnome.desktop.a11y.mouse";
  GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
  GSettingsSchema *schema;

  schema = g_settings_schema_source_lookup (source, font_settings_path, TRUE);
  if (!schema)
    {
      g_warning ("Failed to find schema: %s", font_settings_path);
    }
  else
    {
      self->font_settings = g_settings_new_full (schema, NULL, NULL);
      if (self->font_settings)
        {
          init_font_options (self);
          g_signal_connect (self->font_settings, "change-event",
                            G_CALLBACK (on_font_settings_change_event),
                            self);
        }
    }

  schema = g_settings_schema_source_lookup (source, mouse_settings_path, TRUE);
  if (!schema)
    {
      g_warning ("Failed to find schema: %s", mouse_settings_path);
    }
  else
    {
      self->mouse_settings = g_settings_new_full (schema, NULL, NULL);
      if (self->mouse_settings)
        {
          sync_mouse_options (self);
          g_signal_connect (self->mouse_settings, "change-event",
                            G_CALLBACK (on_mouse_settings_change_event),
                            self);
        }
    }

  schema = g_settings_schema_source_lookup (source, mouse_a11y_settings_path, TRUE);
  if (!schema)
    {
      g_warning ("Failed to find schema: %s", mouse_settings_path);
    }
  else
    {
      self->mouse_a11y_settings = g_settings_new_full (schema, NULL, NULL);
      g_signal_connect (self->mouse_a11y_settings, "change-event",
                        G_CALLBACK (on_mouse_a11y_settings_change_event),
                        self);
    }
}

static void
clutter_settings_finalize (GObject *gobject)
{
  ClutterSettings *self = CLUTTER_SETTINGS (gobject);

  g_free (self->font_name);
  g_free (self->xft_hint_style);
  g_free (self->xft_rgba);

  g_clear_object (&self->font_settings);
  g_clear_object (&self->mouse_settings);
  g_clear_object (&self->mouse_a11y_settings);

  G_OBJECT_CLASS (clutter_settings_parent_class)->finalize (gobject);
}

static void
clutter_settings_set_property (GObject      *gobject,
                               guint         prop_id,
                               const GValue *value,
                               GParamSpec   *pspec)
{
  ClutterSettings *self = CLUTTER_SETTINGS (gobject);

  switch (prop_id)
    {
    case PROP_DOUBLE_CLICK_TIME:
      self->double_click_time = g_value_get_int (value);
      break;

    case PROP_DOUBLE_CLICK_DISTANCE:
      self->double_click_distance = g_value_get_int (value);
      break;

    case PROP_DND_DRAG_THRESHOLD:
      self->dnd_drag_threshold = g_value_get_int (value);
      break;

    case PROP_FONT_NAME:
      g_free (self->font_name);
      self->font_name = g_value_dup_string (value);
      settings_update_font_name (self);
      break;

    case PROP_FONT_ANTIALIAS:
      self->xft_antialias = g_value_get_int (value);
      settings_update_font_options (self);
      break;

    case PROP_FONT_DPI:
      self->font_dpi = g_value_get_int (value);
      settings_update_resolution (self);
      break;

    case PROP_FONT_HINTING:
      self->xft_hinting = g_value_get_int (value);
      settings_update_font_options (self);
      break;

    case PROP_FONT_HINT_STYLE:
      g_free (self->xft_hint_style);
      self->xft_hint_style = g_value_dup_string (value);
      settings_update_font_options (self);
      break;

    case PROP_FONT_RGBA:
      g_free (self->xft_rgba);
      self->xft_rgba = g_value_dup_string (value);
      settings_update_font_options (self);
      break;

    case PROP_LONG_PRESS_DURATION:
      self->long_press_duration = g_value_get_int (value);
      break;

    case PROP_PASSWORD_HINT_TIME:
      self->password_hint_time = g_value_get_uint (value);
      break;

    case PROP_UNSCALED_FONT_DPI:
      self->font_dpi = g_value_get_int (value);
      settings_update_resolution (self);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (gobject, prop_id, pspec);
      break;
    }
}

static void
clutter_settings_get_property (GObject    *gobject,
                               guint       prop_id,
                               GValue     *value,
                               GParamSpec *pspec)
{
  ClutterSettings *self = CLUTTER_SETTINGS (gobject);

  switch (prop_id)
    {
    case PROP_DOUBLE_CLICK_TIME:
      g_value_set_int (value, self->double_click_time);
      break;

    case PROP_DOUBLE_CLICK_DISTANCE:
      g_value_set_int (value, self->double_click_distance);
      break;

    case PROP_DND_DRAG_THRESHOLD:
      g_value_set_int (value, self->dnd_drag_threshold);
      break;

    case PROP_FONT_NAME:
      g_value_set_string (value, self->font_name);
      break;

    case PROP_FONT_ANTIALIAS:
      g_value_set_int (value, self->xft_antialias);
      break;

    case PROP_FONT_DPI:
      g_value_set_int (value, (int) (self->resolution * 1024));
      break;

    case PROP_FONT_HINTING:
      g_value_set_int (value, self->xft_hinting);
      break;

    case PROP_FONT_HINT_STYLE:
      g_value_set_string (value, self->xft_hint_style);
      break;

    case PROP_FONT_RGBA:
      g_value_set_string (value, self->xft_rgba);
      break;

    case PROP_LONG_PRESS_DURATION:
      g_value_set_int (value, self->long_press_duration);
      break;

    case PROP_PASSWORD_HINT_TIME:
      g_value_set_uint (value, self->password_hint_time);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (gobject, prop_id, pspec);
      break;
    }
}

static void
clutter_settings_dispatch_properties_changed (GObject     *gobject,
                                              guint        n_pspecs,
                                              GParamSpec **pspecs)
{
  ClutterSettings *self = CLUTTER_SETTINGS (gobject);
  GObjectClass *klass;

  /* chain up to emit ::notify */
  klass = G_OBJECT_CLASS (clutter_settings_parent_class);
  klass->dispatch_properties_changed (gobject, n_pspecs, pspecs);

  /* emit settings-changed just once for multiple properties */
  if (self->backend != NULL)
    g_signal_emit_by_name (self->backend, "settings-changed");
}

static void
clutter_settings_class_init (ClutterSettingsClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  /**
   * ClutterSettings:double-click-time:
   *
   * The time, in milliseconds, that should elapse between button-press
   * events in order to increase the click count by 1.
   */
  obj_props[PROP_DOUBLE_CLICK_TIME] =
    g_param_spec_int ("double-click-time", NULL, NULL,
                      0, G_MAXINT,
                      250,
                      G_PARAM_READWRITE |
                      G_PARAM_STATIC_STRINGS);

  /**
   * ClutterSettings:double-click-distance:
   *
   * The maximum distance, in pixels, between button-press events that
   * determines whether or not to increase the click count by 1.
   */
  obj_props[PROP_DOUBLE_CLICK_DISTANCE] =
    g_param_spec_int ("double-click-distance", NULL, NULL,
                      0, G_MAXINT,
                      5,
                      G_PARAM_READWRITE |
                      G_PARAM_STATIC_STRINGS);

  /**
   * ClutterSettings:dnd-drag-threshold:
   *
   * The default distance that the cursor of a pointer device
   * should travel before a drag operation should start.
   */
  obj_props[PROP_DND_DRAG_THRESHOLD] =
    g_param_spec_int ("dnd-drag-threshold", NULL, NULL,
                      1, G_MAXINT,
                      8,
                      G_PARAM_READWRITE |
                      G_PARAM_STATIC_STRINGS);

  /**
   * ClutterSettings:font-name:
   *
   * The default font name that should be used by text actors, as
   * a string that can be passed to [func@Pango.FontDescription.from_string].
   */
  obj_props[PROP_FONT_NAME] =
    g_param_spec_string ("font-name", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ClutterSettings:font-antialias:
   *
   * Whether or not to use antialiasing when rendering text; a value
   * of 1 enables it unconditionally; a value of 0 disables it
   * unconditionally; and -1 will use the system's default.
   */
  obj_props[PROP_FONT_ANTIALIAS] =
    g_param_spec_int ("font-antialias", NULL, NULL,
                      -1, 1,
                      -1,
                      G_PARAM_READWRITE |
                      G_PARAM_STATIC_STRINGS);

  /**
   * ClutterSettings:font-dpi:
   *
   * The DPI used when rendering text, as a value of 1024 * dots/inch.
   *
   * If set to -1, the system's default will be used instead
   */
  obj_props[PROP_FONT_DPI] =
    g_param_spec_int ("font-dpi", NULL, NULL,
                      -1, 1024 * 1024,
                      -1,
                      G_PARAM_READWRITE |
                      G_PARAM_STATIC_STRINGS);

  obj_props[PROP_UNSCALED_FONT_DPI] =
    g_param_spec_int ("unscaled-font-dpi", NULL, NULL,
                      -1, 1024 * 1024,
                      -1,
                      G_PARAM_WRITABLE |
                      G_PARAM_STATIC_STRINGS);

  /**
   * ClutterSettings:font-hinting:
   *
   * Whether or not to use hinting when rendering text; a value of 1
   * unconditionally enables it; a value of 0 unconditionally disables
   * it; and a value of -1 will use the system's default.
   */
  obj_props[PROP_FONT_HINTING] =
    g_param_spec_int ("font-hinting", NULL, NULL,
                      -1, 1,
                      -1,
                      G_PARAM_READWRITE |
                      G_PARAM_STATIC_STRINGS);

  /**
   * ClutterSettings:font-hint-style:
   *
   * The style of the hinting used when rendering text. Valid values
   * are:
   *
   *   - hintnone
   *   - hintslight
   *   - hintmedium
   *   - hintfull
   */
  obj_props[PROP_FONT_HINT_STYLE] =
    g_param_spec_string ("font-hint-style", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ClutterSettings:font-subpixel-order:
   *
   * The type of sub-pixel antialiasing used when rendering text. Valid
   * values are:
   *
   *   - none
   *   - rgb
   *   - bgr
   *   - vrgb
   *   - vbgr
   */
  obj_props[PROP_FONT_RGBA] =
    g_param_spec_string ("font-subpixel-order", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ClutterSettings:long-press-duration:
   *
   * Sets the minimum duration for a press to be recognized as a long press
   * gesture. The duration is expressed in milliseconds.
   *
   * See also [property@ClickAction:long-press-duration].
   */
  obj_props[PROP_LONG_PRESS_DURATION] =
    g_param_spec_int ("long-press-duration", NULL, NULL,
                      0, G_MAXINT,
                      500,
                      G_PARAM_READWRITE |
                      G_PARAM_STATIC_STRINGS);


  /**
   * ClutterText:password-hint-time:
   *
   * How long should Clutter show the last input character in editable
   * [class@Text] actors. The value is in milliseconds. A value of 0
   * disables showing the password hint. 600 is a good value for
   * enabling the hint.
   */
  obj_props[PROP_PASSWORD_HINT_TIME] =
    g_param_spec_uint ("password-hint-time", NULL, NULL,
                       0, G_MAXUINT,
                       0,
                       G_PARAM_READWRITE |
                       G_PARAM_STATIC_STRINGS);

  gobject_class->set_property = clutter_settings_set_property;
  gobject_class->get_property = clutter_settings_get_property;
  gobject_class->dispatch_properties_changed =
    clutter_settings_dispatch_properties_changed;
  gobject_class->finalize = clutter_settings_finalize;
  g_object_class_install_properties (gobject_class, PROP_LAST, obj_props);
}

static void
clutter_settings_init (ClutterSettings *self)
{
  self->resolution = -1.0;

  self->font_dpi = -1;
  self->unscaled_font_dpi = -1;

  self->double_click_time = 250;
  self->double_click_distance = 5;

  self->dnd_drag_threshold = 8;

  self->font_name = g_strdup (DEFAULT_FONT_NAME);

  self->xft_antialias = -1;
  self->xft_hinting = -1;
  self->xft_hint_style = NULL;
  self->xft_rgba = NULL;

  self->long_press_duration = 500;
}

/**
 * clutter_settings_get_default:
 *
 * Retrieves the singleton instance of #ClutterSettings
 *
 * Return value: (transfer none): the instance of #ClutterSettings. The
 *   returned object is owned by Clutter and it should not be unreferenced
 *   directly
 */
ClutterSettings *
clutter_settings_get_default (void)
{
  static ClutterSettings *settings = NULL;

  if (G_UNLIKELY (settings == NULL))
    settings = g_object_new (CLUTTER_TYPE_SETTINGS, NULL);

  return settings;
}

void
_clutter_settings_set_backend (ClutterSettings *settings,
                               ClutterBackend  *backend)
{
  g_assert (CLUTTER_IS_SETTINGS (settings));
  g_assert (CLUTTER_IS_BACKEND (backend));

  settings->backend = backend;

  load_initial_settings (settings);
}

void
clutter_settings_ensure_pointer_a11y_settings (ClutterSettings *settings,
                                               ClutterSeat     *seat)
{
  sync_pointer_a11y_settings (settings, seat);
}
