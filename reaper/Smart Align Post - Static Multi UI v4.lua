-- Smart Align Post - STATIC MULTI UI v4
-- Primera selección = MASTER. Siguientes = SOURCES.
-- Analiza muestras correspondientes al MISMO INSTANTE DEL TIMELINE.
-- CORRECCIÓN CLAVE: se aplica el delay residual sobre el D_STARTOFFS ACTUAL del SOURCE.
-- No se reconstruye el STARTOFFS desde MASTER, evitando borrar el desfase que se está midiendo.
-- D_POSITION nunca se modifica.

local MIN_CONFIDENCE = 0.80
local MIN_ANALYSIS_SEC = 0.25
local WIN_W, WIN_H = 820, 560

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

local function rgb(r, g, b) gfx.set(r / 255, g / 255, b / 255, 1) end
local function rect(x, y, w, h, r, g, b) rgb(r, g, b); gfx.rect(x, y, w, h, 1) end
local function text(x, y, s, size, r, g, b)
  rgb(r or 230, g or 230, b or 230)
  gfx.setfont(1, "Arial", size or 16)
  gfx.x, gfx.y = x, y
  gfx.drawstr(tostring(s))
end
local function inside(x, y, w, h, mx, my)
  return mx >= x and mx <= x + w and my >= y and my <= y + h
end

local results = {}
local selectedCount = 0
local masterItem, masterTake, masterPath = nil, nil, nil
local masterPos, masterOffs, masterRate, masterLength = 0, 0, 1, 0
local status, statusKind = "Esperando selección de REAPER.", "info"
local analyzed, applied = false, false
local lastMouseDown = false

local function set_status(msg, kind)
  status, statusKind = msg or "", kind or "info"
end

local function fail(msg)
  results = {}
  analyzed = false
  applied = false
  set_status(msg, "error")
end

local function parse_process_output(processResult)
  if not processResult then return nil, nil, "ExecProcess falló." end
  local normalized = processResult:gsub("\r\n", "\n"):gsub("\r", "\n")
  local eol = normalized:find("\n", 1, true)
  if eol then
    local code = tonumber((normalized:sub(1, eol - 1)):match("^%s*(%-?%d+)%s*$"))
    return code, normalized:sub(eol + 1), normalized
  end
  return tonumber(normalized:match("^%s*(%-?%d+)%s*$")), "", normalized
end

local function analyze_selection()
  results = {}
  analyzed = false
  applied = false
  selectedCount = reaper.CountSelectedMediaItems(0)
  if selectedCount < 2 then
    fail("Seleccioná al menos 2 items: MASTER primero + uno o más SOURCES.")
    return
  end

  masterItem = reaper.GetSelectedMediaItem(0, 0)
  masterPath, masterTake, masterErr = take_source_path(masterItem)
  if not masterPath then fail("MASTER: " .. tostring(masterErr)); return end

  masterPos = reaper.GetMediaItemInfo_Value(masterItem, "D_POSITION")
  masterLength = reaper.GetMediaItemInfo_Value(masterItem, "D_LENGTH")
  masterOffs = reaper.GetMediaItemTakeInfo_Value(masterTake, "D_STARTOFFS")
  masterRate = reaper.GetMediaItemTakeInfo_Value(masterTake, "D_PLAYRATE")
  if masterRate <= 0 then fail("PLAYRATE inválido en MASTER."); return end

  local exe = script_dir() .. "\\SmartAlignPostPrototype.exe"
  local low = 0

  for i = 1, selectedCount - 1 do
    local sourceItem = reaper.GetSelectedMediaItem(0, i)
    local sourcePath, sourceTake, sourceErr = take_source_path(sourceItem)
    if not sourcePath then fail("SOURCE #" .. i .. ": " .. tostring(sourceErr)); return end

    local sourcePos = reaper.GetMediaItemInfo_Value(sourceItem, "D_POSITION")
    local sourceLength = reaper.GetMediaItemInfo_Value(sourceItem, "D_LENGTH")
    local sourceOffs = reaper.GetMediaItemTakeInfo_Value(sourceTake, "D_STARTOFFS")
    local sourceRate = reaper.GetMediaItemTakeInfo_Value(sourceTake, "D_PLAYRATE")
    if sourceRate <= 0 then fail("PLAYRATE inválido en SOURCE #" .. i .. "."); return end

    local commonStart = math.max(masterPos, sourcePos)
    local commonEnd = math.min(masterPos + masterLength, sourcePos + sourceLength)
    if commonEnd - commonStart < MIN_ANALYSIS_SEC then
      fail(string.format("SOURCE #%d: no hay suficiente tramo común en el timeline para analizar (%.3f s).", i, MIN_ANALYSIS_SEC))
      return
    end

    -- Comparamos las muestras que REAPER está reproduciendo en el mismo instante.
    local masterAnalysisStart = masterOffs + (commonStart - masterPos) * masterRate
    local sourceAnalysisStart = sourceOffs + (commonStart - sourcePos) * sourceRate

    local cmd = quote(exe) .. " " .. quote(masterPath) .. " " .. quote(sourcePath)
      .. " " .. quote(string.format("%.12f", masterAnalysisStart))
      .. " " .. quote(string.format("%.12f", sourceAnalysisStart))

    local returnCode, output, normalized = parse_process_output(reaper.ExecProcess(cmd, 60000))
    if returnCode ~= 0 then
      fail("El analizador falló en SOURCE #" .. i .. ".\n\n" .. tostring(normalized))
      return
    end

    local delayMs = tonumber(output:match("DELAY_MS=([%+%-]?[%d%.]+)"))
    local delaySamples = tonumber(output:match("DELAY_SAMPLES=([%+%-]?[%d%.]+)"))
    local confidence = tonumber(output:match("CONFIDENCE=([%+%-]?[%d%.]+)"))
    local correlation = tonumber(output:match("CORRELATION=([%+%-]?[%d%.]+)"))
    local support = tonumber(output:match("SUPPORT_WINDOWS=([%+%-]?[%d%.]+)"))
    local total = tonumber(output:match("TOTAL_WINDOWS=([%+%-]?[%d%.]+)"))
    if not delayMs then
      fail("SOURCE #" .. i .. " no devolvió DELAY_MS.\n\n" .. tostring(output))
      return
    end

    -- El analyzer devuelve el delay RESIDUAL entre los contenidos actuales.
    -- Para corregir ese residual, ajustamos el STARTOFFS que SOURCE YA TIENE.
    local targetOffs = sourceOffs
      + ((delayMs / 1000.0) * sourceRate / masterRate)
    local deltaSamples = (targetOffs - sourceOffs) * sourceRate

    if not confidence or confidence < MIN_CONFIDENCE then low = low + 1 end

    results[#results + 1] = {
      index = i,
      item = sourceItem,
      take = sourceTake,
      sourcePos = sourcePos,
      sourceOffs = sourceOffs,
      targetOffs = targetOffs,
      deltaSamples = deltaSamples,
      delayMs = delayMs,
      delaySamples = delaySamples or (delayMs * sourceRate / 1000.0),
      confidence = confidence,
      correlation = correlation,
      support = support,
      total = total,
      commonStart = commonStart,
    }
  end

  analyzed = true
  set_status(string.format("Analizados %d SOURCE(s) sobre el mismo instante del timeline. %d con confidence < %.2f.", #results, low, MIN_CONFIDENCE), low > 0 and "warn" or "ok")
end

local function apply_results()
  if not analyzed or #results == 0 then
    set_status("Primero ejecutá ANALYZE.", "warn")
    return
  end

  local low = 0
  for _, r in ipairs(results) do
    if not r.confidence or r.confidence < MIN_CONFIDENCE then low = low + 1 end
  end

  if low > 0 then
    local answer = reaper.ShowMessageBox(
      string.format("Hay %d SOURCE(s) con confidence menor a %.2f.\n\n¿Aplicar igualmente?", low, MIN_CONFIDENCE),
      "Smart Align Post — CONFIDENCE", 4)
    if answer ~= 6 then
      set_status("APPLY cancelado.", "info")
      return
    end
  end

  reaper.Undo_BeginBlock()
  local failures = 0
  local originalPositions = {}

  for _, r in ipairs(results) do
    originalPositions[r] = reaper.GetMediaItemInfo_Value(r.item, "D_POSITION")
    reaper.SetMediaItemTakeInfo_Value(r.take, "D_STARTOFFS", r.targetOffs)
    reaper.UpdateItemInProject(r.item)
  end

  reaper.UpdateArrange()

  for _, r in ipairs(results) do
    local afterOffs = reaper.GetMediaItemTakeInfo_Value(r.take, "D_STARTOFFS")
    local afterPos = reaper.GetMediaItemInfo_Value(r.item, "D_POSITION")
    if math.abs(afterOffs - r.targetOffs) > 1e-8 or math.abs(afterPos - originalPositions[r]) > 1e-8 then
      failures = failures + 1
    end
  end

  reaper.Undo_EndBlock("Smart Align Post - STATIC MULTI APPLY", -1)

  if failures > 0 then
    set_status(string.format("APPLY: %d SOURCE(s) con error de verificación.", failures), "error")
    return
  end

  applied = true
  set_status(string.format("Aplicado: %d SOURCE(s). D_POSITION intacto. Undo disponible.", #results), "ok")
end

local function draw_button(x, y, w, h, label, enabled, primary)
  local hover = enabled and inside(x, y, w, h, gfx.mouse_x, gfx.mouse_y)
  if not enabled then
    rect(x, y, w, h, 55, 55, 60)
  elseif primary then
    rect(x, y, w, h, hover and 70 or 52, hover and 155 or 125, hover and 245 or 210)
  else
    rect(x, y, w, h, hover and 78 or 64, hover and 78 or 64, hover and 85 or 70)
  end
  local tw = gfx.measurestr(label)
  text(x + (w - tw) / 2, y + 10, label, 16, enabled and 245 or 135, enabled and 245 or 135, enabled and 250 or 135)
end

local function draw_ui()
  rect(0, 0, gfx.w, gfx.h, 24, 25, 29)
  text(24, 18, "SMART ALIGN POST", 25, 245, 245, 250)
  text(24, 49, "STATIC · MULTI SOURCE v4", 15, 160, 170, 185)
  text(24, 73, "MASTER = primer item seleccionado", 14, 195, 200, 210)
  text(24, 92, "Análisis residual sobre el mismo instante · ajuste en D_STARTOFFS", 13, 145, 155, 170)

  local n = reaper.CountSelectedMediaItems(0)
  text(585, 24, "Seleccionados: " .. n, 15, 205, 210, 220)
  text(585, 48, "MASTER: " .. (masterItem and "OK" or "—"), 14, 160, 175, 185)

  local tableY = 125
  rect(18, tableY, gfx.w - 36, 32, 45, 47, 53)
  text(30, tableY + 8, "SOURCE", 14, 190, 195, 205)
  text(145, tableY + 8, "RESIDUAL", 14, 190, 195, 205)
  text(245, tableY + 8, "Δ SAMPLES", 14, 190, 195, 205)
  text(385, tableY + 8, "CONF", 14, 190, 195, 205)
  text(455, tableY + 8, "CORR", 14, 190, 195, 205)
  text(545, tableY + 8, "COMMON START", 14, 190, 195, 205)
  text(700, tableY + 8, "STATE", 14, 190, 195, 205)

  local rowY = tableY + 32
  for idx, r in ipairs(results) do
    if rowY > gfx.h - 105 then break end
    local conf = r.confidence or 0
    local good = conf >= MIN_CONFIDENCE
    rect(18, rowY, gfx.w - 36, 34, (idx % 2 == 0) and 34 or 30, 34, 39)
    text(30, rowY + 8, "SOURCE " .. r.index, 14, 235, 235, 240)
    text(145, rowY + 8, string.format("%+.3f ms", r.delayMs), 14, 225, 230, 235)
    text(245, rowY + 8, string.format("%+.2f", r.deltaSamples), 14, 225, 230, 235)
    text(385, rowY + 8, string.format("%.3f", conf), 14, good and 120 or 235, good and 220 or 170, good and 150 or 130)
    text(455, rowY + 8, string.format("%.4f", r.correlation or 0), 14, 210, 215, 225)
    text(545, rowY + 8, string.format("%.3f s", r.commonStart or 0), 14, 210, 215, 225)
    text(700, rowY + 8, applied and "APPLIED" or (good and "READY" or "CHECK"), 14, applied and 120 or (good and 145 or 235), applied and 220 or (good and 205 or 170), applied and 155 or (good and 235 or 130))
    rowY = rowY + 34
  end

  local footerY = gfx.h - 72
  local kindR, kindG, kindB = 180, 190, 205
  if statusKind == "ok" then kindR, kindG, kindB = 120, 220, 150 end
  if statusKind == "warn" then kindR, kindG, kindB = 240, 200, 110 end
  if statusKind == "error" then kindR, kindG, kindB = 245, 120, 120 end

  text(24, footerY - 18, status, 13, kindR, kindG, kindB)
  draw_button(18, footerY + 8, 150, 38, "ANALYZE", n >= 2, true)
  draw_button(180, footerY + 8, 150, 38, "APPLY", analyzed and #results > 0, true)
  draw_button(gfx.w - 168, footerY + 8, 150, 38, "CLOSE", true, false)
end

local function handle_mouse()
  local down = gfx.mouse_cap & 1 == 1
  if down and not lastMouseDown then
    local footerY = gfx.h - 72
    if inside(18, footerY + 8, 150, 38, gfx.mouse_x, gfx.mouse_y) then
      analyze_selection()
    elseif inside(180, footerY + 8, 150, 38, gfx.mouse_x, gfx.mouse_y) then
      apply_results()
    elseif inside(gfx.w - 168, footerY + 8, 150, 38, gfx.mouse_x, gfx.mouse_y) then
      gfx.quit()
      return true
    end
  end
  lastMouseDown = down
  return false
end

local function loop()
  if gfx.getchar() < 0 then return end
  if handle_mouse() then return end
  gfx.update()
  reaper.defer(loop)
end

gfx.init("Smart Align Post — STATIC MULTI v4", WIN_W, WIN_H)
gfx.clear = 24 + 25 * 256 + 29 * 65536
gfx.setfont(1, "Arial", 16)
reaper.defer(loop)
