#include "remote/status_page.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRandomGenerator>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtNetwork/QHostAddress>

namespace {

QJsonValue timeValue(const std::optional<QDateTime>& time)
{
    return time && time->isValid() ? QJsonValue(time->toUTC().toString(Qt::ISODateWithMs)) : QJsonValue();
}

QByteArray refusal(const QString& text)
{
    return ("<!doctype html><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>SeleneWatch</title><body style='background:#0d141b;color:#eef3f5;font:16px/1.5 system-ui,sans-serif;padding:24px'>"
            "<p>" + text.toHtmlEscaped() + "</p></body>").toUtf8();
}

constexpr auto kPage = R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><meta name="color-scheme" content="dark">
<meta name="referrer" content="no-referrer"><title>SeleneWatch</title>
<style>
:root{--bg:#0d141b;--panel:#16212b;--line:#2b3a46;--ink:#eef3f5;--muted:#9fb3bc;--good:#5dd39e;--warn:#ffd166;--bad:#ff6b6b;--accent:#86e0c4}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:16px/1.45 system-ui,-apple-system,"Segoe UI",sans-serif}
main{max-width:640px;margin:auto;padding:14px 16px 36px}header{display:flex;justify-content:space-between;align-items:baseline;gap:10px;flex-wrap:wrap}
.brand{font-size:.72rem;letter-spacing:.14em;font-weight:700;color:var(--accent)}#conn{font-size:.85rem;color:var(--muted)}
#state{font-size:1.9rem;font-weight:700;margin:8px 0 0;line-height:1.15}#detail{color:var(--muted);overflow-wrap:anywhere}
section{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:10px 14px 12px;margin:12px 0}
h2{font-size:.75rem;letter-spacing:.1em;text-transform:uppercase;color:var(--muted);margin:2px 0 6px}
.row{display:flex;justify-content:space-between;gap:14px;padding:3px 0;border-top:1px solid var(--line)}.row:first-child{border-top:0}
.row span:first-child{color:var(--muted);flex:none}.row span:last-child{text-align:right;font-variant-numeric:tabular-nums;overflow-wrap:anywhere}
.good{color:var(--good)!important}.warn{color:var(--warn)!important}.bad{color:var(--bad)!important}.muted{color:var(--muted)}
img{width:100%;display:block;border-radius:8px;margin-top:8px;background:#000}[hidden]{display:none!important}
.pill{display:inline-block;border-radius:5px;padding:0 7px;margin-right:6px;font-size:.78rem;font-weight:700;color:#0b1218;background:var(--muted)}
.pill.brief{background:#ffd166}.pill.moving{background:#8fb3c4}.pill.wide{background:#f78c6b}.pill.other{background:#c5b3e6}
#last{margin-top:8px;font-size:.92rem}footer{font-size:.8rem;color:var(--muted);margin-top:14px}#message,footer,#last{overflow-wrap:anywhere}
</style></head><body><main>
<header><span class="brand">SELENEWATCH · STATUS</span><span id="conn">Connecting…</span></header>
<div id="state">…</div><div id="detail"></div>
<section><h2>Session</h2><div id="session"></div><img id="stack" hidden alt="The time-lapse's latest stack"></section>
<section><h2>Events</h2><div id="events"></div><div id="last" hidden></div>
<img id="lastImg" hidden alt="The last clip: before, peak, after, and the change"></section>
<section><h2>Guiding</h2><div id="guiding"></div></section>
<section><h2>Camera and disk</h2><div id="camera"></div></section>
<section id="view" hidden><h2>Live view</h2><img id="preview" alt="The live view, refreshed every 15 s"></section>
<p id="message" class="muted"></p><footer id="foot"></footer>
</main><script>
const q=location.search;let answered=0,error='',lastClip='',lastStack=-1,viewAt=0;
const $=id=>document.getElementById(id);
function el(tag,cls,text){const e=document.createElement(tag);if(cls)e.className=cls;if(text!==undefined)e.textContent=text;return e;}
function rows(id,list){$(id).replaceChildren(...list.filter(r=>r&&r[1]!==undefined&&r[1]!==null&&r[1]!=='').map(([k,v,c])=>{
  const r=el('div','row');r.append(el('span','',k),el('span',c||'',String(v)));return r;}));}
function dur(s){s=Math.max(0,Math.round(s));const h=Math.floor(s/3600),m=Math.floor(s/60)%60;
  return h?h+' h '+m+' min':m?m+' min '+(s%60)+' s':s+' s';}
function clock(iso){if(!iso)return '';const d=new Date(iso);
  return d.toLocaleTimeString([],{hour:'2-digit',minute:'2-digit'})+' ('+iso.substr(11,5)+' UTC)';}
function ago(iso,now){return iso?dur((now-new Date(iso))/1000)+' ago':'';}
function size(b){return b==null?'':b>=1e12?(b/1e12).toFixed(2)+' TB':(b/1e9).toFixed(1)+' GB';}
function show(s){
  const now=new Date(s.updated);
  $('state').textContent=s.state;
  $('state').className=s.problem?'bad':(s.state==='Monitoring'||s.state==='Time-lapse')?'good':s.running?'':'muted';
  $('detail').textContent=(s.detail||'').toUpperCase()===(s.state||'').toUpperCase()?'':s.detail;
  rows('session',[['Watching',s.watch],['Source',s.source],['Started',clock(s.since)],
    ['Running for',s.since?dur((now-new Date(s.since))/1000):''],['Ends',(s.ends||'').replace(/^Ends:? ?/,'')],['Time-lapse',s.timelapse]]);
  const groups=Object.entries(s.groups||{}).map(([g,n])=>n+' '+g).join(' · ');
  rows('events',[['Candidates',s.candidates],['Clips saved',s.clipsSaved+(groups?' ('+groups+')':'')],
    ['Not saved',s.clipsNotSaved||'',s.clipsNotSaved?'bad':'']]);
  const c=s.lastClip;$('last').hidden=!c;
  if(c){const p=el('span','pill '+c.group,c.group);$('last').replaceChildren(el('b','','Last clip: '),p,
    el('span','',clock(c.time)+', '+ago(c.time,now)),el('div','muted',c.facts));}
  const key=c?c.time:'';
  if(s.hasLastClipImage&&key!==lastClip){$('lastImg').src='last-clip.png'+q+'&t='+encodeURIComponent(key);lastClip=key;}
  $('lastImg').hidden=!s.hasLastClipImage;
  rows('guiding',[['Lock',s.lock,s.locked?'good':s.lock==='lost'?'bad':''],
    ['Drift',s.drift?'dx '+s.drift.dx.toFixed(1)+', dy '+s.drift.dy.toFixed(1)+' px':''],
    ['Guiding',s.guiding],['Mount',s.mount],['Last correction',ago(s.lastCorrection,now)],['Note',s.guideMessage]]);
  rows('camera',[['Frame rate',s.running?s.fps.toFixed(1)+' fps':''],['Frames',s.running?s.frames.toLocaleString():''],
    ['Dropped',s.dropped==null?'':s.dropped,s.dropped?'warn':''],['Free on the output drive',size(s.freeBytes)]]);
  $('stack').hidden=!s.hasLastStack;
  if(s.hasLastStack&&s.stacks!==lastStack){$('stack').src='last-stack.png'+q+'&t='+s.stacks;lastStack=s.stacks;}
  $('view').hidden=!s.hasPreview;
  if(s.hasPreview&&Date.now()-viewAt>15000){$('preview').src='preview.jpg'+q+'&t='+Date.now();viewAt=Date.now();}
  $('message').textContent=s.message?(s.messageTime?s.messageTime.substr(11,8)+' UTC · ':'')+s.message:'';
  $('foot').textContent=s.software+' · its clock: '+s.updated.substr(11,8)+' UTC · read-only';
}
async function tick(){
  try{const r=await fetch('status.json'+q,{cache:'no-store'});
    if(!r.ok)throw Error(r.status===403?'The key is wrong: scan the QR code in SeleneWatch again.':'SeleneWatch answered '+r.status);
    show(await r.json());answered=Date.now();error='';}
  catch(e){error=e.message||'No answer';}
  age();
}
function age(){const s=answered?Math.round((Date.now()-answered)/1000):0;const conn=$('conn');
  conn.className=!answered||s>20?'bad':'';
  conn.textContent=!answered?(error||'Connecting…'):s>20?'No answer for '+dur(s)+(error?': '+error:''):'Updated '+s+' s ago';}
tick();setInterval(tick,5000);setInterval(age,1000);
</script></body></html>
)HTML";

const char* reason(int code)
{
    switch (code)
    {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    default: return "Error";
    }
}

} // namespace

QByteArray statusJson(const RemoteStatus& s)
{
    QJsonObject o;
    o["software"] = s.software;
    o["updated"] = timeValue(s.updated);
    o["state"] = s.state;
    o["detail"] = s.detail;
    o["running"] = s.running;
    o["problem"] = s.problem;
    o["watch"] = s.watch;
    o["source"] = s.source;
    o["since"] = timeValue(s.since);
    o["ends"] = s.ends;
    o["fps"] = s.fps;
    o["frames"] = static_cast<double>(s.frames);
    o["dropped"] = s.dropped ? QJsonValue(static_cast<double>(*s.dropped)) : QJsonValue();
    o["candidates"] = s.candidates;
    o["clipsSaved"] = s.clipsSaved;
    o["clipsNotSaved"] = s.clipsNotSaved;
    QJsonObject groups;
    for (auto it = s.groups.cbegin(); it != s.groups.cend(); ++it) groups[it.key()] = it.value();
    o["groups"] = groups;
    if (s.lastClip)
        o["lastClip"] = QJsonObject{{"time", timeValue(s.lastClip->time)}, {"group", s.lastClip->group}, {"facts", s.lastClip->facts}};
    else
        o["lastClip"] = QJsonValue();
    o["lock"] = s.lock;
    o["locked"] = s.locked;
    o["drift"] = s.drift ? QJsonValue(QJsonObject{{"dx", s.drift->x()}, {"dy", s.drift->y()}}) : QJsonValue();
    o["guiding"] = s.guiding;
    o["mount"] = s.mount;
    o["lastCorrection"] = timeValue(s.lastCorrection);
    o["guideMessage"] = s.guideMessage;
    o["timelapse"] = s.timelapse;
    o["stacks"] = s.stacks;
    o["freeBytes"] = s.freeBytes ? QJsonValue(static_cast<double>(*s.freeBytes)) : QJsonValue();
    o["message"] = s.message;
    o["messageTime"] = timeValue(s.messageTime);
    o["hasLastClipImage"] = s.hasLastClipImage;
    o["hasLastStack"] = s.hasLastStack;
    o["hasPreview"] = s.hasPreview;
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

QByteArray statusPageHtml() { return QByteArray(kPage); }

RemoteResponse respond(const QByteArray& requestHead, const QString& key, const RemoteSources& sources)
{
    const QList<QByteArray> words = requestHead.left(requestHead.indexOf("\r\n")).split(' ');
    if (words.size() != 3 || !words[2].startsWith("HTTP/1.") || !words[1].startsWith('/'))
        return {400, "text/plain; charset=utf-8", "Bad request"};
    if (words[0] != "GET") return {405, "text/plain; charset=utf-8", "SeleneWatch's status page can only be read."};
    const QUrl target = QUrl::fromEncoded("http://selenewatch" + words[1]);
    const QString path = target.path();
    if (path == "/favicon.ico") return {404, "text/plain; charset=utf-8", "Not found"};
    if (key.isEmpty() || QUrlQuery(target).queryItemValue("key") != key)
        return {403, "text/html; charset=utf-8", refusal("This address needs the access key. Scan the QR code in SeleneWatch "
                                                          "(Session → Status on your phone…) again.")};
    if (path == "/" || path == "/index.html") return {200, "text/html; charset=utf-8", statusPageHtml()};
    if (path == "/status.json") return {200, "application/json", statusJson(sources.status ? sources.status() : RemoteStatus{})};
    const auto image = [](const std::function<QByteArray()>& source, const QByteArray& type) -> RemoteResponse {
        const QByteArray bytes = source ? source() : QByteArray();
        if (bytes.isEmpty()) return {404, "text/plain; charset=utf-8", "No image yet"};
        return {200, type, bytes};
    };
    if (path == "/last-clip.png") return image(sources.lastClipImage, "image/png");
    if (path == "/last-stack.png") return image(sources.lastStack, "image/png");
    if (path == "/preview.jpg") return image(sources.preview, "image/jpeg");
    return {404, "text/plain; charset=utf-8", "Not found"};
}

QByteArray httpResponse(const RemoteResponse& response)
{
    QByteArray head = "HTTP/1.1 " + QByteArray::number(response.code) + " " + reason(response.code) + "\r\n";
    head += "Content-Type: " + response.type + "\r\n";
    head += "Content-Length: " + QByteArray::number(response.body.size()) + "\r\n";
    head += "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\nConnection: close\r\n\r\n";
    return head + response.body;
}

bool allowedPeer(const QHostAddress& peer)
{
    QHostAddress address = peer;
    bool ipv4 = false;
    const quint32 v4 = peer.toIPv4Address(&ipv4);
    if (ipv4) address = QHostAddress(v4);
    if (address.isLoopback() || address.isLinkLocal() || address.isPrivateUse()) return true;
    return ipv4 && address.isInSubnet(QHostAddress("100.64.0.0"), 10);
}

QString newAccessKey()
{
    static const QString letters = "abcdefghjkmnpqrstuvwxyz23456789";
    QString key;
    for (int i = 0; i < 12; ++i) key += letters[static_cast<qsizetype>(QRandomGenerator::system()->bounded(letters.size()))];
    return key;
}
