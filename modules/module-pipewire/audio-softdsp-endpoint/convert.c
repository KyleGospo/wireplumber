﻿/* WirePlumber
 *
 * Copyright © 2019 Collabora Ltd.
 *    @author Julian Bouzas <julian.bouzas@collabora.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include <pipewire/pipewire.h>
#include <spa/utils/names.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/param/props.h>

#include "convert.h"
#include "../algorithms.h"

enum {
  PROP_0,
  PROP_TARGET,
  PROP_FORMAT,
};

struct _WpAudioConvert
{
  WpAudioStream parent;

  /* Props */
  WpAudioStream *target;
  struct spa_audio_info_raw format;

  /* Proxies */
  GPtrArray *link_proxies;
};

static GAsyncInitableIface *wp_audio_convert_parent_interface = NULL;
static void wp_audio_convert_async_initable_init (gpointer iface,
    gpointer iface_data);

G_DEFINE_TYPE_WITH_CODE (WpAudioConvert, wp_audio_convert, WP_TYPE_AUDIO_STREAM,
    G_IMPLEMENT_INTERFACE (G_TYPE_ASYNC_INITABLE,
                           wp_audio_convert_async_initable_init))

static void
create_link_cb (WpProperties *props, gpointer user_data)
{
  WpAudioConvert *self = WP_AUDIO_CONVERT (user_data);
  g_autoptr (WpCore) core = NULL;
  WpLink *link;

  core = wp_audio_stream_get_core (WP_AUDIO_STREAM (self));
  g_return_if_fail (core);

  /* make the link passive, which means it will not keep
     the audioconvert node in the running state if the number of non-passive
     links (i.e. the ones linking another endpoint to this one) drops to 0 */
  wp_properties_set (props, PW_KEY_LINK_PASSIVE, "1");

  /* Create the link */
  link = wp_link_new_from_factory (core, "link-factory",
      wp_properties_ref (props));
  g_return_if_fail (link);
  g_ptr_array_add(self->link_proxies, link);
}

static void
on_audio_convert_running(WpAudioConvert *self)
{
  g_autoptr (GVariant) src_props = NULL;
  g_autoptr (GVariant) sink_props = NULL;
  g_autoptr (GError) error = NULL;
  enum pw_direction direction =
      wp_audio_stream_get_direction (WP_AUDIO_STREAM (self));

  g_debug ("%p linking audio convert to target", self);

  if (direction == PW_DIRECTION_INPUT) {
    wp_audio_stream_prepare_link (WP_AUDIO_STREAM (self), &src_props, &error);
    wp_audio_stream_prepare_link (self->target, &sink_props, &error);
  } else {
    wp_audio_stream_prepare_link (self->target, &src_props, &error);
    wp_audio_stream_prepare_link (WP_AUDIO_STREAM (self), &sink_props, &error);
  }

  multiport_link_create (src_props, sink_props, create_link_cb, self, &error);
}

static void
wp_audio_convert_event_info (WpProxy * proxy, GParamSpec *spec,
    WpAudioConvert * self)
{
  const struct pw_node_info *info = wp_proxy_get_info (proxy);

  /* Handle the different states */
  switch (info->state) {
  case PW_NODE_STATE_IDLE:
    g_ptr_array_set_size (self->link_proxies, 0);
    break;
  case PW_NODE_STATE_RUNNING:
    on_audio_convert_running (self);
    break;
  case PW_NODE_STATE_SUSPENDED:
    break;
  default:
    break;
  }
}

static WpSpaPod *
format_audio_raw_build (const struct spa_audio_info_raw *info)
{
  g_autoptr (WpSpaPodBuilder) builder = wp_spa_pod_builder_new_object (
      "Format", "Format");
  wp_spa_pod_builder_add (builder,
      "mediaType",    "I", SPA_MEDIA_TYPE_audio,
      "mediaSubtype", "I", SPA_MEDIA_SUBTYPE_raw,
      "format",       "I", info->format,
      "rate",         "i", info->rate,
      "channels",     "i", info->channels,
      NULL);

   if (!SPA_FLAG_IS_SET (info->flags, SPA_AUDIO_FLAG_UNPOSITIONED)) {
     /* Build the position array spa pod */
     g_autoptr (WpSpaPodBuilder) position_builder = wp_spa_pod_builder_new_array ();
     for (guint i = 0; i < info->channels; i++)
       wp_spa_pod_builder_add_id (position_builder, info->position[i]);

     /* Add the position property */
     wp_spa_pod_builder_add_property (builder, "position");
     g_autoptr (WpSpaPod) position = wp_spa_pod_builder_end (position_builder);
     wp_spa_pod_builder_add_pod (builder, position);
   }

   return wp_spa_pod_builder_end (builder);
}

static void
on_audio_convert_core_done (WpCore *core, GAsyncResult *res,
    WpAudioConvert *self)
{
  g_autoptr (GError) error = NULL;
  enum pw_direction direction =
      wp_audio_stream_get_direction (WP_AUDIO_STREAM (self));
  g_autoptr (WpSpaPod) format = NULL;
  g_autoptr (WpSpaPod) pod = NULL;
  gboolean control;

  wp_core_sync_finish (core, res, &error);
  if (error) {
    g_message("WpAudioConvert:%p initial sync failed: %s", self, error->message);
    wp_audio_stream_init_task_finish (WP_AUDIO_STREAM (self),
        g_steal_pointer (&error));
    return;
  }

  g_debug ("%s:%p setting format", G_OBJECT_TYPE_NAME (self), self);

  format = format_audio_raw_build (&self->format);

  /* Only enable control port for input streams */
  control =
#if defined(HAVE_AUDIOFADE)
    (direction == PW_DIRECTION_INPUT);
#else
    FALSE;
#endif

  /* Configure audioconvert to be both merger and splitter; this means it will
     have an equal number of input and output ports and just passthrough the
     same format, but with altered volume.
     In the future we need to consider writing a simpler volume node for this,
     as doing merge + split is heavy for our needs */
  pod = wp_spa_pod_new_object ("PortConfig",  "PortConfig",
      "direction",  "I", pw_direction_reverse(direction),
      "mode",       "I", SPA_PARAM_PORT_CONFIG_MODE_dsp,
      "format",     "P", format,
      NULL);
  wp_audio_stream_set_port_config (WP_AUDIO_STREAM (self), pod);

  pod = wp_spa_pod_new_object ("PortConfig",  "PortConfig",
      "direction",  "I", direction,
      "mode",       "I", SPA_PARAM_PORT_CONFIG_MODE_dsp,
      "control",    "b", control,
      "format",     "P", format,
      NULL);
  wp_audio_stream_set_port_config (WP_AUDIO_STREAM (self), pod);
  wp_audio_stream_finish_port_config (WP_AUDIO_STREAM (self));
}

static void
wp_audio_convert_init_async (GAsyncInitable *initable, int io_priority,
    GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
  WpAudioConvert *self = WP_AUDIO_CONVERT (initable);
  g_autoptr (WpProxy) proxy = NULL;
  g_autoptr (WpProperties) props = NULL;
  g_autoptr (WpCore) core = wp_audio_stream_get_core (WP_AUDIO_STREAM (self));
  WpNode *node;

  /* Create the properties */
  node = wp_audio_stream_get_node (self->target);
  props = wp_properties_copy (wp_proxy_get_properties (WP_PROXY (node)));

  wp_properties_setf (props, PW_KEY_OBJECT_PATH, "%s:%s",
      wp_properties_get(props, PW_KEY_OBJECT_PATH),
      wp_audio_stream_get_name (WP_AUDIO_STREAM (self)));
  wp_properties_setf (props, PW_KEY_NODE_NAME, "%s/%s/%s",
      SPA_NAME_AUDIO_CONVERT,
      wp_properties_get(props, PW_KEY_NODE_NAME),
      wp_audio_stream_get_name (WP_AUDIO_STREAM (self)));
  wp_properties_set (props, PW_KEY_MEDIA_CLASS, "Audio/Convert");
  wp_properties_set (props, SPA_KEY_FACTORY_NAME, SPA_NAME_AUDIO_CONVERT);

  /* Create the proxy */
  proxy = (WpProxy *) wp_node_new_from_factory (core, "spa-node-factory",
      g_steal_pointer (&props));
  g_return_if_fail (proxy);

  g_object_set (self, "node", proxy, NULL);
  g_signal_connect_object (proxy, "notify::info",
      (GCallback) wp_audio_convert_event_info, self, 0);

  /* Call the parent interface */
  wp_audio_convert_parent_interface->init_async (initable, io_priority,
      cancellable, callback, data);

  /* Register a callback to be called after all the initialization is done */
  wp_core_sync (core, NULL,
      (GAsyncReadyCallback) on_audio_convert_core_done, self);
}

static void
wp_audio_convert_async_initable_init (gpointer iface, gpointer iface_data)
{
  GAsyncInitableIface *ai_iface = iface;

  /* Set the parent interface */
  wp_audio_convert_parent_interface = g_type_interface_peek_parent (iface);

  ai_iface->init_async = wp_audio_convert_init_async;
}

static void
wp_audio_convert_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  WpAudioConvert *self = WP_AUDIO_CONVERT (object);

  switch (property_id) {
  case PROP_TARGET:
    self->target = g_value_dup_object (value);
    break;
  case PROP_FORMAT: {
    const struct spa_audio_info_raw *f = g_value_get_pointer (value);
    if (f)
      self->format = *f;
    else
      g_warning ("WpAudioConvert:%p Format needs to be valid", self);
    break;
  }
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}

static void
wp_audio_convert_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  WpAudioConvert *self = WP_AUDIO_CONVERT (object);

  switch (property_id) {
  case PROP_TARGET:
    g_value_set_object (value, self->target);
    break;
  case PROP_FORMAT:
    g_value_set_pointer (value, &self->format);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}

static void
wp_audio_convert_finalize (GObject * object)
{
  WpAudioConvert *self = WP_AUDIO_CONVERT (object);

  g_clear_pointer (&self->link_proxies, g_ptr_array_unref);
  g_clear_object (&self->target);

  G_OBJECT_CLASS (wp_audio_convert_parent_class)->finalize (object);
}

static void
wp_audio_convert_init (WpAudioConvert * self)
{
  self->link_proxies = g_ptr_array_new_with_free_func (g_object_unref);
}

static void
wp_audio_convert_class_init (WpAudioConvertClass * klass)
{
  GObjectClass *object_class = (GObjectClass *) klass;

  object_class->finalize = wp_audio_convert_finalize;
  object_class->set_property = wp_audio_convert_set_property;
  object_class->get_property = wp_audio_convert_get_property;

  /* Install the properties */
  g_object_class_install_property (object_class, PROP_TARGET,
      g_param_spec_object ("target", "target", "The target stream",
          WP_TYPE_AUDIO_STREAM,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_FORMAT,
      g_param_spec_pointer ("format", "format", "The accepted format",
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

void
wp_audio_convert_new (WpBaseEndpoint *endpoint, guint stream_id,
    const char *stream_name, enum pw_direction direction,
    WpAudioStream *target, const struct spa_audio_info_raw *format,
    GAsyncReadyCallback callback, gpointer user_data)
{
  g_async_initable_new_async (
      WP_TYPE_AUDIO_CONVERT, G_PRIORITY_DEFAULT, NULL, callback, user_data,
      "endpoint", endpoint,
      "id", stream_id,
      "name", stream_name,
      "direction", direction,
      "target", target,
      "format", format,
      NULL);
}
