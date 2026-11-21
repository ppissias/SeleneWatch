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
.picture{display:block;background:#080d11}.picture img{width:100%;aspect-ratio:1.9;object-fit:contain;display:block}.body{padding:14px 16px}.row h3{margin:0}.mark{cursor:pointer}.mark[aria-pressed=true]{color:var(--accent)}
.tag{border:1px solid var(--line);padding:2px 6px;border-radius:4px;font-size:.8rem}summary{cursor:pointer}details{margin:10px 0}li{overflow-wrap:anywhere}article ul{max-height:18rem;overflow:auto;padding-left:18px;font-size:.86rem}
.animation img{max-width:100%}footer{border-top:1px solid var(--line);padding-top:10px;font-size:.86rem}[hidden]{display:none!important}
.coverage{position:relative;height:14px;background:var(--line);border-radius:3px;overflow:hidden}.coverage span{position:absolute;top:0;bottom:0;background:var(--accent)}
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
    document.querySelector('#count').textContent=shown+' of '+cards.length+' clips';
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
  filter.onchange=update;update();
}
document.querySelectorAll('.animation').forEach(d=>d.ontoggle=()=>{
  const img=d.querySelector('img');if(d.open)img.src=img.dataset.src;else img.removeAttribute('src');
});
</script>)HTML";
