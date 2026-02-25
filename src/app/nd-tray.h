/* nd-tray.h
 *
 * Copyright 2024 GNOME Network Displays contributors
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
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _NdTray NdTray;

NdTray  *nd_tray_new           (GApplication *app);
void     nd_tray_set_streaming (NdTray       *self,
                                gboolean      streaming);
void     nd_tray_destroy       (NdTray       *self);

G_END_DECLS
