-- Smart Align Post
-- Unified production workflow for REAPER.
-- PHASE BATCH = primary correction engine.
-- RESIDUAL = automatic post-apply verification/correction layer.
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
-- DYNAMIC renders the SOURCE item sample-domain and replaces its source
-- with the verified corrected WAV while preserving the SOURCE item's position.

local WIN_W, WIN_H = 1080, 650
local MIN_CONFIDENCE = 0.72
local EXE_NAME = "SmartAlignPostPrototype.exe"
local REQUIRED_ENGINE_VERSION = "20260921-onset-evidence-2"

-- Residual verification is intentionally cheap: three short STATIC
-- measurements after APPLY. Only inconclusive cases pay for full analysis.
local RESIDUAL_WINDOW_SEC = 1.5
local RESIDUAL_CHECK_FRACTIONS = {0.20, 0.50, 0.80}
local RESIDUAL_MIN_CONFIDENCE = 0.72
local RESIDUAL_SIGNIFICANT_SAMPLES = 2.0
local RESIDUAL_POST_TOLERANCE_SAMPLES = 1.0
local RESIDUAL_SPREAD_TOLERANCE_SAMPLES = 2.0

local status = "Seleccioná primero un item del MASTER y luego al menos un item de cada SOURCE."
local statusKind = "info"
local masterTrack = nil
local masterItem = nil
local jobs = {}
local analyzing = false
local analyzingIndex = 0
local applying = false
local applyState = nil
local applied = false
local mouseDown = false
local dynamicRenderSerial = 0
local draw_ui
local residual_pass

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

local function item_guid(item)
  if not item then return "" end
  local _, guid =
    reaper.GetSetMediaItemInfo_String(
      item, "GUID", "", false)
  return guid or ""
end

local function master_scene_index(item)
  if not item or not masterTrack then
    return 0
  end

  local count = reaper.CountTrackMediaItems(masterTrack)
  for i = 0, count - 1 do
    if reaper.GetTrackMediaItem(masterTrack, i) == item then
      return i + 1
    end
  end

  return 0
end

local function master_scene_label(item)
  if not item then return "MASTER —" end
  local index = master_scene_index(item)
  local pos = reaper.GetMediaItemInfo_Value(item, "D_POSITION")
  local len = reaper.GetMediaItemInfo_Value(item, "D_LENGTH")
  return string.format(
    "MASTER %02d · %.2f–%.2f s",
    index, pos, pos + len)
end

local function find_master_match(sourceItem)
  if not masterTrack then return nil end

  local sPos =
    reaper.GetMediaItemInfo_Value(
      sourceItem, "D_POSITION")
  local sLen =
    reaper.GetMediaItemInfo_Value(
      sourceItem, "D_LENGTH")
  local sEnd = sPos + sLen

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
    local mEnd = mPos + mLen

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
  if not best or best.overlap < 0.5 then
    return nil
  end

  local secondOverlap =
    candidates[2] and
    candidates[2].overlap or 0.0

  local sourceCoverage =
    sLen > 0 and
    best.overlap / sLen or 0.0

  local ambiguous =
    secondOverlap >= 0.5 and
    sourceCoverage < 0.95

  return {
    item = best.item,
    overlap = best.overlap,
    secondOverlap = secondOverlap,
    sourceCoverage = sourceCoverage,
    ambiguous = ambiguous
  }
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
    local match = find_master_match(sourceItem)

    if match then
      local masterItem = match.item

      local sPos =
        reaper.GetMediaItemInfo_Value(
          sourceItem, "D_POSITION")
      local sLen =
        reaper.GetMediaItemInfo_Value(
          sourceItem, "D_LENGTH")
      local mPos =
        reaper.GetMediaItemInfo_Value(
          masterItem, "D_POSITION")
      local mLen =
        reaper.GetMediaItemInfo_Value(
          masterItem, "D_LENGTH")

      local commonStart =
        math.max(sPos, mPos)
      local commonEnd =
        math.min(
          sPos + sLen,
          mPos + mLen)

      if commonEnd - commonStart >= 0.5 then
        local masterPath, masterTake, masterErr =
          take_source_path(masterItem)
        local sourcePath, sourceTake, sourceErr =
          take_source_path(sourceItem)

        local job = {
          sourceItem = sourceItem,
          sourceTake = sourceTake,
          sourcePath = sourcePath,
          masterItem = masterItem,
          masterTake = masterTake,
          masterPath = masterPath,
          commonStart = commonStart,
          commonEnd = commonEnd,
          overlap = match.overlap,
          sourceCoverage = match.sourceCoverage,
          secondOverlap = match.secondOverlap,
          ambiguous = match.ambiguous,
          masterSceneIndex = master_scene_index(masterItem),
          status = "PENDING",
          modeUsed = nil,
          confidence = 0.0,
          delayMs = 0.0,
          curve = {},
          error = nil,
          sourceTrack = reaper.GetMediaItem_Track(sourceItem)
        }

        job.masterGuid = item_guid(masterItem)
        job.sourceGuid = item_guid(sourceItem)

        reaper.GetSetMediaItemInfo_String(
          sourceItem,
          "P_EXT:SmartAlignPost.Scene.MasterGUID",
          job.masterGuid,
          true)

        reaper.GetSetMediaItemInfo_String(
          sourceItem,
          "P_EXT:SmartAlignPost.Scene.MasterIndex",
          tostring(job.masterSceneIndex),
          true)

        reaper.GetSetMediaItemInfo_String(
          sourceItem,
          "P_EXT:SmartAlignPost.Scene.SourceGUID",
          job.sourceGuid,
          true)

        reaper.GetSetMediaItemInfo_String(
          sourceItem,
          "P_EXT:SmartAlignPost.Scene.CommonStart",
          string.format("%.9f", commonStart),
          true)

        reaper.GetSetMediaItemInfo_String(
          sourceItem,
          "P_EXT:SmartAlignPost.Scene.CommonEnd",
          string.format("%.9f", commonEnd),
          true)

        reaper.GetSetMediaItemInfo_String(
          sourceItem,
          "P_EXT:SmartAlignPost.Scene.Coverage",
          string.format("%.6f", match.sourceCoverage),
          true)

        if match.ambiguous then
          job.status = "SCENE REVIEW"
          job.error = string.format(
            "SOURCE cruza escenas · cobertura %.1f%% · segundo solapamiento %.2f s",
            match.sourceCoverage * 100.0,
            match.secondOverlap)
        elseif not masterPath or not sourcePath then
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
    scoutRobustShiftMs = tonumber(output:match("SCOUT_ROBUST_SHIFT_MS=([%+%-]?[%d%.eE]+)")) or 0.0,
    scoutCoherent = output:match("SCOUT_COHERENT=([01])") == "1",
    delaySamples = tonumber(output:match("DELAY_SAMPLES=([%+%-]?[%d%.]+)")) or 0.0,
    delayMs = tonumber(output:match("DELAY_MS=([%+%-]?[%d%.]+)")) or 0.0,
    confidence = tonumber(output:match("CONFIDENCE=([%+%-]?[%d%.]+)")) or 0.0,
    correlation = tonumber(output:match("CORRELATION=([%+%-]?[%d%.eE]+)")) or 0.0,
    supportWindows = tonumber(output:match("SUPPORT_WINDOWS=([%d]+)")) or 0,
    evidenceInsufficient = output:match("EVIDENCE_INSUFFICIENT=1") ~= nil,
    analyzeMs = tonumber(output:match("ANALYZE_MS=([%+%-]?[%d%.]+)")) or 0.0,
    curve = {},
    scoutTelemetry = output:find("SCOUT_POINTS=", 1, true) ~= nil
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

  if result.engineVersion ~= REQUIRED_ENGINE_VERSION then
    job.status = "ERROR"
    job.error = string.format(
      "ENGINE_VERSION incompatible: %s · se requiere %s.",
      result.engineVersion,
      REQUIRED_ENGINE_VERSION)
    return
  end

  if not result.scoutTelemetry then
    job.status = "ERROR"
    job.error = "El ejecutable no reportó telemetría SCOUT. Reemplazá SmartAlignPostPrototype.exe por el del último artefacto."
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
  job.scoutRobustShiftMs = result.scoutRobustShiftMs
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
          "%s · %s: rate %.6f/%.6f ratio %.6f · conf %.3f · scout %d %.2f→%.2f ms R² %.3f dir %.3f robust %.2f ms coh %s · %s→%s→%s",
          master_scene_label(job.masterItem),
          track_label(job.sourceTrack),
          job.masterRate or 0.0,
          job.sourceRate or 0.0,
          job.rateRatio or 1.0,
          job.confidence or 0.0,
          job.scoutPoints or 0,
          job.scoutFirstMs or 0.0,
          job.scoutLastMs or 0.0,
          job.scoutR2 or 0.0,
          job.scoutDirection or 0.0,
          job.scoutRobustShiftMs or 0.0,
          job.scoutCoherent and "YES" or "NO",
          job.modeRequested or "?",
          job.modeEffective or "?",
          job.modeUsed or "?")
      end

      set_status(
        string.format(
          "ANÁLISIS COMPLETO · ENGINE %s · %d/%d listos · %d DYNAMIC · %d con confidence < %.2f · %s",
          (#jobs > 0 and jobs[1].engineVersion) or "?",
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

local function directStaticApplyEvidence(job)
  if not job or
     job.status ~= "READY" or
     job.modeUsed ~= "STATIC" then
    return false
  end

  -- AUTO may deliberately return low statistical confidence when there is
  -- only one physical onset anchor. That penalty measures redundancy, not
  -- the quality of the direct-arrival measurement itself.
  --
  -- Permit APPLY only for this narrowly defined case:
  --   • exactly one scout observation;
  --   • one static support window;
  --   • engine explicitly reports insufficient redundancy;
  --   • measured direct correlation remains strong.
  --
  -- This is designed for isolated impulsive/direct-arrival material such as
  -- the BOOM/LAV onset case. It does not weaken the normal 0.72 gate for
  -- ordinary low-confidence STATIC results.
  return job.scoutPoints == 1 and
         job.supportWindows == 1 and
         job.evidenceInsufficient == true and
         (job.correlation or 0.0) >= 0.55
end

local function correctionSeconds(job)
  return (job.delayMs or 0.0) / 1000.0
end

local function apply_static_correction(job, delayMs)
  local take = job.sourceTake
  if not take then
    return false, "sin take"
  end

  local rate =
    reaper.GetMediaItemTakeInfo_Value(
      take, "D_PLAYRATE")

  if rate <= 0 then
    return false, "PLAYRATE inválido"
  end

  local offs =
    reaper.GetMediaItemTakeInfo_Value(
      take, "D_STARTOFFS")

  local correctionSec =
    (delayMs or 0.0) / 1000.0

  local targetOffs =
    offs + correctionSec * rate

  -- Normal STATIC path: adjust the media source offset while preserving
  -- the editorial item position.
  if targetOffs >= -1.0e-9 then
    reaper.SetMediaItemTakeInfo_Value(
      take,
      "D_STARTOFFS",
      targetOffs)

    reaper.UpdateItemInProject(
      job.sourceItem)

    local appliedOffs =
      reaper.GetMediaItemTakeInfo_Value(
        take, "D_STARTOFFS")

    if math.abs(appliedOffs - targetOffs) <= 1.0e-9 then
      return true
    end
  end

  -- Edge case that matters for the BOOM/LAV onset test:
  -- SOURCE starts at sample 0 and arrives early, so correcting it by
  -- decreasing D_STARTOFFS would require a negative source offset.
  -- In that situation the only physically valid non-rendered correction is
  -- to move the SOURCE ITEM later by exactly -delay.
  reaper.SetMediaItemTakeInfo_Value(
    take,
    "D_STARTOFFS",
    offs)

  reaper.UpdateItemInProject(
    job.sourceItem)

  local itemPos =
    reaper.GetMediaItemInfo_Value(
      job.sourceItem, "D_POSITION")

  reaper.SetMediaItemPosition(
    job.sourceItem,
    itemPos - correctionSec,
    true)

  reaper.UpdateItemInProject(
    job.sourceItem)

  job.usedItemPositionFallback = true
  job.appliedItemShiftSec = -correctionSec

  return true
end

local function applyStatic(job)
  return apply_static_correction(
    job,
    job.delayMs or 0.0)
end

local function applyDynamic(job)
  local take = job.sourceTake
  if not take or #job.curve < 2 then
    return false, "curva insuficiente"
  end

  local itemPos =
    reaper.GetMediaItemInfo_Value(
      job.sourceItem, "D_POSITION")

  local itemLen =
    reaper.GetMediaItemInfo_Value(
      job.sourceItem, "D_LENGTH")

  local masterPos =
    reaper.GetMediaItemInfo_Value(
      job.masterItem, "D_POSITION")

  local sourcePos =
    reaper.GetMediaItemInfo_Value(
      job.sourceItem, "D_POSITION")

  local masterTake = job.masterTake
  local sourceTake = job.sourceTake

  local masterOffs =
    reaper.GetMediaItemTakeInfo_Value(
      masterTake, "D_STARTOFFS")

  local sourceOffs =
    reaper.GetMediaItemTakeInfo_Value(
      sourceTake, "D_STARTOFFS")

  local masterRate =
    reaper.GetMediaItemTakeInfo_Value(
      masterTake, "D_PLAYRATE")

  local sourceRate =
    reaper.GetMediaItemTakeInfo_Value(
      sourceTake, "D_PLAYRATE")

  if masterRate <= 0 or sourceRate <= 0 or itemLen <= 0 then
    return false, "parámetros de item/playrate inválidos"
  end

  -- DYNAMIC_RENDER can safely cover small SOURCE edges that fall
  -- outside the corresponding MASTER item: the measured curve is defined
  -- over the common scene interval, and the renderer holds the first/last
  -- measured delay outside that interval instead of inventing a new scene.
  --
  -- Cross-scene SOURCE items are still rejected earlier by scene matching
  -- (job.ambiguous), so this only removes the unnecessary requirement that
  -- MASTER and SOURCE items have identical editorial boundaries.
  if job.ambiguous then
    return false, string.format(
      "DYNAMIC no aplicado · SOURCE cruza escenas (MASTER %02d, cobertura %.1f%%).",
      job.masterSceneIndex or 0,
      job.sourceCoverage and job.sourceCoverage * 100.0 or 0.0)
  end

  local extrapolatedEdges =
    (job.commonStart > itemPos + 1e-6) or
    (job.commonEnd < itemPos + itemLen - 1e-6)

  local masterStart =
    masterOffs +
    (itemPos - masterPos) * masterRate

  local sourceStart =
    sourceOffs +
    (itemPos - sourcePos) * sourceRate

  local exe = script_dir() .. "\\" .. EXE_NAME
  if reaper.file_exists and not reaper.file_exists(exe) then
    return false, "No se encontró " .. EXE_NAME
  end

  local outDir =
    reaper.GetResourcePath() ..
    "\\SmartAlignPost_Dynamic"

  if reaper.RecursiveCreateDirectory then
    reaper.RecursiveCreateDirectory(outDir, 0)
  end

  dynamicRenderSerial = dynamicRenderSerial + 1

  local outputPath =
    outDir ..
    "\\SAP_Dynamic_" ..
    tostring(os.time()) ..
    "_" ..
    tostring(dynamicRenderSerial) ..
    ".wav"

  local curvePath =
    outDir ..
    "\\SAP_Curve_" ..
    tostring(os.time()) ..
    "_" ..
    tostring(dynamicRenderSerial) ..
    ".csv"

  local curveFile =
    io.open(curvePath, "w")

  if not curveFile then
    return false,
      "No se pudo crear el archivo de curva DYNAMIC"
  end

  -- The analysis curve is measured from commonStart. DYNAMIC_RENDER renders
  -- from the SOURCE item start, so move the exact measured curve onto the
  -- item-relative timeline before passing it to the renderer.
  local curveOriginShift =
    job.commonStart - itemPos

  for _, point in ipairs(job.curve) do
    curveFile:write(
      string.format(
        "%.12f,%.12f\n",
        (point.time or 0.0) + curveOriginShift,
        point.delay or 0.0))
  end

  curveFile:close()

  local cmd =
    quote(exe) .. " " ..
    quote(job.masterPath) .. " " ..
    quote(job.sourcePath) .. " " ..
    string.format(
      "\"%.9f\" \"%.9f\" \"%.6f\" \"%.9f\" \"%.9f\" DYNAMIC_RENDER %s %s",
      masterStart,
      sourceStart,
      itemLen,
      masterRate,
      sourceRate,
      quote(outputPath),
      quote(curvePath))

  set_status(
    "APPLY DYNAMIC · renderizando sample-accurate · " ..
    track_label(job.sourceTrack) ..
    (extrapolatedEdges and " · bordes extrapolados" or ""),
    "info")

  draw_ui()
  gfx.update()

  local processResult =
    reaper.ExecProcess(cmd, 120000)

  if not processResult then
    return false, "ExecProcess falló en DYNAMIC_RENDER"
  end

  processResult =
    processResult:gsub("\r\n", "\n"):gsub("\r", "\n")

  local firstNl =
    processResult:find("\n", 1, true)

  local code = nil
  local output = processResult

  if firstNl then
    code = tonumber(
      processResult:sub(1, firstNl - 1))

    if code ~= nil then
      output =
        processResult:sub(firstNl + 1)
    end
  end

  if output:sub(1, 6) == "ERROR=" then
    return false,
      output:match("^ERROR=(.*)") or output
  end

  if code ~= nil and code ~= 0 then
    local validation =
      output:match("POST_DELAY_MS=([%+%-]?[%d%.eE]+)")
        or "n/a"

    return false,
      "DYNAMIC no validado · residual " ..
      validation .. " ms"
  end

  local postValid =
    output:match("POST_VALID=([01])")

  local outputWav =
    output:match("OUTPUT_WAV=(.-)\n")

  if not outputWav or postValid ~= "1" then
    local residual =
      output:match("POST_DELAY_MS=([%+%-]?[%d%.eE]+)")
        or "n/a"

    return false,
      "DYNAMIC no validado · residual " ..
      residual .. " ms"
  end

  local newSource =
    reaper.PCM_Source_CreateFromFile(
      outputWav)

  if not newSource then
    return false,
      "REAPER no pudo abrir el WAV corregido"
  end

  if not reaper.SetMediaItemTake_Source(
      take,
      newSource) then
    return false,
      "No se pudo reemplazar el SOURCE del take"
  end

  local refreshedPath, refreshedTake =
    take_source_path(job.sourceItem)

  if refreshedPath then
    job.sourcePath = refreshedPath
    job.sourceTake = refreshedTake
  end

  -- The generated WAV has no REAPER peak cache yet. Build it now so the
  -- corrected waveform is immediately visible at normal zoom levels.
  if reaper.PCM_Source_BuildPeaks then
    local remaining =
      reaper.PCM_Source_BuildPeaks(
        newSource, 0)

    local guard = 0
    while remaining and remaining > 0 and guard < 256 do
      remaining =
        reaper.PCM_Source_BuildPeaks(
          newSource, 1)
      guard = guard + 1
    end

    if remaining and remaining <= 0 then
      reaper.PCM_Source_BuildPeaks(
        newSource, 2)
    end
  end

  reaper.SetMediaItemTakeInfo_Value(
    take, "D_STARTOFFS", 0.0)

  reaper.SetMediaItemTakeInfo_Value(
    take, "D_PLAYRATE", 1.0)

  reaper.SetMediaItemTakeInfo_Value(
    take, "B_PPITCH", 0)

  reaper.UpdateItemInProject(
    job.sourceItem)

  reaper.UpdateArrange()

  job.postValid = true
  job.postDelayMs =
    tonumber(
      output:match(
        "POST_DELAY_MS=([%+%-]?[%d%.eE]+)")) or 0.0

  job.postConfidence =
    tonumber(
      output:match(
        "POST_CONFIDENCE=([%+%-]?[%d%.eE]+)")) or 0.0

  job.outputWav = outputWav

  return true
end

local function finalize_apply_phase()
  local state = applyState
  if not state then
    applying = false
    return
  end

  reaper.UpdateArrange()
  reaper.Undo_EndBlock(
    "Smart Align Post — Phase Batch Apply",
    -1)

  applyState = nil
  applied = true

  if state.failures > 0 then
    local detail =
      #state.applyErrors > 0 and
      (" · " .. table.concat(state.applyErrors, " | ")) or
      ""
    set_status(
      string.format(
        "APPLY parcial · %d aplicados · %d omitidos · %d errores%s. Undo disponible.",
        state.appliedCount,
        state.skipped,
        state.failures,
        detail),
      "error")
  else
    set_status(
      string.format(
        "APPLY COMPLETO · %d STATIC · %d DYNAMIC · %d omitidos. MASTER y D_POSITION intactos.",
        state.staticCount,
        state.dynamicCount,
        state.skipped),
      "ok")
  end

  draw_ui()
  gfx.update()

  -- Give the UI one full REAPER cycle before starting RESIDUAL.
  reaper.defer(function()
    residual_pass()
    applying = false
    draw_ui()
    gfx.update()
  end)
end

local function apply_all_phase_step()
  local state = applyState
  if not state or not applying then
    return
  end

  if state.index > #jobs then
    finalize_apply_phase()
    return
  end

  local jobIndex = state.index
  state.index = state.index + 1
  local job = jobs[jobIndex]

  local dynamicReady =
    job.status == "READY" and
    job.modeUsed == "DYNAMIC" and
    #job.curve >= 2

  local directStaticReady =
    directStaticApplyEvidence(job)

  local confidenceAccept =
    job.confidence >= MIN_CONFIDENCE or
    dynamicReady or
    directStaticReady

  set_status(
    string.format(
      "APPLY %d/%d · %s · %s",
      jobIndex,
      #jobs,
      track_label(job.sourceTrack),
      job.modeUsed or "—"),
    "info")
  draw_ui()
  gfx.update()

  if job.status ~= "READY" or not confidenceAccept then
    state.skipped = state.skipped + 1
  else
    local ok = false
    local applyError = nil

    if job.modeUsed == "DYNAMIC" then
      ok, applyError = applyDynamic(job)
      if ok then
        state.dynamicCount = state.dynamicCount + 1
      end
    else
      ok, applyError = applyStatic(job)
      if ok then
        state.staticCount = state.staticCount + 1
      end
    end

    if not ok and applyError then
      job.error = applyError
      job.status = "APPLY ERROR"
      state.applyErrors[#state.applyErrors + 1] =
        string.format(
          "%s / %s: %s",
          master_scene_label(job.masterItem),
          track_label(job.sourceTrack),
          applyError)
    end

    if ok then
      state.appliedCount = state.appliedCount + 1
      if job.modeUsed == "DYNAMIC" and job.postValid then
        job.status =
          string.format(
            "APPLIED · residual %.3f ms",
            job.postDelayMs or 0.0)
      else
        job.status = "APPLIED"
      end
    else
      state.failures = state.failures + 1
    end
  end

  draw_ui()
  gfx.update()

  -- Yield between every item so REAPER can repaint and process window events.
  reaper.defer(apply_all_phase_step)
end

local function apply_all_phase()
  if analyzing or applying or #jobs == 0 then
    set_status("Primero ANALYZE.", "warn")
    return
  end

  local lowStatic = 0
  local lowDynamic = 0

  for _, job in ipairs(jobs) do
    if job.status == "READY" and
       job.confidence < MIN_CONFIDENCE then
      if job.modeUsed == "DYNAMIC" and #job.curve >= 2 then
        lowDynamic = lowDynamic + 1
      elseif not directStaticApplyEvidence(job) then
        lowStatic = lowStatic + 1
      end
    end
  end

  if lowStatic > 0 or lowDynamic > 0 then
    local parts = {}

    if lowDynamic > 0 then
      parts[#parts + 1] =
        string.format(
          "%d DYNAMIC con confidence < %.2f: se aplicarán porque la deriva temporal fue detectada y existe una curva válida.",
          lowDynamic,
          MIN_CONFIDENCE)
    end

    if lowStatic > 0 then
      parts[#parts + 1] =
        string.format(
          "%d STATIC con confidence < %.2f: se omitirán.",
          lowStatic,
          MIN_CONFIDENCE)
    end

    parts[#parts + 1] = "¿Continuar con APPLY?"

    local answer =
      reaper.ShowMessageBox(
        table.concat(parts, "\n\n"),
        "Smart Align Post — confidence",
        1)

    if answer ~= 1 then
      return
    end
  end

  reaper.Undo_BeginBlock()

  applyState = {
    index = 1,
    appliedCount = 0,
    skipped = 0,
    failures = 0,
    dynamicCount = 0,
    staticCount = 0,
    applyErrors = {}
  }

  applied = false
  applying = true

  set_status(
    string.format(
      "APPLY INICIADO · %d jobs · preparando…",
      #jobs),
    "info")
  draw_ui()
  gfx.update()

  reaper.defer(apply_all_phase_step)
end

local function median_values(values)
  if #values == 0 then return 0.0 end

  local copy = {}
  for i, value in ipairs(values) do
    copy[i] = value
  end

  table.sort(copy)

  local mid = math.floor((#copy + 1) / 2)
  if #copy % 2 == 1 then
    return copy[mid]
  end

  return 0.5 * (copy[mid] + copy[mid + 1])
end

local function run_quick_residual(job)
  local masterPath, masterTake, masterErr =
    take_source_path(job.masterItem)
  local sourcePath, sourceTake, sourceErr =
    take_source_path(job.sourceItem)

  if not masterPath or not sourcePath then
    return {
      state = "REVIEW",
      error = sourceErr or masterErr or "source inválido"
    }
  end

  job.masterTake = masterTake
  job.sourceTake = sourceTake
  job.masterPath = masterPath
  job.sourcePath = sourcePath

  local masterPos =
    reaper.GetMediaItemInfo_Value(job.masterItem, "D_POSITION")
  local sourcePos =
    reaper.GetMediaItemInfo_Value(job.sourceItem, "D_POSITION")

  local masterOffs =
    reaper.GetMediaItemTakeInfo_Value(masterTake, "D_STARTOFFS")
  local sourceOffs =
    reaper.GetMediaItemTakeInfo_Value(sourceTake, "D_STARTOFFS")

  local masterRate =
    reaper.GetMediaItemTakeInfo_Value(masterTake, "D_PLAYRATE")
  local sourceRate =
    reaper.GetMediaItemTakeInfo_Value(sourceTake, "D_PLAYRATE")

  if masterRate <= 0 or sourceRate <= 0 then
    return {
      state = "REVIEW",
      error = "PLAYRATE inválido"
    }
  end

  local liveMasterPos =
    reaper.GetMediaItemInfo_Value(
      job.masterItem, "D_POSITION")
  local liveMasterLen =
    reaper.GetMediaItemInfo_Value(
      job.masterItem, "D_LENGTH")
  local liveSourcePos =
    reaper.GetMediaItemInfo_Value(
      job.sourceItem, "D_POSITION")
  local liveSourceLen =
    reaper.GetMediaItemInfo_Value(
      job.sourceItem, "D_LENGTH")

  local liveCommonStart =
    math.max(
      liveMasterPos,
      liveSourcePos)
  local liveCommonEnd =
    math.min(
      liveMasterPos + liveMasterLen,
      liveSourcePos + liveSourceLen)

  local overlapDuration =
    liveCommonEnd - liveCommonStart

  job.residualCommonStart =
    liveCommonStart
  job.residualCommonEnd =
    liveCommonEnd

  if overlapDuration < 0.5 then
    return {
      state = "REVIEW",
      error = "solapamiento residual insuficiente"
    }
  end

  local windowSec =
    math.min(
      RESIDUAL_WINDOW_SEC,
      math.max(
        0.30,
        overlapDuration * 0.45))

  if windowSec > overlapDuration then
    windowSec = overlapDuration
  end

  if windowSec <= 0.20 then
    return {
      state = "REVIEW",
      error = "ventana residual demasiado corta"
    }
  end

  local measurements = {}

  for index, fraction in ipairs(RESIDUAL_CHECK_FRACTIONS) do
    local center =
      liveCommonStart +
      overlapDuration * fraction

    local startTime =
      math.max(
        liveCommonStart,
        math.min(
          center - windowSec * 0.5,
          liveCommonEnd - windowSec))

    if startTime < job.commonStart then
      startTime = job.commonStart
    end

    local masterStart =
      masterOffs +
      (startTime - masterPos) * masterRate

    local sourceStart =
      sourceOffs +
      (startTime - sourcePos) * sourceRate

    local exe =
      script_dir() ..
      "\\" ..
      EXE_NAME

    local cmd =
      quote(exe) .. " " ..
      quote(masterPath) .. " " ..
      quote(sourcePath) .. " " ..
      string.format(
        "\"%.9f\" \"%.9f\" \"%.6f\" \"%.9f\" \"%.9f\" STATIC",
        masterStart,
        sourceStart,
        windowSec,
        masterRate,
        sourceRate)

    set_status(
      string.format(
        "RESIDUAL CHECK %d/%d · %s · %.1f s",
        index,
        #RESIDUAL_CHECK_FRACTIONS,
        track_label(job.sourceTrack),
        windowSec),
      "info")

    draw_ui()
    gfx.update()

    local processResult =
      reaper.ExecProcess(cmd, 120000)

    if not processResult then
      return {
        state = "REVIEW",
        error = "ExecProcess falló en residual"
      }
    end

    processResult =
      processResult:gsub("\r\n", "\n")
        :gsub("\r", "\n")

    local firstNl =
      processResult:find("\n", 1, true)
    local code = nil
    local output = processResult

    if firstNl then
      code =
        tonumber(
          processResult:sub(
            1,
            firstNl - 1))

      if code ~= nil then
        output =
          processResult:sub(firstNl + 1)
      end
    end

    if output:sub(1, 6) == "ERROR=" or
       (code ~= nil and code ~= 0) then
      return {
        state = "REVIEW",
        error =
          output ~= "" and
          output or
          ("exit code " .. tostring(code))
      }
    end

    local result =
      parse_output(output)

    if not result.engineVersion then
      return {
        state = "REVIEW",
        error = "el ejecutable no reportó ENGINE_VERSION"
      }
    end

    if result.confidence >=
       RESIDUAL_MIN_CONFIDENCE then

      measurements[#measurements + 1] = {
        samples = result.delaySamples,
        ms = result.delayMs,
        confidence = result.confidence
      }
    end
  end

  if #measurements < 2 then
    return {
      state = "REVIEW",
      error = "menos de 2 mediciones residuales confiables",
      measurements = measurements
    }
  end

  local sampleValues = {}
  local msValues = {}
  local confidenceValues = {}
  local minSample = math.huge
  local maxSample = -math.huge

  for _, measurement in ipairs(measurements) do
    sampleValues[#sampleValues + 1] =
      measurement.samples
    msValues[#msValues + 1] =
      measurement.ms
    confidenceValues[#confidenceValues + 1] =
      measurement.confidence

    minSample =
      math.min(
        minSample,
        measurement.samples)

    maxSample =
      math.max(
        maxSample,
        measurement.samples)
  end

  local residualSamples =
    median_values(sampleValues)

  local residualMs =
    median_values(msValues)

  local confidence =
    median_values(confidenceValues)

  local spread =
    maxSample - minSample

  job.residualSamples = residualSamples
  job.residualMs = residualMs
  job.residualConfidence = confidence
  job.residualSpread = spread

  if confidence < RESIDUAL_MIN_CONFIDENCE or
     spread > RESIDUAL_SPREAD_TOLERANCE_SAMPLES then

    return {
      state = "REVIEW",
      residualSamples = residualSamples,
      residualMs = residualMs,
      confidence = confidence,
      spread = spread,
      measurements = measurements,
      error =
        spread > RESIDUAL_SPREAD_TOLERANCE_SAMPLES and
        string.format(
          "mediciones no consistentes · spread %.3f samples",
          spread) or
        "confidence residual insuficiente"
    }
  end

  if math.abs(residualSamples) <
     RESIDUAL_SIGNIFICANT_SAMPLES then

    return {
      state = "ALIGNED",
      residualSamples = residualSamples,
      residualMs = residualMs,
      confidence = confidence,
      spread = spread,
      measurements = measurements
    }
  end

  return {
    state = "ELIGIBLE",
    residualSamples = residualSamples,
    residualMs = residualMs,
    confidence = confidence,
    spread = spread,
    measurements = measurements
  }
end

local function apply_residual_direct(job, result)
  if not result or
     result.state ~= "ELIGIBLE" then
    return false, "residual no elegible"
  end

  local originalPos =
    reaper.GetMediaItemInfo_Value(
      job.sourceItem, "D_POSITION")
  local originalOffs =
    reaper.GetMediaItemTakeInfo_Value(
      job.sourceTake, "D_STARTOFFS")

  local ok, applyError =
    apply_static_correction(
      job,
      result.residualMs or 0.0)

  if not ok then
    return false, applyError
  end

  local verification =
    run_quick_residual(job)

  if not verification or
     verification.state ~= "ALIGNED" or
     math.abs(
       verification.residualSamples or 0.0) >
     RESIDUAL_POST_TOLERANCE_SAMPLES then

    reaper.SetMediaItemTakeInfo_Value(
      job.sourceTake,
      "D_STARTOFFS",
      originalOffs)

    reaper.SetMediaItemPosition(
      job.sourceItem,
      originalPos,
      true)

    reaper.UpdateItemInProject(
      job.sourceItem)

    return false,
      string.format(
        "residual no verificado · antes %+0.3f samples · después %+0.3f samples · corrección revertida",
        result.residualSamples or 0.0,
        verification and
          verification.residualSamples or
          0.0)
  end

  job.residualBefore =
    result.residualSamples

  job.residualAfter =
    verification.residualSamples

  job.residualApplied =
    true

  return true
end

local function write_residual_metadata(
  job,
  state,
  message)

  if not job.sourceItem then
    return
  end

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
      job.residualConfidence or 0.0),
    true)

  reaper.GetSetMediaItemInfo_String(
    job.sourceItem,
    "P_EXT:SmartAlignPost.Residual.Status",
    state or "",
    true)

  if message then
    reaper.GetSetMediaItemInfo_String(
      job.sourceItem,
      "P_EXT:SmartAlignPost.Residual.Message",
      message,
      true)
  end
end

residual_pass = function()
  if not applied or #jobs == 0 then
    return
  end

  local aligned = 0
  local corrected = 0
  local review = 0
  local errors = {}

  reaper.Undo_BeginBlock()

  for _, job in ipairs(jobs) do
    if type(job.status) == "string" and
       job.status:match("^APPLIED") then

      local quick =
        run_quick_residual(job)

      if quick.state == "ALIGNED" then
        aligned = aligned + 1

        job.residualSamples =
          quick.residualSamples
        job.residualMs =
          quick.residualMs
        job.residualConfidence =
          quick.confidence
        job.status = "ALIGNED"

        write_residual_metadata(
          job,
          "ALIGNED",
          "Quick residual check OK.")

      elseif quick.state == "ELIGIBLE" then
        local ok, err =
          apply_residual_direct(
            job,
            quick)

        if ok then
          corrected = corrected + 1
          job.status =
            string.format(
              "RESIDUAL APPLIED · %.3f→%.3f samples",
              quick.residualSamples or 0.0,
              job.residualAfter or 0.0)

          write_residual_metadata(
            job,
            "APPLIED",
            "Residual directo verificado.")
        else
          review = review + 1
          job.status = "RESIDUAL REVIEW"
          job.error = err

          errors[#errors + 1] =
            string.format(
              "%s / %s: %s",
              master_scene_label(job.masterItem),
              track_label(job.sourceTrack),
              err)

          write_residual_metadata(
            job,
            "REVIEW",
            err)
        end

      else
        -- Only inconclusive quick checks pay the cost of the full
        -- analyzer. PHASE BATCH itself is not modified.
        analyze_job(job)

        local fullResidualSamples =
          (job.delayMs / 1000.0) *
          (job.masterRate or 48000.0)

        if job.status == "READY" and
           job.modeUsed == "STATIC" and
           math.abs(fullResidualSamples) >=
             RESIDUAL_SIGNIFICANT_SAMPLES and
           job.confidence >= MIN_CONFIDENCE then

          local fallback = {
            state = "ELIGIBLE",
            residualSamples = fullResidualSamples,
            residualMs = job.delayMs,
            confidence = job.confidence
          }

          local ok, err =
            apply_residual_direct(
              job,
              fallback)

          if ok then
            corrected = corrected + 1
            job.status =
              string.format(
                "RESIDUAL APPLIED · %.3f→%.3f samples",
                fallback.residualSamples,
                job.residualAfter or 0.0)

            write_residual_metadata(
              job,
              "APPLIED",
              "Residual verificado tras full fallback.")
          else
            review = review + 1
            job.status = "RESIDUAL REVIEW"
            job.error = err

            errors[#errors + 1] =
              string.format(
                "%s / %s: %s",
                master_scene_label(job.masterItem),
                track_label(job.sourceTrack),
                err)

            write_residual_metadata(
              job,
              "REVIEW",
              err)
          end

        elseif job.status == "READY" and
               job.modeUsed == "STATIC" and
               math.abs(fullResidualSamples) <
                 RESIDUAL_SIGNIFICANT_SAMPLES then

          aligned = aligned + 1

          job.residualSamples =
            fullResidualSamples
          job.residualMs =
            job.delayMs
          job.residualConfidence =
            job.confidence
          job.status = "ALIGNED"

          write_residual_metadata(
            job,
            "ALIGNED",
            "Full residual fallback OK.")
        else
          review = review + 1
          job.status = "RESIDUAL REVIEW"
          local message = quick.error or
            "residual requiere revisión"

          job.error = message

          errors[#errors + 1] =
            string.format(
              "%s / %s: %s",
              master_scene_label(job.masterItem),
              track_label(job.sourceTrack),
              message)

          write_residual_metadata(
            job,
            "REVIEW",
            message)
        end
      end

      draw_ui()
      gfx.update()
    end
  end

  reaper.UpdateArrange()

  reaper.Undo_EndBlock(
    "Smart Align Post — Residual Verify",
    -1)

  if #errors > 0 then
    set_status(
      string.format(
        "RESIDUAL PARCIAL · %d OK · %d corregidos · %d revisión · %d errores · %s",
        aligned,
        corrected,
        review,
        #errors,
        table.concat(errors, " | ")),
      "warn")
  else
    set_status(
      string.format(
        "RESIDUAL VERIFICADO · %d OK · %d corregidos · %d revisión.",
        aligned,
        corrected,
        review),
      review > 0 and "warn" or "ok")
  end
end

local function apply_all()
  if analyzing or applying then
    return
  end
  applied = false
  apply_all_phase()
end

local function reset_results()
  if applying then
    return
  end
  jobs = {}
  analyzing = false
  analyzingIndex = 0
  applying = false
  applyState = nil
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
    "PHASE BATCH · GCC-PHAT + waveform refinement · RESIDUAL AUTO VERIFY",
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
  text(195, y + 8, "MASTER", 13, 190, 195, 205)
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

    -- DYNAMIC with a validated temporal curve is actionable even when
    -- its acoustic confidence is below the static threshold. The number shown
    -- is still the acoustic confidence; the green state means the dynamic
    -- trajectory itself has been validated.
    local confGood =
      job.confidence >= MIN_CONFIDENCE or
      (job.modeUsed == "DYNAMIC" and #job.curve >= 2)

    text(
      28, rowY + 9,
      track_label(job.sourceTrack),
      13, 235, 235, 240)

    text(
      195, rowY + 9,
      master_scene_label(job.masterItem),
      11, 210, 215, 225)

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

  local footer =
    "Diseñado y creado por Ramiro N. Alvarez · con herramientas de IA."
  local footerWidth = gfx.measurestr(footer)

  text(
    (gfx.w - footerWidth) * 0.5,
    gfx.h - 18,
    footer,
    11, 125, 130, 140)

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
    #jobs > 0 and not analyzing and not applying,
    true)

  button(
    589, fy + 8, 120, 38,
    "CLEAR",
    not analyzing and not applying,
    false)

  button(
    gfx.w - 145, fy + 8, 127, 38,
    "CLOSE",
    not analyzing and not applying,
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

  if not analyzing and not applying then
    initializeMaster()
  end

  draw_ui()
  gfx.update()
  reaper.defer(loop)
end

gfx.init(
  "Smart Align Post",
  WIN_W, WIN_H)

gfx.clear =
  24 + 25 * 256 + 29 * 65536

initializeMaster()
draw_ui()
gfx.update()
reaper.defer(loop)
