-- Smart Align Post - DYNAMIC MULTI UI v2
-- MASTER = única referencia. Nunca se alinea una SOURCE contra otra SOURCE.
-- ANALYZE: calcula delay dinámico MASTER -> cada SOURCE.
-- APPLY: convierte la curva delay(t) en time-warp mediante stretch markers
-- de REAPER. DYNAMIC ya no intenta resolver cambios temporales variables
-- únicamente con D_STARTOFFS.
-- D_POSITION nunca se modifica.

local MIN_CONFIDENCE = 0.80
-- Procesamos en bloques cortos para que REAPER no quede esperando más de 120 s
-- aunque el equipo/archivo real sea mucho más lento que el smoke test de CI.
-- El resultado final se consolida entre bloques; MASTER siempre es la referencia.
local CHUNK_SEC = 5.0
local CHUNK_OVERLAP_SEC = 1.0
local CURVE_SKIP_START_SEC = 0.05
-- Keep the temporal control grid dense enough to follow real motion of the
-- SOURCE microphone. The analyzer now measures at 10 ms, so collapsing to
-- 40 ms was unnecessarily low-pass filtering the physical delay trajectory.
local CONSOLIDATE_MAX_SEC = 0.02
local CONSOLIDATE_MIN_DELTA_SAMPLES = 0.10
-- Ganancia adicional aplicada SOLO a la variación de delay durante el
-- time-warp. 1.0 = curva medida; valores mayores hacen que REAPER adapte
-- temporalmente el SOURCE con más decisión. El offset inicial permanece igual.
local DYNAMIC_WARP_GAIN = 1.0
local LANDMARK_WARP_VERSION = "LANDMARK WARP"
local WIN_W, WIN_H = 920, 600

local results = {}
local selectedCount = 0
local masterItem, masterTake, masterPath = nil, nil, nil
local masterPos, masterOffs, masterRate, masterLength = 0, 0, 1, 0
local status, statusKind = "Esperando selección.", "info"
local analyzed, applied = false, false
local lastMouseDown = false

local function script_dir()
  local src = debug.getinfo(1, "S").source
  if src:sub(1,1) == "@" then src = src:sub(2) end
  return src:match("^(.*)[/\\][^/\\]+$") or "."
end
local function quote(s) return '"' .. tostring(s):gsub('"', '\\"') .. '"' end
local function take_source_path(item)
  local take = reaper.GetActiveTake(item); if not take then return nil,nil,"sin take" end
  local src = reaper.GetMediaItemTake_Source(take); if not src then return nil,take,"sin source" end
  local path = reaper.GetMediaSourceFileName(src, ""); if not path or path=="" then return nil,take,"source sin archivo" end
  return path,take
end
local function set_status(msg,kind) status,statusKind=msg or "",kind or "info" end
local function fail(msg) results={}; analyzed=false; applied=false; set_status(msg,"error") end
local function parse_process_output(processResult)
  if not processResult then return nil,nil,"ExecProcess falló." end
  local n=processResult:gsub("\r\n","\n"):gsub("\r","\n"); local e=n:find("\n",1,true)
  if e then return tonumber((n:sub(1,e-1)):match("^%s*(%-?%d+)%s*$")),n:sub(e+1),n end
  return tonumber(n:match("^%s*(%-?%d+)%s*$")),"",n
end
local function run_chunk(masterFile,sourceFile,masterStart,sourceStart,duration,initialDelay)
  local exe=script_dir().."\\SmartAlignPostPrototype.exe"
  if reaper.file_exists and not reaper.file_exists(exe) then
    return nil,"No se encontró SmartAlignPostPrototype.exe en la misma carpeta del Lua.\\n\\nRuta buscada:\\n"..exe
  end
  local cmd=quote(exe).." "..quote(masterFile).." "..quote(sourceFile).." "..quote(string.format("%.12f",masterStart)).." "..quote(string.format("%.12f",sourceStart)).." "..quote(string.format("%.6f",duration))
  if initialDelay~=nil then cmd=cmd.." "..quote(string.format("%.6f",initialDelay)) end
  local code,out,norm=parse_process_output(reaper.ExecProcess(cmd,120000))
  if code~=0 then
    if code==259 then
      return nil,"timeout del analizador (>120 s) en un bloque de "..string.format("%.1f",duration).." s."
    end
    return nil,norm
  end
  local curve={}; for t,ms,d,c,k in out:gmatch("POINT=([%+%-]?[%d%.]+),([%+%-]?[%d%.]+),([%+%-]?[%d%.]+),([%+%-]?[%d%.]+),([01]?)") do curve[#curve+1]={time=tonumber(t),delayMs=tonumber(ms),delay=tonumber(d),confidence=tonumber(c),keyPoint=(k=="1")} end
  return {staticDelay=tonumber(out:match("DELAY_SAMPLES=([%+%-]?[%d%.]+)")) or 0,staticDelayMs=tonumber(out:match("DELAY_MS=([%+%-]?[%d%.]+)")) or 0,staticConfidence=tonumber(out:match("CONFIDENCE=([%+%-]?[%d%.]+)")) or 0,curve=curve},nil
end
local function consolidate(points)
  table.sort(points,function(a,b)
    if a.time~=b.time then return a.time<b.time end
    return (a.keyPoint and not b.keyPoint)
  end)

  local out,last={},nil
  for _,p in ipairs(points) do
    if (p.confidence or 0)>=MIN_CONFIDENCE then
      -- Key points are temporal anchors. Never average them with nearby
      -- ordinary measurements; they represent a locally refined acoustic
      -- event where the warp should have a control point.
      if p.keyPoint then
        local anchor={
          time=p.time,
          delay=p.delay,
          delayMs=p.delayMs,
          confidence=p.confidence,
          keyPoint=true
        }
        out[#out+1]=anchor
        last=anchor
      elseif not last then
        last={
          time=p.time,
          delay=p.delay,
          delayMs=p.delayMs,
          confidence=p.confidence,
          keyPoint=false
        }
        out[#out+1]=last
      elseif last.keyPoint then
        -- Keep the anchor isolated until the next ordinary point is far
        -- enough away to justify another control point.
        if p.time-last.time>=CONSOLIDATE_MAX_SEC then
          last={
            time=p.time,
            delay=p.delay,
            delayMs=p.delayMs,
            confidence=p.confidence,
            keyPoint=false
          }
          out[#out+1]=last
        end
      else
        local dt=p.time-last.time
        local dd=math.abs(p.delay-last.delay)
        if dt>=CONSOLIDATE_MAX_SEC or dd>=CONSOLIDATE_MIN_DELTA_SAMPLES then
          last={
            time=p.time,
            delay=p.delay,
            delayMs=p.delayMs,
            confidence=p.confidence,
            keyPoint=false
          }
          out[#out+1]=last
        else
          if p.confidence>last.confidence then last.confidence=p.confidence end
          last.time=p.time
          last.delay=(last.delay+p.delay)*0.5
          last.delayMs=(last.delayMs+p.delayMs)*0.5
        end
      end
    end
  end
  return out
end

local function analyze_source(item,index)
  local path,take,err=take_source_path(item); if not path then return nil,"SOURCE #"..index..": "..tostring(err) end
  local pos=reaper.GetMediaItemInfo_Value(item,"D_POSITION"); local len=reaper.GetMediaItemInfo_Value(item,"D_LENGTH"); local offs=reaper.GetMediaItemTakeInfo_Value(take,"D_STARTOFFS"); local rate=reaper.GetMediaItemTakeInfo_Value(take,"D_PLAYRATE")
  if rate<=0 then return nil,"PLAYRATE inválido en SOURCE #"..index end
  local commonStart=math.max(masterPos,pos); local commonEnd=math.min(masterPos+masterLength,pos+len); if commonEnd-commonStart<0.5 then return nil,string.format("SOURCE #%d: tramo común demasiado corto.",index) end
  local points={}; local chunkStart=commonStart; local first=true; local fallbackDelay,fallbackDelayMs,fallbackConf=0,0,0; local priorDelay=nil
  while chunkStart<commonEnd-0.05 do
    local duration=math.min(CHUNK_SEC,commonEnd-chunkStart); if duration<0.5 then break end
    local masterStart=masterOffs+(chunkStart-masterPos)*masterRate; local sourceStart=offs+(chunkStart-pos)*rate
    local a,e=run_chunk(masterPath,path,masterStart,sourceStart,duration,priorDelay); if not a then return nil,string.format("SOURCE #%d: %s",index,tostring(e)) end
    fallbackDelay,fallbackDelayMs,fallbackConf=a.staticDelay,a.staticDelayMs,a.staticConfidence
    if #a.curve>0 then priorDelay=a.curve[#a.curve].delay end
    for _,p in ipairs(a.curve) do local abs=chunkStart+p.time; if abs>=commonStart+((first and 0) or CURVE_SKIP_START_SEC) and abs<commonEnd-0.05 then points[#points+1]={time=abs,delay=p.delay,delayMs=p.delayMs,confidence=p.confidence,keyPoint=p.keyPoint==true} end end
    if commonEnd-chunkStart<=CHUNK_SEC+0.001 then break end; chunkStart=chunkStart+CHUNK_SEC-CHUNK_OVERLAP_SEC; first=false
  end
  local curve=consolidate(points); if #curve==0 then curve[1]={time=commonStart,delay=fallbackDelay,delayMs=fallbackDelayMs,confidence=fallbackConf,keyPoint=false} elseif curve[1].time>commonStart+1e-6 then table.insert(curve,1,{time=commonStart,delay=curve[1].delay,delayMs=curve[1].delayMs,confidence=curve[1].confidence,keyPoint=false}) end
  local minConf,maxAbs=1,0; for _,p in ipairs(curve) do minConf=math.min(minConf,p.confidence or 0); maxAbs=math.max(maxAbs,math.abs(p.delay or 0)) end
  return {index=index,item=item,take=take,sourcePos=pos,sourceOffs=offs,sourceRate=rate,commonStart=commonStart,commonEnd=commonEnd,curve=curve,pointCount=#curve,minConfidence=minConf,maxAbsDelay=maxAbs},nil
end
local function analyze_selection()
  results={}; analyzed=false; applied=false; selectedCount=reaper.CountSelectedMediaItems(0); if selectedCount<2 then fail("Seleccioná MASTER + al menos un SOURCE."); return end
  masterItem=reaper.GetSelectedMediaItem(0,0); masterPath,masterTake,masterErr=take_source_path(masterItem); if not masterPath then fail("MASTER: "..tostring(masterErr)); return end
  masterPos=reaper.GetMediaItemInfo_Value(masterItem,"D_POSITION"); masterLength=reaper.GetMediaItemInfo_Value(masterItem,"D_LENGTH"); masterOffs=reaper.GetMediaItemTakeInfo_Value(masterTake,"D_STARTOFFS"); masterRate=reaper.GetMediaItemTakeInfo_Value(masterTake,"D_PLAYRATE"); if masterRate<=0 then fail("PLAYRATE inválido en MASTER."); return end
  local low=0; for i=1,selectedCount-1 do local r,e=analyze_source(reaper.GetSelectedMediaItem(0,i),i); if not r then fail(e); return end; if r.minConfidence<MIN_CONFIDENCE then low=low+1 end; results[#results+1]=r end
  analyzed=true; set_status(string.format("LANDMARK WARP: %d SOURCE(s) analizados contra MASTER. %d con confidence < %.2f.",#results,low,MIN_CONFIDENCE),low>0 and "warn" or "ok")
end
local function set_absolute_correction(item,startPos,originalPos,originalOffs,rate,delayMs)
  local take=reaper.GetActiveTake(item); if not take then return false end
  -- delayMs is a TIME value; /1000 gives seconds, then *rate/masterRate
  -- accounts for playback-rate differences (same pattern as STATIC's
  -- apply step). Previously this divided a SAMPLE COUNT by D_PLAYRATE,
  -- which is not a samples->seconds conversion: at the typical playrate
  -- of 1.0 it shifted D_STARTOFFS by the raw sample count in SECONDS
  -- (e.g. a 56-sample correction became a 56s offset instead of ~1.2ms).
  local correctionSeconds=(delayMs/1000.0)*rate/masterRate
  local target=originalOffs+(startPos-originalPos)*rate+correctionSeconds; reaper.SetMediaItemTakeInfo_Value(take,"D_STARTOFFS",target); reaper.UpdateItemInProject(item)
  return math.abs(reaper.GetMediaItemTakeInfo_Value(take,"D_STARTOFFS")-target)<1e-7
end
local function apply_source(r)
  if r.minConfidence<MIN_CONFIDENCE then
    local ans=reaper.ShowMessageBox(
      string.format(
        "SOURCE %d tiene confidence mínima %.3f.\\n\\n¿Aplicar igualmente el time-warp DYNAMIC?",
        r.index,r.minConfidence
      ),
      "Smart Align Post — DYNAMIC CONFIDENCE",4
    )
    if ans~=6 then return 0,true end
  end

  local take = r.take
  if not take then return 0,false end

  -- DYNAMIC no usa splits + D_STARTOFFS. La curva delay(t) se convierte
  -- en una deformación temporal continua mediante stretch markers de REAPER.
  -- Cada marcador fija:
  --   pos    = posición temporal dentro del item
  --   srcpos = posición equivalente dentro del source, corregida por delay(t)
  --
  -- El item conserva D_POSITION y D_LENGTH. La compensación varía suavemente
  -- entre marcadores, que es exactamente lo que necesitamos para seguir un
  -- delay acústico que cambia durante la toma.
  local markerCount = reaper.GetTakeNumStretchMarkers(take)
  if markerCount and markerCount > 0 then
    reaper.DeleteTakeStretchMarkers(take, 0, markerCount)
  end

  -- Preservar pitch mientras REAPER realiza el time-warp.
  reaper.SetMediaItemTakeInfo_Value(take, "B_PPITCH", 1)

  local itemPos = r.sourcePos
  local itemLen = reaper.GetMediaItemInfo_Value(r.item, "D_LENGTH")
  local rate = r.sourceRate
  if rate <= 0 or itemLen <= 0 then return 0,false end

  local function correction_seconds(delayMs)
    return (delayMs / 1000.0)
  end

  local points = r.curve
  if #points == 0 then return 0,false end

  local inserted = 0
  local lastPos = -1e12
  local lastSrc = -1e12
  local anchorDelayMs = points[1].delayMs

  local function amplified_delay(delayMs)
    return anchorDelayMs +
      (delayMs - anchorDelayMs) * DYNAMIC_WARP_GAIN
  end

  local function add_marker(itemTime, delayMs)
    itemTime = math.max(0.0, math.min(itemLen, itemTime))
    local effectiveDelayMs = amplified_delay(delayMs)
    local correction = correction_seconds(effectiveDelayMs)
    local baselineSrc = r.sourceOffs + itemTime * rate
    local srcPos = baselineSrc + correction * rate

    -- El mapeo fuente debe ser estrictamente creciente. El control de
    -- maxSlew del motor debería garantizarlo, pero protegemos el APPLY
    -- contra una curva excepcional o ruido residual.
    if itemTime <= lastPos + 1e-7 then
      return true
    end
    if srcPos <= lastSrc + 1e-7 then
      srcPos = lastSrc + 1e-7
    end

    local idx = reaper.SetTakeStretchMarker(take, -1, itemTime, srcPos)
    if idx < 0 then return false end
    lastPos = itemTime
    lastSrc = srcPos
    inserted = inserted + 1
    return true
  end

  -- Boundary at the start: extend the first measured correction backwards
  -- so the whole item participates in the same mapping.
  local first = points[1]
  if not add_marker(0.0, first.delayMs) then return 0,false end

  -- Insert the complete measured curve. The curve is deliberately
  -- dense: moving the SOURCE changes the acoustic delay continuously, so
  -- REAPER needs temporal control points throughout the item rather than
  -- one global stretch ratio. Landmark points remain independent anchors.
  for _,p in ipairs(points) do
    local relative = p.time - r.sourcePos
    if relative > 0.001 and relative < itemLen - 0.001 then
      if not add_marker(relative, p.delayMs) then return inserted,false end
    end
  end

  -- Boundary at the end: hold the last measured correction through the tail.
  local last = points[#points]
  if itemLen > 0.001 then
    if not add_marker(itemLen, last.delayMs) then return inserted,false end
  end

  reaper.UpdateItemInProject(r.item)
  return inserted, inserted >= 2
end

local function apply_results()
  if not analyzed or #results==0 then set_status("Primero ejecutá ANALYZE.","warn"); return end
  reaper.Undo_BeginBlock(); local totalSplits,failures=0,0; local masterBefore=reaper.GetMediaItemInfo_Value(masterItem,"D_POSITION")
  for _,r in ipairs(results) do local s,ok=apply_source(r); totalSplits=totalSplits+s; if not ok then failures=failures+1 end end
  local masterAfter=reaper.GetMediaItemInfo_Value(masterItem,"D_POSITION"); reaper.UpdateArrange(); reaper.Undo_EndBlock("Smart Align Post - DYNAMIC MULTI APPLY",-1)
  if math.abs(masterAfter-masterBefore)>1e-8 then failures=failures+1 end
  if failures>0 then set_status(string.format("APPLY: %d error(es). MASTER permaneció protegido.",failures),"error"); return end
  applied=true; set_status(string.format("Aplicado contra MASTER: %d SOURCE(s), %d stretch markers. Warp x%.2f. D_POSITION intacto. Undo disponible.",#results,totalSplits,DYNAMIC_WARP_GAIN),"ok")
end
local function rgb(r,g,b) gfx.set(r/255,g/255,b/255,1) end
local function rect(x,y,w,h,r,g,b) rgb(r,g,b); gfx.rect(x,y,w,h,1) end
local function text(x,y,s,size,r,g,b) rgb(r or 230,g or 230,b or 230); gfx.setfont(1,"Arial",size or 16); gfx.x,gfx.y=x,y; gfx.drawstr(tostring(s)) end
local function draw_button(x,y,w,h,label,enabled,primary)
  local hover=enabled and gfx.mouse_x>=x and gfx.mouse_x<=x+w and gfx.mouse_y>=y and gfx.mouse_y<=y+h
  if not enabled then rect(x,y,w,h,55,55,60) elseif primary then rect(x,y,w,h,hover and 70 or 52,hover and 155 or 125,hover and 245 or 210) else rect(x,y,w,h,hover and 78 or 64,hover and 78 or 64,hover and 85 or 70) end
  local tw=gfx.measurestr(label); text(x+(w-tw)/2,y+10,label,16,enabled and 245 or 135,enabled and 245 or 135,enabled and 250 or 135)
end
local function draw_ui()
  rect(0,0,gfx.w,gfx.h,24,25,29); text(24,18,"SMART ALIGN POST",25,245,245,250); text(24,49,"DYNAMIC · MULTI SOURCE v2 · "..LANDMARK_WARP_VERSION,15,160,170,185); text(24,73,"MASTER = referencia absoluta · nunca se modifica",14,195,200,210); text(24,94,"Corrección dinámica MASTER → cada SOURCE mediante time-warp",13,145,155,170)
  local n=reaper.CountSelectedMediaItems(0); text(720,24,"Seleccionados: "..n,15,205,210,220); text(720,48,"MASTER: "..(masterItem and "OK" or "—"),14,160,175,185)
  local y=125; rect(18,y,gfx.w-36,32,45,47,53); text(28,y+8,"SOURCE",14,190,195,205); text(145,y+8,"PUNTOS",14,190,195,205); text(245,y+8,"MAX |DELAY|",14,190,195,205); text(400,y+8,"MIN CONF",14,190,195,205); text(535,y+8,"TRAMO",14,190,195,205); text(705,y+8,"STATE",14,190,195,205)
  local rowY=y+32; for i,r in ipairs(results) do if rowY>gfx.h-105 then break end; local good=r.minConfidence>=MIN_CONFIDENCE; rect(18,rowY,gfx.w-36,36,(i%2==0) and 34 or 30,34,39); text(28,rowY+9,"SOURCE "..r.index,14,235,235,240); text(145,rowY+9,tostring(r.pointCount),14,225,230,235); text(245,rowY+9,string.format("%.2f samp",r.maxAbsDelay),14,225,230,235); text(400,rowY+9,string.format("%.3f",r.minConfidence),14,good and 120 or 235,good and 220 or 170,good and 150 or 130); text(535,rowY+9,string.format("%.1f s",r.commonEnd-r.commonStart),14,210,215,225); text(705,rowY+9,applied and "APPLIED" or (good and "READY" or "CHECK"),14,applied and 120 or (good and 145 or 235),applied and 220 or (good and 205 or 170),applied and 155 or (good and 235 or 130)); rowY=rowY+36 end
  local fy=gfx.h-72; local rr,gg,bb=180,190,205; if statusKind=="ok" then rr,gg,bb=120,220,150 elseif statusKind=="warn" then rr,gg,bb=240,200,110 elseif statusKind=="error" then rr,gg,bb=245,120,120 end; text(24,fy-18,status,13,rr,gg,bb); draw_button(18,fy+8,165,38,"ANALYZE DYNAMIC",n>=2,true); draw_button(198,fy+8,165,38,"APPLY",analyzed and #results>0,true); draw_button(gfx.w-165,fy+8,147,38,"CLOSE",true,false)
end
local function handle_mouse()
  local down=gfx.mouse_cap & 1 == 1
  if down and not lastMouseDown then local fy=gfx.h-72; if gfx.mouse_x>=18 and gfx.mouse_x<=183 and gfx.mouse_y>=fy+8 and gfx.mouse_y<=fy+46 then analyze_selection() elseif gfx.mouse_x>=198 and gfx.mouse_x<=363 and gfx.mouse_y>=fy+8 and gfx.mouse_y<=fy+46 then apply_results() elseif gfx.mouse_x>=gfx.w-165 and gfx.mouse_y>=fy+8 then gfx.quit(); return true end end
  lastMouseDown=down; return false
end
local function loop() if gfx.getchar()<0 then return end; if handle_mouse() then return end; draw_ui(); gfx.update(); reaper.defer(loop) end
gfx.init("Smart Align Post — DYNAMIC MULTI v2",WIN_W,WIN_H); gfx.clear=24+25*256+29*65536; gfx.setfont(1,"Arial",16); draw_ui(); gfx.update(); reaper.defer(loop)
