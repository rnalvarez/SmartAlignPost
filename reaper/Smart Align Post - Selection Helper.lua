-- Smart Align Post - REAPER helper prototype
-- V1: selection/role helper only.
-- This script does NOT alter media. It prints selected items and suggested routing.
-- The full item-aware offline bridge is planned for V1.1.

local n = reaper.CountSelectedMediaItems(0)
if n < 2 then
  reaper.ShowMessageBox("Seleccioná al menos MASTER + 1 SOURCE.", "Smart Align Post", 0)
  return
end

local lines = {}
for i=0,n-1 do
  local item = reaper.GetSelectedMediaItem(0,i)
  local take = reaper.GetActiveTake(item)
  local name = take and reaper.GetTakeName(take) or "(sin take)"
  local role = (i == 0) and "MASTER" or ("SOURCE "..i)
  lines[#lines+1] = string.format("%s: %s", role, name)
end

local msg = table.concat(lines, "\n") ..
  "\n\nV1 prototype:\n" ..
  "• Primer item seleccionado = MASTER\n" ..
  "• Restantes = SOURCES\n" ..
  "• La alineación DSP está en el motor C++.\n" ..
  "• El puente offline que aplicará cambios a items se implementará en la siguiente iteración."

reaper.ShowMessageBox(msg, "Smart Align Post V1", 0)
