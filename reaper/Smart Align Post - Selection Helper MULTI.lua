-- Smart Align Post - REAPER STATIC MULTI bridge
-- Primer item seleccionado = MASTER. Los siguientes = SOURCES.
-- Cada SOURCE conserva su D_POSITION y se alinea modificando D_STARTOFFS.
-- Esto permite trabajar con varios micrófonos grabados simultáneamente.
-- La posición relativa de cada item también forma parte del cálculo.

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
if n < 2 then
  reaper.ShowMessageBox("Seleccioná al menos 2 items: primero MASTER y luego uno o más SOURCES.", "Smart Align Post", 0)
  return
end

local masterItem = reaper.GetSelectedMediaItem(0, 0)
local masterPath, masterTake, masterErr = take_source_path(masterItem)
if not masterPath then
  reaper.ShowMessageBox("MASTER: " .. masterErr, "Smart Align Post", 0)
  return
end

local masterPos = reaper.GetMediaItemInfo_Value(masterItem, "D_POSITION")
local masterOffs = reaper.GetMediaItemTakeInfo_Value(masterTake, "D_STARTOFFS")
local masterRate = reaper.GetMediaItemTakeInfo_Value(masterTake, "D_PLAYRATE")
if masterRate <= 0 then
  reaper.ShowMessageBox("PLAYRATE inválido en MASTER.", "Smart Align Post — ERROR", 0)
  return
end

local exe = script_dir() .. "\\SmartAlignPostPrototype.exe"
local results = {}
local analysisLines = {}
local anyLowConfidence = false

for i = 1, n - 1 do
  local sourceItem = reaper.GetSelectedMediaItem(0, i)
  local sourcePath, sourceTake, sourceErr = take_source_path(sourceItem)
  if not sourcePath then
    reaper.ShowMessageBox("SOURCE #" .. tostring(i) .. ": " .. sourceErr, "Smart Align Post — ERROR", 0)
    return
  end

  local sourcePos = reaper.GetMediaItemInfo_Value(sourceItem, "D_POSITION")
  local sourceOffs = reaper.GetMediaItemTakeInfo_Value(sourceTake, "D_STARTOFFS")
  local sourceRate = reaper.GetMediaItemTakeInfo_Value(sourceTake, "D_PLAYRATE")
  if sourceRate <= 0 then
    reaper.ShowMessageBox("PLAYRATE inválido en SOURCE #" .. tostring(i) .. ".", "Smart Align Post — ERROR", 0)
    return
  end

  local cmd = quote(exe) .. " " .. quote(masterPath) .. " " .. quote(sourcePath)
  local processResult = reaper.ExecProcess(cmd, 60000)
  if not processResult then
    reaper.ShowMessageBox("ExecProcess falló en SOURCE #" .. tostring(i) .. ".", "Smart Align Post — ERROR", 0)
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

  if returnCode == nil or returnCode ~= 0 then
    reaper.ShowMessageBox(
      "El analizador falló en SOURCE #" .. tostring(i) .. ".\n\n" .. normalized,
      "Smart Align Post — ERROR", 0)
    return
  end

  local dspDelayMs = tonumber(output:match("DELAY_MS=([%+%-]?[%d%.]+)"))
  local dspDelaySamples = tonumber(output:match("DELAY_SAMPLES=([%+%-]?[%d%.]+)"))
  local confidence = tonumber(output:match("CONFIDENCE=([%+%-]?[%d%.]+)"))
  local correlation = tonumber(output:match("CORRELATION=([%+%-]?[%d%.]+)"))
  local supportWindows = tonumber(output:match("SUPPORT_WINDOWS=([%+%-]?[%d%.]+)"))
  local totalWindows = tonumber(output:match("TOTAL_WINDOWS=([%+%-]?[%d%.]+)"))
  if not dspDelayMs then
    reaper.ShowMessageBox("SOURCE #" .. tostring(i) .. " no devolvió DELAY_MS.\n\n" .. output, "Smart Align Post — ERROR", 0)
    return
  end

  local delaySeconds = dspDelayMs / 1000.0
  -- Importante: D_STARTOFFS está en tiempo de media source. Si MASTER y SOURCE
  -- no arrancan en la misma posición del timeline, hay que compensar también
  -- esa diferencia. Con esto, items en 0 y items empezando más adelante siguen
  -- usando la misma referencia temporal real.
  local targetSourceOffs = masterOffs
    + ((sourcePos - masterPos) * sourceRate)
    + (delaySeconds * sourceRate / masterRate)

  local appliedOffsetSeconds = targetSourceOffs - sourceOffs
  local appliedOffsetSamples = appliedOffsetSeconds * sourceRate

  if not confidence or confidence < MIN_CONFIDENCE then
    anyLowConfidence = true
  end

  results[#results + 1] = {
    item = sourceItem,
    take = sourceTake,
    sourcePos = sourcePos,
    sourceOffs = sourceOffs,
    targetOffs = targetSourceOffs,
    appliedSamples = appliedOffsetSamples,
    delayMs = dspDelayMs,
    delaySamples = dspDelaySamples or (dspDelayMs * 48000.0 / 1000.0),
    confidence = confidence,
    correlation = correlation,
    support = supportWindows,
    total = totalWindows,
  }

  analysisLines[#analysisLines + 1] = string.format(
    "SOURCE %d\n" ..
    "  Delay: %+0.6f ms (%+.3f samples)\n" ..
    "  Timeline: %.6f s → %.6f s\n" ..
    "  Confidence: %.3f   Correlación: %.6f\n" ..
    "  STARTOFFS: %.9f s → %.9f s\n" ..
    "  Cambio: %+.3f samples\n" ..
    "  Soporte: %d / %d ventanas",
    i, dspDelayMs, dspDelaySamples or (dspDelayMs * 48.0),
    masterPos, sourcePos,
    confidence or 0.0, correlation or 0.0,
    sourceOffs, targetSourceOffs, appliedOffsetSamples,
    supportWindows or 0, totalWindows or 0)
end

local summary = table.concat({
  "ANÁLISIS STATIC — MULTI",
  "",
  "MASTER: item " .. string.format("%.9f s", masterPos),
  "Los items NO se moverán.",
  "",
  table.concat(analysisLines, "\n\n"),
  "",
  "¿Aplicar todos los SOURCES?"
}, "\n")

local answer = reaper.ShowMessageBox(summary, "Smart Align Post — ANALIZAR MULTI", 4)
if answer ~= 6 then
  reaper.ShowMessageBox("No se modificó ningún SOURCE.", "Smart Align Post — CANCELADO", 0)
  return
end

if anyLowConfidence then
  local lowAnswer = reaper.ShowMessageBox(
    "Uno o más SOURCES tienen confidence menor a " .. string.format("%.2f", MIN_CONFIDENCE) .. ".\n\n¿Aplicar igualmente todos los candidatos?",
    "Smart Align Post — CONFIDENCE BAJO", 4)
  if lowAnswer ~= 6 then
    reaper.ShowMessageBox("No se modificó ningún SOURCE.", "Smart Align Post — CANCELADO", 0)
    return
  end
end

reaper.Undo_BeginBlock()
local failures = {}
for _, r in ipairs(results) do
  local beforeOffs = reaper.GetMediaItemTakeInfo_Value(r.take, "D_STARTOFFS")
  local beforePos = reaper.GetMediaItemInfo_Value(r.item, "D_POSITION")
  reaper.SetMediaItemTakeInfo_Value(r.take, "D_STARTOFFS", r.targetOffs)
  reaper.UpdateItemInProject(r.item)
  local afterOffs = reaper.GetMediaItemTakeInfo_Value(r.take, "D_STARTOFFS")
  local afterPos = reaper.GetMediaItemInfo_Value(r.item, "D_POSITION")

  if math.abs(afterOffs - r.targetOffs) > 1e-8 or math.abs(afterPos - beforePos) > 1e-8 then
    failures[#failures + 1] = string.format(
      "SOURCE %.9f s → STARTOFFS %.9f / %.9f",
      beforePos, r.targetOffs, afterOffs)
  end
end
reaper.UpdateArrange()
reaper.Undo_EndBlock("Smart Align Post - STATIC MULTI APPLY", -1)

if #failures > 0 then
  reaper.ShowMessageBox("Uno o más SOURCES no pudieron aplicarse correctamente:\n\n" .. table.concat(failures, "\n"), "Smart Align Post — ERROR APPLY", 0)
  return
end

local appliedLines = {}
for i, r in ipairs(results) do
  local afterOffs = reaper.GetMediaItemTakeInfo_Value(r.take, "D_STARTOFFS")
  appliedLines[#appliedLines + 1] = string.format(
    "SOURCE %d: %+0.3f samples · STARTOFFS %.9f → %.9f",
    i, r.appliedSamples, r.sourceOffs, afterOffs)
end

reaper.ShowMessageBox(
  "STATIC MULTI — APLICADO\n\n" ..
  table.concat(appliedLines, "\n") ..
  "\n\nLos items permanecen en sus posiciones originales.\nUndo disponible.",
  "Smart Align Post — APPLY OK", 0)
