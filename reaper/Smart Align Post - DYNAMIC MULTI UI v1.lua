-- Smart Align Post - DYNAMIC MULTI UI v1
-- Primera selección = MASTER. Siguientes = SOURCES.
-- El MASTER es la única referencia y NUNCA se modifica.
-- El delay dinámico se estima MASTER -> cada SOURCE por ventanas GCC-PHAT.
-- APPLY mantiene D_POSITION y modifica D_STARTOFFS por segmentos, siguiendo la curva temporal.
-- Se evita alinear SOURCE contra SOURCE: cada cálculo se hace exclusivamente contra MASTER.

local MIN_CONFIDENCE = 0.80
local CHUNK_SEC = 180.0
local CHUNK_OVERLAP_SEC = 0.30
local CURVE_SKIP_START_SEC = 0.12
local CONSOLIDATE_MAX_SEC = 0.20
local CONSOLIDATE_MIN_DELTA_SAMPLES = 0.75
local WIN_W, WIN_H = 920, 620

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

local function run_dynamic_chunk(masterPath, sourcePath, masterStart, sourceStart, duration)
  local exe = script_dir() .. "\\SmartAlignPostPrototype.exe"
  local cmd = quote(exe) .. " " .. quote(masterPath) .. " " .. quote(sourcePath)
    .. " " .. quote(string.format("%.12f", masterStart))
    .. " " .. quote(string.format("%.12f", sourceStart))
    .. " " .. quote(string.format("%.6f", duration))

  local returnCode, output, normalized = parse_process_output(reaper.ExecProcess(cmd, 60000))
  if returnCode ~= 0 then return nil, "" .. tostring(normalized) end

  local staticDelay = tonumber(output:match("DELAY_SAMPLES=([%+%-]?[%d%.]+)")) or 0
  local staticDelayMs = tonumber(output:match("DELAY_MS=([%+%-]?[%d%.]+)")) or 0
  local staticConf = tonumber(output:match("CONFIDENCE=([%+%-]?[%d%.]+)")) or 0
  local curve = {}
  for t, ms, d, c in output:gmatch("POINT=([%+%-]?[%d%.]+),([%+%-]?[%d%.]+),([%+%-]?[%d%.]+),([%+%-]?[%d%.]+)") do
    curve[#curve + 1] = {time = tonumber(t), delayMs = tonumber(ms), delay = tonumber(d), confidence = tonumber(c)}
  end
  return {
    staticDelay = staticDelay,
    staticDelayMs = staticDelayMs,
    staticConfidence = staticConf,
    curve = curve,
  }, nil
end

local function consolidate_curve(points)
  if #points == 0 then return {} end
  table.sort(points, function(a,b) return a.time < b.time end)

  local out = {}
  local last = nil
  for _, p in ipairs(points) do
    if p.confidence >= MIN_CONFIDENCE then
      if not last then
        last = {time = p.time, delay = p.delay, delayMs = p.delayMs, confidence = p.confidence}
        out[#out + 1] = last
      else
        local dt = p.time - last.time
        local dd = math.abs(p.delay - last.delay)
        if dt >= CONSOLIDATE_MAX_SEC or dd >= CONSOLIDATE_MIN_DELTA_SAMPLES then
          last = {time = p.time, delay = p.delay, delayMs = p.delayMs, confidence = p.confidence}
          out[#out + 1] = last
        else
          if p.confidence > last.confidence then last.confidence = p.confidence end
          last.time = p.time
        end
      end
    end
  end

  return out
end

local function analyze_source(sourceItem, sourceIndex)
  local sourcePath, sourceTake, sourceErr = take_source_path(sourceItem)
  if not sourcePath then return nil, "SOURCE #" .. sourceIndex .. ": " .. tostring(sourceErr) end

  local sourcePos = reaper.GetMediaItemInfo_Value(sourceItem, "D_POSITION")
  local sourceLength = reaper.GetMediaItemInfo_Value(sourceItem, "D_LENGTH")
  local sourceOffs = reaper.GetMediaItemTakeInfo_Value(sourceTake, "D_STARTOFFS")
  local sourceRate = reaper.GetMediaItemTakeInfo_Value(sourceTake, "D_PLAYRATE")
  if sourceRate <= 0 then return nil, "PLAYRATE inválido en SOURCE #" .. sourceIndex end

  local commonStart = math.max(masterPos, sourcePos)
  local commonEnd = math.min(masterPos + masterLength, sourcePos + sourceLength)
  if commonEnd - commonStart < 0.5 then
    return nil, string.format("SOURCE #%d: tramo común demasiado corto.", sourceIndex)
  end

  local points = {}
  local chunkStart = commonStart
  local firstChunk = true
  local lastStaticDelay = 0
  local lastStaticDelayMs = 0
  local lastStaticConf = 0

  while chunkStart < commonEnd - 0.05 do
    local duration = math.min(CHUNK_SEC, commonEnd - chunkStart)
    if duration < 0.5 then break end

    local masterAnalysisStart = masterOffs + (chunkStart - masterPos) * masterRate
    local sourceAnalysisStart = sourceOffs + (chunkStart - sourcePos) * sourceRate

    local analysis, err = run_dynamic_chunk(masterPath, sourcePath, masterAnalysisStart, sourceAnalysisStart, duration)
    if not analysis then return nil, string.format("SOURCE #%d: %s", sourceIndex, err) end

    lastStaticDelay = analysis.staticDelay
    lastStaticDelayMs = analysis.staticDelayMs
    lastStaticConf = analysis.staticConfidence

    for _, p in ipairs(analysis.curve) do
      local absoluteTime = chunkStart + p.time
      if absoluteTime >= commonStart + (firstChunk and 0 or CURVE_SKIP_START_SEC)
         and absoluteTime < commonEnd - 0.05 then
        points[#points + 1] = {
          time = absoluteTime,
          delay = p.delay,
          delayMs = p.delayMs,
          confidence = p.confidence,
        }
      end
    end

    if commonEnd - chunkStart <= CHUNK_SEC + 0.001 then break end
    chunkStart = chunkStart + CHUNK_SEC - CHUNK_OVERLAP_SEC
    firstChunk = false
  end

  table.sort(points, function(a,b) return a.time < b.time end)
  local curve = consolidate_curve(points)
  if #curve == 0 then
    curve[1] = {time = commonStart, delay = lastStaticDelay, delayMs = lastStaticDelayMs, confidence = lastStaticConf}
  end

  -- Forzamos una referencia explícita MASTER -> SOURCE: el primer punto siempre
  -- parte de la medición MASTER/SOURCE y nunca de otra SOURCE.
  if curve[1].time > commonStart + 1e-6 then
    table.insert(curve, 1, {time = commonStart, delay = lastStaticDelay, delayMs = lastStaticDelayMs, confidence = lastStaticConf})
  end

  local maxAbs = 0
  local minConf = 1
  for _, p in ipairs(curve) do
    maxAbs = math.max(maxAbs, math.abs(p.delay))
    minConf = math.min(minConf, p.confidence or 0)
  end

  return {
    index = sourceIndex,
    item = sourceItem,
    take = sourceTake,
    sourcePos = sourcePos,
    sourceOffs = sourceOffs,
    sourceRate = sourceRate,
    commonStart = commonStart,
    commonEnd = commonEnd,
    curve = curve,
    pointCount = #curve,
    maxAbsDelay = maxAbs,
    minConfidence = minConf,
  }, nil
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

  local low = 0
  for i = 1, selectedCount - 1 do
    local r, err = analyze_source(reaper.GetSelectedMediaItem(0, i), i)
    if not r then fail(err); return end
    if r.minConfidence < MIN_CONFIDENCE then low = low + 1 end
    results[#results + 1] = r
  end

  analyzed = true
  set_status(string.format(
    "Analizados %d SOURCE(s) con MASTER como única referencia. %d con confidence < %.2f.",
    #results, low, MIN_CONFIDENCE), low > 0 and "warn" or "ok")
end

local function split_and_apply_source(r)
  local item = r.item
  local currentDelayMs = nil
  local current = item
  local splitCount = 0

  -- Primer punto: corregimos el OFFSET del segmento inicial respecto del MASTER.
  -- p.delayMs es un valor de TIEMPO (ms); /1000 da segundos y *sourceRate/masterRate
  -- ajusta por diferencias de PLAYRATE entre SOURCE y MASTER (mismo patrón que
  -- STATIC). Antes se dividía delay EN SAMPLES por D_PLAYRATE, que no es una
  -- conversión samples->segundos: con el playrate típico de 1.0 esto corría el
  -- STARTOFFS la cantidad de samples pero en SEGUNDOS (p.ej. 56 samples -> 56s
  -- en vez de ~1.2ms).
  local firstPoint = r.curve[1]
  local initialOffs = reaper.GetMediaItemTakeInfo_Value(reaper.GetActiveTake(current), "D_STARTOFFS")
  local firstDelaySeconds = (firstPoint.delayMs / 1000.0) * r.sourceRate / masterRate
  reaper.SetMediaItemTakeInfo_Value(reaper.GetActiveTake(current), "D_STARTOFFS", initialOffs + firstDelaySeconds)
  reaper.UpdateItemInProject(current)
  currentDelayMs = firstPoint.delayMs

  for idx = 2, #r.curve do
    local p = r.curve[idx]
    local splitPos = p.time
    local curPos = reaper.GetMediaItemInfo_Value(current, "D_POSITION")
    local curLen = reaper.GetMediaItemInfo_Value(current, "D_LENGTH")
    if splitPos <= curPos + 0.005 or splitPos >= curPos + curLen - 0.005 then
      goto continue
    end

    local right = reaper.SplitMediaItem(current, splitPos)
    if not right then goto continue end

    local rightTake = reaper.GetActiveTake(right)
    if not rightTake then goto continue end

    -- SplitMediaItem conserva el timeline y avanza el STARTOFFS automáticamente.
    -- Solo aplicamos el cambio de delay respecto del segmento anterior (en ms,
    -- ver nota de unidades más arriba).
    local rightOffs = reaper.GetMediaItemTakeInfo_Value(rightTake, "D_STARTOFFS")
    local deltaDelaySeconds = ((p.delayMs - currentDelayMs) / 1000.0) * r.sourceRate / masterRate
    reaper.SetMediaItemTakeInfo_Value(rightTake, "D_STARTOFFS", rightOffs + deltaDelaySeconds)
    reaper.UpdateItemInProject(right)

    current = right
    currentDelayMs = p.delayMs
    splitCount = splitCount + 1

    ::continue::
  end

  return splitCount
end

local function apply_results()
  if not analyzed or #results == 0 then
    set_status("Primero ejecutá ANALYZE.", "warn")
    return
  end

  for _, r in ipairs(results) do
    if r.minConfidence < MIN_CONFIDENCE then
      local answer = reaper.ShowMessageBox(
        string.format("SOURCE %d tiene confidence mínima %.2f.\n\n¿Aplicar igualmente?", r.index, r.minConfidence),
        "Smart Align Post — DYNAMIC CONFIDENCE", 4)
      if answer ~= 6 then
        set_status("APPLY cancelado.", "info")
        return
      end
      break
    end
  end

  reaper.Undo_BeginBlock()
  local totalSplits = 0
  local failures = 0
  local masterBefore = reaper.GetMediaItemInfo_Value(masterItem, "D_POSITION")

  for _, r in ipairs(results) do
    local beforePos = reaper.GetMediaItemInfo_Value(r.item, "D_POSITION")
    totalSplits = totalSplits + split_and_apply_source(r)
    local afterPos = reaper.GetMediaItemInfo_Value(r.item, "D_POSITION")
    if math.abs(afterPos - beforePos) > 1e-8 then failures = failures + 1 end
  end

  local masterAfter = reaper.GetMediaItemInfo_Value(masterItem, "D_POSITION")
  reaper.UpdateArrange()
  reaper.Undo_EndBlock("Smart Align Post - DYNAMIC MULTI APPLY", -1)

  if math.abs(masterAfter - masterBefore) > 1e-8 then
    failures = failures + 1
  end

  if failures > 0 then
    set_status(string.format("APPLY terminó con %d error(es) de verificación.", failures), "error")
    return
  end

  applied = true
  set_status(string.format(
    "Aplicado contra MASTER: %d SOURCE(s), %d segmentos dinámicos. D_POSITION intacto. Undo disponible.",
    #results, totalSplits, #results), "ok")
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
  text(24, 49, "DYNAMIC · MULTI SOURCE v1", 15, 160, 170, 185)
  text(24, 73, "MASTER = primer item seleccionado · referencia única", 14, 195, 200, 210)
  text(24, 94, "Corrección temporal variable por SOURCE mediante D_STARTOFFS + segmentos", 13, 145, 155, 170)

  local n = reaper.CountSelectedMediaItems(0)
  text(720, 24, "Seleccionados: " .. n, 15, 205, 210, 220)
  text(720, 48, "MASTER: " .. (masterItem and "OK" or "—"), 14, 160, 175, 185)

  local tableY = 125
  rect(18, tableY, gfx.w - 36, 32, 45, 47, 53)
  text(28, tableY + 8, "SOURCE", 14, 190, 195, 205)
  text(145, tableY + 8, "CURVA", 14, 190, 195, 205)
  text(245, tableY + 8, "MAX |DELAY|", 14, 190, 195, 205)
  text(380, tableY + 8, "MIN CONF", 14, 190, 195, 205)
  text(515, tableY + 8, "TRAMO", 14, 190, 195, 205)
  text(705, tableY + 8, "STATE", 14, 190, 195, 205)

  local rowY = tableY + 32
  for idx, r in ipairs(results) do
    if rowY > gfx.h - 105 then break end
    local good = r.minConfidence >= MIN_CONFIDENCE
    rect(18, rowY, gfx.w - 36, 38, (idx % 2 == 0) and 34 or 30, 34, 39)
    text(28, rowY + 10, "SOURCE " .. r.index, 14, 235, 235, 240)
    text(145, rowY + 10, tostring(r.pointCount) .. " puntos", 14, 225, 230, 235)
    text(245, rowY + 10, string.format("%.2f smp", r.maxAbsDelay), 14, 225, 230, 235)
    text(380, rowY + 10, string.format("%.3f", r.minConfidence), 14, good and 120 or 235, good and 220 or 170, good and 150 or 130)
    text(515, rowY + 10, string.format("%.1f s", r.commonEnd - r.commonStart), 14, 210, 215, 225)
    text(705, rowY + 10, applied and "APPLIED" or (good and "READY" or "CHECK"), 14, applied and 120 or (good and 145 or 235), applied and 220 or (good and 205 or 170), applied and 155 or (good and 235 or 130))
    rowY = rowY + 38
  end

  local footerY = gfx.h - 72
  local kindR, kindG, kindB = 180, 190, 205
  if statusKind == "ok" then kindR, kindG, kindB = 120, 220, 150 end
  if statusKind == "warn" then kindR, kindG, kindB = 240, 200, 110 end
  if statusKind == "error" then kindR, kindG, kindB = 245, 120, 120 end

  text(24, footerY - 18, status, 13, kindR, kindG, kindB)
  draw_button(18, footerY + 8, 165, 38, "ANALYZE DYNAMIC", n >= 2, true)
  draw_button(198, footerY + 8, 165, 38, "APPLY", analyzed and #results > 0, true)
  draw_button(gfx.w - 165, footerY + 8, 147, 38, "CLOSE", true, false)
end

local function handle_mouse()
  local down = gfx.mouse_cap & 1 == 1
  if down and not lastMouseDown then
    local footerY = gfx.h - 72
    if inside(18, footerY + 8, 165, 38, gfx.mouse_x, gfx.mouse_y) then
      analyze_selection()
    elseif inside(198, footerY + 8, 165, 38, gfx.mouse_x, gfx.mouse_y) then
      apply_results()
    elseif inside(gfx.w - 165, footerY + 8, 147, 38, gfx.mouse_x, gfx.mouse_y) then
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
  draw_ui()
  gfx.update()
  reaper.defer(loop)
end

gfx.init("Smart Align Post — DYNAMIC MULTI v1", WIN_W, WIN_H)
gfx.clear = 24 + 25 * 256 + 29 * 65536
gfx.setfont(1, "Arial", 16)
draw_ui()
gfx.update()
reaper.defer(loop)
