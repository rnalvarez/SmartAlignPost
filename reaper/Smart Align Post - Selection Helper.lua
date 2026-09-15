-- Smart Align Post - REAPER STATIC bridge
-- Primer item seleccionado = MASTER. Segundo = SOURCE.
-- STATIC corrige el desfase de contenido mediante D_STARTOFFS del SOURCE.
-- La posición D_POSITION de los items NO se modifica.

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
  if not take then return nil, nil, "sin take" end
  local src = reaper.GetMediaItemTake_Source(take)
  if not src then return nil, take, "sin source" end
  local path = reaper.GetMediaSourceFileName(src, "")
  if not path or path == "" then return nil, take, "source sin archivo" end
  return path, take
end

local n = reaper.CountSelectedMediaItems(0)
if n ~= 2 then
  reaper.ShowMessageBox("Seleccioná exactamente 2 items: primero MASTER, segundo SOURCE.", "Smart Align Post", 0)
  return
end

local masterItem = reaper.GetSelectedMediaItem(0, 0)
local sourceItem = reaper.GetSelectedMediaItem(0, 1)
local masterPath, masterTake, masterErr = take_source_path(masterItem)
local sourcePath, sourceTake, sourceErr = take_source_path(sourceItem)

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
local masterOffs = reaper.GetMediaItemTakeInfo_Value(masterTake, "D_STARTOFFS")
local sourceOffs = reaper.GetMediaItemTakeInfo_Value(sourceTake, "D_STARTOFFS")
local masterRate = reaper.GetMediaItemTakeInfo_Value(masterTake, "D_PLAYRATE")
local sourceRate = reaper.GetMediaItemTakeInfo_Value(sourceTake, "D_PLAYRATE")

if masterRate <= 0 or sourceRate <= 0 then
  reaper.ShowMessageBox("PLAYRATE inválido en uno de los takes.", "Smart Align Post — ERROR", 0)
  return
end

local exe = script_dir() .. "\\SmartAlignPostPrototype.exe"
local cmd = quote(exe) .. " " .. quote(masterPath) .. " " .. quote(sourcePath)

local processResult = reaper.ExecProcess(cmd, 60000)
if not processResult then
  reaper.ShowMessageBox("ExecProcess falló completamente.\n\n" .. cmd, "Smart Align Post — ERROR", 0)
  return
end

local normalized = processResult:gsub("\r\n", "\n"):gsub("\r", "\n")
local eol = normalized:find("\n", 1, true)
local returnCode
local output
if eol then
  returnCode = tonumber((normalized:sub(1, eol - 1)):match("^%s*(%-?%d+)%s*$"))
  output = normalized:sub(eol + 1)
else
  returnCode = tonumber(normalized:match("^%s*(%-?%d+)%s*$"))
  output = ""
end

if returnCode == nil then
  reaper.ShowMessageBox("No se pudo interpretar ExecProcess.\n\nRespuesta recibida:\n" .. normalized, "Smart Align Post — ERROR", 0)
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

local delaySeconds = dspDelayMs / 1000.0

-- Mantenemos los items donde están. Ajustamos únicamente el inicio de lectura
-- del SOURCE dentro de su WAV para compensar el retardo detectado.
local targetSourceOffs = masterOffs + (delaySeconds * sourceRate / masterRate)
local appliedOffsetSeconds = targetSourceOffs - sourceOffs
local appliedOffsetSamples = appliedOffsetSeconds * sourceRate

local confidenceText = confidence and string.format("%.3f", confidence) or "N/D"
local correlationText = correlation and string.format("%.6f", correlation) or "N/D"
local supportText = (supportWindows and totalWindows)
  and string.format("%d / %d ventanas", supportWindows, totalWindows)
  or "N/D"

local analysisMsg = string.format(
  "ANÁLISIS STATIC\n\n" ..
  "Delay DSP:                 %+0.6f ms\n" ..
  "Delay DSP:                 %+0.3f samples\n\n" ..
  "MASTER item:                %.9f s\n" ..
  "SOURCE item:                %.9f s\n" ..
  "(los items NO se moverán)\n\n" ..
  "MASTER STARTOFFS:           %.9f s\n" ..
  "SOURCE STARTOFFS actual:    %.9f s\n" ..
  "SOURCE STARTOFFS objetivo:  %.9f s\n\n" ..
  "Cambio de STARTOFFS:         %+0.3f samples\n" ..
  "Cambio temporal:            %+0.6f ms\n\n" ..
  "Confidence:                 %s\n" ..
  "Correlación:                %s\n" ..
  "Apoyo del delay:            %s\n\n" ..
  "¿Aplicar?",
  dspDelayMs,
  dspDelaySamples or (dspDelayMs * 48000.0 / 1000.0),
  masterPos, sourcePos,
  masterOffs, sourceOffs, targetSourceOffs,
  appliedOffsetSamples,
  appliedOffsetSeconds * 1000.0,
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
local beforeOffs = reaper.GetMediaItemTakeInfo_Value(sourceTake, "D_STARTOFFS")
local beforePos = reaper.GetMediaItemInfo_Value(sourceItem, "D_POSITION")
reaper.SetMediaItemTakeInfo_Value(sourceTake, "D_STARTOFFS", targetSourceOffs)
reaper.UpdateItemInProject(sourceItem)
reaper.UpdateArrange()
local afterOffs = reaper.GetMediaItemTakeInfo_Value(sourceTake, "D_STARTOFFS")
local afterPos = reaper.GetMediaItemInfo_Value(sourceItem, "D_POSITION")
reaper.Undo_EndBlock("Smart Align Post - STATIC TAKE ALIGN", -1)

local offsTolerance = 1e-8
local posTolerance = 1e-8
if math.abs(afterOffs - targetSourceOffs) > offsTolerance or math.abs(afterPos - beforePos) > posTolerance then
  reaper.ShowMessageBox(
    string.format(
      "APPLY FALLÓ\n\n" ..
      "SOURCE posición antes:  %.9f s\n" ..
      "SOURCE posición después: %.9f s\n\n" ..
      "STARTOFFS antes:     %.9f s\n" ..
      "STARTOFFS objetivo:  %.9f s\n" ..
      "STARTOFFS después:   %.9f s\n\n" ..
      "No se considera aplicado.",
      beforePos, afterPos, beforeOffs, targetSourceOffs, afterOffs
    ),
    "Smart Align Post — ERROR APPLY", 0)
  return
end

reaper.ShowMessageBox(
  string.format(
    "STATIC — APLICADO\n\n" ..
    "MASTER item:          %.9f s\n" ..
    "SOURCE item:          %.9f s\n\n" ..
    "STARTOFFS antes:      %.9f s\n" ..
    "STARTOFFS después:    %.9f s\n\n" ..
    "Delay DSP:            %+0.6f ms\n" ..
    "Cambio STARTOFFS:     %+0.3f samples\n\n" ..
    "Los items permanecen en su posición original.\n" ..
    "Undo disponible.",
    masterPos, sourcePos, beforeOffs, afterOffs,
    dspDelayMs, appliedOffsetSamples
  ),
  "Smart Align Post — APPLY OK", 0)
