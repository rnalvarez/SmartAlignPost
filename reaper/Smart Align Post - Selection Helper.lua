-- Smart Align Post - REAPER STATIC bridge
-- Primer item seleccionado = MASTER. Segundo = SOURCE.
-- El delay DSP es la diferencia temporal entre el contenido de ambos WAV.
-- Por eso la posición final del SOURCE se calcula directamente desde MASTER:
--     sourceTarget = masterPos - delayDSP
-- No se suma nuevamente el desfase actual del item.

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
  reaper.ShowMessageBox("Seleccioná exactamente 2 items: primero MASTER, segundo SOURCE.", "Smart Align Post", 0)
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

local exe = script_dir() .. "\\SmartAlignPostPrototype.exe"
local cmd = quote(exe) .. " " .. quote(masterPath) .. " " .. quote(sourcePath)

local processResult = reaper.ExecProcess(cmd, 60000)
if not processResult then
  reaper.ShowMessageBox("ExecProcess falló completamente.\n\n" .. cmd, "Smart Align Post — ERROR", 0)
  return
end

local eol = processResult:find("[\\r\\n]")
local returnCode
local output
if eol then
  returnCode = tonumber(processResult:sub(1, eol - 1))
  local outputStart = eol + 1
  if processResult:sub(eol, eol) == "\\r" and processResult:sub(eol + 1, eol + 1) == "\\n" then
    outputStart = eol + 2
  end
  output = processResult:sub(outputStart)
else
  returnCode = tonumber(processResult)
  output = ""
end

if returnCode == nil then
  reaper.ShowMessageBox("No se pudo interpretar ExecProcess.\n\n" .. processResult, "Smart Align Post — ERROR", 0)
  return
end

if returnCode ~= 0 then
  reaper.ShowMessageBox("El analizador terminó con error.\n\nCódigo: " .. tostring(returnCode) .. "\n\n" .. output, "Smart Align Post — ERROR", 0)
  return
end

local dspDelayMs = tonumber(output:match("DELAY_MS=([%+%-]?[%d%.]+)"))
local dspDelaySamples = tonumber(output:match("DELAY_SAMPLES=([%+%-]?[%d%.]+)"))
local confidence = tonumber(output:match("CONFIDENCE=([%+%-]?[%d%.]+)"))
local correlation = tonumber(output:match("CORRELATION=([%+%-]?[%d%.]+)"))
local supportWindows = tonumber(output:match("SUPPORT_WINDOWS=([%+%-]?[%d%.]+)"))
local totalWindows = tonumber(output:match("TOTAL_WINDOWS=([%+%-]?[%d%.]+)"))

if not dspDelayMs then
  reaper.ShowMessageBox("El CLI no devolvió DELAY_MS.\n\n" .. output, "Smart Align Post — ERROR", 0)
  return
end

-- POSICIÓN ABSOLUTA CORRECTA:
-- MASTER representa el tiempo de referencia del contenido.
-- SOURCE debe quedar retrasado/adelantado respecto del MASTER únicamente por
-- el delay que existe dentro de sus WAV.
local targetPos = masterPos - (dspDelayMs / 1000.0)
local targetDeltaMs = (targetPos - sourcePos) * 1000.0
local sampleRate = 48000.0
local appliedDeltaSamples = (targetPos - sourcePos) * sampleRate

local confidenceText = confidence and string.format("%.3f", confidence) or "N/D"
local correlationText = correlation and string.format("%.6f", correlation) or "N/D"
local supportText = (supportWindows and totalWindows)
  and string.format("%d / %d ventanas", supportWindows, totalWindows)
  or "N/D"

local analysisMsg = string.format(
  "ANÁLISIS STATIC\n\n" ..
  "Delay DSP:             %+0.6f ms\n" ..
  "Delay DSP:             %+0.3f samples\n\n" ..
  "MASTER actual:         %.9f s\n" ..
  "SOURCE actual:         %.9f s\n" ..
  "SOURCE objetivo:       %.9f s\n\n" ..
  "Movimiento del item:   %+0.6f ms\n" ..
  "Movimiento del item:   %+0.3f samples\n\n" ..
  "Confidence:            %s\n" ..
  "Correlación:           %s\n" ..
  "Apoyo del delay:       %s\n\n" ..
  "¿Aplicar?",
  dspDelayMs,
  dspDelaySamples or (dspDelayMs * sampleRate / 1000.0),
  masterPos, sourcePos, targetPos,
  targetDeltaMs, appliedDeltaSamples,
  confidenceText, correlationText, supportText
)

local answer = reaper.ShowMessageBox(analysisMsg, "Smart Align Post — ANALIZAR", 4)
if not confidence or confidence < MIN_CONFIDENCE then
  local lowAnswer = reaper.ShowMessageBox(
    string.format("Confidence %.3f < %.2f\n\n¿Aplicar igualmente?", confidence or 0.0, MIN_CONFIDENCE),
    "Smart Align Post — CONFIDENCE BAJO", 4)
  if lowAnswer ~= 6 then return end
else
  if answer ~= 6 then return end
end

reaper.Undo_BeginBlock()
reaper.SetMediaItemPosition(sourceItem, targetPos, true)
reaper.UpdateItemInProject(sourceItem)
reaper.UpdateArrange()
local stateAfter = reaper.GetMediaItemInfo_Value(sourceItem, "D_POSITION")
reaper.Undo_EndBlock("Smart Align Post - STATIC APPLY", -1)

local tolerance = 1e-8
if math.abs(stateAfter - targetPos) > tolerance then
  reaper.ShowMessageBox(
    string.format(
      "APPLY FALLÓ\n\nAntes:      %.9f s\nObjetivo:   %.9f s\nDespués:   %.9f s\n\nREAPER no dejó el item en la posición objetivo.",
      sourcePos, targetPos, stateAfter
    ),
    "Smart Align Post — ERROR APPLY", 0)
  return
end

reaper.ShowMessageBox(
  string.format(
    "STATIC — APLICADO\n\n" ..
    "MASTER:         %.9f s\n" ..
    "SOURCE antes:   %.9f s\n" ..
    "SOURCE después: %.9f s\n\n" ..
    "Delay DSP:      %+0.6f ms\n" ..
    "Movimiento:     %+0.3f samples\n\n" ..
    "Undo disponible.",
    masterPos, sourcePos, stateAfter,
    dspDelayMs, appliedDeltaSamples
  ),
  "Smart Align Post — APPLY OK", 0)
