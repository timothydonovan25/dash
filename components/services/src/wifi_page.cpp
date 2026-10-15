// The one page the update hotspot serves (see wifi_service.cpp). Plain
// HTML and a little script, no external files: the phone viewing it is on
// the dash's own network and has no internet.
namespace services {

extern const char kPage[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CRF250F Dash</title>
<style>
body{font:16px/1.4 system-ui,sans-serif;margin:0 auto;padding:16px;max-width:560px;background:#111;color:#eee}
h1{font-size:20px;margin:0 0 12px}h2{font-size:17px;margin:20px 0 8px;color:#9cf}
.card{background:#1c1c1c;border-radius:8px;padding:12px;margin-bottom:12px}
.row{display:flex;justify-content:space-between;gap:8px;padding:4px 0;border-bottom:1px solid #2a2a2a}
.row:last-child{border:0}.k{color:#999}a{color:#6cf}
button,input[type=file]{font:inherit;width:100%;margin-top:8px}
button{padding:10px;border:0;border-radius:6px;background:#2a6;color:#fff}
button:disabled{background:#444}
progress{width:100%;height:14px;margin-top:8px}
.msg{margin-top:8px}.bad{color:#f66}.good{color:#6d6}.dim{color:#888}
.row button{width:auto;margin:0;padding:2px 10px;background:#633;font-size:14px}
.row button.gpx{background:#246;margin-right:6px}
.set{display:flex;align-items:center;justify-content:space-between;gap:8px;padding:5px 0;border-bottom:1px solid #2a2a2a}
.set:last-child{border:0}.set input,.set select{font:inherit;width:120px;background:#222;color:#eee;border:1px solid #444;border-radius:4px;padding:3px}
.set input[type=checkbox]{width:22px;height:22px}.u{color:#888;font-size:13px;min-width:40px}
.ok{color:#6d6}.err{color:#f66}
details{margin-bottom:8px}summary{cursor:pointer;color:#9cf;padding:4px 0}
</style></head><body>
<h1>CRF250F Dash</h1>

<div class="card" id="fw">
<div class="row"><span class="k">Running</span><span id="run">&hellip;</span></div>
<div class="row"><span class="k">Dev slot</span><span id="dev">&hellip;</span></div>
</div>

<h2>Upload firmware</h2>
<div class="card">
<div class="dim">Writes dash.bin to the dev slot. The bike must be parked with the
engine off. Then on the dash: DIAG &rarr; Update &rarr; Boot dev once.</div>
<input type="file" id="file" accept=".bin">
<button id="up" disabled>Upload</button>
<progress id="prog" value="0" max="100" hidden></progress>
<div class="msg" id="upmsg"></div>
</div>

<h2>Ride logs</h2>
<div class="card" id="logs"><span class="dim">&hellip;</span></div>

<h2>Settings</h2>
<div class="card" id="settings"><span class="dim">&hellip;</span></div>

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
  }catch(e){}
}
async function logs(){
  const el=$('logs');
  try{
    const l=await (await fetch('/api/logs')).json();
    if(!l.length){el.innerHTML='<span class="dim">No logs, or no card.</span>';return}
    l.sort((a,b)=>a.name<b.name?1:-1);
    el.innerHTML=l.map(f=>'<div class="row"><a href="/logs/'+f.name+'" download>'+f.name+
      '</a><span class="dim">'+kb(f.size)+' <button class="gpx" data-gpx="'+f.name+'">GPX</button>'+
      '<button data-del="'+f.name+'">Delete</button></span></div>').join('');
    el.querySelectorAll('[data-gpx]').forEach(b=>b.onclick=()=>gpx(b.dataset.gpx,b));
    el.querySelectorAll('[data-del]').forEach(b=>b.onclick=async()=>{
      const n=b.dataset.del;if(!confirm('Delete '+n+' from the card?'))return;
      b.disabled=true;
      let r={};try{r=await (await fetch('/logs/'+n,{method:'DELETE'})).json()}catch(e){r={error:'connection lost'}}
      if(r.ok)logs();else{alert('Not deleted: '+r.error);b.disabled=false}
    });
  }catch(e){el.innerHTML='<span class="bad">Could not list logs.</span>'}
}
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
