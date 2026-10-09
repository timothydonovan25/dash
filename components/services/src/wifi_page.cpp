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
      '</a><span class="dim">'+kb(f.size)+' <button data-del="'+f.name+'">Delete</button></span></div>').join('');
    el.querySelectorAll('[data-del]').forEach(b=>b.onclick=async()=>{
      const n=b.dataset.del;if(!confirm('Delete '+n+' from the card?'))return;
      b.disabled=true;
      let r={};try{r=await (await fetch('/logs/'+n,{method:'DELETE'})).json()}catch(e){r={error:'connection lost'}}
      if(r.ok)logs();else{alert('Not deleted: '+r.error);b.disabled=false}
    });
  }catch(e){el.innerHTML='<span class="bad">Could not list logs.</span>'}
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
