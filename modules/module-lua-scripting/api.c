/* WirePlumber
 *
 * Copyright © 2020 Collabora Ltd.
 *    @author George Kiagiadakis <george.kiagiadakis@collabora.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include "wp/global-proxy.h"
#include <wp/wp.h>
#include <wplua/wplua.h>

#define URI_API "resource:///org/freedesktop/pipewire/wireplumber/m-lua-scripting/api.lua"

/* WpDebug */

static int
log_log (lua_State *L, GLogLevelFlags lvl)
{
  lua_Debug ar;
  const gchar *message;
  gchar line_str[11];
  gconstpointer instance = NULL;
  GType type = G_TYPE_INVALID;
  int index = 1;

  if (!wp_log_level_is_enabled (lvl))
    return 0;

  lua_getstack (L, 1, &ar);
  lua_getinfo (L, "nSl", &ar);

  if (wplua_isobject (L, 1, G_TYPE_OBJECT)) {
    instance = wplua_toobject (L, 1);
    type = G_TYPE_FROM_INSTANCE (instance);
    index++;
  }

  message = luaL_checkstring (L, index);
  sprintf (line_str, "%d", ar.currentline);

  wp_log_structured_standard (G_LOG_DOMAIN, lvl,
      ar.source, line_str, ar.name, type, instance, "%s", message);
  return 0;
}

static int
log_warning (lua_State *L) { return log_log (L, G_LOG_LEVEL_WARNING); }

static int
log_message (lua_State *L) { return log_log (L, G_LOG_LEVEL_MESSAGE); }

static int
log_info (lua_State *L) { return log_log (L, G_LOG_LEVEL_INFO); }

static int
log_debug (lua_State *L) { return log_log (L, G_LOG_LEVEL_DEBUG); }

static int
log_trace (lua_State *L) { return log_log (L, WP_LOG_LEVEL_TRACE); }

static const luaL_Reg log_funcs[] = {
  { "warning", log_warning },
  { "message", log_message },
  { "info", log_info },
  { "debug", log_debug },
  { "trace", log_trace },
  { NULL, NULL }
};

/* WpGlobalProxy */

static int
global_proxy_request_destroy (lua_State *L)
{
  WpGlobalProxy * p = wplua_checkobject (L, 1, WP_TYPE_GLOBAL_PROXY);
  wp_global_proxy_request_destroy (p);
  return 0;
}

static const luaL_Reg global_proxy_methods[] = {
  { "request_destroy", global_proxy_request_destroy },
  { NULL, NULL }
};

/* WpIterator */

static int
iterator_next (lua_State *L)
{
  WpIterator *it = wplua_checkboxed (L, 1, WP_TYPE_ITERATOR);
  GValue v = G_VALUE_INIT;
  if (wp_iterator_next (it, &v)) {
    return wplua_gvalue_to_lua (L, &v);
  } else {
    lua_pushnil (L);
    return 1;
  }
}

static int
push_wpiterator (lua_State *L, WpIterator *it)
{
  lua_pushcfunction (L, iterator_next);
  wplua_pushboxed (L, WP_TYPE_ITERATOR, it);
  return 2;
}

/* Metadata WpIterator */

static int
metadata_iterator_next (lua_State *L)
{
  WpIterator *it = wplua_checkboxed (L, 1, WP_TYPE_ITERATOR);
  GValue item = G_VALUE_INIT;
  if (wp_iterator_next (it, &item)) {
    guint32 s = 0;
    const gchar *k = NULL, *t = NULL, *v = NULL;
    wp_metadata_iterator_item_extract (&item, &s, &k, &t, &v);
    lua_pushinteger (L, s);
    lua_pushstring (L, k);
    lua_pushstring (L, t);
    lua_pushstring (L, v);
    return 4;
  } else {
    lua_pushnil (L);
    return 1;
  }
}

static int
push_metadata_wpiterator (lua_State *L, WpIterator *it)
{
  lua_pushcfunction (L, metadata_iterator_next);
  wplua_pushboxed (L, WP_TYPE_ITERATOR, it);
  return 2;
}

/* WpObjectInterest */

static GVariant *
constraint_value_to_variant (lua_State *L, int idx)
{
  switch (lua_type (L, idx)) {
  case LUA_TBOOLEAN:
    return g_variant_new_boolean (lua_toboolean (L, idx));
  case LUA_TSTRING:
    return g_variant_new_string (lua_tostring (L, idx));
  case LUA_TNUMBER:
    if (lua_isinteger (L, idx))
      return g_variant_new_int64 (lua_tointeger (L, idx));
    else
      return g_variant_new_double (lua_tonumber (L, idx));
  default:
    return NULL;
  }
}

static void
object_interest_new_add_constraint (lua_State *L, GType type,
    WpObjectInterest *interest)
{
  int constraint_idx;
  WpConstraintType ctype;
  const gchar *subject;
  WpConstraintVerb verb;
  GVariant *value = NULL;

  constraint_idx = lua_absindex (L, -1);

  /* verify this is a Constraint{} */
  if (lua_type (L, constraint_idx) != LUA_TTABLE) {
    luaL_error (L, "Interest: expected Constraint at index %d",
        lua_tointeger (L, -2));
  }

  if (luaL_getmetafield (L, constraint_idx, "__name") == LUA_TNIL ||
      g_strcmp0 (lua_tostring (L, -1), "Constraint") != 0) {
    luaL_error (L, "Interest: expected Constraint at index %d",
        lua_tointeger (L, -2));
  }
  lua_pop (L, 1);

  /* get the constraint type */
  lua_pushliteral (L, "type");
  if (lua_gettable (L, constraint_idx) == LUA_TNUMBER)
    ctype = lua_tointeger (L, -1);
  else
    ctype = g_type_is_a (type, WP_TYPE_GLOBAL_PROXY) ?
        WP_CONSTRAINT_TYPE_PW_GLOBAL_PROPERTY : WP_CONSTRAINT_TYPE_G_PROPERTY;
  lua_pop (L, 1);

  /* get t[1] (the subject) and t[2] (the verb) */
  lua_geti (L, constraint_idx, 1);
  subject = lua_tostring (L, -1);

  lua_geti (L, constraint_idx, 2);
  verb = lua_tostring (L, -1)[0];

  switch (verb) {
  case WP_CONSTRAINT_VERB_EQUALS:
  case WP_CONSTRAINT_VERB_MATCHES: {
    lua_geti (L, constraint_idx, 3);
    value = constraint_value_to_variant (L, -1);
    if (G_UNLIKELY (!value))
      luaL_error (L, "Constraint: bad value type");
    break;
  }
  case WP_CONSTRAINT_VERB_IN_RANGE: {
    GVariant *values[2];
    lua_geti (L, constraint_idx, 3);
    lua_geti (L, constraint_idx, 4);
    values[0] = constraint_value_to_variant (L, -2);
    values[1] = constraint_value_to_variant (L, -1);
    if (G_UNLIKELY (!values[0] || !values[1])) {
      g_clear_pointer (&values[0], g_variant_unref);
      g_clear_pointer (&values[1], g_variant_unref);
      luaL_error (L, "Constraint: bad value type");
    }
    value = g_variant_new_tuple (values, 2);
    break;
  }
  case WP_CONSTRAINT_VERB_IN_LIST: {
    GPtrArray *values =
        g_ptr_array_new_with_free_func ((GDestroyNotify) g_variant_unref);
    int i = 3;
    while (lua_geti (L, constraint_idx, i++) != LUA_TNIL) {
      GVariant *tmp = constraint_value_to_variant (L, -1);
      if (G_UNLIKELY (!tmp)) {
        g_ptr_array_unref (values);
        luaL_error (L, "Constraint: bad value type");
      }
      g_ptr_array_add (values, g_variant_ref_sink (tmp));
      lua_pop (L, 1);
    }
    value = g_variant_new_tuple ((GVariant **) values->pdata, values->len);
    g_ptr_array_unref (values);
    break;
  }
  default:
    break;
  }

  wp_object_interest_add_constraint (interest, ctype, subject, verb, value);
  lua_settop (L, constraint_idx);
}

static int
object_interest_new (lua_State *L)
{
  WpObjectInterest *interest = NULL;
  GType type = 0;
  gchar *typestr;

  luaL_checktype (L, 1, LUA_TTABLE);

  /* type = "string" -> required */
  lua_pushliteral (L, "type");
  if (lua_gettable (L, -2) != LUA_TSTRING)
    luaL_error (L, "Interest: expected 'type' as string");

  /* "device" -> "WpDevice" */
  typestr = g_strdup_printf ("Wp%s", lua_tostring (L, -1));
  if (typestr[2] != 0) {
    typestr[2] = g_ascii_toupper (typestr[2]);
    type = g_type_from_name (typestr);
  }
  g_free (typestr);
  lua_pop (L, 1);

  if (!type)
    luaL_error (L, "Interest: unknown type '%s'", lua_tostring (L, -1));

  interest = wp_object_interest_new_type (type);
  wplua_pushboxed (L, WP_TYPE_OBJECT_INTEREST, interest);

  /* add constraints */
  lua_pushnil (L);
  while (lua_next (L, 1)) {
    /* if the key isn't "type" */
    if (!(lua_type (L, -2) == LUA_TSTRING &&
          !g_strcmp0 ("type", lua_tostring (L, -2))))
      object_interest_new_add_constraint (L, type, interest);
    lua_pop (L, 1);
  }

  return 1;
}

/* WpObjectManager */

static int
object_manager_new (lua_State *L)
{
  WpObjectManager *om;

  /* validate arguments */
  luaL_checktype (L, 1, LUA_TTABLE);

  /* push to Lua asap to have a way to unref in case of error */
  om = wp_object_manager_new ();
  wplua_pushobject (L, om);

  lua_pushnil (L);
  while (lua_next (L, 1)) {
    if (!wplua_isboxed (L, -1, WP_TYPE_OBJECT_INTEREST))
      luaL_error (L, "ObjectManager: expected Interest");

    /* steal the interest out of the GValue to avoid doing mem copy */
    GValue *v = lua_touserdata (L, -1);
    wp_object_manager_add_interest_full (om, g_value_get_boxed (v));
    memset (v, 0, sizeof (GValue));
    g_value_init (v, WP_TYPE_OBJECT_INTEREST);

    lua_pop (L, 1);
  }

  /* request all the features for Lua scripts to make their job easier */
  wp_object_manager_request_object_features (om,
      WP_TYPE_OBJECT, WP_OBJECT_FEATURES_ALL);

  return 1;
}

static int
object_manager_activate (lua_State *L)
{
  WpObjectManager *om = wplua_checkobject (L, 1, WP_TYPE_OBJECT_MANAGER);
  WpCore *core;

  lua_pushliteral (L, "wireplumber_core");
  lua_gettable (L, LUA_REGISTRYINDEX);
  core = lua_touserdata (L, -1);

  wp_core_install_object_manager (core, om);
  return 0;
}

static int
object_manager_iterate (lua_State *L)
{
  WpObjectManager *om = wplua_checkobject (L, 1, WP_TYPE_OBJECT_MANAGER);
  WpIterator *it = wp_object_manager_iterate (om);
  return push_wpiterator (L, it);
}

static int
object_manager_lookup (lua_State *L)
{
  WpObjectManager *om = wplua_checkobject (L, 1, WP_TYPE_OBJECT_MANAGER);
  WpObject *o = NULL;
  if (lua_isuserdata (L, 2)) {
    WpObjectInterest *oi = wplua_checkboxed (L, 2, WP_TYPE_OBJECT_INTEREST);
    o = wp_object_manager_lookup_full (om, oi);
  } else {
    o = wp_object_manager_lookup (om, WP_TYPE_OBJECT, NULL);
  }
  wplua_pushobject (L, o);
  return 1;
}

static const luaL_Reg object_manager_methods[] = {
  { "activate", object_manager_activate },
  { "iterate", object_manager_iterate },
  { "lookup", object_manager_lookup },
  { NULL, NULL }
};

/* WpMetadata */

static int
metadata_iterate (lua_State *L)
{
  WpMetadata *metadata = wplua_checkobject (L, 1, WP_TYPE_METADATA);
  lua_Integer subject = luaL_checkinteger (L, 2);
  g_autoptr (WpIterator) it = wp_metadata_iterate (metadata, subject);
  return push_metadata_wpiterator (L, it);
}

static int
metadata_find (lua_State *L)
{
  WpMetadata *metadata = wplua_checkobject (L, 1, WP_TYPE_METADATA);
  lua_Integer subject = luaL_checkinteger (L, 2);
  const char *key = luaL_checkstring (L, 3), *v = NULL, *t = NULL;
  v = wp_metadata_find (metadata, subject, key, &t);
  lua_pushstring (L, v);
  lua_pushstring (L, t);
  return 2;
}

static const luaL_Reg metadata_methods[] = {
  { "iterate", metadata_iterate },
  { "find", metadata_find },
  { NULL, NULL }
};

/* WpSession */

static int
session_iterate_endpoints (lua_State *L)
{
  WpSession *session = wplua_checkobject (L, 1, WP_TYPE_SESSION);
  WpIterator *it = wp_session_iterate_endpoints (session);
  return push_wpiterator (L, it);
}

static int
session_iterate_links (lua_State *L)
{
  WpSession *session = wplua_checkobject (L, 1, WP_TYPE_SESSION);
  WpIterator *it = wp_session_iterate_links (session);
  return push_wpiterator (L, it);
}

static const luaL_Reg session_methods[] = {
  { "iterate_endpoints", session_iterate_endpoints },
  { "iterate_links", session_iterate_links },
  { NULL, NULL }
};

/* WpEndpoint */

static int
endpoint_iterate_streams (lua_State *L)
{
  WpEndpoint *ep = wplua_checkobject (L, 1, WP_TYPE_ENDPOINT);
  WpIterator *it = wp_endpoint_iterate_streams (ep);
  return push_wpiterator (L, it);
}

static int
endpoint_create_link (lua_State *L)
{
  WpEndpoint *ep = wplua_checkobject (L, 1, WP_TYPE_ENDPOINT);
  luaL_checktype (L, 2, LUA_TTABLE);
  WpProperties *props = wplua_table_to_properties (L, 2);
  wp_endpoint_create_link (ep, props);
  return 0;
}

static const luaL_Reg endpoint_methods[] = {
  { "iterate_streams", endpoint_iterate_streams },
  { "create_link", endpoint_create_link },
  { NULL, NULL }
};

/* WpEndpointLink */

static int
endpoint_link_get_state (lua_State *L)
{
  WpEndpointLink *eplink = wplua_checkobject (L, 1, WP_TYPE_ENDPOINT_LINK);
  const gchar *error = NULL;
  WpEndpointLinkState state = wp_endpoint_link_get_state (eplink, &error);
  g_autoptr (GEnumClass) state_class =
      g_type_class_ref (WP_TYPE_ENDPOINT_LINK_STATE);
  lua_pushstring (L, g_enum_get_value (state_class, state)->value_nick);
  if (error)
    lua_pushstring (L, error);
  return error ? 2 : 1;
}

static int
endpoint_link_request_state (lua_State *L)
{
  WpEndpointLink *eplink = wplua_checkobject (L, 1, WP_TYPE_ENDPOINT_LINK);
  const gchar *states[] = { "inactive", "active" };
  int state = luaL_checkoption (L, 2, NULL, states);
  wp_endpoint_link_request_state (eplink, (WpEndpointLinkState) (state+1));
  return 0;
}

static int
endpoint_link_get_linked_object_ids (lua_State *L)
{
  WpEndpointLink *eplink = wplua_checkobject (L, 1, WP_TYPE_ENDPOINT_LINK);
  guint32 output_endpoint, output_stream;
  guint32 input_endpoint, input_stream;
  wp_endpoint_link_get_linked_object_ids (eplink,
      &output_endpoint, &output_stream,
      &input_endpoint, &input_stream);
  lua_pushinteger (L, output_endpoint);
  lua_pushinteger (L, output_stream);
  lua_pushinteger (L, input_endpoint);
  lua_pushinteger (L, input_stream);
  return 4;
}

static const luaL_Reg endpoint_link_methods[] = {
  { "get_state", endpoint_link_get_state },
  { "request_state", endpoint_link_request_state },
  { "get_linked_object_ids", endpoint_link_get_linked_object_ids },
  { NULL, NULL }
};

void
wp_lua_scripting_api_init (lua_State *L)
{
  g_autoptr (GError) error = NULL;

  luaL_newlib (L, log_funcs);
  lua_setglobal (L, "WpDebug");

  wplua_register_type_methods (L, WP_TYPE_GLOBAL_PROXY,
      NULL, global_proxy_methods);
  wplua_register_type_methods (L, WP_TYPE_OBJECT_INTEREST,
      object_interest_new, NULL);
  wplua_register_type_methods (L, WP_TYPE_OBJECT_MANAGER,
      object_manager_new, object_manager_methods);
  wplua_register_type_methods (L, WP_TYPE_METADATA,
      NULL, metadata_methods);
  wplua_register_type_methods (L, WP_TYPE_SESSION,
      NULL, session_methods);
  wplua_register_type_methods (L, WP_TYPE_ENDPOINT,
      NULL, endpoint_methods);
  wplua_register_type_methods (L, WP_TYPE_ENDPOINT_LINK,
      NULL, endpoint_link_methods);

  wplua_load_uri (L, URI_API, &error);
  if (G_UNLIKELY (error))
    wp_critical ("Failed to load api: %s", error->message);
}
