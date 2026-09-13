-- Smart Align Post - REAPER prototype bridge
-- V0: selected items -> C++ AlignEngine -> move SOURCE item by measured delay.
-- Primer item seleccionado = MASTER. Restantes = SOURCES.
-- Para una primera prueba usamos MASTER + 1 SOURCE.

local function script_dir()
  local src = debug.getinfo(1, "S").source
  if src:sub(1, 1) == "@" then src = src:sub(2) end
  return src:match("^(.*)[/\\][^/\\]+$") or "."
end

local function quote(s)
  return '"' .. tostring(s):gsub('"', '\\"') .. '"'
end

local function parse_output(text)
  local delay = tonumber(text:match("DELAY_MS=([%+%-]?[%d%.]+)"))
  local confidence = tonumber(text:match("CONFIDENCE=([%+%-]?[%d%.]+)"))
  return delay, confidence
end

local function take_source_path(item)
  local take = reaper.GetActiveTake(item)
  if not take then return nil, "sin take" end
  local src = reaper.GetMediaItemTake_Source(take)
  if not src then return nil, "sin source" end
  local path = reaper.GetMediaSourceFileName(src, "")
  if not path or path == "" then return nil, "source sin archivo" end
  return path
end

local n = reaper.CountSelectedMediaItems(0)
if n < 2 then
  reaper.ShowMessageBox("Seleccioná MASTER + al menos 1 SOURCE.", "Smart Align Post", 0)
  return
end

local masterItem = reaper.GetSelectedMediaItem(0, 0)
local masterPath, masterErr = take_source_path(masterItem)
if not masterPath then
  reaper.ShowMessageBox("MASTER: " .. masterErr, "Smart Align Post", 0)
  return
end

local exe = script_dir() .. "\\SmartAlignPostPrototype.exe"

reaper.Undo_BeginBlock()
reaper.PreventUIRefresh(1)

local report = {}
local failures = 0

for i = 1, n - 1 do
  local sourceItem = reaper.GetSelectedMediaItem(0, i)
  local sourcePath, sourceErr = take_source_path(sourceItem)

  if not sourcePath then
    report[#report + 1] = string.format("SOURCE %d: ERROR %s", i, sourceErr)
    failures = failures + 1
  else
    local cmd = quote(exe) .. " " .. quote(masterPath) .. " " .. quote(sourcePath)
    local rv, output = reaper.ExecProcess(cmd, 60000)

    if rv ~= 0 then
      report[#report + 1] = string.format("SOURCE %d: ERROR ejecutando analizador (code %s)\n%s", i, tostring(rv), tostring(output))
      failures = failures + 1
    else
      local delayMs, confidence = parse_output(output or "")
      if not delayMs or not confidence then
        report[#report + 1] = string.format("SOURCE %d: ERROR salida inesperada\n%s", i, tostring(output))
        failures = failures + 1
      else
        -- Positive delay means SOURCE is late relative to MASTER.
        -- Move the SOURCE item earlier by that amount on the REAPER timeline.
        local pos = reaper.GetMediaItemInfo_Value(sourceItem, "D_POSITION")
        local newPos = pos - (delayMs / 1000.0)
        reaper.SetMediaItemInfo_Value(sourceItem, "D_POSITION", newPos)
        reaper.UpdateItemInProject(sourceItem)

        report[#report + 1] = string.format(
          "SOURCE %d: %+0.3f ms | confidence %.3f | item %.3f -> %.3f s",
          i, delayMs, confidence, pos, newPos)
      end
    end
  end
end

reaper.PreventUIRefresh(-1)
reaper.Undo_EndBlock("Smart Align Post - prototype alignment", -1)
reaper.UpdateArrange()

local title = failures == 0 and "Smart Align Post — PROTOTIPO OK" or "Smart Align Post — PROTOTIPO CON ERRORES"
local msg = "MASTER:\n" .. masterPath .. "\n\n" .. table.concat(report, "\n") ..
  "\n\nV0: STATIC alignment mediante AlignEngine C++.\n" ..
  "El SOURCE se mueve en el timeline; no se modifica el archivo de audio."

reaper.ShowMessageBox(msg, title, 0)
