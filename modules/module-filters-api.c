/* WirePlumber
 *
 * Copyright © 2023 Collabora Ltd.
 *    @author Julian Bouzas <julian.bouzas@collabora.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include <pipewire/keys.h>

#include <wp/wp.h>

struct _WpFiltersApi
{
  WpPlugin parent;

  WpObjectManager *metadata_om;
  WpObjectManager *stream_nodes_om;
  WpObjectManager *nodes_om;
  WpObjectManager *filter_nodes_om;
  guint n_playback_stream_nodes;
  guint n_capture_stream_nodes;
  GList *filters[2];
  GHashTable *targets;
};

enum {
  ACTION_IS_FILTER_ENABLED,
  ACTION_GET_FILTER_TARGET,
  ACTION_GET_FILTER_FROM_TARGET,
  ACTION_GET_DEFAULT_FILTER,
  SIGNAL_CHANGED,
  N_SIGNALS
};

static guint signals[N_SIGNALS] = {0};

G_DECLARE_FINAL_TYPE (WpFiltersApi, wp_filters_api, WP, FILTERS_API, WpPlugin)
G_DEFINE_TYPE (WpFiltersApi, wp_filters_api, WP_TYPE_PLUGIN)

struct _Filter {
  gchar *link_group;
  WpDirection direction;
  WpNode *node;
  WpNode *stream;
  gchar *target;
  gboolean enabled;
  gint priority;
};
typedef struct _Filter Filter;

static guint
get_filter_priority (const gchar *link_group)
{
  if (strstr (link_group, "loopback"))
    return 300;
  if (strstr (link_group, "filter-chain"))
    return 200;
  /* By default echo-cancel is the lowest priority to properly cancel audio */
  if (strstr (link_group, "echo-cancel"))
    return 0;
  return 100;
}

static Filter *
filter_new (const gchar *link_group, WpDirection dir, gboolean is_stream,
    WpNode *node)
{
  Filter *f = g_malloc0 (sizeof (Filter));
  f->link_group = g_strdup (link_group);
  f->direction = dir;
  f->node = is_stream ? NULL : g_object_ref (node);
  f->stream = is_stream ? g_object_ref (node) : NULL;
  f->target = NULL;
  f->enabled = TRUE;
  f->priority = get_filter_priority (link_group);
  return f;
}

static void
filter_free (Filter *f)
{
  g_clear_pointer (&f->link_group, g_free);
  g_clear_pointer (&f->target, g_free);
  g_clear_object (&f->node);
  g_clear_object (&f->stream);
  g_free (f);
}

static gint
filter_equal_func (const Filter *f, const gchar *link_group)
{
  return g_str_equal (f->link_group, link_group) ? 0 : 1;
}

static gint
filter_compare_func (const Filter *a, const Filter *b)
{
  gint diff = a->priority - b->priority;
  if (diff != 0)
    return diff;
  return g_strcmp0 (a->link_group, b->link_group);
}

static void
wp_filters_api_init (WpFiltersApi * self)
{
}

static gboolean
wp_filters_api_is_filter_enabled (WpFiltersApi * self, const gchar *direction,
    const gchar *link_group)
{
  WpDirection dir = WP_DIRECTION_INPUT;
  GList *filters;
  Filter *found = NULL;

  g_return_val_if_fail (direction, FALSE);
  g_return_val_if_fail (link_group, FALSE);

  /* Get the filters for the given direction */
  if (g_str_equal (direction, "output") || g_str_equal (direction, "Output"))
    dir = WP_DIRECTION_OUTPUT;
  filters = self->filters[dir];

  /* Find the filter in the filters list */
  filters = g_list_find_custom (filters, link_group,
      (GCompareFunc) filter_equal_func);
  if (!filters)
    return FALSE;

  found = filters->data;
  return found->enabled;
}

static gint
wp_filters_api_get_filter_target (WpFiltersApi * self, const gchar *direction,
    const gchar *link_group)
{
  WpDirection dir = WP_DIRECTION_INPUT;
  GList *filters;
  Filter *found;

  g_return_val_if_fail (direction, -1);
  g_return_val_if_fail (link_group, -1);

  /* Get the filters for the given direction */
  if (g_str_equal (direction, "output") || g_str_equal (direction, "Output"))
    dir = WP_DIRECTION_OUTPUT;
  filters = self->filters[dir];

  /* Find the filter in the filters list */
  filters = g_list_find_custom (filters, link_group,
      (GCompareFunc) filter_equal_func);
  if (!filters)
    return -1;
  found = filters->data;
  if (!found->enabled)
    return -1;

  /* Return the previous filter with matching target that is enabled */
  while ((filters = g_list_previous (filters))) {
    Filter *prev = (Filter *) filters->data;
    if ((prev->target == found->target ||
        (prev->target && found->target &&
        g_str_equal (prev->target, found->target))) &&
        prev->enabled)
      return wp_proxy_get_bound_id (WP_PROXY (prev->node));
  }

  /* Find the target */
  if (found->target) {
    WpNode *node = g_hash_table_lookup (self->targets, found->target);
    if (node)
      return wp_proxy_get_bound_id (WP_PROXY (node));
  }

  return -1;
}

static gint
wp_filters_api_get_filter_from_target (WpFiltersApi * self,
    const gchar *direction, gint target_id)
{
  WpDirection dir = WP_DIRECTION_INPUT;
  GList *filters;
  gboolean found = FALSE;
  const gchar *target = NULL;
  gint res = target_id;

  g_return_val_if_fail (direction, res);

  /* Get the filters for the given direction */
  if (g_str_equal (direction, "output") || g_str_equal (direction, "Output"))
    dir = WP_DIRECTION_OUTPUT;
  filters = self->filters[dir];

  /* Find the first target matching target_id */
  while (filters) {
    Filter *f = (Filter *) filters->data;
    gint f_target_id = wp_filters_api_get_filter_target (self, direction,
        f->link_group);
    if (f_target_id == target_id && f->enabled) {
      target = f->target;
      found = TRUE;
      break;
    }

    /* Advance */
    filters = g_list_next (filters);
  }

  /* Just return if target was not found */
  if (!found)
    return res;

  /* Get the last filter node ID of the target found */
  filters = self->filters[dir];
  while (filters) {
    Filter *f = (Filter *) filters->data;
    if ((f->target == target ||
        (f->target && target && g_str_equal (f->target, target))) &&
        f->enabled)
      res = wp_proxy_get_bound_id (WP_PROXY (f->node));

    /* Advance */
    filters = g_list_next (filters);
  }

  return res;
}

static void
sync_changed (WpCore * core, GAsyncResult * res, WpFiltersApi * self)
{
  g_autoptr (GError) error = NULL;

  if (!wp_core_sync_finish (core, res, &error)) {
    wp_warning_object (self, "core sync error: %s", error->message);
    return;
  }

  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

static WpNode *
find_target_node (WpFiltersApi *self, WpSpaJson *props_json)
{
  g_auto (GValue) item = G_VALUE_INIT;
  g_autoptr (WpIterator) it = NULL;
  g_autoptr (WpObjectInterest) interest = NULL;

  /* Make sure the properties are a JSON object */
  if (!props_json || !wp_spa_json_is_object (props_json)) {
    wp_warning_object (self, "Target properties must be a JSON object");
    return NULL;
  }

  /* Create the object intereset with the target properties */
  interest = wp_object_interest_new (WP_TYPE_NODE, NULL);
  it = wp_spa_json_new_iterator (props_json);
  for (; wp_iterator_next (it, &item); g_value_unset (&item)) {
    WpSpaJson *j = g_value_get_boxed (&item);
    g_autofree gchar *key = NULL;
    WpSpaJson *value_json;
    g_autofree gchar *value = NULL;

    key = wp_spa_json_parse_string (j);
    g_value_unset (&item);
    if (!wp_iterator_next (it, &item)) {
      wp_warning_object (self,
        "Could not get valid key-value pairs from target properties");
      break;
    }
    value_json = g_value_get_boxed (&item);
    value = wp_spa_json_parse_string (value_json);
    if (!value) {
      wp_warning_object (self,
        "Could not get '%s' value from target properties", key);
      break;
    }

    wp_object_interest_add_constraint (interest, WP_CONSTRAINT_TYPE_PW_PROPERTY,
        key, WP_CONSTRAINT_VERB_MATCHES, g_variant_new_string (value));
  }

  return wp_object_manager_lookup_full (self->nodes_om,
      wp_object_interest_ref (interest));
}

static gboolean
reevaluate_targets (WpFiltersApi *self)
{
  g_autoptr (WpMetadata) m = NULL;
  const gchar *json_str;
  g_autoptr (WpSpaJson) json = NULL;
  g_auto (GValue) item = G_VALUE_INIT;
  g_autoptr (WpIterator) it = NULL;
  gboolean changed = FALSE;

  g_hash_table_remove_all (self->targets);

  /* Make sure the metadata exists */
  m = wp_object_manager_lookup (self->metadata_om, WP_TYPE_METADATA, NULL);
  if (!m)
    return FALSE;

  /* Don't update anything if the metadata value is not set */
  json_str = wp_metadata_find (m, 0, "filters.configured.targets", NULL);
  if (!json_str)
    return FALSE;

  /* Make sure the metadata value is an object */
  json = wp_spa_json_new_from_string (json_str);
  if (!json || !wp_spa_json_is_object (json)) {
    wp_warning_object (self,
        "ignoring metadata value as it is not a JSON object: %s", json_str);
    return FALSE;
  }

  /* Find the target node for each target, and add it to the hash table */
  it = wp_spa_json_new_iterator (json);
  for (; wp_iterator_next (it, &item); g_value_unset (&item)) {
    WpSpaJson *j = g_value_get_boxed (&item);
    g_autofree gchar *key = NULL;
    WpSpaJson *props;
    g_autoptr (WpNode) target = NULL;
    WpNode *curr_target;

    key = wp_spa_json_parse_string (j);
    g_value_unset (&item);
    if (!wp_iterator_next (it, &item)) {
      wp_warning_object (self,
        "Could not get valid key-value pairs from target object");
      break;
    }
    props = g_value_get_boxed (&item);

    /* Get current target */
    curr_target = g_hash_table_lookup (self->targets, key);

    /* Find the node and insert it into the table if found */
    target = find_target_node (self, props);
    if (target) {
      /* Check if the target changed */
      if (curr_target) {
        guint32 target_bound_id = wp_proxy_get_bound_id (WP_PROXY (target));
        guint32 curr_bound_id = wp_proxy_get_bound_id (WP_PROXY (curr_target));
        if (target_bound_id != curr_bound_id)
          changed = TRUE;
      }

      g_hash_table_insert (self->targets, g_strdup (key),
          g_steal_pointer (&target));
    } else {
      if (curr_target)
        changed = TRUE;
    }
  }

  return changed;
}

static gboolean
update_values_from_metadata (WpFiltersApi * self, Filter *f)
{
  g_autoptr (WpMetadata) m = NULL;
  const gchar *f_stream_name;
  const gchar *f_node_name;
  const gchar *json_str;
  g_autoptr (WpSpaJson) json = NULL;
  g_auto (GValue) item = G_VALUE_INIT;
  g_autoptr (WpIterator) it = NULL;
  gboolean changed = FALSE;

  /* Make sure the metadata exists */
  m = wp_object_manager_lookup (self->metadata_om, WP_TYPE_METADATA, NULL);
  if (!m)
    return FALSE;

  /* Make sure both the stream and node are available */
  if (!f->stream || !f->node)
    return FALSE;
  f_stream_name = wp_pipewire_object_get_property (
      WP_PIPEWIRE_OBJECT (f->stream), PW_KEY_NODE_NAME);
  f_node_name = wp_pipewire_object_get_property (
      WP_PIPEWIRE_OBJECT (f->node), PW_KEY_NODE_NAME);

  /* Don't update anything if the metadata value is not set */
  json_str = wp_metadata_find (m, 0, "filters.configured.filters", NULL);
  if (!json_str)
    return FALSE;

  /* Make sure the metadata value is an array */
  json = wp_spa_json_new_from_string (json_str);
  if (!json || !wp_spa_json_is_array (json)) {
    wp_warning_object (self,
        "ignoring metadata value as it is not a JSON array: %s", json_str);
    return FALSE;
  }

  /* Find the filter values in the metadata */
  it = wp_spa_json_new_iterator (json);
  for (; wp_iterator_next (it, &item); g_value_unset (&item)) {
    WpSpaJson *j = g_value_get_boxed (&item);
    g_autofree gchar *stream_name = NULL;
    g_autofree gchar *node_name = NULL;
    g_autofree gchar *direction = NULL;
    g_autofree gchar *target = NULL;
    g_autofree gchar *mode = NULL;
    WpDirection dir = WP_DIRECTION_INPUT;
    gint priority;

    if (!j || !wp_spa_json_is_object (j))
      continue;

    /* Parse mandatory fields */
    if (!wp_spa_json_object_get (j, "stream-name", "s", &stream_name,
        "node-name", "s", &node_name, "direction", "s", &direction, NULL)) {
      g_autofree gchar *str = wp_spa_json_to_string (j);
      wp_warning_object (self,
          "failed to parse stream-name, node-name and direction in filter: %s",
          str);
      continue;
    }

    /* Make sure direction is valid */
    if (g_str_equal (direction, "input")) {
      dir = WP_DIRECTION_INPUT;
    } else if (g_str_equal (direction, "output")) {
      dir = WP_DIRECTION_OUTPUT;
    } else {
      g_autofree gchar *str = wp_spa_json_to_string (j);
      wp_warning_object (self,
          "direction %s is not valid for filter: %s", direction, str);
    }

    /* Find first filter matching stream-name, node-name and direction */
    if (g_str_equal (f_stream_name, stream_name) &&
        g_str_equal (f_node_name, node_name) &&
        f->direction == dir) {

      /* Update target */
      if (wp_spa_json_object_get (j, "target", "s", &target, NULL)) {
        if (!f->target || !g_str_equal (f->target, target)) {
          g_clear_pointer (&f->target, g_free);
          f->target = g_strdup (target);
          changed = TRUE;
        }
      } else {
        if (f->target) {
          g_clear_pointer (&f->target, g_free);
          changed = TRUE;
        }
      }

      /* Update mode */
      if (wp_spa_json_object_get (j, "mode", "s", &mode, NULL)) {
        if (g_str_equal (mode, "always")) {
          if (!f->enabled) {
            f->enabled = TRUE;
            changed = TRUE;
          }
        } else if (g_str_equal (mode, "never")) {
          if (f->enabled) {
            f->enabled = FALSE;
            changed = TRUE;
          }
        } else if (g_str_equal (mode, "playback-only")) {
          if (f->enabled != (self->n_playback_stream_nodes > 0)) {
            f->enabled = self->n_playback_stream_nodes > 0;
            changed = TRUE;
          }
        } else if (g_str_equal (mode, "capture-only")) {
          if (f->enabled != (self->n_capture_stream_nodes > 0)) {
            f->enabled = self->n_capture_stream_nodes > 0;
            changed = TRUE;
          }
        } else {
          wp_warning_object (self,
              "The '%s' value is not a valid for the 'mode' filter field",
              mode);
        }
      }

      /* Update priority */
      if (wp_spa_json_object_get (j, "priority", "i", &priority, NULL)) {
        if (f->priority != priority) {
          f->priority = priority;
          changed = TRUE;
        }
      }
      break;
    }
  }

  return changed;
}

static gboolean
reevaluate_filters (WpFiltersApi *self, WpDirection direction)
{
  GList *filters;
  gboolean changed = FALSE;

  /* Update filter values */
  filters = self->filters[direction];
  while (filters) {
    Filter *f = (Filter *) filters->data;
    if (update_values_from_metadata (self, f))
      changed = TRUE;
    filters = g_list_next (filters);
  }

  /* Sort filters if changed */
  if (changed)
    self->filters[direction] = g_list_sort (self->filters[direction],
        (GCompareFunc) filter_compare_func);

  return changed;
}

static void
schedule_changed (WpFiltersApi * self)
{
  g_autoptr (WpCore) core = wp_object_get_core (WP_OBJECT (self));
  g_return_if_fail (core);

  wp_core_sync_closure (core, NULL, g_cclosure_new_object (
      G_CALLBACK (sync_changed), G_OBJECT (self)));
}

static void
on_stream_node_added (WpObjectManager *om, WpPipewireObject *proxy, gpointer d)
{
  WpFiltersApi * self = WP_FILTERS_API (d);
  const gchar* media_class = wp_pipewire_object_get_property (proxy,
      PW_KEY_MEDIA_CLASS);

  if (g_str_equal (media_class, "Stream/Output/Audio"))
    self->n_playback_stream_nodes++;
  else if (g_str_equal (media_class, "Stream/Input/Audio"))
    self->n_capture_stream_nodes++;
}

static void
on_stream_node_removed (WpObjectManager *om, WpPipewireObject *proxy, gpointer d)
{
  WpFiltersApi * self = WP_FILTERS_API (d);
  const gchar* media_class = wp_pipewire_object_get_property (proxy,
      PW_KEY_MEDIA_CLASS);

  if (g_str_equal (media_class, "Stream/Output/Audio") &&
      self->n_playback_stream_nodes > 0)
    self->n_playback_stream_nodes--;
  else if (g_str_equal (media_class, "Stream/Input/Audio") &&
      self->n_capture_stream_nodes > 0)
    self->n_capture_stream_nodes--;
}

static void
on_stream_nodes_changed (WpObjectManager *om, gpointer d)
{
  WpFiltersApi * self = WP_FILTERS_API (d);
  gboolean changed = FALSE;

  /* Reevaluate everything */
  for (guint i = 0; i < 2; i++)
    if (reevaluate_filters (self, i))
      changed = TRUE;

  if (changed)
    schedule_changed (self);
}

static void
on_node_added (WpObjectManager *om, WpPipewireObject *proxy, gpointer d)
{
  WpFiltersApi * self = WP_FILTERS_API (d);
  reevaluate_targets (self);
}

static void
on_node_removed (WpObjectManager *om, WpPipewireObject *proxy, gpointer d)
{
  WpFiltersApi * self = WP_FILTERS_API (d);
  reevaluate_targets (self);
}

static void
on_filter_node_added (WpObjectManager *om, WpPipewireObject *proxy, gpointer d)
{
  WpFiltersApi * self = WP_FILTERS_API (d);
  const gchar *key;
  WpDirection dir;
  gboolean is_stream;
  GList *found;

  /* Get direction */
  key = wp_pipewire_object_get_property (proxy, PW_KEY_MEDIA_CLASS);
  if (!key)
    return;

  if (g_str_equal (key, "Audio/Sink") ||
      g_str_equal (key, "Stream/Output/Audio")) {
    dir = WP_DIRECTION_INPUT;
  } else if (g_str_equal (key, "Audio/Source") ||
      g_str_equal (key, "Stream/Input/Audio")) {
    dir = WP_DIRECTION_OUTPUT;
  } else {
    wp_debug_object (self, "ignoring node with media class: %s", key);
    return;
  }

  /* Check whether the proxy is a stream or not */
  is_stream = FALSE;
  if (g_str_equal (key, "Stream/Output/Audio") ||
      g_str_equal (key, "Stream/Input/Audio"))
    is_stream = TRUE;

  /* We use the link group as filter name */
  key = wp_pipewire_object_get_property (proxy, PW_KEY_NODE_LINK_GROUP);
  if (!key) {
    wp_debug_object (self, "ignoring node without link group");
    return;
  }

  /* Check if the filter already exists, and add it if it does not exist */
  found = g_list_find_custom (self->filters[dir], key,
      (GCompareFunc) filter_equal_func);
  if (!found) {
    Filter *f = filter_new (key, dir, is_stream, WP_NODE (proxy));
    update_values_from_metadata (self, f);
    self->filters[dir] = g_list_insert_sorted (self->filters[dir],
        f, (GCompareFunc) filter_compare_func);
  } else {
    Filter *f = found->data;
    if (is_stream) {
      g_clear_object (&f->stream);
      f->stream = g_object_ref (WP_NODE (proxy));
    } else {
      g_clear_object (&f->node);
      f->node = g_object_ref (WP_NODE (proxy));
    }
    update_values_from_metadata (self, f);
    self->filters[dir] = g_list_sort (self->filters[dir],
        (GCompareFunc) filter_compare_func);
  }
}

static void
on_filter_node_removed (WpObjectManager *om, WpPipewireObject *proxy,
    gpointer d)
{
  WpFiltersApi * self = WP_FILTERS_API (d);

  const gchar *key;
  WpDirection dir;
  GList *found;

  /* Get direction */
  key = wp_pipewire_object_get_property (proxy, PW_KEY_MEDIA_CLASS);
  if (!key)
    return;

  if (g_str_equal (key, "Audio/Sink") ||
      g_str_equal (key, "Stream/Output/Audio")) {
    dir = WP_DIRECTION_INPUT;
  } else if (g_str_equal (key, "Audio/Source") ||
      g_str_equal (key, "Stream/Input/Audio")) {
    dir = WP_DIRECTION_OUTPUT;
  } else {
    wp_debug_object (self, "ignoring node with media class: %s", key);
    return;
  }

  /* We use the link group as filter name */
  key = wp_pipewire_object_get_property (proxy, PW_KEY_NODE_LINK_GROUP);
  if (!key) {
    wp_debug_object (self, "ignoring node without link group");
    return;
  }

  /* Find and remove the filter */
  found = g_list_find_custom (self->filters[dir], key,
      (GCompareFunc) filter_equal_func);
  if (found) {
    self->filters[dir] = g_list_remove (self->filters[dir], found->data);
  }
}

static void
on_metadata_changed (WpMetadata *m, guint32 subject,
    const gchar *key, const gchar *type, const gchar *value, gpointer d)
{
  WpFiltersApi * self = WP_FILTERS_API (d);
  gboolean changed = FALSE;

  /* Reevaluate everything */
  if (reevaluate_targets (self))
    changed = TRUE;
  for (guint i = 0; i < 2; i++)
    if (reevaluate_filters (self, i))
      changed = TRUE;

  if (changed)
    schedule_changed (self);
}

static void
on_metadata_added (WpObjectManager *om, WpMetadata *metadata, gpointer d)
{
  WpFiltersApi * self = WP_FILTERS_API (d);
  gboolean changed = FALSE;

  /* Handle the changed signal */
  g_signal_connect_object (metadata, "changed",
      G_CALLBACK (on_metadata_changed), self, 0);

  /* Reevaluate everything */
  if (reevaluate_targets (self))
    changed = TRUE;
  for (guint i = 0; i < 2; i++)
    if (reevaluate_filters (self, i))
      changed = TRUE;

  if (changed)
    schedule_changed (self);
}

static void
on_metadata_installed (WpObjectManager * om, WpFiltersApi * self)
{
  g_autoptr (WpCore) core = wp_object_get_core (WP_OBJECT (self));

  /* Create the stream nodes object manager */
  self->stream_nodes_om = wp_object_manager_new ();
  wp_object_manager_add_interest (self->stream_nodes_om, WP_TYPE_NODE,
      WP_CONSTRAINT_TYPE_PW_PROPERTY, PW_KEY_MEDIA_CLASS, "#s", "Stream/*/Audio",
      WP_CONSTRAINT_TYPE_PW_PROPERTY, PW_KEY_NODE_LINK_GROUP, "-",
      NULL);
  wp_object_manager_request_object_features (self->stream_nodes_om,
      WP_TYPE_NODE, WP_OBJECT_FEATURES_ALL);
  g_signal_connect_object (self->stream_nodes_om, "object-added",
      G_CALLBACK (on_stream_node_added), self, 0);
  g_signal_connect_object (self->stream_nodes_om, "object-removed",
      G_CALLBACK (on_stream_node_removed), self, 0);
  g_signal_connect_object (self->stream_nodes_om, "objects-changed",
      G_CALLBACK (on_stream_nodes_changed), self, 0);
  wp_core_install_object_manager (core, self->stream_nodes_om);

  /* Create the nodes object manager */
  self->nodes_om = wp_object_manager_new ();
  wp_object_manager_add_interest (self->nodes_om, WP_TYPE_NODE,
      WP_CONSTRAINT_TYPE_PW_PROPERTY, PW_KEY_MEDIA_CLASS, "#s", "Audio/*",
      WP_CONSTRAINT_TYPE_PW_PROPERTY, PW_KEY_NODE_LINK_GROUP, "-",
      NULL);
  wp_object_manager_request_object_features (self->nodes_om,
      WP_TYPE_NODE, WP_OBJECT_FEATURES_ALL);
  g_signal_connect_object (self->nodes_om, "object-added",
      G_CALLBACK (on_node_added), self, 0);
  g_signal_connect_object (self->nodes_om, "object-removed",
      G_CALLBACK (on_node_removed), self, 0);
  g_signal_connect_object (self->nodes_om, "objects-changed",
      G_CALLBACK (schedule_changed), self, G_CONNECT_SWAPPED);
  wp_core_install_object_manager (core, self->nodes_om);

  /* Create the filter nodes object manager */
  self->filter_nodes_om = wp_object_manager_new ();
  wp_object_manager_add_interest (self->filter_nodes_om, WP_TYPE_NODE,
      WP_CONSTRAINT_TYPE_PW_PROPERTY, PW_KEY_NODE_LINK_GROUP, "+",
      NULL);
  wp_object_manager_request_object_features (self->filter_nodes_om,
      WP_TYPE_NODE, WP_OBJECT_FEATURES_ALL);
  g_signal_connect_object (self->filter_nodes_om, "object-added",
      G_CALLBACK (on_filter_node_added), self, 0);
  g_signal_connect_object (self->filter_nodes_om, "object-removed",
      G_CALLBACK (on_filter_node_removed), self, 0);
  g_signal_connect_object (self->filter_nodes_om, "objects-changed",
      G_CALLBACK (schedule_changed), self, G_CONNECT_SWAPPED);
  wp_core_install_object_manager (core, self->filter_nodes_om);

  wp_object_update_features (WP_OBJECT (self), WP_PLUGIN_FEATURE_ENABLED, 0);
}

static void
wp_filters_api_enable (WpPlugin * plugin, WpTransition * transition)
{
  WpFiltersApi * self = WP_FILTERS_API (plugin);
  g_autoptr (WpCore) core = wp_object_get_core (WP_OBJECT (self));

  self->targets = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
      g_object_unref);

  /* Create the metadata object manager */
  self->metadata_om = wp_object_manager_new ();
  wp_object_manager_add_interest (self->metadata_om, WP_TYPE_METADATA,
      WP_CONSTRAINT_TYPE_PW_GLOBAL_PROPERTY, "metadata.name", "=s", "filters",
      NULL);
  wp_object_manager_request_object_features (self->metadata_om,
      WP_TYPE_METADATA, WP_OBJECT_FEATURES_ALL);
  g_signal_connect_object (self->metadata_om, "object-added",
      G_CALLBACK (on_metadata_added), self, 0);
  g_signal_connect_object (self->metadata_om, "installed",
      G_CALLBACK (on_metadata_installed), self, 0);
  wp_core_install_object_manager (core, self->metadata_om);
}

static void
wp_filters_api_disable (WpPlugin * plugin)
{
  WpFiltersApi * self = WP_FILTERS_API (plugin);

  for (guint i = 0; i < 2; i++) {
    if (self->filters[i]) {
      g_list_free_full (self->filters[i], (GDestroyNotify) filter_free);
      self->filters[i] = NULL;
    }
  }
  g_clear_pointer (&self->targets, g_hash_table_unref);

  g_clear_object (&self->metadata_om);
  g_clear_object (&self->stream_nodes_om);
  g_clear_object (&self->nodes_om);
  g_clear_object (&self->filter_nodes_om);
}

static void
wp_filters_api_class_init (WpFiltersApiClass * klass)
{
  WpPluginClass *plugin_class = (WpPluginClass *) klass;

  plugin_class->enable = wp_filters_api_enable;
  plugin_class->disable = wp_filters_api_disable;

  signals[ACTION_IS_FILTER_ENABLED] = g_signal_new_class_handler (
      "is-filter-enabled", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
      (GCallback) wp_filters_api_is_filter_enabled,
      NULL, NULL, NULL,
      G_TYPE_BOOLEAN, 2, G_TYPE_STRING, G_TYPE_STRING);

  signals[ACTION_GET_FILTER_TARGET] = g_signal_new_class_handler (
      "get-filter-target", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
      (GCallback) wp_filters_api_get_filter_target,
      NULL, NULL, NULL,
      G_TYPE_INT, 2, G_TYPE_STRING, G_TYPE_STRING);

  signals[ACTION_GET_FILTER_FROM_TARGET] = g_signal_new_class_handler (
      "get-filter-from-target", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
      (GCallback) wp_filters_api_get_filter_from_target,
      NULL, NULL, NULL,
      G_TYPE_INT, 2, G_TYPE_STRING, G_TYPE_INT);

  signals[SIGNAL_CHANGED] = g_signal_new (
      "changed", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
      G_TYPE_NONE, 0);
}

WP_PLUGIN_EXPORT gboolean
wireplumber__module_init (WpCore * core, GVariant * args, GError ** error)
{
  wp_plugin_register (g_object_new (wp_filters_api_get_type (),
      "name", "filters-api",
      "core", core,
      NULL));
  return TRUE;
}
