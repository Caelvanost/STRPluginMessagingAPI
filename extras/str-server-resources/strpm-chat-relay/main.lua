-- Experimental STRPM relay for private Skyrim Together Reborn servers.
--
-- This resource consumes chat messages that begin with STRPM|v1| and relays
-- them back through the STR server chat channel. It is intentionally simple:
-- the SKSE/client-side STRPM bridge must still filter these messages from the
-- visible chat UI and translate them into STRPM receive callbacks.

local PREFIX = "STRPM|v1|"
local MAX_ENVELOPE_LENGTH = 24576

local gameServer = GameServer:get()
local playerManager = PlayerManager:get()

local function startsWith(value, prefix)
  return string.sub(value, 1, string.len(prefix)) == prefix
end

local function getPlayerByCharacter(entityId)
  local allPlayers = playerManager:GetAllPlayers()
  for _, player in ipairs(allPlayers) do
    if player:GetCharacter() == entityId then
      return player
    end
  end

  return nil
end

local function splitFields(envelope)
  local fields = {}

  for token in string.gmatch(envelope, "([^|]+)") do
    local separator = string.find(token, "=", 1, true)
    if separator then
      local key = string.sub(token, 1, separator - 1)
      local value = string.sub(token, separator + 1)
      fields[key] = value
    end
  end

  return fields
end

local function safeField(value)
  return string.gsub(tostring(value or ""), "|", "_")
end

local function relayToTarget(target, envelope)
  if target == nil or target == "" or target == "all" then
    gameServer:SendGlobalChatMessage(envelope)
    return
  end

  local id = string.match(target, "^id:(%d+)$")
  if id ~= nil then
    gameServer:SendChatMessage(tonumber(id), envelope)
    return
  end

  if target == "server" then
    return
  end

  -- STR server scripting does not expose a documented host lookup here.
  -- Until the native bridge exists, unknown targets are safest as all-player.
  gameServer:SendGlobalChatMessage(envelope)
end

addEventHandler("onChatMessage", function(entityId, message)
  if type(message) ~= "string" or not startsWith(message, PREFIX) then
    return
  end

  cancelEvent("STRPM relay envelope")

  if string.len(message) > MAX_ENVELOPE_LENGTH then
    print("[STRPM] Dropped oversized envelope")
    return
  end

  local player = getPlayerByCharacter(entityId)
  if player == nil then
    print("[STRPM] Dropped envelope from unknown character " .. tostring(entityId))
    return
  end

  local fields = splitFields(message)
  if fields["channel"] == nil or fields["payload"] == nil then
    print("[STRPM] Dropped malformed envelope")
    return
  end

  local relayEnvelope =
    message ..
    "|sender=" .. tostring(player:GetConnectionId()) ..
    "|senderName=" .. safeField(player:GetUsername()) ..
    "|serverTick=" .. tostring(gameServer:GetTick())

  relayToTarget(fields["target"], relayEnvelope)
end)

print("[STRPM] Experimental chat relay resource loaded")
