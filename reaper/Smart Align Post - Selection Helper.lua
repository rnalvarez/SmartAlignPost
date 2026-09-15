-- Smart Align Post - REAPER prototype bridge
-- STATIC: selected items -> C++ AlignEngine -> analyze -> confirm -> move SOURCE.
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
  reaper.ShowMessageBox("Seleccioná exactamente 2 items: primero MASTER (BOOM), segundo SOURCE (CORBATERO).", "Smart Align Post", 0)
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
local windowSec = tonumber(output:match("WINDOW_SEC=([%+%-]?[%d%.]+)"))
local correlation = tonumber(output:match("CORRELATION=([%+%-]?[%d%.]+)"))
local supportWindows = tonumber(output:match("SUPPORT_WINDOWS=([%+%-]?[%d%.]+)"))
local totalWindows = tonumber(output:match("TOTAL_WINDOWS=([%+%-]?[%d%.]+)"))

if not dspDelayMs then
  reaper.ShowMessageBox("El CLI terminó correctamente, pero no devolvió DELAY_MS.\n\nSalida:\n" .. output, "Smart Align Post — ERROR", 0)
  return
end

-- IMPORTANTE: DELAY_MS ya representa el desfase temporal total entre ambos
-- WAV en las condiciones actuales, incluyendo cualquier desfase previo en el
-- timeline. No volver a sumarle (sourcePos-masterPos), porque eso duplica la
-- corrección cuando el usuario ya desplazó manualmente el item.
local timelineDelayMs = (sourcePos - masterPos) * 1000.0
local totalDelayMs = dspDelayMs
local correctionMs = dspDelayMs
local confidenceText = confidence and string.format("%.3f", confidence) or "N/D"
local windowText = windowSec and string.format("%.3f s", windowSec) or "N/D"
local correlationText = correlation and string.format("%.6f", correlation) or "N/D"
local supportText = (supportWindows and totalWindows)
  and string.format("%d / %d ventanas", supportWindows, totalWindows)
  or "N/D"

-- Mover SOURCE hacia atrás exactamente el delay detectado por el DSP.
local newPos = sourcePos - (correctionMs / 1000.0)
local expectedAppliedSamples = (sourcePos - newPos) * 48000.0

local analysisMsg = string.format(
  "ANÁLISIS STATIC\n\n" ..
  "Delay DSP detectado: %+0.6f ms\n" ..
  "Desfase actual en timeline: %+0.6f ms\n\n" ..
  "Corrección a aplicar: %+0.6f ms\n\n" ..
  "Confidence:         %s\n" ..
  "Correlación:        %s\n" ..
  "Ventana analizada:  %s\n" ..
  "Apoyo del delay:    %s\n\n" ..
  "Corrección propuesta: %+0.3f samples\n" ..
  "SOURCE actual:       %.9f s\n" ..
  "SOURCE propuesto:    %.9f s\n\n" ..
  "¿Aplicar alineamiento?",
  dspDelayMs, timelineDelayMs,
  correctionMs,
  confidenceText, correlationText, windowText, supportText,
  expectedAppliedSamples, sourcePos, newPos
)

local answer = reaper.ShowMessageBox(analysisMsg, "Smart Align Post — ANALIZAR", 4)

if not confidence or confidence < MIN_CONFIDENCE then
  local lowConfidenceMsg = string.format(
    "ADVERTENCIA — CONFIANZA BAJA\n\n" ..
    "Confidence: %s (mínimo automático: %.2f)\n" ..
    "Correlación: %s\n" ..
    "Apoyo del delay: %s\n\n" ..
    "El candidato es: %+0.6f ms\n" ..
    "Corrección propuesta: %+0.3f samples\n\n" ..
    "Sí = APLICAR DE TODOS MODOS\n" ..
    "No = CANCELAR",
    confidenceText, MIN_CONFIDENCE,
    correlationText, supportText,
    correctionMs, expectedAppliedSamples
  )

  local lowAnswer = reaper.ShowMessageBox(lowConfidenceMsg, "Smart Align Post — CONFIDENCE BAJO", 4)
  if lowAnswer ~= 6 then
    reaper.ShowMessageBox("No se modificó la posición del SOURCE.", "Smart Align Post — CANCELADO", 0)
    return
  end
else
  if answer ~= 6 then
    reaper.ShowMessageBox("No se modificó la posición del SOURCE.", "Smart Align Post — CANCELADO", 0)
    return
  end
end

reaper.Undo_BeginBlock()
local beforeApplyPos = reaper.GetMediaItemInfo_Value(sourceItem, "D_POSITION")
local setOk = reaper.SetMediaItemPosition(sourceItem, newPos, true)
reaper.UpdateItemInProject(sourceItem)
reaper.UpdateArrange()
local stateAfter = reaper.GetMediaItemInfo_Value(sourceItem, "D_POSITION")
reaper.Undo_EndBlock("Smart Align Post - STATIC APPLY", -1)

local positionTolerance = 0.0000001
if not setOk or math.abs(stateAfter - newPos) > positionTolerance then
  reaper.ShowMessageBox(
    string.format(
      "ERROR DE APPLY\n\n" ..
      "REAPER no confirmó la posición solicitada.\n\n" ..
      "Antes:       %.9f s\n" ..
      "Solicitada:  %.9f s\n" ..
      "Después:     %.9f s\n\n" ..
      "No se considera aplicado.",
      beforeApplyPos, newPos, stateAfter
    ),
    "Smart Align Post — ERROR APPLY", 0)
  return
end

local masterSamples = masterPos * 48000.0
local sourceBeforeSamples = sourcePos * 48000.0
local sourceAfterSamples = stateAfter * 48000.0
local appliedSamples = (sourcePos - stateAfter) * 48000.0

local msg = string.format(
  "STATIC — APLICADO\n\n" ..
  "MASTER antes:       %.9f s  (%.2f samples)\n" ..
  "SOURCE antes:       %.9f s  (%.2f samples)\n\n" ..
  "Delay DSP:          %+0.6f ms\n" ..
  "Desfase timeline:   %+0.6f ms\n" ..
  "Corrección aplicada: %+0.6f ms\n" ..
  "Confidence:         %s\n" ..
  "Correlación:        %s\n" ..
  "Apoyo del delay:    %s\n\n" ..
  "Corrección aplicada: %+0.3f samples\n" ..
  "SOURCE después:     %.9f s  (%.2f samples)\n\n" ..
  "Undo disponible en REAPER.\n" ..
  "El archivo WAV original no fue modificado.",
  masterPos, masterSamples,
  sourcePos, sourceBeforeSamples,
  dspDelayMs, timelineDelayMs, correctionMs,
  confidenceText, correlationText, supportText,
  appliedSamples, stateAfter, sourceAfterSamples
)

reaper.ShowMessageBox(msg, "Smart Align Post — APPLY OK", 0)
