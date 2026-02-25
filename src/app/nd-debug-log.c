/* nd-debug-log.c
 *
 * Debug logging system for desktopCast.
 * Captures GLib and GStreamer log messages into a circular buffer
 * that can be saved to a file for troubleshooting.
 *
 * Thread-safe: uses a fixed-size circular buffer with a mutex.
 */

#include "nd-debug-log.h"
#include <gst/gst.h>
#include <stdio.h>
#include <sys/utsname.h>
#include "gnome-network-displays-config.h"

#define MAX_LOG_LINES 5000

/* Circular buffer */
static GMutex    log_mutex;
static gchar    *log_ring[MAX_LOG_LINES];
static guint     log_head = 0;   /* next write position */
static guint     log_count = 0;  /* number of entries stored */
static gboolean  verbose_mode = FALSE;

static const gchar *
level_str (GLogLevelFlags level)
{
  if (level & G_LOG_LEVEL_ERROR)    return "ERROR";
  if (level & G_LOG_LEVEL_CRITICAL) return "CRIT ";
  if (level & G_LOG_LEVEL_WARNING)  return "WARN ";
  if (level & G_LOG_LEVEL_MESSAGE)  return "MSG  ";
  if (level & G_LOG_LEVEL_INFO)     return "INFO ";
  if (level & G_LOG_LEVEL_DEBUG)    return "DEBUG";
  return "TRACE";
}

static void
append_line (gchar *line)
{
  g_mutex_lock (&log_mutex);

  /* Free old entry if overwriting */
  g_free (log_ring[log_head]);
  log_ring[log_head] = line;  /* takes ownership */

  log_head = (log_head + 1) % MAX_LOG_LINES;
  if (log_count < MAX_LOG_LINES)
    log_count++;

  g_mutex_unlock (&log_mutex);
}

static GLogWriterOutput
debug_log_writer (GLogLevelFlags   log_level,
                  const GLogField *fields,
                  gsize            n_fields,
                  gpointer         user_data)
{
  const gchar *domain = NULL;
  const gchar *message = NULL;

  for (gsize i = 0; i < n_fields; i++)
    {
      if (g_strcmp0 (fields[i].key, "GLIB_DOMAIN") == 0)
        domain = fields[i].value;
      else if (g_strcmp0 (fields[i].key, "MESSAGE") == 0)
        message = fields[i].value;
    }

  if (!message)
    message = "(no message)";
  if (!domain)
    domain = "app";

  g_autoptr(GDateTime) now = g_date_time_new_now_local ();
  g_autofree gchar *timestamp = g_date_time_format (now, "%H:%M:%S.%f");
  if (timestamp && strlen (timestamp) > 12)
    timestamp[12] = '\0';

  gchar *line = g_strdup_printf ("[%s] %s  %s: %s",
                                 timestamp,
                                 level_str (log_level),
                                 domain,
                                 message);

  /* In verbose mode, print to stderr */
  if (verbose_mode)
    g_printerr ("%s\n", line);

  append_line (line);  /* takes ownership */

  /* For errors/criticals, also use default handler so we get abort behavior */
  if (log_level & (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL))
    return g_log_writer_default (log_level, fields, n_fields, user_data);

  return G_LOG_WRITER_HANDLED;
}

static void
gst_debug_handler (GstDebugCategory *category,
                   GstDebugLevel     level,
                   const gchar      *file,
                   const gchar      *function,
                   gint              line,
                   GObject          *object,
                   GstDebugMessage  *msg,
                   gpointer          user_data)
{
  /* In non-verbose mode, only capture warnings and above */
  if (!verbose_mode && level > GST_LEVEL_WARNING)
    return;

  /* In verbose mode, capture up to FIXME level (INFO is too noisy for streaming) */
  if (verbose_mode && level > GST_LEVEL_FIXME)
    return;

  const gchar *cat_name = gst_debug_category_get_name (category);
  const gchar *level_name = gst_debug_level_get_name (level);

  g_autoptr(GDateTime) now = g_date_time_new_now_local ();
  g_autofree gchar *timestamp = g_date_time_format (now, "%H:%M:%S.%f");
  if (timestamp && strlen (timestamp) > 12)
    timestamp[12] = '\0';

  gchar *log_line = g_strdup_printf ("[%s] GST:%-5s %s:%d:%s: %s",
                                     timestamp,
                                     level_name,
                                     cat_name,
                                     line,
                                     function,
                                     gst_debug_message_get (msg));

  if (verbose_mode)
    g_printerr ("%s\n", log_line);

  append_line (log_line);  /* takes ownership */
}

void
nd_debug_log_init (void)
{
  g_mutex_init (&log_mutex);
  memset (log_ring, 0, sizeof (log_ring));

  /* Install our GLib log writer */
  g_log_set_writer_func (debug_log_writer, NULL, NULL);

  /* Install GStreamer debug handler and suppress default output */
  gst_debug_add_log_function (gst_debug_handler, NULL, NULL);
  gst_debug_remove_log_function (gst_debug_log_default);
  gst_debug_set_default_threshold (GST_LEVEL_WARNING);

  append_line (g_strdup ("=== desktopCast debug log started ==="));
}

void
nd_debug_log_set_verbose (gboolean enabled)
{
  verbose_mode = enabled;

  if (enabled)
    {
      /* FIXME level captures warnings + fixme notes — good signal-to-noise */
      gst_debug_set_default_threshold (GST_LEVEL_FIXME);

      append_line (g_strdup (">>> DEBUG MODE ENABLED <<<"));
      g_message ("Debug mode enabled — verbose logging active");
    }
  else
    {
      gst_debug_set_default_threshold (GST_LEVEL_WARNING);

      append_line (g_strdup (">>> DEBUG MODE DISABLED <<<"));
      g_message ("Debug mode disabled");
    }
}

gboolean
nd_debug_log_get_verbose (void)
{
  return verbose_mode;
}

gchar *
nd_debug_log_save (void)
{
  g_autoptr(GString) dump = NULL;
  g_autoptr(GDateTime) now = NULL;
  g_autofree gchar *time_str = NULL;
  g_autofree gchar *filename = NULL;
  g_autofree gchar *dir = NULL;
  g_autoptr(GError) error = NULL;
  struct utsname sys_info;

  dump = g_string_new (NULL);

  /* Header */
  now = g_date_time_new_now_local ();
  time_str = g_date_time_format (now, "%Y-%m-%d %H:%M:%S");

  g_string_append_printf (dump, "desktopCast Debug Log\n");
  g_string_append_printf (dump, "Version: %s\n", PACKAGE_VERSION);
  g_string_append_printf (dump, "Saved: %s\n", time_str);

  if (uname (&sys_info) == 0)
    g_string_append_printf (dump, "System: %s %s %s\n",
                            sys_info.sysname, sys_info.release, sys_info.machine);

  g_string_append_printf (dump, "Debug mode: %s\n", verbose_mode ? "ON" : "OFF");
  g_string_append (dump, "---\n\n");

  /* Log entries — read from circular buffer in order */
  g_mutex_lock (&log_mutex);

  guint start = (log_count < MAX_LOG_LINES) ? 0 : log_head;
  for (guint i = 0; i < log_count; i++)
    {
      guint idx = (start + i) % MAX_LOG_LINES;
      if (log_ring[idx])
        {
          g_string_append (dump, log_ring[idx]);
          g_string_append_c (dump, '\n');
        }
    }

  guint saved_count = log_count;
  g_mutex_unlock (&log_mutex);

  /* Write to file */
  dir = g_build_filename (g_get_home_dir (), "desktopcast-logs", NULL);
  g_mkdir_with_parents (dir, 0755);

  g_autofree gchar *file_time = g_date_time_format (now, "%Y%m%d-%H%M%S");
  filename = g_build_filename (dir, g_strdup_printf ("desktopcast-%s.log", file_time), NULL);

  if (!g_file_set_contents (filename, dump->str, dump->len, &error))
    {
      g_warning ("Failed to save debug log: %s", error->message);
      return NULL;
    }

  g_message ("Debug log saved to %s (%u entries)", filename, saved_count);

  return g_strdup (filename);
}

void
nd_debug_log_shutdown (void)
{
  gst_debug_remove_log_function (gst_debug_handler);

  g_mutex_lock (&log_mutex);
  for (guint i = 0; i < MAX_LOG_LINES; i++)
    g_clear_pointer (&log_ring[i], g_free);
  log_count = 0;
  log_head = 0;
  g_mutex_unlock (&log_mutex);

  g_mutex_clear (&log_mutex);
}
