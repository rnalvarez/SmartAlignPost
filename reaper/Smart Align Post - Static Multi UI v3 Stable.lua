-- Smart Align Post - STATIC MULTI UI v3 Stable
-- Primera selección = MASTER. Siguientes = SOURCES.
-- Esta versión restaura el método que venía funcionando:
-- analiza cada WAV desde su D_STARTOFFS y corrige D_STARTOFFS.
-- D_POSITION nunca se modifica.

local MIN_CONFIDENCE = 0.80
local WIN_W, WIN_H = 780, 520

local function script_dir()
  local src = debug.getinfo(1, "S").source
  if src:sub(1,1) == "@" then src = src:sub(2) end
  return src:match("^(.*)[/\\][^/\\]+$") or "."
end

local function quote(s)
  return '"' .. tostring(s):gsub('"', '\\"') .. '"'
end

local function source_info(item)
  local take = reaper.GetActiveTake(item)
  if not take then return nil, nil, "sin take" end
  local src = reaper.GetMediaItemTake_Source(take)
  if not src then return nil, take, "sin source" end
  local path = reaper.GetMediaSourceFileName(src, "")
  if not path or path == "" then return nil, take, "source sin archivo" end
  return path, take
end

local function rgb(r,g,b) gfx.set(r/255,g/255,b/255,1) end
local function rect(x,y,w,h,r,g,b) rgb(r,g,b); gfx.rect(x,y,w,h,1) end
local function text(x,y,s,size,r,g,b)
  rgb(r or 230,g or 230,b or 230)
  gfx.setfont(1,"Arial",size or 16)
  gfx.x,gfx.y=x,y
  gfx.drawstr(tostring(s))
end
local function inside(x,y,w,h,mx,my)
  return mx>=x and mx<=x+w and my>=y and my<=y+h
end

local results = {}
local analyzed = false
local applied = false
local status = "Esperando selección de REAPER."
local statusKind = "info"
local lastMouseDown = false

local function set_status(msg,kind)
  status = msg or ""
  statusKind = kind or "info"
end

local function fail(msg)
  results = {}
  analyzed = false
  applied = false
  set_status(msg,"error")
end

local function parse_process_output(p)
  if not p then return nil,nil,"ExecProcess falló." end
  p = p:gsub("\r\n","\n"):gsub("\r","\n")
  local eol = p:find("\n",1,true)
  if not eol then return tonumber(p:match("^%s*(%-?%d+)%s*$")),"",p end
  local code = tonumber((p:sub(1,eol-1)):match("^%s*(%-?%d+)%s*$"))
  return code,p:sub(eol+1),p
end

local function analyze_selection()
  results = {}
  analyzed = false
  applied = false

  local n = reaper.CountSelectedMediaItems(0)
  if n < 2 then
    fail("Seleccioná al menos 2 items: MASTER primero + uno o más SOURCES.")
    return
  end

  local masterItem = reaper.GetSelectedMediaItem(0,0)
  local masterPath, masterTake, masterErr = source_info(masterItem)
  if not masterPath then fail("MASTER: "..tostring(masterErr)); return end

  local masterPos = reaper.GetMediaItemInfo_Value(masterItem,"D_POSITION")
  local masterOffs = reaper.GetMediaItemTakeInfo_Value(masterTake,"D_STARTOFFS")
  local masterRate = reaper.GetMediaItemTakeInfo_Value(masterTake,"D_PLAYRATE")
  if masterRate <= 0 then fail("PLAYRATE inválido en MASTER."); return end

  local exe = script_dir().."\\SmartAlignPostPrototype.exe"
  local low = 0

  for i=1,n-1 do
    local sourceItem = reaper.GetSelectedMediaItem(0,i)
    local sourcePath, sourceTake, sourceErr = source_info(sourceItem)
    if not sourcePath then fail("SOURCE #"..i..": "..tostring(sourceErr)); return end

    local sourcePos = reaper.GetMediaItemInfo_Value(sourceItem,"D_POSITION")
    local sourceOffs = reaper.GetMediaItemTakeInfo_Value(sourceTake,"D_STARTOFFS")
    local sourceRate = reaper.GetMediaItemTakeInfo_Value(sourceTake,"D_PLAYRATE")
    if sourceRate <= 0 then fail("PLAYRATE inválido en SOURCE #"..i.."."); return end

    -- Importante: vuelve al contexto de análisis que estaba funcionando.
    -- El WAV se analiza desde D_STARTOFFS; la posición relativa del item
    -- se incorpora solamente al TARGET D_STARTOFFS.
    local cmd = quote(exe).." "..quote(masterPath).." "..quote(sourcePath)
    local code, output, normalized = parse_process_output(reaper.ExecProcess(cmd,60000))
    if code ~= 0 then
      fail("El analizador falló en SOURCE #"..i..".\n\n"..tostring(normalized))
      return
    end

    local delayMs = tonumber(output:match("DELAY_MS=([%+%-]?[%d%.]+)"))
    local delaySamples = tonumber(output:match("DELAY_SAMPLES=([%+%-]?[%d%.]+)"))
    local confidence = tonumber(output:match("CONFIDENCE=([%+%-]?[%d%.]+)"))
    local correlation = tonumber(output:match("CORRELATION=([%+%-]?[%d%.]+)"))
    local support = tonumber(output:match("SUPPORT_WINDOWS=([%+%-]?[%d%.]+)"))
    local total = tonumber(output:match("TOTAL_WINDOWS=([%+%-]?[%d%.]+)"))
    if not delayMs then
      fail("SOURCE #"..i.." no devolvió DELAY_MS.\n\n"..tostring(output))
      return
    end

    local targetOffs = masterOffs
      + ((sourcePos-masterPos)*sourceRate)
      + ((delayMs/1000.0)*sourceRate/masterRate)
    local deltaSamples = (targetOffs-sourceOffs)*sourceRate

    if not confidence or confidence < MIN_CONFIDENCE then low=low+1 end

    results[#results+1] = {
      index=i,item=sourceItem,take=sourceTake,
      sourcePos=sourcePos,sourceOffs=sourceOffs,
      targetOffs=targetOffs,deltaSamples=deltaSamples,
      delayMs=delayMs,delaySamples=delaySamples,
      confidence=confidence,correlation=correlation,
      support=support,total=total
    }
  end

  analyzed=true
  set_status(string.format("Analizados %d SOURCE(s). %d con confidence < %.2f.",#results,low,MIN_CONFIDENCE),low>0 and "warn" or "ok")
end

local function apply_results()
  if not analyzed or #results==0 then set_status("Primero ejecutá ANALYZE.","warn"); return end

  local low=0
  for _,r in ipairs(results) do
    if not r.confidence or r.confidence<MIN_CONFIDENCE then low=low+1 end
  end
  if low>0 then
    local answer = reaper.ShowMessageBox(string.format("Hay %d SOURCE(s) con confidence menor a %.2f.\n\n¿Aplicar igualmente?",low,MIN_CONFIDENCE),"Smart Align Post — CONFIDENCE",4)
    if answer~=6 then set_status("APPLY cancelado.","info"); return end
  end

  reaper.Undo_BeginBlock()
  local failures=0
  local originalPositions={}

  for _,r in ipairs(results) do
    originalPositions[r]=reaper.GetMediaItemInfo_Value(r.item,"D_POSITION")
    reaper.SetMediaItemTakeInfo_Value(r.take,"D_STARTOFFS",r.targetOffs)
    reaper.UpdateItemInProject(r.item)
  end
  reaper.UpdateArrange()

  for _,r in ipairs(results) do
    local afterOffs=reaper.GetMediaItemTakeInfo_Value(r.take,"D_STARTOFFS")
    local afterPos=reaper.GetMediaItemInfo_Value(r.item,"D_POSITION")
    if math.abs(afterOffs-r.targetOffs)>1e-8 or math.abs(afterPos-originalPositions[r])>1e-8 then failures=failures+1 end
  end

  reaper.Undo_EndBlock("Smart Align Post - STATIC MULTI APPLY",-1)

  if failures>0 then set_status(string.format("APPLY: %d SOURCE(s) con error de verificación.",failures),"error"); return end
  applied=true
  set_status(string.format("Aplicado: %d SOURCE(s). D_POSITION intacto. Undo disponible.",#results),"ok")
end

local function draw_button(x,y,w,h,label,enabled,primary)
  local hover=enabled and inside(x,y,w,h,gfx.mouse_x,gfx.mouse_y)
  if not enabled then rect(x,y,w,h,55,55,60)
  elseif primary then rect(x,y,w,h,hover and 70 or 52,hover and 155 or 125,hover and 245 or 210)
  else rect(x,y,w,h,hover and 78 or 64,hover and 78 or 64,hover and 85 or 70) end
  local tw=gfx.measurestr(label)
  text(x+(w-tw)/2,y+10,label,16,enabled and 245 or 135,enabled and 245 or 135,enabled and 250 or 135)
end

local function draw_ui()
  rect(0,0,gfx.w,gfx.h,24,25,29)
  text(24,18,"SMART ALIGN POST",25,245,245,250)
  text(24,49,"STATIC · MULTI SOURCE",15,160,170,185)
  text(24,73,"MASTER = primer item seleccionado",14,195,200,210)
  text(24,92,"Método estable: análisis desde D_STARTOFFS · corrección en D_STARTOFFS",13,145,155,170)

  local n=reaper.CountSelectedMediaItems(0)
  text(585,24,"Seleccionados: "..n,15,205,210,220)

  local tableY=125
  rect(18,tableY,gfx.w-36,32,45,47,53)
  text(30,tableY+8,"SOURCE",14,190,195,205)
  text(145,tableY+8,"DELAY",14,190,195,205)
  text(245,tableY+8,"Δ SAMPLES",14,190,195,205)
  text(385,tableY+8,"CONF",14,190,195,205)
  text(455,tableY+8,"CORR",14,190,195,205)
  text(545,tableY+8,"SUPPORT",14,190,195,205)
  text(700,tableY+8,"STATE",14,190,195,205)

  local y=tableY+32
  for i,r in ipairs(results) do
    if y>gfx.h-105 then break end
    local conf=r.confidence or 0
    local good=conf>=MIN_CONFIDENCE
    rect(18,y,gfx.w-36,34,(i%2==0) and 34 or 30,34,39)
    text(30,y+8,"SOURCE "..r.index,14,235,235,240)
    text(145,y+8,string.format("%+.3f ms",r.delayMs),14,225,230,235)
    text(245,y+8,string.format("%+.2f",r.deltaSamples),14,225,230,235)
    text(385,y+8,string.format("%.3f",conf),14,good and 120 or 235,good and 220 or 170,good and 150 or 130)
    text(455,y+8,string.format("%.4f",r.correlation or 0),14,210,215,225)
    text(545,y+8,string.format("%d/%d",r.support or 0,r.total or 0),14,210,215,225)
    text(700,y+8,applied and "APPLIED" or (good and "READY" or "CHECK"),14,applied and 120 or (good and 145 or 235),applied and 220 or (good and 205 or 170),applied and 155 or (good and 235 or 130))
    y=y+34
  end

  local footerY=gfx.h-72
  local r,g,b=180,190,205
  if statusKind=="ok" then r,g,b=120,220,150 end
  if statusKind=="warn" then r,g,b=240,200,110 end
  if statusKind=="error" then r,g,b=245,120,120 end
  text(24,footerY-18,status,13,r,g,b)
  draw_button(18,footerY+8,150,38,"ANALYZE",n>=2,true)
  draw_button(180,footerY+8,150,38,"APPLY",analyzed and #results>0,true)
  draw_button(gfx.w-168,footerY+8,150,38,"CLOSE",true,false)
end

local function loop()
  if gfx.getchar()<0 then return end
  local down=gfx.mouse_cap&1==1
  if down and not lastMouseDown then
    local footerY=gfx.h-72
    if inside(18,footerY+8,150,38,gfx.mouse_x,gfx.mouse_y) then analyze_selection()
    elseif inside(180,footerY+8,150,38,gfx.mouse_x,gfx.mouse_y) then apply_results()
    elseif inside(gfx.w-168,footerY+8,150,38,gfx.mouse_x,gfx.mouse_y) then gfx.quit(); return end
  end
  lastMouseDown=down
  draw_ui()
  gfx.update()
  reaper.defer(loop)
end

gfx.init("Smart Align Post — STATIC MULTI v3 STABLE",WIN_W,WIN_H)
gfx.clear=24+25*256+29*65536
gfx.setfont(1,"Arial",16)
reaper.defer(loop)
