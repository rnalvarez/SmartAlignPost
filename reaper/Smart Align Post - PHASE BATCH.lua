-- Smart Align Post — PHASE BATCH
-- Phase-first production workflow for REAPER.
--
-- MASTER = track of the first selected item.
-- SOURCE TRACKS = tracks represented by the other selected items.
--
-- ANALYZE PROJECT scans every item on the declared SOURCE TRACKS and pairs
-- each one with the MASTER item with the greatest temporal overlap.
--
-- ANALYZE SELECTION analyzes only currently selected SOURCE items.
--
-- The C++ core performs:
--   1) energy-guided anchor selection;
--   2) GCC-PHAT phase-delay measurement;
--   3) sub-sample waveform refinement;
--   4) independent phase/waveform agreement check;
--   5) STATIC / DYNAMIC / AUTO decision.
--
-- APPLY never moves D_POSITION. STATIC changes only D_STARTOFFS.
-- DYNAMIC converts the measured project-time delay curve to REAPER stretch
-- markers while preserving the SOURCE item's position.

local WIN_W, WIN_H = 1080, 650
local MIN_CONFIDENCE = 0.72
local EXE_NAME = "SmartAlignPostPrototype.exe"

local status = "Seleccioná primero un item del MASTER y luego al menos un item de cada SOURCE."
local statusKind = "info"
local masterTrack = nil
local masterItem = nil
local jobs = {}
local analyzing = false
local analyzingIndex = 0
local applied = false
local mouseDown = false
local draw_ui

local function script_dir()
  local src = debug.getinfo(1, "S").source
  if src:sub(1, 1) == "@" then src = src:sub(2) end
  return src:match("^(.*)[/\\][^/\\]+$") or "."
end

local function quote(s)
  return '"' .. tostring(s):gsub('"', '\\"') .. '"'
end

local function set_status(msg, kind)
  status = msg or ""
  statusKind = kind or "info"
end

local function take_source_path(item)
  local take = reaper.GetActiveTake(item)
  if not take then
    return nil, nil, "sin take"
  end

  local source = reaper.GetMediaItemTake_Source(take)
  if not source then
    return nil, take, "sin source"
  end

  local path = reaper.GetMediaSourceFileName(source, "")
  if not path or path == "" then
    return nil, take, "source sin archivo"
  end

  return path, take, nil
end

local function track_label(track)
  if not track then return "—" end
  local _, name = reaper.GetSetMediaTrackInfo_String(track, "P_NAME", "", false)
  if not name or name == "" then
    local number = reaper.GetMediaTrackInfo_Value(track, "IP_TRACKNUMBER")
    return string.format("Track %.0f", number or 0)
  end
  return name
end

local function item_label(item)
  local pos = reaper.GetMediaItemInfo_Value(item, "D_POSITION")
  local len = reaper.GetMediaItemInfo_Value(item, "D_LENGTH")
  return string.format("%.2f–%.2f s", pos, pos + len)
end

local function collect_source_tracks()
  local tracks = {}
  local seen = {}

  local count = reaper.CountSelectedMediaItems(0)
  for i = 0, count - 1 do
    local item = reaper.GetSelectedMediaItem(0, i)
    local track = reaper.GetMediaItem_Track(item)
    if track and track ~= masterTrack and not seen[track] then
      seen[track] = true
      tracks[#tracks + 1] = track
    end
  end

  return tracks
end

local function find_best_master_item(sourceItem)
  if not masterTrack then return nil end

  local sPos = reaper.GetMediaItemInfo_Value(sourceItem, "D_POSITION")
  local sEnd = sPos + reaper.GetMediaItemInfo_Value(sourceItem, "D_LENGTH")

  local best = nil
  local bestOverlap = 0.0

  local count = reaper.CountTrackMediaItems(masterTrack)
  for i = 0, count - 1 do
    local masterItem = reaper.GetTrackMediaItem(masterTrack, i)
    local mPos = reaper.GetMediaItemInfo_Value(masterItem, "D_POSITION")
    local mEnd = mPos + reaper.GetMediaItemInfo_Value(masterItem, "D_LENGTH")

    local overlap = math.min(sEnd, mEnd) - math.max(sPos, mPos)
    if overlap > bestOverlap then
      bestOverlap = overlap
      best = masterItem
    end
  end

  return best
end

local function selected_source_items()
  local out = {}
  local seen = {}

  local count = reaper.CountSelectedMediaItems(0)
  for i = 0, count - 1 do
    local item = reaper.GetSelectedMediaItem(0, i)
    if item and item ~= reaper.GetSelectedMediaItem(0, 0) and not seen[item] then
      local track = reaper.GetMediaItem_Track(item)
      if track and track ~= masterTrack then
        out[#out + 1] = item
        seen[item] = true
      end
    end
  end

  return out
end

local function project_source_items()
  local tracks = collect_source_tracks()
  local out = {}
  local seen = {}

  for _, track in ipairs(tracks) do
    local count = reaper.CountTrackMediaItems(track)
    for i = 0, count - 1 do
      local item = reaper.GetTrackMediaItem(track, i)
      if item and not seen[item] then
        seen[item] = true
        out[#out + 1] = item
      end
    end
  end

  table.sort(out, function(a, b)
    return reaper.GetMediaItemInfo_Value(a, "D_POSITION") <
           reaper.GetMediaItemInfo_Value(b, "D_POSITION")
  end)

  return out
end

local function build_jobs(items)
  jobs = {}

  for _, sourceItem in ipairs(items) do
    local masterItem = find_best_master_item(sourceItem)
    if masterItem then
      local sPos = reaper.GetMediaItemInfo_Value(sourceItem, "D_POSITION")
      local sLen = reaper.GetMediaItemInfo_Value(sourceItem, "D_LENGTH")
      local mPos = reaper.GetMediaItemInfo_Value(masterItem, "D_POSITION")
      local mLen = reaper.GetMediaItemInfo_Value(masterItem, "D_LENGTH")

      local commonStart = math.max(sPos, mPos)
      local commonEnd = math.min(sPos + sLen, mPos + mLen)

      if commonEnd - commonStart >= 0.5 then
        local masterPath, masterTake, masterErr = take_source_path(masterItem)
        local sourcePath, sourceTake, sourceErr = take_source_path(sourceItem)

        if masterPath and sourcePath then
          jobs[#jobs + 1] = {
            sourceItem = sourceItem,
            sourceTake = sourceTake,
            sourcePath = sourcePath,
            masterItem = masterItem,
            masterTake = masterTake,
            masterPath = masterPath,
            commonStart = commonStart,
            commonEnd = commonEnd,
            status = "PENDING",
            modeUsed = nil,
            confidence = 0.0,
            delayMs = 0.0,
            curve = {},
            error = nil,
            sourceTrack = reaper.GetMediaItem_Track(sourceItem)
          }
        else
          jobs[#jobs + 1] = {
            sourceItem = sourceItem,
            masterItem = masterItem,
            commonStart = commonStart,
            commonEnd = commonEnd,
            status = "ERROR",
            error = sourceErr or masterErr or "source inválido",
            curve = {},
            confidence = 0.0,
            sourceTrack = reaper.GetMediaItem_Track(sourceItem)
          }
        end
      end
    end
  end
end

local function parse_output(output)
  local result = {
    engineVersion = output:match("ENGINE_VERSION=([^%s]+)"),
    modeRequested = output:match("MODE_REQUESTED=([A-Z]+)"),
    modeEffective = output:match("MODE_EFFECTIVE=([A-Z]+)"),
    modeUsed = output:match("MODE_USED=([A-Z]+)"),
    rateRatio = tonumber(output:match("RATE_RATIO=([%+%-]?[%d%.eE]+)")),
    scoutPoints = tonumber(output:match("SCOUT_POINTS=([%d]+)")) or 0,
    scoutFirstMs = tonumber(output:match("SCOUT_FIRST_MS=([%+%-]?[%d%.eE]+)")) or 0.0,
    scoutLastMs = tonumber(output:match("SCOUT_LAST_MS=([%+%-]?[%d%.eE]+)")) or 0.0,
    scoutR2 = tonumber(output:match("SCOUT_R2=([%+%-]?[%d%.eE]+)")) or 0.0,
    scoutDirection = tonumber(output:match("SCOUT_DIRECTION=([%+%-]?[%d%.eE]+)")) or 0.0,
    scoutCoherent = output:match("SCOUT_COHERENT=([01])") == "1",
    delaySamples = tonumber(output:match("DELAY_SAMPLES=([%+%-]?[%d%.]+)")) or 0.0,
    delayMs = tonumber(output:match("DELAY_MS=([%+%-]?[%d%.]+)")) or 0.0,
    confidence = tonumber(output:match("CONFIDENCE=([%+%-]?[%d%.]+)")) or 0.0,
    analyzeMs = tonumber(output:match("ANALYZE_MS=([%+%-]?[%d%.]+)")) or 0.0,
    curve = {}
  }

  for t, ms, samples, conf, key, phat, wave, agreement in
      output:gmatch(
        "POINT=([%+%-]?[%d%.]+),([%+%-]?[%d%.]+),([%+%-]?[%d%.]+),([%+%-]?[%d%.]+),([01]),([%+%-]?[%d%.]+),([%+%-]?[%d%.]+),([%+%-]?[%d%.]+)"
      ) do
    result.curve[#result.curve + 1] = {
      time = tonumber(t),
      delayMs = tonumber(ms),
      delay = tonumber(samples),
      confidence = tonumber(conf),
      keyPoint = key == "1",
      phatMs = tonumber(phat),
      waveformMs = tonumber(wave),
      phaseAgreement = tonumber(agreement)
    }
  end

  return result
end

local function analyze_job(job)
  if not job.masterPath or not job.sourcePath then
    job.status = "ERROR"
    job.error = job.error or "archivo no válido"
    return
  end

  local masterPos =
    reaper.GetMediaItemInfo_Value(job.masterItem, "D_POSITION")
  local sourcePos =
    reaper.GetMediaItemInfo_Value(job.sourceItem, "D_POSITION")

  local masterTake = job.masterTake
  local sourceTake = job.sourceTake

  local masterOffs =
    reaper.GetMediaItemTakeInfo_Value(masterTake, "D_STARTOFFS")
  local sourceOffs =
    reaper.GetMediaItemTakeInfo_Value(sourceTake, "D_STARTOFFS")

  local masterRate =
    reaper.GetMediaItemTakeInfo_Value(masterTake, "D_PLAYRATE")
  local sourceRate =
    reaper.GetMediaItemTakeInfo_Value(sourceTake, "D_PLAYRATE")

  if masterRate <= 0 or sourceRate <= 0 then
    job.status = "ERROR"
    job.error = "PLAYRATE inválido"
    return
  end

  local duration =
    math.min(600.0, job.commonEnd - job.commonStart)

  local masterStart =
    masterOffs + (job.commonStart - masterPos) * masterRate
  local sourceStart =
    sourceOffs + (job.commonStart - sourcePos) * sourceRate

  local exe = script_dir() .. "\\" .. EXE_NAME
  if reaper.file_exists and not reaper.file_exists(exe) then
    job.status = "ERROR"
    job.error = "No se encontró " .. EXE_NAME
    return
  end

  job.masterRate = masterRate
  job.sourceRate = sourceRate

  local cmd =
    quote(exe) .. " " ..
    quote(job.masterPath) .. " " ..
    quote(job.sourcePath) .. " " ..
    string.format(
      "\"%.9f\" \"%.9f\" \"%.6f\" \"%.9f\" \"%.9f\" AUTO",
      masterStart,
      sourceStart,
      duration,
      masterRate,
      sourceRate)

  set_status(
    string.format(
      "Analizando %d/%d · %s · %.1f s",
      analyzingIndex,
      #jobs,
      track_label(job.sourceTrack),
      duration),
    "info")

  draw_ui()
  gfx.update()

  local processResult = reaper.ExecProcess(cmd, 120000)
  if not processResult then
    job.status = "ERROR"
    job.error = "ExecProcess falló."
    return
  end

  processResult = processResult:gsub("\r\n", "\n"):gsub("\r", "\n")
  local firstNl = processResult:find("\n", 1, true)
  local code = nil
  local output = processResult

  if firstNl then
    code = tonumber(processResult:sub(1, firstNl - 1))
    if code ~= nil then
      output = processResult:sub(firstNl + 1)
    end
  end

  if output:sub(1, 6) == "ERROR=" then
    job.status = "ERROR"
    job.error = output:match("^ERROR=(.*)") or output
    return
  end

  if code ~= nil and code ~= 0 then
    job.status = "ERROR"
    job.error = output ~= "" and output or ("exit code " .. tostring(code))
    return
  end

  local result = parse_output(output)

  if not result.engineVersion then
    job.status = "ERROR"
    job.error = "El ejecutable no reportó ENGINE_VERSION. Reemplazá SmartAlignPostPrototype.exe por el del último artefacto."
    return
  end

  job.engineVersion = result.engineVersion
  job.modeRequested = result.modeRequested
  job.modeEffective = result.modeEffective
  job.modeUsed = result.modeUsed or "UNKNOWN"
  job.rateRatio = result.rateRatio
  job.scoutPoints = result.scoutPoints
  job.scoutFirstMs = result.scoutFirstMs
  job.scoutLastMs = result.scoutLastMs
  job.scoutR2 = result.scoutR2
  job.scoutDirection = result.scoutDirection
  job.scoutCoherent = result.scoutCoherent
  job.confidence = result.confidence
  job.delayMs = result.delayMs
  job.curve = result.curve
  job.analyzeMs = result.analyzeMs

  if job.rateRatio and math.abs(job.rateRatio - 1.0) > 1e-6 and job.modeUsed ~= "DYNAMIC" then
    job.status = "ERROR"
    job.error = string.format(
      "Inconsistencia del motor: RATE_RATIO=%.9f pero MODE_USED=%s.",
      job.rateRatio,
      job.modeUsed)
    return
  end

  job.status = "READY"

  if job.modeUsed == "DYNAMIC" and #job.curve >= 2 then
    local first = job.curve[1].delayMs or job.delayMs
    local middle =
      job.curve[math.max(1, math.floor((#job.curve + 1) / 2))].delayMs or first
    local last = job.curve[#job.curve].delayMs or middle
    job.firstMs = first
    job.middleMs = middle
    job.lastMs = last
  else
    job.firstMs = job.delayMs
    job.middleMs = job.delayMs
    job.lastMs = job.delayMs
  end
end

local function run_analysis(items)
  if analyzing then return end
  if not masterTrack then
    set_status("No hay MASTER definido. El primer item seleccionado debe pertenecer al MASTER.", "error")
    return
  end

  build_jobs(items)

  if #jobs == 0 then
    set_status("No encontré pares MASTER → SOURCE con al menos 0.5 s de solapamiento.", "error")
    return
  end

  analyzing = true
  applied = false
  analyzingIndex = 0

  local function step()
    if not analyzing then return end

    analyzingIndex = analyzingIndex + 1
    if analyzingIndex > #jobs then
      analyzing = false

      local ready = 0
      local low = 0
      local dynamic = 0

      for _, job in ipairs(jobs) do
        if job.status == "READY" then
          ready = ready + 1
          if job.confidence < MIN_CONFIDENCE then
            low = low + 1
          end
          if job.modeUsed == "DYNAMIC" then
            dynamic = dynamic + 1
          end
        end
      end

      local diagnostics = {}
      for _, job in ipairs(jobs) do
        diagnostics[#diagnostics + 1] = string.format(
          "%s: rate %.6f/%.6f ratio %.6f · scout %d %.2f→%.2f ms R² %.3f dir %.3f coh %s · %s→%s→%s",
          track_label(job.sourceTrack),
          job.masterRate or 0.0,
          job.sourceRate or 0.0,
          job.rateRatio or 1.0,
          job.scoutPoints or 0,
          job.scoutFirstMs or 0.0,
          job.scoutLastMs or 0.0,
          job.scoutR2 or 0.0,
          job.scoutDirection or 0.0,
          job.scoutCoherent and "YES" or "NO",
          job.modeRequested or "?",
          job.modeEffective or "?",
          job.modeUsed or "?")
      end

      set_status(
        string.format(
          "ANÁLISIS COMPLETO · %d/%d listos · %d DYNAMIC · %d con confidence < %.2f · %s",
          ready, #jobs, dynamic, low, MIN_CONFIDENCE,
          table.concat(diagnostics, " | ")),
        low > 0 and "warn" or "ok")

      return
    end

    analyze_job(jobs[analyzingIndex])

    draw_ui()
    gfx.update()

    reaper.defer(step)
  end

  reaper.defer(step)
end

local function correctionSeconds(job)
  return (job.delayMs or 0.0) / 1000.0
end

local function applyStatic(job)
  local take = job.sourceTake
  if not take then return false, "sin take" end

  local rate =
    reaper.GetMediaItemTakeInfo_Value(take, "D_PLAYRATE")

  if rate <= 0 then
    return false, "PLAYRATE inválido"
  end

  local offs =
    reaper.GetMediaItemTakeInfo_Value(take, "D_STARTOFFS")

  -- Positive delay means SOURCE arrives later. Moving D_STARTOFFS forward
  -- advances its media under the fixed timeline item without changing
  -- D_POSITION.
  local target =
    offs + correctionSeconds(job) * rate

  reaper.SetMediaItemTakeInfo_Value(
    take, "D_STARTOFFS", target)

  reaper.UpdateItemInProject(job.sourceItem)

  return true
end

local function applyDynamic(job)
  local take = job.sourceTake
  if not take or #job.curve < 2 then
    return false, "curva insuficiente"
  end

  local oldMarkers =
    reaper.GetTakeNumStretchMarkers(take)

  if oldMarkers and oldMarkers > 0 then
    reaper.DeleteTakeStretchMarkers(
      take, 0, oldMarkers)
  end

  reaper.SetMediaItemTakeInfo_Value(
    take, "B_PPITCH", 1)

  local itemPos =
    reaper.GetMediaItemInfo_Value(
      job.sourceItem, "D_POSITION")

  local itemLen =
    reaper.GetMediaItemInfo_Value(
      job.sourceItem, "D_LENGTH")

  local rate =
    reaper.GetMediaItemTakeInfo_Value(
      take, "D_PLAYRATE")

  local offs =
    reaper.GetMediaItemTakeInfo_Value(
      take, "D_STARTOFFS")

  if rate <= 0 or itemLen <= 0 then
    return false, "item inválido"
  end

  reaper.SetMediaItemTakeInfo_Value(
    take, "D_PLAYRATE", 1.0)

  local lastItemPos = -1e30
  local lastSourcePos = -1e30
  local inserted = 0

  local function sourcePosFor(itemTime, delayMs)
    return offs + rate * (
      itemTime + delayMs / 1000.0)
  end

  -- The first/last measured anchors normally sit inside the common overlap.
  -- Extrapolate only a short distance to the item boundaries; do not hold a
  -- potentially stale interior delay over a long unmeasured region.
  local function delayAtProjectTime(projectTime)
    local t = projectTime - job.commonStart
    local points = job.curve
    if #points == 0 then
      return job.delayMs or 0.0
    end
    if #points == 1 then
      return points[1].delayMs
    end

    local first = points[1]
    local second = points[2]
    local penult = points[#points - 1]
    local last = points[#points]

    local edgeHorizon = 0.75
    if t <= first.time then
      if first.time - t <= edgeHorizon and
         second.time > first.time + 1e-9 then
        local u = (t - first.time) /
                  (second.time - first.time)
        return first.delayMs +
               (second.delayMs - first.delayMs) * u
      end
      return first.delayMs
    end

    if t >= last.time then
      if t - last.time <= edgeHorizon and
         last.time > penult.time + 1e-9 then
        local u = (t - last.time) /
                  (last.time - penult.time)
        return last.delayMs +
               (last.delayMs - penult.delayMs) * u
      end
      return last.delayMs
    end

    for i = 1, #points - 1 do
      local a = points[i]
      local b = points[i + 1]
      if t >= a.time and t <= b.time then
        local u = (t - a.time) /
                  math.max(1e-9, b.time - a.time)
        return a.delayMs +
               (b.delayMs - a.delayMs) * u
      end
    end

    return last.delayMs
  end

  local function addMarker(itemTime, delayMs)
    itemTime =
      math.max(0.0,
        math.min(itemLen, itemTime))

    local src =
      sourcePosFor(itemTime, delayMs)

    if itemTime <= lastItemPos + 1e-7 then
      return true
    end

    if src <= lastSourcePos + 1e-7 then
      src = lastSourcePos + 1e-7
    end

    local idx =
      reaper.SetTakeStretchMarker(
        take, -1, itemTime, src)

    if idx < 0 then
      return false
    end

    lastItemPos = itemTime
    lastSourcePos = src
    inserted = inserted + 1

    return true
  end

  local firstDelay = delayAtProjectTime(itemPos)
  if not addMarker(
      0.0, firstDelay) then
    return false, "falló marker inicial"
  end

  for _, point in ipairs(job.curve) do
    local projectTime =
      job.commonStart + point.time
    local relative =
      projectTime - itemPos

    if relative > 0.001 and
       relative < itemLen - 0.001 then
      if not addMarker(
          relative, point.delayMs) then
        return false, "falló marker intermedio"
      end
    end
  end

  local lastDelay =
    delayAtProjectTime(itemPos + itemLen)
  if not addMarker(
      itemLen, lastDelay) then
    return false, "falló marker final"
  end

  reaper.UpdateItemInProject(job.sourceItem)

  return inserted >= 2
end

local function apply_all()
  if analyzing or #jobs == 0 then
    set_status("Primero ANALYZE.", "warn")
    return
  end

  local low = 0
  for _, job in ipairs(jobs) do
    if job.status == "READY" and
       job.confidence < MIN_CONFIDENCE then
      low = low + 1
    end
  end

  if low > 0 then
    local answer =
      reaper.ShowMessageBox(
        string.format(
          "%d SOURCE(s) tienen confidence inferior a %.2f.\n\nAPPLY omitirá esos casos y aplicará únicamente los READY confiables.",
          low,
          MIN_CONFIDENCE),
        "Smart Align Post — confidence",
        1)

    if answer ~= 1 then
      return
    end
  end

  reaper.Undo_BeginBlock()

  local appliedCount = 0
  local skipped = 0
  local failures = 0
  local dynamicCount = 0
  local staticCount = 0

  for _, job in ipairs(jobs) do
    if job.status ~= "READY" or
       job.confidence < MIN_CONFIDENCE then
      skipped = skipped + 1
    else
      local ok = false

      if job.modeUsed == "DYNAMIC" then
        ok = select(1, applyDynamic(job))
        if ok then dynamicCount = dynamicCount + 1 end
      else
        ok = select(1, applyStatic(job))
        if ok then staticCount = staticCount + 1 end
      end

      if ok then
        appliedCount = appliedCount + 1
        job.status = "APPLIED"
      else
        failures = failures + 1
      end
    end
  end

  reaper.UpdateArrange()
  reaper.Undo_EndBlock(
    "Smart Align Post — Phase Batch Apply",
    -1)

  applied = true

  if failures > 0 then
    set_status(
      string.format(
        "APPLY parcial · %d aplicados · %d omitidos · %d errores. Undo disponible.",
        appliedCount, skipped, failures),
      "error")
  else
    set_status(
      string.format(
        "APPLY COMPLETO · %d STATIC · %d DYNAMIC · %d omitidos. MASTER y D_POSITION intactos.",
        staticCount, dynamicCount, skipped),
      "ok")
  end
end

local function reset_results()
  jobs = {}
  analyzing = false
  analyzingIndex = 0
  applied = false
  set_status(
    "Resultados limpiados. MASTER = " ..
      track_label(masterTrack),
    "info")
end

local function rgb(r, g, b)
  gfx.set(r / 255, g / 255, b / 255, 1)
end

local function rect(x, y, w, h, r, g, b)
  rgb(r, g, b)
  gfx.rect(x, y, w, h, 1)
end

local function text(x, y, s, size, r, g, b)
  rgb(
    r or 230,
    g or 230,
    b or 230)
  gfx.setfont(1, "Arial", size or 16)
  gfx.x = x
  gfx.y = y
  gfx.drawstr(tostring(s))
end

local function button(
  x, y, w, h,
  label, enabled, primary)

  local hover =
    enabled and
    gfx.mouse_x >= x and
    gfx.mouse_x <= x + w and
    gfx.mouse_y >= y and
    gfx.mouse_y <= y + h

  if not enabled then
    rect(x, y, w, h, 55, 56, 62)
  elseif primary then
    rect(
      x, y, w, h,
      hover and 70 or 52,
      hover and 155 or 125,
      hover and 245 or 210)
  else
    rect(x, y, w, h,
      hover and 82 or 64,
      hover and 82 or 64,
      hover and 90 or 70)
  end

  local tw = gfx.measurestr(label)
  text(
    x + (w - tw) / 2,
    y + 10,
    label,
    16,
    enabled and 245 or 135,
    enabled and 245 or 135,
    enabled and 250 or 135)
end

draw_ui = function()
  rect(
    0, 0, gfx.w, gfx.h,
    24, 25, 29)

  text(
    24, 18,
    "SMART ALIGN POST",
    25, 245, 245, 250)

  text(
    24, 50,
    "PHASE BATCH · GCC-PHAT + waveform refinement",
    15, 160, 175, 190)

  text(
    24, 75,
    "MASTER = track del primer item seleccionado · SOURCE = otros tracks declarados por selección",
    13, 190, 195, 205)

  text(
    24, 98,
    "ANALYZE PROJECT recorre todos los items de esos SOURCE tracks. ANALYZE SELECTION usa sólo los seleccionados.",
    13, 160, 165, 178)

  text(
    760, 24,
    "MASTER",
    13, 150, 160, 175)

  text(
    760, 45,
    masterTrack and track_label(masterTrack) or "—",
    16, 235, 235, 240)

  text(
    760, 72,
    "JOBS " .. #jobs,
    14, 180, 185, 195)

  local y = 130

  rect(18, y, gfx.w - 36, 32,
    45, 47, 53)

  text(28, y + 8, "SOURCE", 13, 190, 195, 205)
  text(195, y + 8, "TRAMO", 13, 190, 195, 205)
  text(330, y + 8, "MODE", 13, 190, 195, 205)
  text(430, y + 8, "PUNTOS", 13, 190, 195, 205)
  text(520, y + 8, "CONF", 13, 190, 195, 205)
  text(610, y + 8, "INICIO", 13, 190, 195, 205)
  text(705, y + 8, "MEDIO", 13, 190, 195, 205)
  text(800, y + 8, "FINAL", 13, 190, 195, 205)
  text(895, y + 8, "STATE", 13, 190, 195, 205)

  local rowY = y + 32
  for i, job in ipairs(jobs) do
    if rowY > gfx.h - 120 then
      break
    end

    rect(
      18, rowY, gfx.w - 36, 34,
      (i % 2 == 0) and 34 or 30,
      34,
      39)

    local confGood =
      job.confidence >= MIN_CONFIDENCE

    text(
      28, rowY + 9,
      track_label(job.sourceTrack),
      13, 235, 235, 240)

    text(
      195, rowY + 9,
      item_label(job.sourceItem),
      12, 210, 215, 225)

    text(
      330, rowY + 9,
      job.modeUsed or "—",
      13,
      job.modeUsed == "DYNAMIC" and 115 or 215,
      job.modeUsed == "DYNAMIC" and 205 or 220,
      job.modeUsed == "DYNAMIC" and 240 or 225)

    text(
      430, rowY + 9,
      tostring(#job.curve),
      13, 220, 225, 235)

    text(
      520, rowY + 9,
      string.format("%.3f", job.confidence or 0),
      13,
      confGood and 125 or 240,
      confGood and 220 or 170,
      confGood and 155 or 130)

    text(
      610, rowY + 9,
      string.format("%.2f", job.firstMs or job.delayMs or 0),
      12, 220, 225, 235)

    text(
      705, rowY + 9,
      string.format("%.2f", job.middleMs or job.delayMs or 0),
      12, 220, 225, 235)

    text(
      800, rowY + 9,
      string.format("%.2f", job.lastMs or job.delayMs or 0),
      12, 220, 225, 235)

    local state = job.status or "—"
    if job.error then
      state = "ERROR"
    end

    text(
      895, rowY + 9,
      state,
      12,
      state == "ERROR" and 245 or
        state == "APPLIED" and 120 or
        confGood and 145 or 235,
      state == "ERROR" and 125 or
        state == "APPLIED" and 220 or
        confGood and 205 or 170,
      state == "ERROR" and 120 or
        state == "APPLIED" and 155 or
        confGood and 180 or 130)

    rowY = rowY + 34
  end

  local fy = gfx.h - 78

  local rr, gg, bb = 180, 190, 205
  if statusKind == "ok" then
    rr, gg, bb = 120, 220, 150
  elseif statusKind == "warn" then
    rr, gg, bb = 240, 200, 110
  elseif statusKind == "error" then
    rr, gg, bb = 245, 120, 120
  end

  text(
    24, fy - 18,
    status,
    13, rr, gg, bb)

  local hasMaster = masterTrack ~= nil
  local hasSelection = reaper.CountSelectedMediaItems(0) >= 2

  button(
    18, fy + 8, 190, 38,
    "ANALYZE PROJECT",
    hasMaster and not analyzing,
    true)

  button(
    220, fy + 8, 190, 38,
    "ANALYZE SELECTION",
    hasMaster and hasSelection and not analyzing,
    true)

  button(
    422, fy + 8, 155, 38,
    "APPLY ALL",
    #jobs > 0 and not analyzing,
    true)

  button(
    589, fy + 8, 120, 38,
    "CLEAR",
    not analyzing,
    false)

  button(
    gfx.w - 145, fy + 8, 127, 38,
    "CLOSE",
    true,
    false)
end

local function initializeMaster()
  local count =
    reaper.CountSelectedMediaItems(0)

  if count < 1 then
    if masterItem ~= nil then
      masterItem = nil
      masterTrack = nil
      jobs = {}
    end
    if not analyzing then
      set_status(
        "Seleccioná al menos un item. El primero seleccionado define el MASTER.",
        "warn")
    end
    return
  end

  local first =
    reaper.GetSelectedMediaItem(0, 0)

  if first ~= masterItem then
    masterItem = first
    masterTrack = reaper.GetMediaItem_Track(first)
    if not analyzing then
      set_status(
        "MASTER = " ..
          track_label(masterTrack) ..
          " · seleccioná items SOURCE en los demás tracks.",
        "info")
    end
  end
end

local function mouse_handler()
  local down =
    (gfx.mouse_cap & 1) == 1

  if down and not mouseDown then
    local fy = gfx.h - 78

    if gfx.mouse_x >= 18 and
       gfx.mouse_x <= 208 and
       gfx.mouse_y >= fy + 8 and
       gfx.mouse_y <= fy + 46 then

      run_analysis(
        project_source_items())

    elseif gfx.mouse_x >= 220 and
           gfx.mouse_x <= 410 and
           gfx.mouse_y >= fy + 8 and
           gfx.mouse_y <= fy + 46 then

      run_analysis(
        selected_source_items())

    elseif gfx.mouse_x >= 422 and
           gfx.mouse_x <= 577 and
           gfx.mouse_y >= fy + 8 and
           gfx.mouse_y <= fy + 46 then

      apply_all()

    elseif gfx.mouse_x >= 589 and
           gfx.mouse_x <= 709 and
           gfx.mouse_y >= fy + 8 and
           gfx.mouse_y <= fy + 46 then

      reset_results()

    elseif gfx.mouse_x >= gfx.w - 145 and
           gfx.mouse_y >= fy + 8 and
           gfx.mouse_y <= fy + 46 then

      gfx.quit()
      return true
    end
  end

  mouseDown = down
  return false
end

local function loop()
  if gfx.getchar() < 0 then
    return
  end

  if mouse_handler() then
    return
  end

  if not analyzing then
    initializeMaster()
  end

  draw_ui()
  gfx.update()
  reaper.defer(loop)
end

gfx.init(
  "Smart Align Post — PHASE BATCH",
  WIN_W, WIN_H)

gfx.clear =
  24 + 25 * 256 + 29 * 65536

initializeMaster()
draw_ui()
gfx.update()
reaper.defer(loop)
