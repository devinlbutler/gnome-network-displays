/* nd-controller.h
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

#include <glib-object.h>
#include "nd-sink.h"

G_BEGIN_DECLS

#define ND_TYPE_CONTROLLER (nd_controller_get_type ())
G_DECLARE_FINAL_TYPE (NdController, nd_controller, ND, CONTROLLER, GObject)

NdController *nd_controller_new          (void);

guint         nd_controller_get_n_sinks  (NdController *self);
NdSink       *nd_controller_get_sink     (NdController *self,
                                          guint         index);

void          nd_controller_connect_sink (NdController *self,
                                          NdSink       *sink);
void          nd_controller_disconnect   (NdController *self);

gboolean      nd_controller_is_streaming (NdController *self);
NdSinkState   nd_controller_get_state    (NdController *self);
NdSink       *nd_controller_get_stream_sink (NdController *self);

G_END_DECLS
