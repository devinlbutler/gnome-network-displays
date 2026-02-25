/* nd-debug-log.h
 *
 * Debug logging system for desktopCast.
 * Captures GLib and GStreamer log messages into a ring buffer
 * that can be saved to a file for troubleshooting.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

void     nd_debug_log_init        (void);
void     nd_debug_log_set_verbose (gboolean verbose);
gboolean nd_debug_log_get_verbose (void);
gchar   *nd_debug_log_save        (void);
void     nd_debug_log_shutdown    (void);

G_END_DECLS
