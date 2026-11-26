#pragma once

// All report assets are local so an archived watch opens offline.
constexpr auto reportStyle = R"HTML(<style>
:root{color-scheme:dark;--bg:#101822;--panel:#192632;--ink:#eff4f5;--muted:#afc3ca;--line:#344954;--accent:#86e0c4}
*{box-sizing:border-box}html{scroll-behavior:smooth}body{margin:0;background:var(--bg);color:var(--ink);font:16px/1.5 "Segoe UI",system-ui,sans-serif}
main{max-width:1320px;margin:auto;padding:40px clamp(16px,4vw,56px)}header{border-top:3px solid var(--accent);padding:24px 0 8px}
.brand{letter-spacing:.16em;font-size:.75rem;font-weight:700;color:var(--accent)}h1{font:normal clamp(2.8rem,6vw,5rem)/1.06 Georgia,serif;margin:14px 0 0}
header p,.hint,.ends,#count{color:var(--muted)}h2{font:normal 2rem Georgia,serif;margin:0 0 18px}h3{font-size:.96rem}
.stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:1px;background:var(--line);border:1px solid var(--line);border-radius:8px;overflow:hidden}
.stats div{background:var(--panel);padding:14px;color:var(--muted);font-size:.86rem}.stats strong{display:block;font-size:1.5rem;color:var(--ink);font-variant-numeric:tabular-nums}
a,summary{color:var(--accent)}a:focus-visible,button:focus-visible,summary:focus-visible,select:focus-visible{outline:2px solid white;outline-offset:2px}
nav,footer,.tools,.row,.ends{display:flex;gap:16px;align-items:center;flex-wrap:wrap}nav{padding:18px 0;border-bottom:1px solid var(--line)}
section{padding:30px 0;border-bottom:1px solid var(--line);scroll-margin:15px}.ruler{position:relative;background:var(--line);height:16px;margin:14px 6px 6px;border-radius:3px}
.ruler a{position:absolute;top:-3px;bottom:-3px;width:4px;background:var(--accent);transform:translateX(-50%)}.ends,.row{justify-content:space-between}.ends,.hint{font-size:.83rem}
.tools{margin:0 0 18px}button,select{font:inherit;color:var(--ink);background:var(--panel);border:1px solid var(--line);border-radius:5px;padding:3px 9px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(min(100%,330px),1fr));gap:16px;align-items:start}article{background:var(--panel);border:1px solid var(--line);border-radius:10px;overflow:hidden;scroll-margin:12px}
.picture{display:block;background:#080d11}.picture img{width:100%;aspect-ratio:1.9;object-fit:contain;display:block}.body{padding:14px 16px}.row h3{margin:0}.actions{display:flex;gap:8px;align-items:center}.mark,.gifToggle{cursor:pointer}.mark[aria-pressed=true],.gifToggle[aria-pressed=true],#toggleAllGifs[aria-pressed=true]{color:var(--accent);border-color:var(--accent)}.gifToggle{font-size:.82rem;padding:2px 8px}.cardGif{background:#080d11;border-top:1px solid var(--line)}.cardGif img{width:100%;max-height:360px;object-fit:contain;display:block}
.tag{border:1px solid var(--line);padding:2px 6px;border-radius:4px;font-size:.8rem}summary{cursor:pointer}details{margin:10px 0}li{overflow-wrap:anywhere}article ul{max-height:18rem;overflow:auto;padding-left:18px;font-size:.86rem}
.animation img{max-width:100%}footer{border-top:1px solid var(--line);padding-top:10px;font-size:.86rem}[hidden]{display:none!important}
.coverage{position:relative;height:14px;background:var(--line);border-radius:3px;overflow:hidden}.coverage span{position:absolute;top:0;bottom:0;background:var(--accent)}
.summary{font-size:1.1rem;color:var(--ink)}.strip{display:block;overflow:hidden;background:#080d11}.strip img{width:100%;display:block}
.badge{display:inline-block;border-radius:4px;padding:0 7px;margin-right:7px;font-size:.78rem;font-weight:700;color:#0b1218;vertical-align:1px}
.badge.brief,.ruler a.brief{background:#ffd166}.badge.moving,.ruler a.moving{background:#8fb3c4}.badge.wide,.ruler a.wide{background:#f78c6b}.badge.other,.ruler a.other{background:#c5b3e6}
.groupBlock{margin:28px 0 8px;scroll-margin:15px}.groupTitle{font-size:1.2rem;margin:0 0 2px}.facts{margin:8px 0;font-size:.92rem}
ol.clips{padding-left:20px;margin:6px 0}ol.clips>li{margin:10px 0;padding-top:8px;border-top:1px solid var(--line);font-size:.9rem}
.configGrid{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(100%,300px),1fr));gap:16px}
.configCard{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:16px 18px}
.configCard h3{margin:0 0 10px;font-size:.98rem;color:var(--accent)}
.kv{width:100%;border-collapse:collapse;font-size:.86rem}
.kv th{text-align:left;color:var(--muted);font-weight:500;padding:4px 10px 4px 0;vertical-align:top;white-space:nowrap}
.kv td{color:var(--ink);padding:4px 0;overflow-wrap:anywhere}
.kv tr+tr th,.kv tr+tr td{border-top:1px solid rgba(52,73,84,.55)}
.warn{color:#f78c6b;font-weight:600}
.guideChart{background:#0c141d;border:1px solid var(--line);border-radius:10px;padding:12px 14px;margin:16px 0 8px}
.guideChart svg{width:100%;height:auto;display:block}
.legend{display:flex;gap:16px;flex-wrap:wrap;font-size:.82rem;color:var(--muted);margin-top:8px;align-items:center}
.swatch{display:inline-block;width:12px;height:3px;border-radius:2px;vertical-align:middle;margin-right:6px}
.swatch.box{height:10px;width:10px;border-radius:2px}
</style>)HTML";

constexpr auto reportScript = R"HTML(<script>
const cards=[...document.querySelectorAll('article')];
const filter=document.querySelector('#filter');
if(filter){
  const tags=new Set(cards.flatMap(c=>c.dataset.tags.split('|').filter(Boolean)));
  for(const tag of [...tags].sort()) filter.add(new Option(tag,tag));
  const storage='SeleneWatch review '+location.pathname;
  let marked=new Set();
  try { marked=new Set(JSON.parse(localStorage.getItem(storage)||'[]')); } catch {}
  function update(){
    let shown=0;
    for(const c of cards){
      const star=c.querySelector('.mark'), saved=marked.has(c.dataset.key);
      star.textContent=saved?'★':'☆';star.setAttribute('aria-pressed',String(saved));
      c.hidden=filter.value==='*'?!saved:!!filter.value&&!c.dataset.tags.split('|').includes(filter.value);
      if(!c.hidden)shown++;
    }
    for(const g of document.querySelectorAll('.groupBlock'))g.hidden=![...g.querySelectorAll('article')].some(c=>!c.hidden);
    document.querySelector('#count').textContent=shown+' of '+cards.length+' events';
  }
  for(const c of cards)c.querySelector('.mark').onclick=()=>{
    const key=c.dataset.key;
    if(marked.has(key))marked.delete(key);else marked.add(key);
    try{localStorage.setItem(storage,JSON.stringify([...marked]));}catch{document.querySelector('.hint').textContent='Marks are available only until this page closes.';}
    update();
  };
  document.querySelectorAll('.ruler a').forEach(a=>a.onclick=()=>{filter.value='';update()});
  for(const b of document.querySelectorAll('.kindFilter')) b.onclick=()=>{filter.value=b.value;update();};
  const session=document.querySelector('header p').textContent;
  const notice=document.querySelector('#markStatus');
  document.querySelector('#saveMarks').onclick=()=>{
    const data=JSON.stringify({session,clips:[...marked]},null,2);
    const uri=URL.createObjectURL(new Blob([data],{type:'application/json'}));
    const a=document.createElement('a');a.href=uri;a.download='follow-up.json';a.click();
    setTimeout(()=>URL.revokeObjectURL(uri),1000);
  };
  document.querySelector('#loadMarks').onchange=async e=>{
    try{
      const file=e.target.files[0];if(!file)return;
      const saved=JSON.parse(await file.text());
      if(saved.session!==session||!Array.isArray(saved.clips))throw Error('Choose marks saved for this watch.');
      marked=new Set(cards.map(c=>c.dataset.key).filter(key=>saved.clips.includes(key)));
      update();notice.textContent='Loaded '+marked.size+' follow-up marks.';
      try{localStorage.setItem(storage,JSON.stringify([...marked]));}catch{}
    }catch(err){notice.textContent=err.message;}
  };
  const toggleAll=document.querySelector('#toggleAllGifs');
  function setCardGif(c,show){
    const box=c.querySelector('.cardGif'),btn=c.querySelector('.gifToggle');
    if(!box||!btn)return;
    const img=box.querySelector('img');
    box.hidden=!show;
    if(show){if(!img.getAttribute('src'))img.src=img.dataset.src;}else img.removeAttribute('src');
    btn.setAttribute('aria-pressed',String(show));
    btn.textContent=show?'■ Hide GIF':'▶ Play GIF';
  }
  function syncToggleAll(){
    if(!toggleAll)return;
    const vis=cards.filter(c=>!c.hidden&&c.querySelector('.cardGif'));
    const allOpen=vis.length>0&&vis.every(c=>!c.querySelector('.cardGif').hidden);
    toggleAll.setAttribute('aria-pressed',String(allOpen));
    toggleAll.textContent=allOpen?'Hide all GIFs':'Play all GIFs';
  }
  for(const c of cards){
    const btn=c.querySelector('.gifToggle');
    if(btn)btn.onclick=()=>{setCardGif(c,c.querySelector('.cardGif').hidden);syncToggleAll();};
  }
  if(toggleAll)toggleAll.onclick=()=>{
    const vis=cards.filter(c=>!c.hidden&&c.querySelector('.cardGif'));
    const openNext=!vis.every(c=>!c.querySelector('.cardGif').hidden);
    vis.forEach(c=>setCardGif(c,openNext));
    syncToggleAll();
  };
  const origUpdate=update;
  update=()=>{origUpdate();syncToggleAll();};
  filter.onchange=update;update();
  const briefGifs=cards.filter(c=>c.dataset.tags.split('|').includes('brief')&&c.querySelector('.cardGif'));
  if(briefGifs.length>0&&briefGifs.length<=20){briefGifs.forEach(c=>setCardGif(c,true));syncToggleAll();}
}
document.querySelectorAll('.animation').forEach(d=>d.ontoggle=()=>{
  const img=d.querySelector('img');if(d.open)img.src=img.dataset.src;else img.removeAttribute('src');
});
// Feature-46: SeleneWatch opens these files in their own programs.
document.querySelectorAll('a[data-open]').forEach(a=>{
  a.href='selenewatch:'+a.dataset.open+'?file='+encodeURIComponent(new URL(a.getAttribute('href'),location.href).href);
});
</script>)HTML";
