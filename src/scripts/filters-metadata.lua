-- WirePlumber
--
-- Copyright © 2023 Collabora Ltd.
--    @author Julian Bouzas <julian.bouzas@collabora.com>
--
-- SPDX-License-Identifier: MIT

-- Receive script arguments
local config = ... or {}
config["filters"] = config["filters"] or {}
config["targets"] = config["targets"] or {}

f_metadata = ImplMetadata("filters")
f_metadata:activate(Features.ALL, function (m, e)
  if e then
    Log.warning("failed to activate filters metadata: " .. tostring(e))
    return
  end

  Log.info("activated filters metadata")

  -- Set filters metadata
  local filters = {}
  for _, f in ipairs(config["filters"]) do
    table.insert (filters, Json.Object (f))
  end
  local filters_json = Json.Array (filters)
  m:set (0, "filters.configured.filters", "Spa:String:JSON",
          filters_json:to_string())

  -- Set targets metadata
  local targets = {}
  for name, props in pairs(config["targets"]) do
    targets[name] = Json.Object (props)
  end
  local targets_json = Json.Object (targets)
  m:set (0, "filters.configured.targets", "Spa:String:JSON",
          targets_json:to_string())
end)
