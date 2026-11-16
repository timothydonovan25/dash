// The one page the update hotspot serves (see wifi_service.cpp). Plain
// HTML and a little script, no external files: the phone viewing it is on
// the dash's own network and has no internet.
namespace services {

extern const char kPage[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CRF250F Dash</title>
<style>
:root{--bg:#0d0e10;--card:#17191d;--line:#262a30;--txt:#e9eaec;--dim:#8b919a;--acc:#3d9cff;--red:#e5484d;--grn:#46c46e}
*{box-sizing:border-box}
body{font:16px/1.45 system-ui,-apple-system,sans-serif;margin:0 auto;padding:0 14px 40px;max-width:600px;background:var(--bg);color:var(--txt);-webkit-tap-highlight-color:transparent}
header{position:sticky;top:0;z-index:5;background:linear-gradient(var(--bg) 75%,transparent);padding:14px 0 10px;margin:0 -2px}
h1{font-size:21px;margin:0;letter-spacing:.3px}h1 b{color:var(--acc)}
h2{font-size:13px;margin:22px 2px 8px;color:var(--dim);text-transform:uppercase;letter-spacing:1px;font-weight:600}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:14px;margin-bottom:12px}
.row{display:flex;justify-content:space-between;align-items:center;gap:8px;padding:6px 0;border-bottom:1px solid var(--line)}
.row:last-child{border:0}.k{color:var(--dim)}a{color:var(--acc);text-decoration:none}
button{font:inherit;font-weight:600;width:100%;margin-top:10px;padding:11px;border:0;border-radius:10px;background:var(--acc);color:#fff;cursor:pointer}
button:active{filter:brightness(.85)}button:disabled{background:#2a2d33;color:#666;cursor:default}
button.danger{background:var(--red)}button.warn{background:#b8661b}
input[type=file]{font:inherit;width:100%;margin-top:10px;color:var(--dim)}
input[type=file]::file-selector-button{font:inherit;border:1px solid var(--line);background:#22252a;color:var(--txt);border-radius:8px;padding:7px 12px;margin-right:10px}
progress{width:100%;height:8px;margin-top:10px;accent-color:var(--acc)}
.msg{margin-top:8px;font-size:15px}.msg:empty{display:none}.bad{color:var(--red)}.good{color:var(--grn)}.dim{color:var(--dim)}
.row button,.log button{width:auto;margin:0;padding:6px 11px;font-size:13px;border-radius:7px;background:#2a2d33;color:var(--txt)}
.row button{background:#5a2a2c}
.set{display:flex;align-items:center;justify-content:space-between;gap:8px;padding:7px 0;border-bottom:1px solid var(--line)}
.set:last-child{border:0}.set input,.set select{font:inherit;width:120px;background:#0f1013;color:var(--txt);border:1px solid var(--line);border-radius:7px;padding:5px}
.set input[type=checkbox]{width:22px;height:22px}.u{color:var(--dim);font-size:13px;min-width:40px}
.ok{color:var(--grn)}.err{color:var(--red)}
details{border-bottom:1px solid var(--line)}details:last-child{border:0}
summary{cursor:pointer;padding:10px 2px;list-style:none;display:flex;align-items:center;gap:10px}
summary::-webkit-details-marker{display:none}
summary::after{content:'\203A';margin-left:auto;color:var(--dim);font-size:22px;line-height:1;transition:transform .15s}
details[open]>summary::after{transform:rotate(90deg)}
input[type=checkbox]{width:20px;height:20px;margin:0;accent-color:var(--acc);flex:none}
.bar{display:flex;align-items:center;gap:10px}.bar label{display:flex;align-items:center;gap:8px;font-weight:600}
.bar #lsel{margin-left:auto;font-size:14px}
.acts{display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px}.acts button{margin-top:10px;padding:10px 4px;font-size:14px}
.day summary .dn{font-weight:600}.day summary .dim{font-size:13px}
.log{display:flex;align-items:center;gap:10px;padding:8px 2px 8px 30px;border-top:1px solid #1f2227}
.log label{flex:1;display:flex;align-items:center;gap:10px;min-width:0}
.log .nm{font-variant-numeric:tabular-nums}.log .sz{color:var(--dim);font-size:13px;margin-left:auto}
.log .b{display:flex;gap:6px}
#logs{margin-top:8px}
#an canvas{width:100%;display:block;background:#111316;border-radius:8px;margin-top:8px;touch-action:none}
#an table{width:100%;border-collapse:collapse;font-size:13px;font-variant-numeric:tabular-nums;margin-top:8px}
#an th{color:var(--dim);font-weight:normal;text-align:right;padding:3px 4px}
#an td{text-align:right;padding:3px 4px;border-top:1px solid var(--line)}
#an td:first-child,#an th:first-child{text-align:left}
#an tr.best td:nth-child(2){color:var(--grn)}#an td.sb{color:#c8f}#an tr.dirty td{color:#777}
#an input[type=radio]{width:auto;margin:0;accent-color:var(--acc)}
.ka{color:#4af}.kb{color:#fa4}#anread{min-height:1.4em;font-size:14px;font-variant-numeric:tabular-nums}
</style></head><body>
<header><h1>CRF250F <b>Dash</b></h1></header>

<h2>Screens</h2>
<div class="card">
<div class="dim">Draw your own screens and arrange the modes: shapes, text and any
reading the dash has. Kept on the card in ui/; no card, and the dash shows its
built-in screens.</div>
<a href="/screens"><button>Open the screen editor</button></a>
</div>

<h2>Ride logs</h2>
<div class="card">
<div class="bar"><label><input type="checkbox" id="lall">Select all</label><span id="lsel" class="dim"></span></div>
<div class="acts"><button id="ldl" disabled>Download</button><button id="ldd" class="warn" disabled>Download &amp; delete</button><button id="ldel" class="danger" disabled>Delete</button></div>
<progress id="lprog" value="0" max="1" hidden></progress>
<div class="msg" id="lmsg"></div>
<div id="logs"><span class="dim">&hellip;</span></div>
</div>

<div id="anwrap" hidden>
<h2>Lap analysis</h2>
<div class="card" id="an">
<div id="anhead"></div>
<div class="dim">Pick two laps: <span class="ka">A</span> is the reference,
<span class="kb">B</span> is compared with it. Drag along the chart.</div>
<table id="antab"></table>
<canvas id="anmap" height="260"></canvas>
<canvas id="anplot" height="300"></canvas>
<div id="anread" class="dim"></div>
</div>
</div>

<div id="gameswrap" hidden>
<h2>Games</h2>
<div class="card">
<div class="dim">A Doom .wad for DIAG &rarr; Doom, kept in games/ on the card
(the shareware DOOM1.WAD, DOOM.WAD, DOOM2.WAD or Freedoom).</div>
<div id="games"></div>
<input type="file" id="gfile" accept=".wad,.WAD">
<button id="gup" disabled>Upload</button>
<progress id="gprog" value="0" max="100" hidden></progress>
<div class="msg" id="gmsg"></div>
</div>
</div>

<h2>Map</h2>
<div class="card">
<div class="row"><span>Routing map</span><span id="mapdate" class="dim">&hellip;</span></div>
<div class="dim">map.rgf (the roads) or search.idx (addresses and places for
Where to?), built by tools/routegraph (docs/navigation.md). Each replaces the
one on the card once all of it has arrived.</div>
<input type="file" id="mfile" accept=".rgf,.idx">
<button id="mup" disabled>Upload</button>
<progress id="mprog" value="0" max="100" hidden></progress>
<div class="msg" id="mmsg"></div>
</div>

<h2>Settings</h2>
<div class="card" id="settings"><span class="dim">&hellip;</span></div>

<h2>Firmware</h2>
<div class="card" id="fw">
<div class="row"><span class="k">Running</span><span id="run">&hellip;</span></div>
<div class="row"><span class="k">Dev slot</span><span id="dev">&hellip;</span></div>
<div class="dim">Writes dash.bin to the dev slot. The bike must be parked with the
engine off. Then on the dash: DIAG &rarr; Update &rarr; Boot dev once.</div>
<input type="file" id="file" accept=".bin">
<button id="up" disabled>Upload</button>
<progress id="prog" value="0" max="100" hidden></progress>
<div class="msg" id="upmsg"></div>
</div>

<h2>Firmware backup</h2>
<div class="card">
<div class="row"><a href="/fw/stable.bin">Stable</a><span class="dim">the image power-on boots</span></div>
<div class="row"><a href="/fw/dev.bin">Dev</a><span class="dim">the dev slot, if any</span></div>
<div class="dim">Saved as dash-&lt;version&gt;-&lt;date&gt;-&lt;board&gt;-&lt;feature&gt;.bin</div>
</div>

<script>
const $=id=>document.getElementById(id);
const kb=n=>n<1048576?(n/1024).toFixed(0)+' kB':(n/1048576).toFixed(1)+' MB';
async function status(){
  try{
    const s=await (await fetch('/api/status')).json();
    $('run').textContent=s.running+' '+s.running_date+' ('+s.role+', '+s.slot+')';
    $('dev').textContent=s.other?s.other+' '+s.other_date:'empty';
    if(s.games&&$('gameswrap').hidden){$('gameswrap').hidden=false;games()}
    const m=s.map;$('mapdate').textContent=m?'OSM '+String(m).replace(/(\d{4})(\d\d)(\d\d)/,'$1-$2-$3'):'none';
  }catch(e){}
}
async function games(){
  const el=$('games');
  try{
    const l=await (await fetch('/api/games')).json();
    el.innerHTML=l.length?l.map(f=>'<div class="row"><span>'+f.name+'</span><span class="dim">'+kb(f.size)+
      ' <button data-gdel="'+f.name+'">Delete</button></span></div>').join(''):
      '<div class="dim">No .wad on the card yet.</div>';
    el.querySelectorAll('[data-gdel]').forEach(b=>b.onclick=async()=>{
      const n=b.dataset.gdel;if(!confirm('Delete '+n+' from the card?'))return;
      b.disabled=true;
      let r={};try{r=await (await fetch('/games/'+n,{method:'DELETE'})).json()}catch(e){r={error:'connection lost'}}
      if(r.ok)games();else{alert('Not deleted: '+r.error);b.disabled=false}
    });
  }catch(e){el.innerHTML='<span class="bad">Could not list games.</span>'}
}
$('gfile').onchange=()=>{$('gup').disabled=!$('gfile').files.length;$('gmsg').textContent=''};
$('gup').onclick=()=>{
  const f=$('gfile').files[0];if(!f)return;
  const x=new XMLHttpRequest(),p=$('gprog'),m=$('gmsg');
  $('gup').disabled=true;p.hidden=false;p.value=0;m.className='msg dim';m.textContent='Sending…';
  x.upload.onprogress=e=>{if(e.lengthComputable)p.value=100*e.loaded/e.total;
    if(e.loaded===e.total)m.textContent='Writing to the card…'};
  x.onload=()=>{
    let r={};try{r=JSON.parse(x.responseText)}catch(e){r={ok:false,error:x.responseText||x.status}}
    m.className='msg '+(r.ok?'good':'bad');
    m.textContent=r.ok?'On the card. Start it from DIAG → Doom.':'Failed: '+r.error;
    $('gup').disabled=false;games();
  };
  x.onerror=()=>{m.className='msg bad';m.textContent='Connection lost.';$('gup').disabled=false};
  x.open('PUT','/games/'+encodeURIComponent(f.name));x.send(f);
};
$('mfile').onchange=()=>{$('mup').disabled=!$('mfile').files.length;$('mmsg').textContent=''};
$('mup').onclick=()=>{
  const f=$('mfile').files[0];if(!f)return;
  const x=new XMLHttpRequest(),p=$('mprog'),m=$('mmsg');
  $('mup').disabled=true;p.hidden=false;p.value=0;m.className='msg dim';m.textContent='Sending…';
  x.upload.onprogress=e=>{if(e.lengthComputable)p.value=100*e.loaded/e.total;
    if(e.loaded===e.total)m.textContent='Writing to the card…'};
  x.onload=()=>{
    let r={};try{r=JSON.parse(x.responseText)}catch(e){r={ok:false,error:x.responseText||x.status}}
    m.className='msg '+(r.ok?'good':'bad');
    m.textContent=r.ok?'On the card. NAV picks it up within a minute.':'Failed: '+r.error;
    $('mup').disabled=false;status();
  };
  x.onerror=()=>{m.className='msg bad';m.textContent='Connection lost.';$('mup').disabled=false};
  x.open('PUT',f.name.toLowerCase().endsWith('.idx')?'/route/search.idx':'/route/map.rgf');x.send(f);
};
// Ride logs, one fold per day (logs/2026-10-13_s007.csv), newest first.
// Ticked logs are fetched one at a time (the dash serves one request at a
// time), and more than one goes to the phone as a single .zip so the
// browser asks once. "Download & delete" deletes nothing until every byte
// of every log has arrived.
let LOGS=[];
const dayOf=n=>(/^(\d{4}-\d\d-\d\d)_/.exec(n)||[])[1]||'';
const plural=(n,w)=>n+' '+w+(n===1?'':'s');
function dayName(d){
  return d?new Date(d+'T12:00').toLocaleDateString(undefined,{weekday:'short',month:'short',day:'numeric',year:'numeric'}):'Undated';
}
async function logs(){
  const el=$('logs'),was=new Set([...el.querySelectorAll('details[open]')].map(d=>d.dataset.day));
  try{LOGS=await (await fetch('/api/logs')).json()}
  catch(e){el.innerHTML='<span class="bad">Could not list logs.</span>';return}
  const g={};
  LOGS.forEach(f=>(g[dayOf(f.name)]=g[dayOf(f.name)]||[]).push(f));
  const days=Object.keys(g).sort((a,b)=>!a?1:!b?-1:a<b?1:-1);    // undated last
  el.innerHTML=LOGS.length?days.map((d,i)=>{
    const l=g[d].sort((a,b)=>a.name<b.name?1:-1);
    return '<details class="day" data-day="'+d+'"'+((was.size?was.has(d):!i)?' open':'')+'><summary>'+
      '<input type="checkbox" class="dchk"><span class="dn">'+dayName(d)+'</span><span class="dim">'+
      plural(l.length,'ride')+' &middot; '+kb(l.reduce((s,f)=>s+f.size,0))+'</span></summary>'+
      l.map(f=>'<div class="log"><label><input type="checkbox" class="fchk" value="'+f.name+'">'+
        '<span class="nm">'+f.name.slice(d?11:0).replace('.csv','')+'</span><span class="sz">'+kb(f.size)+'</span></label>'+
        '<span class="b"><button data-laps="'+f.name+'">Laps</button><button data-gpx="'+f.name+'">GPX</button></span></div>').join('')+
      '</details>'}).join(''):'<span class="dim">No logs, or no card.</span>';
  el.querySelectorAll('[data-gpx]').forEach(b=>b.onclick=()=>gpx(b.dataset.gpx,b));
  el.querySelectorAll('[data-laps]').forEach(b=>b.onclick=()=>analyse(b.dataset.laps,b));
  picked();
}
const ticked=()=>[...$('logs').querySelectorAll('.fchk:checked')].map(c=>c.value);
function picked(){
  const n=ticked(),all=$('logs').querySelectorAll('.fchk');
  const sz=LOGS.filter(f=>n.includes(f.name)).reduce((s,f)=>s+f.size,0);
  $('lsel').textContent=n.length?plural(n.length,'log')+' · '+kb(sz):plural(all.length,'log');
  $('lall').checked=all.length>0&&n.length===all.length;$('lall').indeterminate=n.length>0&&n.length<all.length;
  $('logs').querySelectorAll('details').forEach(d=>{
    const c=d.querySelectorAll('.fchk'),k=d.querySelectorAll('.fchk:checked').length,h=d.querySelector('.dchk');
    h.checked=k===c.length;h.indeterminate=k>0&&k<c.length});
  ['ldl','ldd','ldel'].forEach(id=>$(id).disabled=!n.length||LOGS.busy);
}
$('logs').onchange=e=>{
  if(e.target.classList.contains('dchk'))
    e.target.closest('details').querySelectorAll('.fchk').forEach(c=>c.checked=e.target.checked);
  picked();
};
$('lall').onchange=()=>{$('logs').querySelectorAll('.fchk').forEach(c=>c.checked=$('lall').checked);picked()};
// A .zip with every file stored as it is: CSV is what the phone wants
// anyway, and storing needs only the CRC.
const CRC=new Uint32Array(256).map((_,n)=>{for(let k=0;k<8;k++)n=n&1?0xEDB88320^(n>>>1):n>>>1;return n});
const crc32=d=>{let c=~0;for(let i=0;i<d.length;i++)c=CRC[(c^d[i])&255]^(c>>>8);return~c>>>0};
const le=a=>{const b=new Uint8Array(a.reduce((s,x)=>s+x[1],0)),v=new DataView(b.buffer);let o=0;
  for(const[x,n]of a){n===4?v.setUint32(o,x,true):v.setUint16(o,x,true);o+=n}return b};
function zip(files){
  const out=[],cen=[];let off=0;
  for(const f of files){
    const nm=new TextEncoder().encode(f.name),c=crc32(f.data),z=f.data.length,m=/^(\d{4})-(\d\d)-(\d\d)/.exec(f.name);
    const dt=m?((+m[1]-1980)<<9)|(+m[2]<<5)|+m[3]:33;   // DOS date; 33 is 1980-01-01
    out.push(le([[0x04034b50,4],[20,2],[0,2],[0,2],[0,2],[dt,2],[c,4],[z,4],[z,4],[nm.length,2],[0,2]]),nm,f.data);
    cen.push(le([[0x02014b50,4],[20,2],[20,2],[0,2],[0,2],[0,2],[dt,2],[c,4],[z,4],[z,4],[nm.length,2],[0,2],[0,2],[0,2],[0,2],[0,4],[off,4]]),nm);
    off+=30+nm.length+z;
  }
  const cs=cen.reduce((s,b)=>s+b.length,0);
  return new Blob(out.concat(cen,[le([[0x06054b50,4],[0,2],[0,2],[files.length,2],[files.length,2],[cs,4],[off,4],[0,2]])]),{type:'application/zip'});
}
function save(blob,name){
  const a=document.createElement('a');a.href=URL.createObjectURL(blob);a.download=name;
  document.body.appendChild(a);a.click();a.remove();setTimeout(()=>URL.revokeObjectURL(a.href),60e3);
}
async function bulk(dl,del){
  const names=ticked(),m=$('lmsg'),p=$('lprog'),s=plural(names.length,'log');
  if(!names.length)return;
  if(del&&!confirm((dl?'Download, then delete ':'Delete ')+s+' from the card?'))return;
  LOGS.busy=true;picked();m.className='msg dim';p.hidden=false;p.value=0;
  const bad=[];
  try{
    if(dl){
      const files=[];
      for(const[i,n]of names.entries()){
        m.textContent='Fetching '+(i+1)+' of '+names.length+'…';
        const r=await fetch('/logs/'+n);if(!r.ok)throw n;
        files.push({name:n,data:new Uint8Array(await r.arrayBuffer())});p.value=(i+1)/names.length;
      }
      if(files.length===1)save(new Blob([files[0].data],{type:'text/csv'}),files[0].name);
      else save(zip(files),'dash-logs-'+new Date().toLocaleDateString('sv')+'.zip');
    }
    if(del)for(const[i,n]of names.entries()){
      m.textContent='Deleting '+(i+1)+' of '+names.length+'…';p.value=(i+1)/names.length;
      let r={};try{r=await (await fetch('/logs/'+n,{method:'DELETE'})).json()}catch(e){r={error:'connection lost'}}
      if(!r.ok)bad.push(n+' ('+r.error+')');
    }
    m.className='msg '+(bad.length?'bad':'good');
    m.textContent=bad.length?'Not deleted: '+bad.join(', '):(dl&&del?'Downloaded and deleted ':dl?'Downloaded ':'Deleted ')+s+'.';
  }catch(e){m.className='msg bad';m.textContent='Could not fetch '+e+'. Nothing was deleted.'}
  p.hidden=true;LOGS.busy=false;
  if(del)logs();else picked();
}
$('ldl').onclick=()=>bulk(true,false);
$('ldd').onclick=()=>bulk(true,true);
$('ldel').onclick=()=>bulk(false,true);
// A ride as a GPX track, for Gaia, Strava or Komoot. Converted here, on
// the phone, from the CSV the dash already serves: the dash does no work.
// One point per GPS fix (the log repeats a fix until the next one), only
// rows with a fix, and a new segment across a gap of more than 5 s.
//
// Times need the UTC date. The file is named by the *local* date it was
// started (logs/2026-10-13_s007.csv) and GpsUtc is seconds into the UTC day,
// so the UTC offset setting puts the two together. A log from before the
// GPS knew the date (sNNN.csv) is exported without times.
let tz=0;
async function gpx(n,b){
  const label=b.textContent;b.disabled=true;b.textContent='…';
  try{
    const t=await (await fetch('/logs/'+n)).text();
    const m=/^(\d{4})-(\d\d)-(\d\d)_/.exec(n);
    const base=m?Date.UTC(+m[1],+m[2]-1,+m[3]):null;
    let h=null,last=null,first=-1,prev=-1,day=0,off=0,segs=[],seg=[];
    for(const ln of t.split('\n')){
      if(!ln||ln[0]==='#')continue;
      const c=ln.split(',');
      if(!h){if(c[0]==='Time'){h={};c.forEach((k,i)=>h[k.trim()]=i)}continue}
      if(c.length<=h.GpsUtc||!(+c[h.Flags]&4))continue;    // kFlagFix; a cut-off row
      const u=+c[h.GpsUtc];if(!isFinite(u)||u===last)continue;last=u;
      if(first<0){first=u;const l=u+tz*3600;off=l<0?1:(l>=86400?-1:0)}
      if(prev>=0&&u<prev-43200)day++;                     // past UTC midnight
      if(prev>=0&&u-prev>5&&seg.length){segs.push(seg);seg=[]}
      prev=u;
      let p='<trkpt lat="'+c[h.Latitude]+'" lon="'+c[h.Longitude]+'">';
      if(h.AltFt!==undefined)p+='<ele>'+(+c[h.AltFt]/3.28084).toFixed(1)+'</ele>';
      if(base!==null)p+='<time>'+new Date(base+(off+day)*86400e3+u*1e3).toISOString()+'</time>';
      seg.push(p+'</trkpt>');
    }
    if(seg.length)segs.push(seg);
    if(!segs.length){alert('No GPS fixes in '+n+'.');return}
    const x='<?xml version="1.0" encoding="UTF-8"?>\n<gpx version="1.1" creator="CRF250F Dash" '+
      'xmlns="http://www.topografix.com/GPX/1/1"><trk><name>'+n.replace('.csv','')+'</name>'+
      segs.map(s=>'<trkseg>'+s.join('')+'</trkseg>').join('')+'</trk></gpx>\n';
    const a=document.createElement('a');
    a.href=URL.createObjectURL(new Blob([x],{type:'application/gpx+xml'}));
    a.download=n.replace('.csv','.gpx');document.body.appendChild(a);a.click();a.remove();
  }catch(e){alert('Could not export '+n+'.')}
  finally{b.disabled=false;b.textContent=label}
}
// --- Lap analysis --------------------------------------------------------
// The laps of a ride compared on the phone, from the CSV the dash already
// serves: a table with each lap's time and splits, both laps on a map, and
// speed and the time gained or lost against distance round the lap. The dash
// does no work for it.
//
// Laps come from the RaceRender tags the logger writes ("# Lap 3: 00:01:31.42",
// just before the row the lap ended on) and the rows the Timing flag is set
// on. Two laps are lined up by position, not by distance ridden: each point
// of lap B is matched to the nearest piece of lap A (A is the reference), so
// a wider line through one corner does not shift the rest of the lap. The
// splits are the reference lap's thirds, as the dash places its own.
const LAP_FLAG_FIX=4,LAP_FLAG_TIMING=8;
function lapsFrom(text){
  let h=null,cur=null,curLap=-1,pend=null;const laps=[],all=[];
  for(const ln of text.split('\n')){
    if(!ln)continue;
    if(ln[0]==='#'){
      const m=/^# Lap (\d+): (\d+):(\d+):(\d+)\.(\d+)/.exec(ln);
      if(m)pend={ms:((+m[2]*60+ +m[3])*60+ +m[4])*1000+ +m[5]*10};
      continue;
    }
    const c=ln.split(',');
    if(!h){if(c[0]==='Time'){h={};c.forEach((k,i)=>h[k.trim()]=i)}continue}
    if(c.length<=h.Flags)continue;                       // a row cut off by a power cut
    const f=+c[h.Flags];
    const p={t:+c[h.Time],lat:+c[h.Latitude],lon:+c[h.Longitude],mph:+c[h.MPH],
             rpm:+c[h.RPM],lean:+c[h.Lean],lt:+c[h.LapTime],lap:+c[h.Lap],fix:!!(f&LAP_FLAG_FIX)};
    if(p.fix)all.push(p);
    if(pend){
      // The row the lap ended on: the end of this lap, at its own lap time,
      // and the start of the next.
      if(cur&&cur.length)laps.push({ms:pend.ms,pts:cur.concat([Object.assign({},p,{lt:pend.ms/1000})])});
      cur=[Object.assign({},p,{lt:0})];curLap=p.lap+1;pend=null;continue;
    }
    if(!(f&LAP_FLAG_TIMING)){cur=null;curLap=-1;continue}
    if(p.lap!==curLap){cur=[];curLap=p.lap}
    cur.push(p);
  }
  if(!all.length)return{laps:[],all};
  const lat0=all[0].lat,lon0=all[0].lon,kx=111320*Math.cos(lat0*Math.PI/180),ky=110540;
  const xy=p=>{p.x=(p.lon-lon0)*kx;p.y=(p.lat-lat0)*ky};
  all.forEach(xy);
  laps.forEach((l,i)=>{
    l.n=i+1;
    l.pts=l.pts.filter(p=>p.fix);l.pts.forEach(xy);
    let d=0,top=0,lean=0,gap=0;
    l.pts.forEach((p,k)=>{
      if(k){const q=l.pts[k-1];d+=Math.hypot(p.x-q.x,p.y-q.y);gap=Math.max(gap,p.t-q.t)}
      p.d=d;top=Math.max(top,p.mph);lean=Math.max(lean,Math.abs(p.lean));
    });
    l.dist=d;l.top=top;l.lean=lean;
    // A hole in the fixes may hide a crossing nobody saw: shown, never best.
    l.clean=l.pts.length>2&&gap<=2.5;
  });
  return{laps:laps.filter(l=>l.pts.length>2),all};
}
// Where each point of `lap` lies along `ref`, in metres from the line, by
// the nearest piece of ref within a window ahead of the last match. Then
// lap's time and speed at every one of `n` stations along ref.
function alongRef(ref,lap,n){
  const R=ref.pts,s=[];let j=0;
  for(const p of lap.pts){
    let best=1e18,bs=0,bj=j;
    const lo=Math.max(0,j-5),hi=Math.min(R.length-2,j+80);
    for(let k=lo;k<=hi;k++){
      const a=R[k],b=R[k+1],vx=b.x-a.x,vy=b.y-a.y,l2=vx*vx+vy*vy;
      let u=l2>0?((p.x-a.x)*vx+(p.y-a.y)*vy)/l2:0;u=Math.max(0,Math.min(1,u));
      const dx=a.x+u*vx-p.x,dy=a.y+u*vy-p.y,e=dx*dx+dy*dy;
      if(e<best){best=e;bs=a.d+u*(b.d-a.d);bj=k}
    }
    // Off the reference line by more than this is somewhere else: the pits.
    if(best>60*60)continue;
    if(s.length&&bs<s[s.length-1].s)continue;              // never backwards
    j=bj;s.push({s:bs,t:p.lt,mph:p.mph});
  }
  const out=[];let k=0;
  for(let i=0;i<=n;i++){
    const at=ref.dist*i/n;
    while(k+1<s.length&&s[k+1].s<at)k++;
    if(!s.length||at<s[0].s-1||at>s[s.length-1].s+1){out.push(null);continue}
    const a=s[k],b=s[Math.min(k+1,s.length-1)],w=b.s>a.s?(at-a.s)/(b.s-a.s):0,f=Math.max(0,Math.min(1,w));
    out.push({s:at,t:a.t+f*(b.t-a.t),mph:a.mph+f*(b.mph-a.mph)});
  }
  return out;
}
// Each lap's three splits, at the reference lap's thirds.
function splitsOf(ref,lap){
  const st=alongRef(ref,lap,3);
  if(!st[1]||!st[2])return null;
  const t=lap.ms/1000;
  return[st[1].t,st[2].t-st[1].t,t-st[2].t];
}
function fmtLap(s){if(s==null||!isFinite(s))return'--';const m=Math.floor(s/60);return m+':'+(s-60*m).toFixed(2).padStart(5,'0')}
function fmtGap(s){const v=Math.abs(s)<0.005?0:s;return(v<0?'−':'+')+Math.abs(v).toFixed(2)}
let metric=false,AN=null;
async function analyse(n,b){
  const label=b.textContent;b.disabled=true;b.textContent='…';
  try{
    const r=lapsFrom(await (await fetch('/logs/'+n)).text());
    if(!r.laps.length){alert('No complete laps in '+n+'.');return}
    const clean=r.laps.filter(l=>l.clean);
    const best=(clean.length?clean:r.laps).reduce((a,l)=>l.ms<a.ms?l:a);
    const last=r.laps[r.laps.length-1];
    AN={name:n,laps:r.laps,all:r.all,best,a:best,b:last!==best?last:(r.laps.find(l=>l!==best)||best),cur:null};
    AN.laps.forEach(l=>l.sp=splitsOf(best,l));
    $('anwrap').hidden=false;
    drawAnalysis();
    $('anwrap').scrollIntoView({behavior:'smooth'});
  }catch(e){alert('Could not read '+n+'.')}
  finally{b.disabled=false;b.textContent=label}
}
function drawAnalysis(){
  const A=AN,ok=A.laps.filter(l=>l.clean&&l.sp);
  const sb=[0,1,2].map(k=>ok.length?Math.min(...ok.map(l=>l.sp[k])):null);
  const ideal=sb.every(v=>v!=null)?sb[0]+sb[1]+sb[2]:null;
  $('anhead').innerHTML=A.name.replace('.csv','')+' &middot; '+A.laps.length+' laps &middot; best <b>'+
    fmtLap(A.best.ms/1000)+'</b> (lap '+A.best.n+')'+(ideal?' &middot; ideal '+fmtLap(ideal):'');
  const sp=v=>metric?(v*1.609344).toFixed(0):v.toFixed(0);
  let t='<tr><th>Lap</th><th>Time</th><th>Gap</th><th>S1</th><th>S2</th><th>S3</th><th>Top</th><th>Lean</th><th class="ka">A</th><th class="kb">B</th></tr>';
  A.laps.forEach(l=>{
    const cls=(l===A.best?'best':'')+(l.clean?'':' dirty');
    t+='<tr class="'+cls+'"><td>'+l.n+(l.clean?'':'*')+'</td><td>'+fmtLap(l.ms/1000)+'</td><td>'+
      (l===A.best?'':fmtGap((l.ms-A.best.ms)/1000))+'</td>';
    for(let k=0;k<3;k++){
      const v=l.sp?l.sp[k]:null;
      t+='<td'+(v!=null&&l.clean&&v===sb[k]?' class="sb"':'')+'>'+(v!=null?v.toFixed(2):'--')+'</td>';
    }
    t+='<td>'+sp(l.top)+'</td><td>'+l.lean.toFixed(0)+'&deg;</td>'+
      '<td><input type="radio" name="la" value="'+l.n+'"'+(l===A.a?' checked':'')+'></td>'+
      '<td><input type="radio" name="lb" value="'+l.n+'"'+(l===A.b?' checked':'')+'></td></tr>';
  });
  if(A.laps.some(l=>!l.clean))t+='<tr><td colspan="10" class="dim" style="text-align:left">* the GPS dropped out in this lap: shown, never best</td></tr>';
  $('antab').innerHTML=t;
  $('antab').querySelectorAll('input').forEach(i=>i.onchange=()=>{
    const l=A.laps.find(x=>x.n==i.value);if(i.name==='la')A.a=l;else A.b=l;A.cur=null;drawAnalysis()});
  A.sa=alongRef(A.a,A.a,400);A.sbb=alongRef(A.a,A.b,400);
  drawMap();drawPlot();
}
function canvasFit(c){
  const r=window.devicePixelRatio||1,w=c.clientWidth,h=+c.getAttribute('height');
  c.width=w*r;c.height=h*r;c.style.height=h+'px';const g=c.getContext('2d');g.setTransform(r,0,0,r,0,0);return[g,w,h];
}
function drawMap(){
  const A=AN,[g,w,h]=canvasFit($('anmap'));
  let x0=1e9,x1=-1e9,y0=1e9,y1=-1e9;
  A.laps.forEach(l=>l.pts.forEach(p=>{x0=Math.min(x0,p.x);x1=Math.max(x1,p.x);y0=Math.min(y0,p.y);y1=Math.max(y1,p.y)}));
  const k=Math.min((w-20)/Math.max(1,x1-x0),(h-20)/Math.max(1,y1-y0));
  const ox=(w-k*(x1-x0))/2,oy=(h-k*(y1-y0))/2;
  const X=p=>ox+(p.x-x0)*k,Y=p=>h-oy-(p.y-y0)*k;
  const line=(pts,col,wd)=>{g.strokeStyle=col;g.lineWidth=wd;g.beginPath();pts.forEach((p,i)=>i?g.lineTo(X(p),Y(p)):g.moveTo(X(p),Y(p)));g.stroke()};
  g.lineJoin='round';
  A.laps.forEach(l=>line(l.pts,'#333',1));
  line(A.a.pts,'#4af',3);
  if(A.b!==A.a)line(A.b.pts,'#fa4',2);
  // The line and the two splits, on the reference lap.
  const at=s=>{const P=A.a.pts;let i=0;while(i+1<P.length&&P[i+1].d<s)i++;return P[i]};
  [0,A.a.dist/3,2*A.a.dist/3].forEach((s,i)=>{const p=at(s);g.fillStyle=i?'#c8f':'#fff';g.beginPath();g.arc(X(p),Y(p),i?4:5,0,7);g.fill()});
  if(A.cur!=null){const p=at(A.cur);g.strokeStyle='#fff';g.lineWidth=2;g.beginPath();g.arc(X(p),Y(p),7,0,7);g.stroke()}
}
function drawPlot(){
  const A=AN,c=$('anplot'),[g,w,h]=canvasFit(c);
  const top=h*0.6,L=34,R=w-6,sa=A.sa,sb=A.sbb;
  const xs=s=>L+(R-L)*s/A.a.dist;
  // The speeds ridden, not from zero: the corners are what is worth seeing.
  let vmax=10,vmin=1e9;sa.concat(sb).forEach(p=>{if(p){vmax=Math.max(vmax,p.mph);vmin=Math.min(vmin,p.mph)}});
  vmax=Math.ceil((vmax+1)/10)*10;vmin=Math.max(0,Math.floor((vmin-1)/10)*10);if(vmin>=vmax)vmin=0;
  const dt=sa.map((p,i)=>p&&sb[i]?sb[i].t-p.t:null);
  let dmax=0.5;dt.forEach(v=>{if(v!=null)dmax=Math.max(dmax,Math.abs(v))});
  const ys=v=>8+(top-16)*(1-(v-vmin)/(vmax-vmin)),yd=v=>top+(h-top)/2-((h-top)/2-6)*v/dmax;
  g.font='11px system-ui';g.fillStyle='#777';g.strokeStyle='#2a2a2a';g.lineWidth=1;
  for(let v=vmin;v<=vmax;v+=vmax-vmin>60?20:10){g.beginPath();g.moveTo(L,ys(v));g.lineTo(R,ys(v));g.stroke();g.fillText(metric?(v*1.609344).toFixed(0):v,4,ys(v)+4)}
  g.beginPath();g.moveTo(L,yd(0));g.lineTo(R,yd(0));g.stroke();
  g.fillText('+'+dmax.toFixed(1),4,yd(dmax)+8);g.fillText('−'+dmax.toFixed(1),4,yd(-dmax));
  [1,2].forEach(i=>{const x=xs(A.a.dist*i/3);g.strokeStyle='#4a3a5a';g.beginPath();g.moveTo(x,4);g.lineTo(x,h-4);g.stroke()});
  const trace=(st,f,y,col,wd)=>{g.strokeStyle=col;g.lineWidth=wd;g.beginPath();let on=false;
    st.forEach((p,i)=>{const v=f(p,i);if(v==null){on=false;return}const X=xs(sa[i]?sa[i].s:A.a.dist*i/(st.length-1));
      on?g.lineTo(X,y(v)):g.moveTo(X,y(v));on=true});g.stroke()};
  trace(sa,p=>p?p.mph:null,ys,'#4af',2);
  if(A.b!==A.a)trace(sb,p=>p?p.mph:null,ys,'#fa4',1.5);
  // Time against the reference: above the line B is behind, below ahead.
  if(A.b!==A.a){
    for(let i=1;i<dt.length;i++){if(dt[i]==null||dt[i-1]==null)continue;
      g.strokeStyle=dt[i]>dt[i-1]?'#f66':'#6d6';g.lineWidth=2;g.beginPath();
      g.moveTo(xs(A.a.dist*(i-1)/(dt.length-1)),yd(dt[i-1]));g.lineTo(xs(A.a.dist*i/(dt.length-1)),yd(dt[i]));g.stroke()}
  }
  g.fillStyle='#777';g.fillText(metric?'km/h':'mph',R-34,22);g.fillText('B vs A, s',R-58,top+16);
  if(A.cur!=null){const x=xs(A.cur);g.strokeStyle='#fff';g.lineWidth=1;g.beginPath();g.moveTo(x,4);g.lineTo(x,h-4);g.stroke()}
}
function anPoint(e){
  if(!AN)return;const c=$('anplot'),r=c.getBoundingClientRect(),L=34,R=r.width-6;
  const f=Math.max(0,Math.min(1,(e.clientX-r.left-L)/(R-L)));
  const i=Math.round(f*(AN.sa.length-1)),a=AN.sa[i],b=AN.sbb[i];
  AN.cur=AN.a.dist*f;drawMap();drawPlot();
  const d=metric?(AN.cur).toFixed(0)+' m':(AN.cur*3.28084).toFixed(0)+' ft';
  const v=p=>p?(metric?(p.mph*1.609344).toFixed(0):p.mph.toFixed(0)):'--';
  $('anread').innerHTML=d+' &middot; <span class="ka">A '+v(a)+'</span> &middot; <span class="kb">B '+v(b)+'</span>'+
    (a&&b&&AN.b!==AN.a?' &middot; B '+fmtGap(b.t-a.t)+' s':'');
}
$('anplot').onpointerdown=e=>{$('anplot').setPointerCapture(e.pointerId);anPoint(e)};
$('anplot').onpointermove=e=>{if(e.buttons||e.pointerType==='mouse')anPoint(e)};
window.addEventListener('resize',()=>{if(AN&&!$('anwrap').hidden){drawMap();drawPlot()}});
$('file').onchange=()=>{$('up').disabled=!$('file').files.length;$('upmsg').textContent=''};
$('up').onclick=()=>{
  const f=$('file').files[0];if(!f)return;
  const x=new XMLHttpRequest(),p=$('prog'),m=$('upmsg');
  $('up').disabled=true;p.hidden=false;p.value=0;m.className='msg dim';m.textContent='Sending…';
  x.upload.onprogress=e=>{if(e.lengthComputable)p.value=100*e.loaded/e.total;
    if(e.loaded===e.total)m.textContent='Checking and writing…'};
  x.onload=()=>{
    let r={};try{r=JSON.parse(x.responseText)}catch(e){r={ok:false,error:x.responseText||x.status}}
    m.className='msg '+(r.ok?'good':'bad');
    m.textContent=r.ok?'Installed '+r.dev+' in the dev slot. Boot it from DIAG → Update.':'Failed: '+r.error;
    $('up').disabled=false;status();
  };
  x.onerror=()=>{m.className='msg bad';m.textContent='Connection lost.';$('up').disabled=false};
  x.open('POST','/fw');x.send(f);
};
// Every CONFIG setting, grouped as the dash groups them. Each one is sent
// the moment it changes, and shows the value the dash actually kept.
async function settings(){
  const el=$('settings');
  let l;
  try{l=await (await fetch('/api/settings')).json()}catch(e){el.innerHTML='<span class="bad">Could not read settings.</span>';return}
  const groups={};
  l.forEach(s=>(groups[s.group]=groups[s.group]||[]).push(s));
  const z=l.find(s=>s.label==='UTC offset');if(z)tz=+z.value;
  const un=l.find(s=>s.label==='Units');if(un)metric=+un.value===1;
  el.innerHTML='';
  for(const g in groups){
    const d=document.createElement('details');
    d.innerHTML='<summary>'+g+'</summary>';
    groups[g].forEach(s=>{
      const row=document.createElement('div');row.className='set';
      let inp;
      if(s.kind==='bool'){inp=document.createElement('input');inp.type='checkbox';inp.checked=s.value!=0}
      else if(s.kind==='enum'){inp=document.createElement('select');
        s.options.forEach((o,i)=>{const op=document.createElement('option');op.value=i;op.textContent=o;inp.appendChild(op)});
        inp.value=s.value}
      else{inp=document.createElement('input');inp.type='number';inp.min=s.min;inp.max=s.max;inp.step=s.step;inp.value=s.value}
      const lab=document.createElement('span');lab.textContent=s.label;lab.style.flex='1';
      const u=document.createElement('span');u.className='u';u.textContent=s.unit;
      inp.onchange=async()=>{
        const v=s.kind==='bool'?(inp.checked?1:0):inp.value;
        u.className='u';u.textContent='…';
        try{
          const r=await (await fetch('/api/set?key='+s.key+'&value='+encodeURIComponent(v),{method:'POST'})).json();
          if(!r.ok)throw r.error;
          if(s.kind!=='bool')inp.value=r.value;
          u.className='u ok';u.textContent=(s.unit||'')+' ✓';
        }catch(e){u.className='u err';u.textContent='failed'}
      };
      row.append(lab,inp,u);d.appendChild(row);
    });
    el.appendChild(d);
  }
}
status();logs();settings();
</script>
</body></html>
)HTML";

}  // namespace services
