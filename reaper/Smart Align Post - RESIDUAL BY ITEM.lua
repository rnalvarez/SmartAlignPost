-- Smart Align Post — RESIDUAL BY ITEM
-- Post-batch residual pass.
--
-- MASTER = track of the first selected item.
-- SOURCE TRACKS = tracks represented by the other selected items.
--
-- Each SOURCE item is paired with exactly one MASTER scene item:
-- the MASTER item with the greatest temporal overlap.
--
-- This pass does NOT replace PHASE BATCH. It measures the residual left
-- after the main alignment and, only when the measurement is reliable,
-- automatically inserts the mono Smart Align Post Take FX in that item.
--
-- The VST receives the measured residual in samples. Positive residual means
-- SOURCE is late and the processor advances the SOURCE by that amount.
--
-- Ambiguous/cross-scene items are never auto-corrected.

local WIN_W, WIN_H = 1120, 680
local MIN_CONFIDENCE = 0.72
local MIN_RESIDUAL_SAMPLES = 2.0
local MIN_OVERLAP_SECONDS = 0.5
local MIN_SCENE_COVERAGE = 0.95
local MAX_RESIDUAL_SAMPLES = 256.0

local EXE_NAME = "SmartAlignPostPrototype.exe"
local FX_NAME = "Smart Align Post"

local status = "Seleccioná primero un item del MASTER y luego items de los SOURCE tracks."
local statusKind = "info"

local masterTrack = nil
local masterItem = nil
local jobs = {}
local analyzing = false
local analyzingIndex = 0
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

local function track_label(track)
  if not track then return "—" end

  local _, name =
    reaper.GetSetMediaTrackInfo_String(
      track, "P_NAME", "", false)

  if name and name ~= "" then
    return name
  end

  local number =
    reaper.GetMediaTrackInfo_Value(
      track, "IP_TRACKNUMBER")

  return string.format(
    "Track %.0f",
    number or 0)
end

local function item_guid(item)
  local _, guid =
    reaper.GetSetMediaItemInfo_String(
      item, "GUID", "", false)

  return guid or ""
end

local function item_label(item)
  local pos =
    reaper.GetMediaItemInfo_Value(
      item, "D_POSITION")

  local len =
    reaper.GetMediaItemInfo_Value(
      item, "D_LENGTH")

  return string.format(
    "%.2f–%.2f s",
    pos,
    pos + len)
end

local function scene_label(track, item)
  if not track or not item then
    return "—"
  end

  local count =
    reaper.CountTrackMediaItems(track)

  for i = 0, count - 1 do
    if reaper.GetTrackMediaItem(track, i) == item then
      return string.format(
        "MASTER %02d · %s",
        i + 1,
        item_label(item))
    end
  end

  return item_label(item)
end

local function get_item_ext(item, key)
  if not item then return "" end

  local ok, value =
    reaper.GetSetMediaItemInfo_String(
      item, key, "", false)

  if ok and value then
    return value
  end

  return ""
end

local function find_master_item_by_guid(guid)
  if not masterTrack or not guid or guid == "" then
    return nil
  end

  local count =
    reaper.CountTrackMediaItems(masterTrack)

  for i = 0, count - 1 do
    local item =
      reaper.GetTrackMediaItem(
        masterTrack, i)

    if item_guid(item) == guid then
      return item
    end
  end

  return nil
end

local function take_source_path(item)
  local take =
    reaper.GetActiveTake(item)

  if not take then
    return nil, nil, "sin take"
  end

  if reaper.TakeIsMIDI and
     reaper.TakeIsMIDI(take) then
    return nil, take, "take MIDI"
  end

  local source =
    reaper.GetMediaItemTake_Source(take)

  if not source then
    return nil, take, "sin source"
  end

  local path =
    reaper.GetMediaSourceFileName(
      source, "")

  if not path or path == "" then
    return nil, take, "source sin archivo"
  end

  return path, take, nil
end

local function collect_source_tracks()
  local tracks = {}
  local seen = {}

  local count =
    reaper.CountSelectedMediaItems(0)

  for i = 0, count - 1 do
    local item =
      reaper.GetSelectedMediaItem(0, i)

    local track =
      reaper.GetMediaItem_Track(item)

    if track and
       track ~= masterTrack and
       not seen[track] then

      seen[track] = true
      tracks[#tracks + 1] = track
    end
  end

  return tracks
end

local function selected_source_items()
  local out = {}
  local seen = {}

  local count =
    reaper.CountSelectedMediaItems(0)

  for i = 0, count - 1 do
    local item =
      reaper.GetSelectedMediaItem(0, i)

    local track =
      reaper.GetMediaItem_Track(item)

    if item and
       item ~= masterItem and
       track and
       track ~= masterTrack and
       not seen[item] then

      out[#out + 1] = item
      seen[item] = true
    end
  end

  return out
end

local function project_source_items()
  local tracks = collect_source_tracks()
  local out = {}
  local seen = {}

  for _, track in ipairs(tracks) do
    local count =
      reaper.CountTrackMediaItems(track)

    for i = 0, count - 1 do
      local item =
        reaper.GetTrackMediaItem(track, i)

      if item and not seen[item] then
        seen[item] = true
        out[#out + 1] = item
      end
    end
  end

  table.sort(
    out,
    function(a, b)
      return reaper.GetMediaItemInfo_Value(
          a, "D_POSITION") <
        reaper.GetMediaItemInfo_Value(
          b, "D_POSITION")
    end)

  return out
end

local function find_scene_match(sourceItem)
  if not masterTrack then
    return nil
  end

  -- Prefer the exact MASTER scene mapping written by PHASE BATCH.
  -- This prevents the residual stage from re-guessing the reference.
  local persistedGuid =
    get_item_ext(
      sourceItem,
      "P_EXT:SmartAlignPost.Scene.MasterGUID")

  local persistedMaster =
    find_master_item_by_guid(
      persistedGuid)

  if persistedMaster then
    local sPos =
      reaper.GetMediaItemInfo_Value(
        sourceItem, "D_POSITION")
    local sLen =
      reaper.GetMediaItemInfo_Value(
        sourceItem, "D_LENGTH")
    local mPos =
      reaper.GetMediaItemInfo_Value(
        persistedMaster, "D_POSITION")
    local mLen =
      reaper.GetMediaItemInfo_Value(
        persistedMaster, "D_LENGTH")

    local overlap =
      math.min(
        sPos + sLen,
        mPos + mLen) -
      math.max(
        sPos,
        mPos)

    if overlap >= MIN_OVERLAP_SECONDS then
      local coverage =
        sLen > 0 and
        overlap / sLen or
        0.0

      return {
        item = persistedMaster,
        overlap = overlap,
        secondOverlap = 0.0,
        coverage = coverage,
        ambiguous = false,
        source = "PERSISTED"
      }
    end
  end

  -- Fallback for projects/items that were not processed by PHASE BATCH.
  local sPos =
    reaper.GetMediaItemInfo_Value(
      sourceItem, "D_POSITION")

  local sLen =
    reaper.GetMediaItemInfo_Value(
      sourceItem, "D_LENGTH")

  local sEnd =
    sPos + sLen

  local candidates = {}

  local count =
    reaper.CountTrackMediaItems(masterTrack)

  for i = 0, count - 1 do
    local candidate =
      reaper.GetTrackMediaItem(
        masterTrack, i)

    local mPos =
      reaper.GetMediaItemInfo_Value(
        candidate, "D_POSITION")

    local mLen =
      reaper.GetMediaItemInfo_Value(
        candidate, "D_LENGTH")

    local mEnd =
      mPos + mLen

    local overlap =
      math.min(sEnd, mEnd) -
      math.max(sPos, mPos)

    if overlap > 0 then
      candidates[#candidates + 1] = {
        item = candidate,
        overlap = overlap
      }
    end
  end

  table.sort(
    candidates,
    function(a, b)
      return a.overlap > b.overlap
    end)

  local best = candidates[1]

  if not best or
     best.overlap < MIN_OVERLAP_SECONDS then
    return nil
  end

  local secondOverlap =
    candidates[2] and
    candidates[2].overlap or 0.0

  local coverage =
    sLen > 0 and
    best.overlap / sLen or
    0.0

  local ambiguous =
    secondOverlap >= MIN_OVERLAP_SECONDS and
    coverage < MIN_SCENE_COVERAGE

  return {
    item = best.item,
    overlap = best.overlap,
    secondOverlap = secondOverlap,
    coverage = coverage,
    ambiguous = ambiguous,
    source = "OVERLAP_FALLBACK"
  }
end
local function build_jobs(items)
  jobs = {}

  for _, sourceItem in ipairs(items) do
    local match =
      find_scene_match(sourceItem)

    if match then
      local masterItem =
        match.item

      local masterPath,
            masterTake,
            masterErr =
        take_source_path(masterItem)

      local sourcePath,
            sourceTake,
            sourceErr =
        take_source_path(sourceItem)

      local sourceTrack =
        reaper.GetMediaItem_Track(sourceItem)

      local job = {
        sourceItem = sourceItem,
        sourceTake = sourceTake,
        sourcePath = sourcePath,
        sourceTrack = sourceTrack,

        masterItem = masterItem,
        masterTake = masterTake,
        masterPath = masterPath,

        overlap = match.overlap,
        secondOverlap = match.secondOverlap,
        coverage = match.coverage,
        ambiguous = match.ambiguous,
        mappingSource = match.source or "OVERLAP_FALLBACK",

        status = "PENDING",
        residualSamples = 0.0,
        residualMs = 0.0,
        confidence = 0.0,
        modeUsed = nil,
        error = nil
      }

      if not masterPath or
         not sourcePath then

        job.status = "ERROR"
        job.error =
          sourceErr or
          masterErr or
          "source inválido"
      end

      jobs[#jobs + 1] = job
    end
  end
end

local function parse_output(output)
  return {
    engineVersion =
      output:match(
        "ENGINE_VERSION=([^%s]+)"),

    modeUsed =
      output:match(
        "MODE_USED=([A-Z]+)"),

    delaySamples =
      tonumber(
        output:match(
          "DELAY_SAMPLES=([%+%-]?[%d%.eE]+)")) or
      0.0,

    delayMs =
      tonumber(
        output:match(
          "DELAY_MS=([%+%-]?[%d%.eE]+)")) or
      0.0,

    confidence =
      tonumber(
        output:match(
          "CONFIDENCE=([%+%-]?[%d%.eE]+)")) or
      0.0
  }
end

local function analyze_job(job)
  if not job.masterPath or
     not job.sourcePath then

    job.status = "ERROR"
    job.error =
      job.error or
      "archivo no válido"
    return
  end

  if job.ambiguous then
    job.status = "SCENE REVIEW"
    job.error =
      string.format(
        "ITEM cruza escenas · cobertura %.1f%% · segundo solapamiento %.2f s",
        job.coverage * 100.0,
        job.secondOverlap)
    return
  end

  local masterPos =
    reaper.GetMediaItemInfo_Value(
      job.masterItem, "D_POSITION")

  local sourcePos =
    reaper.GetMediaItemInfo_Value(
      job.sourceItem, "D_POSITION")

  local masterOffs =
    reaper.GetMediaItemTakeInfo_Value(
      job.masterTake, "D_STARTOFFS")

  local sourceOffs =
    reaper.GetMediaItemTakeInfo_Value(
      job.sourceTake, "D_STARTOFFS")

  local masterRate =
    reaper.GetMediaItemTakeInfo_Value(
      job.masterTake, "D_PLAYRATE")

  local sourceRate =
    reaper.GetMediaItemTakeInfo_Value(
      job.sourceTake, "D_PLAYRATE")

  if masterRate <= 0 or
     sourceRate <= 0 then

    job.status = "ERROR"
    job.error = "PLAYRATE inválido"
    return
  end

  local sourceLen =
    reaper.GetMediaItemInfo_Value(
      job.sourceItem, "D_LENGTH")

  local masterLen =
    reaper.GetMediaItemInfo_Value(
      job.masterItem, "D_LENGTH")

  local commonStart =
    math.max(
      sourcePos,
      masterPos)

  local commonEnd =
    math.min(
      sourcePos + sourceLen,
      masterPos + masterLen)

  local duration =
    math.min(
      600.0,
      commonEnd - commonStart)

  if duration < MIN_OVERLAP_SECONDS then
    job.status = "ERROR"
    job.error = "solapamiento insuficiente"
    return
  end

  local masterStart =
    masterOffs +
    (commonStart - masterPos) *
    masterRate

  local sourceStart =
    sourceOffs +
    (commonStart - sourcePos) *
    sourceRate

  local exe =
    script_dir() ..
    "\\" ..
    EXE_NAME

  if reaper.file_exists and
     not reaper.file_exists(exe) then

    job.status = "ERROR"
    job.error =
      "No se encontró " ..
      EXE_NAME
    return
  end

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
      "Analizando residuo %d/%d · %s · escena %s",
      analyzingIndex,
      #jobs,
      track_label(job.sourceTrack),
      scene_label(
        masterTrack,
        job.masterItem) ..
      " · mapping " ..
      (job.mappingSource or "?")),
    "info")

  draw_ui()
  gfx.update()

  local processResult =
    reaper.ExecProcess(
      cmd,
      120000)

  if not processResult then
    job.status = "ERROR"
    job.error = "ExecProcess falló."
    return
  end

  processResult =
    processResult:gsub(
      "\r\n", "\n")
      :gsub("\r", "\n")

  local firstNl =
    processResult:find(
      "\n", 1, true)

  local code = nil
  local output = processResult

  if firstNl then
    code =
      tonumber(
        processResult:sub(
          1, firstNl - 1))

    if code ~= nil then
      output =
        processResult:sub(
          firstNl + 1)
    end
  end

  if output:sub(1, 6) == "ERROR=" then
    job.status = "ERROR"
    job.error =
      output:match("^ERROR=(.*)") or
      output
    return
  end

  if code ~= nil and code ~= 0 then
    job.status = "ERROR"
    job.error =
      output ~= "" and
      output or
      ("exit code " ..
       tostring(code))
    return
  end

  local result =
    parse_output(output)

  if not result.engineVersion then
    job.status = "ERROR"
    job.error =
      "El ejecutable no reportó ENGINE_VERSION."
    return
  end

  job.modeUsed =
    result.modeUsed or "UNKNOWN"

  job.residualSamples =
    result.delaySamples

  job.residualMs =
    result.delayMs

  job.confidence =
    result.confidence

  job.masterGuid =
    item_guid(job.masterItem)

  job.sourceGuid =
    item_guid(job.sourceItem)

  reaper.GetSetMediaItemInfo_String(
    job.sourceItem,
    "P_EXT:SmartAlignPost.Residual.MappingSource",
    job.mappingSource or "",
    true)

  if job.modeUsed == "DYNAMIC" then
    job.status = "REVIEW DYNAMIC"
    job.error =
      "El residuo no es estático; no se auto-inserta corrección fija."
    return
  end

  local significant =
    math.abs(job.residualSamples) >=
    MIN_RESIDUAL_SAMPLES

  local reliable =
    job.confidence >=
    MIN_CONFIDENCE

  if significant and reliable then
    job.status = "ELIGIBLE"
  elseif significant then
    job.status = "LOW CONF"
  else
    job.status = "ALIGNED"
  end
end

local function write_metadata(job)
  if not job.sourceItem then
    return
  end

  reaper.GetSetMediaItemInfo_String(
    job.sourceItem,
    "P_EXT:SmartAlignPost.Residual.MasterGUID",
    job.masterGuid or "",
    true)

  reaper.GetSetMediaItemInfo_String(
    job.sourceItem,
    "P_EXT:SmartAlignPost.Residual.MasterScene",
    scene_label(masterTrack, job.masterItem),
    true)

  reaper.GetSetMediaItemInfo_String(
    job.sourceItem,
    "P_EXT:SmartAlignPost.Residual.Samples",
    string.format(
      "%.9f",
      job.residualSamples or 0.0),
    true)

  reaper.GetSetMediaItemInfo_String(
    job.sourceItem,
    "P_EXT:SmartAlignPost.Residual.Confidence",
    string.format(
      "%.6f",
      job.confidence or 0.0),
    true)

  reaper.GetSetMediaItemInfo_String(
    job.sourceItem,
    "P_EXT:SmartAlignPost.Residual.Status",
    job.status or "",
    true)
end

local function find_existing_fx(take)
  if not take then
    return -1
  end

  local count =
    reaper.TakeFX_GetCount(take)

  for i = 0, count - 1 do
    local ok, name =
      reaper.TakeFX_GetFXName(
        take, i, "")

    if ok and name == FX_NAME then
      return i
    end
  end

  return -1
end

local function inject_residual_fx(job)
  if job.status ~= "ELIGIBLE" then
    return false, "job no elegible"
  end

  local take =
    reaper.GetActiveTake(
      job.sourceItem)

  if not take then
    return false, "sin take"
  end

  local fxIndex =
    find_existing_fx(take)

  if fxIndex < 0 then
    -- ReaScript's TakeFX_AddByName search is name based. Use the explicit
    -- VST3 form first, then the plain factory name as a compatibility
    -- fallback across REAPER installations.
    local candidates = {
      "VST3: " .. FX_NAME,
      FX_NAME
    }

    for _, candidate in ipairs(candidates) do
      local candidateIndex =
        reaper.TakeFX_AddByName(
          take,
          candidate,
          1)

      if candidateIndex and candidateIndex >= 0 then
        fxIndex = candidateIndex
        break
      end
    end
  end

  if not fxIndex or fxIndex < 0 then
    return false,
      "No se pudo insertar " ..
      FX_NAME ..
      " como Take FX. Verificá que el VST3 esté instalado y visible para REAPER."
  end

  local normalized =
    (job.residualSamples +
     MAX_RESIDUAL_SAMPLES) /
    (2.0 * MAX_RESIDUAL_SAMPLES)

  normalized =
    math.max(
      0.0,
      math.min(
        1.0,
        normalized))

  -- Parameter 0 = Residual Samples.
  -- Parameter 1 = Apply Residual.
  local okResidual =
    reaper.TakeFX_SetParam(
      take,
      fxIndex,
      0,
      normalized)

  local okApply =
    reaper.TakeFX_SetParam(
      take,
      fxIndex,
      1,
      1.0)

  if reaper.TakeFX_SetEnabled then
    reaper.TakeFX_SetEnabled(
      take,
      fxIndex,
      true)
  end

  if not okResidual or not okApply then
    return false,
      "El VST3 fue insertado, pero no se pudieron cargar sus parámetros residuales."
  end

  -- REAPER may automatically open the FX UI when an FX is inserted through
  -- the quick-add path. Residual processing is batch-driven, so the windows
  -- must remain closed; the FX stays instantiated and active on the Take.
  if reaper.TakeFX_SetOpen then
    reaper.TakeFX_SetOpen(
      take,
      fxIndex,
      false)
  end

  if reaper.TakeFX_Show then
    -- showFlag=2 hides a floating Take FX window without removing the FX.
    reaper.TakeFX_Show(
      take,
      fxIndex,
      2)
  end

  reaper.GetSetMediaItemInfo_String(
    job.sourceItem,
    "P_EXT:SmartAlignPost.Residual.FX",
    "AUTO_INSERTED",
    true)

  reaper.UpdateItemInProject(
    job.sourceItem)

  return true
end

local function run_analysis(items)
  if analyzing then
    return
  end

  if not masterTrack then
    set_status(
      "No hay MASTER definido.",
      "error")
    return
  end

  build_jobs(items)

  if #jobs == 0 then
    set_status(
      "No encontré pares MASTER → SOURCE.",
      "error")
    return
  end

  analyzing = true
  analyzingIndex = 0

  local function step()
    if not analyzing then
      return
    end

    analyzingIndex =
      analyzingIndex + 1

    if analyzingIndex > #jobs then
      analyzing = false

      local counts = {
        aligned = 0,
        eligible = 0,
        low = 0,
        review = 0,
        errors = 0,
        injected = 0
      }

      reaper.Undo_BeginBlock()

      for _, job in ipairs(jobs) do
        write_metadata(job)

        if job.status == "ALIGNED" then
          counts.aligned =
            counts.aligned + 1

        elseif job.status == "ELIGIBLE" then
          counts.eligible =
            counts.eligible + 1

          local ok =
            inject_residual_fx(job)

          if ok then
            counts.injected =
              counts.injected + 1
            job.status = "FX INSERTED"
            write_metadata(job)
          else
            job.status = "FX ERROR"
            write_metadata(job)
            counts.errors =
              counts.errors + 1
          end

        elseif job.status == "LOW CONF" then
          counts.low =
            counts.low + 1

        elseif job.status == "SCENE REVIEW" or
               job.status == "REVIEW DYNAMIC" then
          counts.review =
            counts.review + 1

        else
          counts.errors =
            counts.errors + 1
        end
      end

      reaper.UpdateArrange()

      reaper.Undo_EndBlock(
        "Smart Align Post — Residual Scan / Item FX",
        -1)

      set_status(
        string.format(
          "RESIDUAL COMPLETO · %d alineados · %d FX insertados · %d baja conf · %d revisión · %d errores",
          counts.aligned,
          counts.injected,
          counts.low,
          counts.review,
          counts.errors),
        counts.errors > 0 and
        "warn" or
        "ok")

      return
    end

    analyze_job(
      jobs[analyzingIndex])

    draw_ui()
    gfx.update()

    reaper.defer(step)
  end

  reaper.defer(step)
end

local function reset()
  jobs = {}
  analyzing = false
  analyzingIndex = 0

  set_status(
    "Resultados limpiados.",
    "info")
end

local function rgb(r, g, b)
  gfx.set(
    r / 255,
    g / 255,
    b / 255,
    1)
end

local function rect(
  x, y, w, h,
  r, g, b)

  rgb(r, g, b)
  gfx.rect(
    x, y, w, h, 1)
end

local function text(
  x, y, s, size,
  r, g, b)

  rgb(
    r or 230,
    g or 230,
    b or 230)

  gfx.setfont(
    1,
    "Arial",
    size or 16)

  gfx.x = x
  gfx.y = y

  gfx.drawstr(
    tostring(s))
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
    rect(
      x, y, w, h,
      55, 56, 62)

  elseif primary then
    rect(
      x, y, w, h,
      hover and 70 or 52,
      hover and 155 or 125,
      hover and 245 or 210)

  else
    rect(
      x, y, w, h,
      hover and 82 or 64,
      hover and 82 or 64,
      hover and 90 or 70)
  end

  local tw =
    gfx.measurestr(label)

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
    22, 18,
    "SMART ALIGN POST",
    25, 245, 245, 250)

  text(
    22, 50,
    "RESIDUAL BY ITEM · MASTER SCENE AWARE",
    15, 160, 175, 190)

  text(
    22, 76,
    "Cada SOURCE item se compara contra el MASTER item con mayor solapamiento temporal.",
    13, 190, 195, 205)

  text(
    22, 98,
    "Los ítems que cruzan escenas o tienen medición poco confiable no reciben corrección automática.",
    13, 160, 165, 178)

  text(
    805, 24,
    "MASTER",
    13, 150, 160, 175)

  text(
    805, 45,
    masterTrack and
      track_label(masterTrack) or
      "—",
    16, 235, 235, 240)

  text(
    805, 72,
    "JOBS " .. #jobs,
    14, 180, 185, 195)

  local y = 130

  rect(
    18, y, gfx.w - 36, 32,
    45, 47, 53)

  text(28,  y + 8, "SOURCE", 12, 190, 195, 205)
  text(175, y + 8, "MASTER SCENE", 12, 190, 195, 205)
  text(390, y + 8, "RESIDUAL", 12, 190, 195, 205)
  text(500, y + 8, "CONF", 12, 190, 195, 205)
  text(585, y + 8, "MODE", 12, 190, 195, 205)
  text(680, y + 8, "COVER", 12, 190, 195, 205)
  text(755, y + 8, "STATE", 12, 190, 195, 205)

  local rowY = y + 32

  for i, job in ipairs(jobs) do
    if rowY > gfx.h - 120 then
      break
    end

    rect(
      18, rowY,
      gfx.w - 36, 36,
      (i % 2 == 0) and 34 or 30,
      34,
      39)

    local rr, gg, bb =
      215, 220, 230

    if job.status == "ELIGIBLE" then
      rr, gg, bb = 125, 220, 155
    elseif job.status == "FX INSERTED" then
      rr, gg, bb = 105, 235, 180
    elseif job.status == "ALIGNED" then
      rr, gg, bb = 145, 205, 180
    elseif job.status == "LOW CONF" or
           job.status == "REVIEW DYNAMIC" or
           job.status == "SCENE REVIEW" then
      rr, gg, bb = 240, 200, 110
    elseif job.status == "ERROR" or
           job.status == "FX ERROR" then
      rr, gg, bb = 245, 125, 125
    end

    text(
      28, rowY + 9,
      track_label(job.sourceTrack),
      12, 235, 235, 240)

    text(
      175, rowY + 9,
      scene_label(
        masterTrack,
        job.masterItem),
      11, 220, 225, 235)

    text(
      390, rowY + 9,
      string.format(
        "%+.3f",
        job.residualSamples or 0.0),
      12, rr, gg, bb)

    text(
      500, rowY + 9,
      string.format(
        "%.3f",
        job.confidence or 0.0),
      12, rr, gg, bb)

    text(
      585, rowY + 9,
      job.modeUsed or "—",
      12, 220, 225, 235)

    text(
      680, rowY + 9,
      string.format(
        "%.0f%%",
        (job.coverage or 0) * 100),
      12, 220, 225, 235)

    text(
      755, rowY + 9,
      job.status or "—",
      12, rr, gg, bb)

    rowY = rowY + 36
  end

  local fy =
    gfx.h - 78

  local rr, gg, bb =
    180, 190, 205

  if statusKind == "ok" then
    rr, gg, bb =
      120, 220, 150
  elseif statusKind == "warn" then
    rr, gg, bb =
      240, 200, 110
  elseif statusKind == "error" then
    rr, gg, bb =
      245, 120, 120
  end

  text(
    22,
    fy - 18,
    status,
    13,
    rr, gg, bb)

  local hasMaster =
    masterTrack ~= nil

  local hasSelection =
    reaper.CountSelectedMediaItems(0) >= 2

  button(
    18, fy + 8, 210, 38,
    "ANALYZE PROJECT",
    hasMaster and not analyzing,
    true)

  button(
    240, fy + 8, 210, 38,
    "ANALYZE SELECTION",
    hasMaster and
      hasSelection and
      not analyzing,
    true)

  button(
    462, fy + 8, 165, 38,
    "CLEAR",
    not analyzing,
    false)

  button(
    gfx.w - 145,
    fy + 8,
    127, 38,
    "CLOSE",
    true,
    false)
end

local function initializeMaster()
  local count =
    reaper.CountSelectedMediaItems(0)

  if count < 1 then
    if not analyzing then
      masterItem = nil
      masterTrack = nil
      jobs = {}

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
    masterTrack =
      reaper.GetMediaItem_Track(first)

    if not analyzing then
      set_status(
        "MASTER = " ..
        track_label(masterTrack) ..
        " · el resto de los tracks seleccionados se tratará como SOURCE.",
        "info")
    end
  end
end

local function mouse_handler()
  local down =
    (gfx.mouse_cap & 1) == 1

  if down and not mouseDown then
    local fy =
      gfx.h - 78

    if gfx.mouse_x >= 18 and
       gfx.mouse_x <= 228 and
       gfx.mouse_y >= fy + 8 and
       gfx.mouse_y <= fy + 46 then

      run_analysis(
        project_source_items())

    elseif gfx.mouse_x >= 240 and
           gfx.mouse_x <= 450 and
           gfx.mouse_y >= fy + 8 and
           gfx.mouse_y <= fy + 46 then

      run_analysis(
        selected_source_items())

    elseif gfx.mouse_x >= 462 and
           gfx.mouse_x <= 627 and
           gfx.mouse_y >= fy + 8 and
           gfx.mouse_y <= fy + 46 then

      reset()

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
  "Smart Align Post — RESIDUAL BY ITEM",
  WIN_W, WIN_H)

initializeMaster()
draw_ui()
gfx.update()

reaper.defer(loop)
