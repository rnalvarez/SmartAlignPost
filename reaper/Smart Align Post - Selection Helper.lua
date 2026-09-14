-- Smart Align Post - REAPER prototype bridge
-- V0 STATIC: selected items -> C++ AlignEngine -> move SOURCE item by measured delay.
-- Primer item seleccionado = MASTER. Segundo = SOURCE.

local MIN_CONFIDENCE = 0.80

local function script_dir()
  local src = debug.getinfo(1, "S").source
  if src:sub(1, 1) == "@" then src = src:sub(2) end
  return src:match("^(.*)[/\\][^/\\]+$") or "."
end

local function quote(s)
  return '"' .. tostring(s):gsub('"', '\\"') .. '"'
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
if n ~= 2 then
  reaper.ShowMessageBox("Para esta primera prueba seleccioná exactamente 2 items: primero BOOM (MASTER), segundo CORBATERO (SOURCE).", "Smart Align Post", 0)
  return
end

local masterItem = reaper.GetSelectedMediaItem(0, 0)
local sourceItem = reaper.GetSelectedMediaItem(0, 1)
local masterPath, masterErr = take_source_path(masterItem)
local sourcePath, sourceErr = take_source_path(sourceItem)

if not masterPath then
  reaper.ShowMessageBox("MASTER: " .. masterErr, "Smart Align Post", 0)
  return
end
if not sourcePath then
  reaper.ShowMessageBox("SOURCE: " .. sourceErr, "Smart Align Post", 0)
  return
end

local masterPos = reaper.GetMediaItemInfo_Value(masterItem, "D_POSITION")
local sourcePos = reaper.GetMediaItemInfo_Value(sourceItem, "D_POSITION")

-- Windows: el ejecutable debe estar junto a este .lua.
local exe = script_dir() .. "\\SmartAlignPostPrototype.exe"
local cmd = quote(exe) .. " " .. quote(masterPath) .. " " .. quote(sourcePath)

-- REAPER ExecProcess devuelve un único string:
-- primera línea = código de retorno; resto = stdout/stderr del proceso.
local processResult = reaper.ExecProcess(cmd, 60000)
if not processResult then
  reaper.ShowMessageBox("ExecProcess falló completamente.\n\nComando:\n" .. cmd, "Smart Align Post — ERROR", 0)
  return
end

local eol = processResult:find("[\r\n]")
local returnCode
local output

if eol then
  returnCode = tonumber(processResult:sub(1, eol - 1))
  local outputStart = eol + 1
  if processResult:sub(eol, eol) == "\r" and processResult:sub(eol + 1, eol + 1) == "\n" then
    outputStart = eol + 2
  end
  output = processResult:sub(outputStart)
else
  returnCode = tonumber(processResult)
  output = ""
end

if returnCode == nil then
  reaper.ShowMessageBox("No se pudo interpretar la respuesta de ExecProcess.\n\nRespuesta:\n" .. processResult, "Smart Align Post — ERROR", 0)
  return
end

if returnCode ~= 0 then
  reaper.ShowMessageBox("El analizador terminó con error.\n\nCódigo: " .. tostring(returnCode) .. "\n\nSalida:\n" .. output, "Smart Align Post — ERROR", 0)
  return
end

local dspDelayMs = tonumber(output:match("DELAY_MS=([%+%-]?[%d%.]+)"))
local confidence = tonumber(output:match("CONFIDENCE=([%+%-]?[%d%.]+)"))

if not dspDelayMs then
  reaper.ShowMessageBox("El CLI terminó correctamente, pero no devolvió DELAY_MS.\n\nSalida:\n" .. output, "Smart Align Post — ERROR", 0)
  return
end

-- Delay total que debemos compensar en el timeline:
--   1) desfase ya existente entre SOURCE y MASTER en REAPER
--   2) delay acústico medido entre los contenidos WAV
local timelineDelayMs = (sourcePos - masterPos) * 1000.0
local totalDelayMs = timelineDelayMs + dspDelayMs

local confidenceText = confidence and string.format("%.3f", confidence) or "N/D"

-- Nunca aplicar un resultado poco confiable.
if not confidence or confidence < MIN_CONFIDENCE then
  local msg = string.format(
    "ANÁLISIS NO CONFIABLE\n\nDelay DSP candidato: %+0.3f ms\nDesfase timeline: %+0.3f ms\nConfidence: %s\n\nUmbral mínimo: %.2f\n\nNo se modificó la posición del SOURCE.",
    dspDelayMs, timelineDelayMs, confidenceText, MIN_CONFIDENCE
  )
  reaper.ShowMessageBox(msg, "Smart Align Post — RECHAZADO", 0)
  return
end

local newPos = sourcePos - (totalDelayMs / 1000.0)

reaper.Undo_BeginBlock()
reaper.SetMediaItemInfo_Value(sourceItem, "D_POSITION", newPos)
reaper.UpdateItemInProject(sourceItem)
reaper.Undo_EndBlock("Smart Align Post - prototype STATIC alignment", -1)
reaper.UpdateArrange()

local msg = string.format(
  "MASTER: BOOM\nSOURCE: CORBATERO\n\nDelay DSP: %+0.3f ms\nDesfase timeline: %+0.3f ms\nCorrección total: %+0.3f ms\nConfidence: %s\n\nSOURCE movido:\n%.6f s → %.6f s\n\nModo: STATIC\nEl archivo WAV original no fue modificado.",
  dspDelayMs, timelineDelayMs, totalDelayMs, confidenceText, sourcePos, newPos
)

reaper.ShowMessageBox(msg, "Smart Align Post — PROTOTIPO OK", 0)
