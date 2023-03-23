-- The smart filter policy configuration.
-- You need to enable "filter.smart" in 10-default-policy.lua
--

-- The default filter metadata configuration when wireplumber starts. They also
-- can be changed at runtime.
default_policy.filters_metadata = {
  ["filters"] = {
    -- Input filters (meant to be linked with Audio/Sink device nodes)
    {
      ["stream-name"] = "output.virtual-sink",  -- loopback playback
      ["node-name"] = "input.virtual-sink",     -- loopback sink
      ["direction"] = "input",  -- can only be 'input' or 'output'
      ["target"] = nil,  -- if nil, the default node will be used as target
      ["mode"] = "always",  -- can be 'always', 'never', 'capture-only' or 'playback-only'
      ["priority"] = 30,
    },
    {
      ["stream-name"] = "filter-chain-playback",  -- filter-chain playback
      ["node-name"] = "filter-chain-sink",        -- filter-chain sink
      ["direction"] = "input",  -- can only be 'input' or 'output'
      ["target"] = "speakers",  -- if nil, the default node will be used as target
      ["mode"] = "always",  -- can be 'always', 'never', 'capture-only' or 'playback-only'
      ["priority"] = 20,
    },
    {
      ["stream-name"] = "echo-cancel-playback",  -- echo-cancel playback
      ["node-name"] = "echo-cancel-sink",        -- echo-cancel sink
      ["direction"] = "input",  -- can only be 'input' or 'output'
      ["target"] = "speakers",  -- if nil, the default node will be used as target
      ["mode"] = "capture-only",  -- can be 'always', 'never', 'playback-only' or 'capture-only'
      ["priority"] = 10,
    },

    -- Output filters (meant to be linked with Audio/Source device nodes)
    {
      ["stream-name"] = "input.virtual-source",  -- loopback capture
      ["node-name"] = "output.virtual-source",   -- loopback source
      ["direction"] = "output",  -- can only be 'input' or 'output'
      ["target"] = nil,  -- if nil, the default node will be used as target
      ["mode"] = "always",  -- can be 'always', 'never', 'playback-only' or 'capture-only'
      ["priority"] = 30,
    },
    {
      ["stream-name"] = "filter-chain-capture",  -- filter-chain capture
      ["node-name"] = "filter-chain-source",     -- filter-chain source
      ["direction"] = "output",  -- can only be 'input' or 'output'
      ["target"] = "microphone",  -- if nil, the default node will be used as target
      ["mode"] = "capture-only",  -- can be 'always', 'never', 'playback-only' or 'capture-only'
      ["priority"] = 20,
    },
    {
      ["stream-name"] = "echo-cancel-capture",  -- echo-cancel capture
      ["node-name"] = "echo-cancel-source",     -- echo-cancel source
      ["direction"] = "output",  -- can only be 'input' or 'output'
      ["target"] = "microphone",  -- if nil, the default node will be used as target
      ["mode"] = "capture-only",  -- can be 'always', 'never', 'playback-only' or 'capture-only'
      ["priority"] = 10,
    }
  },

  -- The target node properties (any node properties can be defined)
  ["targets"] = {
    ["speakers"] = {
      ["media.class"] = "Audio/Sink",
      ["alsa.card_name"] = "acp5x",
      ["device.profile.description"] = "Speaker",
    },
    ["microphone"] = {
      ["media.class"] = "Audio/Source",
      ["alsa.card_name"] = "acp5x",
    }
  }
}
