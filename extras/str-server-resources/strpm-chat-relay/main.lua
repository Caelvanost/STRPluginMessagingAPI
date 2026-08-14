-- STR Plugin Messaging chat relay for unmodified Skyrim Together servers.
--
-- This resource only handles server-side routing. A client bridge still has
-- to inject STRPM envelopes into STR chat and consume relayed envelopes before
-- they are shown by the overlay.

local PREFIX = "STRPM|v2|"
local MAX_ENVELOPE_LENGTH = 4096
local MAX_CHANNEL_LENGTH = 96
local MAX_PARTS = 64

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

local function validUnsigned(value)
  return value ~= nil and string.match(value, "^%d+$") ~= nil
end

local function validChannel(value)
  return value ~= nil
    and string.len(value) > 0
    and string.len(value) <= MAX_CHANNEL_LENGTH
    and string.match(value, "^[A-Za-z0-9._%-]+$") ~= nil
end

local function safeField(value)
  local result = tostring(value or "")
  result = string.gsub(result, "[|\r\n]", "_")
  return result
end

local function validateEnvelope(fields)
  if fields["msg"] == nil or not validUnsigned(fields["seq"]) then
    return false, "missing msg/seq"
  end
  if not validChannel(fields["channel"]) then
    return false, "invalid channel"
  end
  if fields["payload"] == nil or string.match(fields["payload"], "^[0-9A-Fa-f]*$") == nil then
    return false, "invalid payload"
  end
  if not validUnsigned(fields["part"]) or not validUnsigned(fields["parts"]) then
    return false, "invalid fragment metadata"
  end

  local part = tonumber(fields["part"])
  local parts = tonumber(fields["parts"])
  if part == nil or parts == nil or parts < 1 or parts > MAX_PARTS or part < 1 or part > parts then
    return false, "fragment out of range"
  end

  local target = fields["target"] or "all"
  if target ~= "all" and target ~= "server" and string.match(target, "^id:%d+$") == nil then
    return false, "invalid target"
  end

  return true, nil
end

local function relayToTarget(target, envelope)
  if target == nil or target == "" or target == "all" then
    gameServer:SendGlobalChatMessage(envelope)
    return true
  end

  local id = string.match(target, "^id:(%d+)$")
  if id ~= nil then
    gameServer:SendChatMessage(tonumber(id), envelope)
    return true
  end

  if target == "server" then
    return true
  end

  return false
end

addEventHandler("onChatMessage", function(entityId, message)
  if type(message) ~= "string" or not startsWith(message, PREFIX) then
    return
  end

  -- Prevent the transport envelope from being rebroadcast as ordinary chat.
  cancelEvent("STRPM transport envelope")

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
  local valid, reason = validateEnvelope(fields)
  if not valid then
    print("[STRPM] Dropped malformed envelope: " .. tostring(reason))
    return
  end

  local relayEnvelope =
    message ..
    "|sender=" .. tostring(player:GetConnectionId()) ..
    "|senderName=" .. safeField(player:GetUsername()) ..
    "|serverTick=" .. tostring(gameServer:GetTick())

  if not relayToTarget(fields["target"], relayEnvelope) then
    print("[STRPM] Dropped envelope with unsupported target")
  end
end)

print("[STRPM] Chat relay v2 loaded")
