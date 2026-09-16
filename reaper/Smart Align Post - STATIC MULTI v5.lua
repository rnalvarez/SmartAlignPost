-- Smart Align Post - STATIC MULTI v5
-- OBJETIVO: TODAS las SOURCES quedan en fase respecto del MASTER.
-- El MASTER nunca se modifica.
-- Cada SOURCE se mide EXCLUSIVAMENTE contra MASTER.
-- APPLY corrige D_STARTOFFS y vuelve a medir contra MASTER hasta 3 pasadas.
-- D_POSITION nunca se modifica.
-- Antes de ANALYZE se puede hacer click sobre una fila para elegir MASTER.

local MIN_CONFIDENCE = 0.80
local MAX_PASSES = 3
local CONVERGED_SAMPLES = 0.25
local MIN_ANALYSIS_SEC = 0.25
local WIN_W, WIN_H = 900, 620

local function script_dir()
  local src = debug.getinfo(1, "S").source
  if src:sub(1, 1) == "@" then src = src:sub(2) end
  return src:match("^(.*)[/\\][^/\\]+$") or "."
end

local function quote(s)
  return '"' .. tostring(s):gsub('"', '\\"') .. '"'
end

local function rgb(r, g, b) gfx.set((r or 230)/255, (g or 230)/255, (b or 230)/255, 1) end
local function rect(x, y, w, h, r, g, b) rgb(r,g,b); gfx.rect(x,y,w,h,1) end
local function text(x, y, s, size, r, g, b)
  rgb(r,g,b)
  gfx.setfont(1, "Arial", size or 16)
  gfx.x, gfx.y = x, y
  gfx.drawstr(tostring(s))
end
local function inside(x,y,w,h,mx,my)
  return mx >= x and mx <= x+w and my >= y and my <= y+h
end

local items = {}
local masterItem = nil
local analyzed = false
local applied = false
local results = {}
local status = "Seleccioná items y elegí cuál es el MASTER."
local statusKind = "info"
local lastMouseDown = false

local function set_status(s, kind)
  status = s or ""
  statusKind = kind or "info"
end

local function item_name(item)
  local ok, name = reaper.GetSetMediaItemInfo_String(item, "P_NAME", "", false)
  if ok and name ~= "" then return name end
  local pos = reaper.GetMediaItemInfo_Value(item, "D_POSITION")
  return string.format("Item %.3f s", pos)
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

local function refresh_selection()
  items = {}
  local n = reaper.CountSelectedMediaItems(0)
  for i=0,n-1 do
    items[#items+1] = reaper.GetSelectedMediaItem(0, i)
  end
  if #items < 1 then
    masterItem = nil
  elseif not masterItem or not (function()
      for _, it in ipairs(items) do if it == masterItem then return true end end
      return false
    end)() then
    masterItem = items[1]
  end
end

local function get_master_index()
  for i,it in ipairs(items) do if it == masterItem then return i end end
  return nil
end

local function parse_process_output(processResult)
  if not processResult then return nil,nil,"ExecProcess falló." end
  local normalized = processResult:gsub("\r\n","\n"):gsub("\r","\n")
  local eol = normalized:find("\n",1,true)
  if eol then
    local code = tonumber((normalized:sub(1,eol-1)):match("^%s*(%-?%d+)%s*$"))
    return code, normalized:sub(eol+1), normalized
  end
  return tonumber(normalized:match("^%s*(%-?%d+)%s*$")), "", normalized
end

local function analyze_pair(masterItem_, sourceItem_)
  local masterPath, masterTake, masterErr = take_source_path(masterItem_)
  if not masterPath then return nil, "MASTER: " .. tostring(masterErr) end
  local sourcePath, sourceTake, sourceErr = take_source_path(sourceItem_)
  if not sourcePath then return nil, "SOURCE: " .. tostring(sourceErr) end

  local masterPos = reaper.GetMediaItemInfo_Value(masterItem_, "D_POSITION")
  local masterLength = reaper.GetMediaItemInfo_Value(masterItem_, "D_LENGTH")
  local masterOffs = reaper.GetMediaItemTakeInfo_Value(masterTake, "D_STARTOFFS")
  local masterRate = reaper.GetMediaItemTakeInfo_Value(masterTake, "D_PLAYRATE")

  local sourcePos = reaper.GetMediaItemInfo_Value(sourceItem_, "D_POSITION")
  local sourceLength = reaper.GetMediaItemInfo_Value(sourceItem_, "D_LENGTH")
  local sourceOffs = reaper.GetMediaItemTakeInfo_Value(sourceTake, "D_STARTOFFS")
  local sourceRate = reaper.GetMediaItemTakeInfo_Value(sourceTake, "D_PLAYRATE")

  if masterRate <= 0 or sourceRate <= 0 then return nil, "PLAYRATE inválido." end

  local commonStart = math.max(masterPos, sourcePos)
  local commonEnd = math.min(masterPos + masterLength, sourcePos + sourceLength)
  if commonEnd - commonStart < MIN_ANALYSIS_SEC then
    return nil, string.format("Tramo común insuficiente (mínimo %.3f s).", MIN_ANALYSIS_SEC)
  end

  local masterStart = masterOffs + (commonStart - masterPos) * masterRate
  local sourceStart = sourceOffs + (commonStart - sourcePos) * sourceRate

  local exe = script_dir() .. "\\SmartAlignPostPrototype.exe"
  local cmd = quote(exe) .. " " .. quote(masterPath) .. " " .. quote(sourcePath)
    .. " " .. quote(string.format("%.12f", masterStart))
    .. " " .. quote(string.format("%.12f", sourceStart))

  local returnCode, output, normalized = parse_process_output(reaper.ExecProcess(cmd, 60000))
  if returnCode ~= 0 then return nil, "Analizador falló.\n\n" .. tostring(normalized) end

  local delayMs = tonumber(output:match("DELAY_MS=([%+%-]?[%d%.]+)"))
  local delaySamples = tonumber(output:match("DELAY_SAMPLES=([%+%-]?[%d%.]+)"))
  local confidence = tonumber(output:match("CONFIDENCE=([%+%-]?[%d%.]+)"))
  local correlation = tonumber(output:match("CORRELATION=([%+%-]?[%d%.]+)"))
  local support = tonumber(output:match("SUPPORT_WINDOWS=([%+%-]?[%d%.]+)"))
  local total = tonumber(output:match("TOTAL_WINDOWS=([%+%-]?[%d%.]+)"))

  if not delayMs then return nil, "El analizador no devolvió DELAY_MS.\n\n" .. tostring(output) end

  -- Positive delay = SOURCE ocurre después de MASTER.
  -- Para adelantar el contenido del SOURCE dentro de su item,
  -- aumentamos D_STARTOFFS. MASTER permanece intacto.
  local correctionSeconds = (delayMs / 1000.0) * sourceRate / masterRate
  local targetOffs = sourceOffs + correctionSeconds

  return {
    item = sourceItem_,
    take = sourceTake,
    sourceName = item_name(sourceItem_),
    sourceOffs = sourceOffs,
    targetOffs = targetOffs,
    correctionSamples = correctionSeconds * sourceRate,
    delayMs = delayMs,
    delaySamples = delaySamples or (delayMs * masterRate / 1000.0),
    confidence = confidence or 0,
    correlation = correlation or 0,
    support = support or 0,
    total = total or 0,
    commonStart = commonStart,
  }
end

local function analyze_selection()
  refresh_selection()
  results = {}
  analyzed = false
  applied = false

  if #items < 2 then
    set_status("Seleccioná al menos 2 items.", "error")
    return
  end
  if not masterItem then
    set_status("No hay MASTER seleccionado.", "error")
    return
  end

  local sourceCount = 0
  local low = 0
  for _, item in ipairs(items) do
    if item ~= masterItem then
      sourceCount = sourceCount + 1
      local r, err = analyze_pair(masterItem, item)
      if not r then
        set_status(string.format("SOURCE #%d: %s", sourceCount, tostring(err)), "error")
        results = {}
        return
      end
      if r.confidence < MIN_CONFIDENCE then low = low + 1 end
      results[#results+1] = r
    end
  end

  analyzed = true
  set_status(string.format("MASTER → %d SOURCE(s). Residual medido exclusivamente contra MASTER.", sourceCount), low > 0 and "warn" or "ok")
end

local function apply_results()
  if not analyzed or #results == 0 then
    set_status("Primero ejecutá ANALYZE.", "warn")
    return
  end

  local low = 0
  for _,r in ipairs(results) do if r.confidence < MIN_CONFIDENCE then low = low + 1 end end
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
  local totalCorrections = 0
  local convergedAll = true
  local verification = {}

  for _,r in ipairs(results) do
    local current = r.sourceOffs
    local finalDelay = nil
    local finalConf = 0
    local passes = 0

    for pass=1,MAX_PASSES do
      local fresh, err = analyze_pair(masterItem, r.item)
      if not fresh then
        reaper.Undo_EndBlock("Smart Align Post - STATIC MULTI APPLY", -1)
        set_status("Error verificando " .. r.sourceName .. ": " .. tostring(err), "error")
        return
      end

      finalDelay = fresh.delaySamples
      finalConf = fresh.confidence
      passes = pass

      if math.abs(finalDelay) <= CONVERGED_SAMPLES then
        break
      end

      -- Recalcular desde el STARTOFFS ACTUAL, siempre MASTER -> SOURCE.
      local deltaSeconds = (fresh.delayMs / 1000.0) *
                           reaper.GetMediaItemTakeInfo_Value(fresh.take, "D_PLAYRATE") /
                           reaper.GetMediaItemTakeInfo_Value(reaper.GetActiveTake(masterItem), "D_PLAYRATE")
      current = reaper.GetMediaItemTakeInfo_Value(fresh.take, "D_STARTOFFS") + deltaSeconds
      reaper.SetMediaItemTakeInfo_Value(fresh.take, "D_STARTOFFS", current)
      reaper.UpdateItemInProject(fresh.item)
      totalCorrections = totalCorrections + math.abs(deltaSeconds * reaper.GetMediaItemTakeInfo_Value(fresh.take, "D_PLAYRATE"))
    end

    local verify, err = analyze_pair(masterItem, r.item)
    if not verify then
      reaper.Undo_EndBlock("Smart Align Post - STATIC MULTI APPLY", -1)
      set_status("Error de verificación en " .. r.sourceName .. ": " .. tostring(err), "error")
      return
    end

    local finalSamples = verify.delaySamples or 0
    local ok = math.abs(finalSamples) <= CONVERGED_SAMPLES
    if not ok then convergedAll = false end
    verification[#verification+1] = {
      name = r.sourceName,
      residual = finalSamples,
      confidence = verify.confidence,
      passes = passes,
      ok = ok,
    }
  end

  reaper.UpdateArrange()
  reaper.Undo_EndBlock("Smart Align Post - STATIC MULTI APPLY", -1)

  applied = true
  local bad = 0
  for _,v in ipairs(verification) do if not v.ok then bad = bad + 1 end end
  if bad == 0 then
    set_status(string.format("APLICADO Y VERIFICADO: %d SOURCE(s) en fase con MASTER. Residual ≤ %.2f samples.", #verification, CONVERGED_SAMPLES), "ok")
  else
    set_status(string.format("Aplicado, pero %d SOURCE(s) no convergieron a MASTER dentro de %.2f samples.", bad, CONVERGED_SAMPLES), "warn")
  end
  results._verification = verification
end

local function draw_button(x,y,w,h,label,enabled,primary)
  local hover = enabled and inside(x,y,w,h,gfx.mouse_x,gfx.mouse_y)
  if not enabled then rect(x,y,w,h,55,55,60)
  elseif primary then rect(x,y,w,h,hover and 70 or 52,hover and 155 or 125,hover and 245 or 210)
  else rect(x,y,w,h,hover and 78 or 64,hover and 78 or 64,hover and 85 or 70) end
  local tw = gfx.measurestr(label)
  text(x+(w-tw)/2,y+10,label,16,enabled and 245 or 135,enabled and 245 or 135,enabled and 250 or 135)
end

local function draw_ui()
  rect(0,0,gfx.w,gfx.h,24,25,29)
  text(24,18,"SMART ALIGN POST",25,245,245,250)
  text(24,49,"STATIC · MULTI SOURCE v5",15,160,170,185)
  text(24,73,"Todas las SOURCES → MASTER · MASTER nunca se modifica",14,195,200,210)
  text(24,94,"Click en una fila para definir el MASTER",13,145,155,170)

  text(650,24,"Seleccionados: " .. #items,15,205,210,220)
  text(650,48,"MASTER: " .. (masterItem and item_name(masterItem) or "—"),14,160,175,185)

  local tableY = 122
  rect(18,tableY,gfx.w-36,32,45,47,53)
  text(30,tableY+8,"ROLE",14,190,195,205)
  text(95,tableY+8,"ITEM",14,190,195,205)
  text(395,tableY+8,"RESIDUAL",14,190,195,205)
  text(500,tableY+8,"CORR SAMPLES",14,190,195,205)
  text(635,tableY+8,"CONF",14,190,195,205)
  text(705,tableY+8,"VERIFY",14,190,195,205)

  local rowY = tableY + 32
  for idx,item in ipairs(items) do
    if rowY > gfx.h-110 then break end
    local isMaster = item == masterItem
    local bg = isMaster and 54 or ((idx%2==0) and 34 or 30)
    rect(18,rowY,gfx.w-36,34,bg,36,42)
    text(30,rowY+8,isMaster and "MASTER" or "SOURCE",14,isMaster and 120 or 210,isMaster and 220 or 215,isMaster and 150 or 225)
    text(95,rowY+8,item_name(item),14,235,235,240)

    if not isMaster then
      local r = nil
      for _,rr in ipairs(results) do if rr.item == item then r = rr break end end
      if r then
        text(395,rowY+8,string.format("%+.3f ms",r.delayMs),14,225,230,235)
        text(500,rowY+8,string.format("%+.2f",r.correctionSamples),14,225,230,235)
        text(635,rowY+8,string.format("%.3f",r.confidence or 0),14,(r.confidence or 0)>=MIN_CONFIDENCE and 120 or 235,(r.confidence or 0)>=MIN_CONFIDENCE and 220 or 170,(r.confidence or 0)>=MIN_CONFIDENCE and 150 or 130)
      end
      local v = results._verification
      if applied and v then
        for _,vv in ipairs(v) do
          if vv.name == item_name(item) then
            text(705,rowY+8,string.format("%+.2f",vv.residual),14,vv.ok and 120 or 245,vv.ok and 220 or 150,vv.ok and 150 or 120)
          end
        end
      end
    end
    rowY = rowY + 34
  end

  local footerY = gfx.h-72
  local kr,kg,kb = 180,190,205
  if statusKind=="ok" then kr,kg,kb=120,220,150 end
  if statusKind=="warn" then kr,kg,kb=240,200,110 end
  if statusKind=="error" then kr,kg,kb=245,120,120 end
  text(24,footerY-18,status,13,kr,kg,kb)
  draw_button(18,footerY+8,150,38,"ANALYZE",#items>=2,true)
  draw_button(180,footerY+8,150,38,"APPLY",analyzed and #results>0,true)
  draw_button(gfx.w-168,footerY+8,150,38,"CLOSE",true,false)
end

local function handle_mouse()
  local down = gfx.mouse_cap & 1 == 1
  if down and not lastMouseDown then
    local tableY = 122
    local rowY = tableY + 32
    for _,item in ipairs(items) do
      if rowY > gfx.h-110 then break end
      if inside(18,rowY,gfx.w-36,34,gfx.mouse_x,gfx.mouse_y) then
        masterItem = item
        results = {}
        analyzed = false
        applied = false
        set_status("MASTER definido: " .. item_name(item) .. ". Ahora ejecutá ANALYZE.", "info")
        lastMouseDown = down
        return false
      end
      rowY = rowY + 34
    end

    local footerY = gfx.h-72
    if inside(18,footerY+8,150,38,gfx.mouse_x,gfx.mouse_y) then
      analyze_selection()
    elseif inside(180,footerY+8,150,38,gfx.mouse_x,gfx.mouse_y) then
      apply_results()
    elseif inside(gfx.w-168,footerY+8,150,38,gfx.mouse_x,gfx.mouse_y) then
      gfx.quit()
      return true
    end
  end
  lastMouseDown = down
  return false
end

refresh_selection()
gfx.init("Smart Align Post — STATIC MULTI v5",WIN_W,WIN_H)
gfx.clear = 24 + 25 * 256 + 29 * 65536
gfx.setfont(1,"Arial",16)
draw_ui()
gfx.update()

local function loop()
  if gfx.getchar() < 0 then return end
  if handle_mouse() then return end
  draw_ui()
  gfx.update()
  reaper.defer(loop)
end

reaper.defer(loop)
