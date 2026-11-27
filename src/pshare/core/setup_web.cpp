// PShare - The phone setup page served by pshared.
// Copyright (C) 2026 PShare contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pshare/core/setup_web.hpp"

#include "pshare/core/text.hpp"

#include <algorithm>
#include <cstdio>

namespace pshare
{
namespace
{
int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

const char *status_text(int status)
{
    switch (status)
    {
    case 200:
        return "OK";
    case 303:
        return "See Other";
    case 400:
        return "Bad Request";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 413:
        return "Payload Too Large";
    case 429:
        return "Too Many Requests";
    default:
        return "Error";
    }
}

// Shared look: the app's palette (deep indigo, coral, aqua, sun yellow).
constexpr const char *kStyle = R"CSS(
:root{--bg:#15123a;--panel:#211c55;--line:#3a3380;--text:#f4f1ff;--muted:#b3acdf;
--coral:#ff5c8a;--aqua:#3ee6e0;--sun:#ffd25e}
*{box-sizing:border-box}
body{margin:0;font:16px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;
color:var(--text);background:var(--bg);
background-image:radial-gradient(circle at 15% -10%,#3b2a8a 0,transparent 45%),
radial-gradient(circle at 110% 20%,#5a1f6e 0,transparent 40%)}
main{max-width:640px;margin:0 auto;padding:20px 16px 64px}
h1{font-size:28px;margin:8px 0 2px;letter-spacing:.5px}
h1 b{color:var(--coral)}
.sub{color:var(--muted);margin:0 0 18px}
.card{background:var(--panel);border:2px solid var(--line);border-radius:18px;
padding:16px;margin:14px 0}
.card h2{font-size:18px;margin:0 0 10px;display:flex;align-items:center;gap:8px}
.tag{font-size:12px;font-weight:600;color:var(--bg);background:var(--aqua);
border-radius:99px;padding:2px 9px}
.off .tag{background:var(--muted)}
summary{cursor:pointer;list-style:none}summary::-webkit-details-marker{display:none}
summary h2::after{content:"\203A";margin-left:auto;font-size:24px;color:var(--muted);
transition:transform .15s}
details[open] summary h2::after{transform:rotate(90deg)}
details.card:not([open]) h2{margin:0}
label{display:block;font-size:13px;color:var(--muted);margin:10px 0 4px}
input,select,textarea{width:100%;font:inherit;color:var(--text);background:#17134180;
border:2px solid var(--line);border-radius:12px;padding:10px 12px}
textarea{min-height:70px}
input:focus,select:focus,textarea:focus{outline:none;border-color:var(--aqua)}
.row{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.note{padding:12px 14px;border-radius:14px;margin:12px 0;font-weight:600;
background:#1f4d4b;border:2px solid var(--aqua)}
.note.err{background:#4d1f33;border-color:var(--coral)}
nav{display:flex;gap:8px;margin:0 0 6px}
nav a{flex:1;text-align:center;padding:10px;border-radius:12px;font-weight:700;
color:var(--text);text-decoration:none;background:var(--panel);border:2px solid var(--line)}
nav a.on{background:var(--coral);border-color:var(--coral);color:#2a0716}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:12px}
.cap{background:var(--panel);border:2px solid var(--line);border-radius:14px;overflow:hidden}
.cap img,.cap video{display:block;width:100%;aspect-ratio:16/9;object-fit:cover;background:#0e0b28}
.cap .open{display:block;position:relative}
.cap .play{position:absolute;right:8px;bottom:8px;width:30px;height:30px;border-radius:50%;
background:#0e0b28cc;color:#fff;font-size:13px;display:grid;place-items:center}
.cap p{margin:6px 9px 0;font-size:13px;font-weight:600;white-space:nowrap;overflow:hidden;
text-overflow:ellipsis}
.cap small{display:block;margin:0 9px}
.cap a.dl{display:block;margin:6px 9px 9px;text-align:center;font-size:13px;font-weight:700;
color:var(--bg);background:var(--aqua);border-radius:9px;padding:6px;text-decoration:none}
.pages{display:flex;justify-content:space-between;margin:16px 0}
.filters{display:grid;grid-template-columns:2fr 1fr;gap:10px;margin:12px 0}
.pages a{color:var(--aqua);font-weight:700}
button{width:100%;font:inherit;font-weight:700;font-size:18px;color:var(--bg);
background:var(--sun);border:0;border-radius:14px;padding:14px;margin-top:18px;
box-shadow:0 5px 0 #c99a2a;cursor:pointer}
small{color:var(--muted)}
)CSS";

std::string page(std::string_view title, std::string_view body)
{
    std::string out;
    out.reserve(body.size() + 4096);
    out += "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           "<title>";
    out += html_escape(title);
    out += "</title><style>";
    out += kStyle;
    out += "</style></head><body><main>"
           "<h1>P<b>Share</b></h1><p class=\"sub\">Made by StonedModder</p>";
    out += body;
    out += "</main></body></html>";
    return out;
}

void notice_html(std::string *out, std::string_view notice, bool error)
{
    if (notice.empty())
        return;
    *out += error ? "<div class=\"note err\">" : "<div class=\"note\">";
    *out += html_escape(notice);
    *out += "</div>";
}

std::string nav_html(bool gallery)
{
    std::string out = "<nav><a href=\"/gallery\"";
    out += gallery ? " class=\"on\"" : "";
    out += ">Captures</a><a href=\"/\"";
    out += gallery ? "" : " class=\"on\"";
    out += ">Settings</a></nav>";
    return out;
}
} // namespace

std::string gallery_page(const std::vector<GalleryItem> &items, int page_index, int pages,
                         std::string_view notice, const GalleryFilter &filter)
{
    std::string body = nav_html(true);
    notice_html(&body, notice, false);
    // Game and order: a form that reloads on change (works without script too).
    body += "<form class=\"filters\" method=\"get\" action=\"/gallery\">"
            "<select name=\"game\" onchange=\"this.form.submit()\"><option value=\"\">All "
            "games</option>";
    for (const GalleryGame &game : filter.games)
    {
        body += "<option value=\"" + html_escape(game.title_id) + "\"";
        body += game.title_id == filter.game ? " selected" : "";
        body += ">" + html_escape(game.name) + " (" + std::to_string(game.count) + ")</option>";
    }
    body += "</select><select name=\"order\" onchange=\"this.form.submit()\">"
            "<option value=\"new\">Newest first</option><option value=\"old\"";
    body += filter.oldest_first ? " selected" : "";
    body += ">Oldest first</option></select><noscript><button>Show</button></noscript></form>";
    std::string keep;
    if (!filter.game.empty())
        keep += "&game=" + url_encode(filter.game);
    if (filter.oldest_first)
        keep += "&order=old";
    if (items.empty())
        body += "<div class=\"card\"><h2>No captures yet</h2><p><small>Take a screenshot "
                "with the Create button.</small></p></div>";
    body += "<div class=\"grid\">";
    for (const GalleryItem &item : items)
    {
        const std::string source = "/file?p=" + url_encode(item.path);
        const std::string view = "/view?p=" + url_encode(item.path);
        body += "<div class=\"cap\"><a class=\"open\" href=\"";
        body += html_escape(view);
        body += "\">";
        if (item.video)
        {
            body += "<video muted playsinline preload=\"metadata\" src=\"";
            body += html_escape(source + "#t=0.5");
            body += "\"></video><span class=\"play\">&#9654;</span>";
        }
        else
        {
            body += "<img loading=\"lazy\" alt=\"\" src=\"";
            body += html_escape(source);
            body += "\">";
        }
        body += "</a>";
        body += "<p>";
        body += html_escape(item.game);
        body += "</p><small>";
        body += html_escape(item.when + " \xC2\xB7 " + item.size);
        body += "</small><a class=\"dl\" href=\"";
        body += html_escape(source + "&dl=1");
        body += "\">Download</a></div>";
    }
    body += "</div>";
    if (pages > 1)
    {
        body += "<div class=\"pages\">";
        body += page_index > 0 ? "<a href=\"/gallery?page=" + std::to_string(page_index - 1) +
                                     html_escape(keep) + "\">&larr; Back</a>"
                               : std::string("<span></span>");
        body +=
            "<small>" + std::to_string(page_index + 1) + " / " + std::to_string(pages) + "</small>";
        body += page_index + 1 < pages
                    ? "<a href=\"/gallery?page=" + std::to_string(page_index + 1) +
                          html_escape(keep) + "\">More &rarr;</a>"
                    : std::string("<span></span>");
        body += "</div>";
    }
    return page("PShare captures", body);
}

namespace
{
constexpr const char *kViewPage = R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>@TITLE@ - PShare</title><style>
:root{--bg:#0e0b28;--panel:#211c55;--line:#3a3380;--text:#f4f1ff;--muted:#b3acdf;
--coral:#ff5c8a;--aqua:#3ee6e0;--sun:#ffd25e;--violet:#7c8cff}
*{box-sizing:border-box}html,body{height:100%}
body{margin:0;background:var(--bg);color:var(--text);
font:15px/1.4 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;display:flex;flex-direction:column;
user-select:none;-webkit-tap-highlight-color:transparent}
header{display:flex;align-items:center;gap:12px;padding:12px 16px 8px}
header a.back{color:var(--text);text-decoration:none;font-weight:700;font-size:20px;
width:38px;height:38px;border-radius:50%;display:grid;place-items:center;background:var(--panel)}
header h1{margin:0;font-size:19px}header small{display:block;color:var(--muted);font-size:13px}
.brand{margin-left:auto;font-weight:800;letter-spacing:.3px;text-align:right}
.brand b{color:var(--coral)}.brand small{display:block;font-weight:600;font-size:12px;
color:var(--muted);letter-spacing:0;white-space:nowrap}[hidden]{display:none!important}
.stage{flex:1;position:relative;display:grid;place-items:center;min-height:0;margin:0 12px;
border-radius:16px;overflow:hidden;background:#06051a}
.stage img,.stage video{max-width:100%;max-height:100%;display:block}
.big{position:absolute;inset:0;margin:auto;width:84px;height:84px;border-radius:50%;border:0;
background:#ff5c8acc;color:#fff;font-size:34px;cursor:pointer}
.bar{display:flex;align-items:center;gap:10px;padding:10px 16px 0}
.time{font:600 14px ui-monospace,Menlo,monospace;color:var(--muted);min-width:44px}
.track{flex:1;height:8px;border-radius:9px;background:#3a338099;position:relative;cursor:pointer}
.track i{position:absolute;left:0;top:0;bottom:0;border-radius:9px;background:var(--aqua)}
.hints{display:flex;flex-wrap:wrap;gap:8px;padding:12px 16px 16px}
.hints button{display:flex;align-items:center;gap:8px;font:inherit;font-weight:650;color:var(--text);
background:var(--panel);border:2px solid var(--line);border-radius:12px;padding:8px 12px;cursor:pointer}
.hints button:active,.hints button.hit{border-color:var(--aqua)}
.g{display:inline-grid;place-items:center;min-width:24px;height:24px;padding:0 5px;border-radius:12px;
font-size:12px;font-weight:800;background:#0e0b28;border:2px solid var(--line)}
.g.x{color:#7c8cff}.g.o{color:#ff5c8a}.g.s{color:#ff8ad8}.g.t{color:#3ee6e0}
.sheet{position:fixed;inset:auto 0 0 0;max-width:520px;margin:0 auto;background:var(--panel);
border:2px solid var(--line);border-radius:20px 20px 0 0;padding:16px;transform:translateY(110%);
transition:transform .22s}
.sheet.on{transform:none}.sheet h2{margin:0 0 10px;font-size:18px}
.sheet .it{padding:12px 14px;border-radius:12px;font-weight:650;cursor:pointer;display:flex;
justify-content:space-between;align-items:center}
.sheet .it.f{background:var(--coral);color:#2a0716}.sheet small{color:inherit;opacity:.75}
.toast{position:fixed;left:50%;top:14px;transform:translate(-50%,-250%);transition:transform .25s;
background:var(--panel);border:2px solid var(--aqua);border-radius:14px;padding:10px 16px;
font-weight:650;max-width:92vw;z-index:9}
.toast.on{transform:translate(-50%,0)}.toast.err{border-color:var(--coral)}
.toast a{color:var(--aqua);word-break:break-all}
.link{padding:0 16px;color:var(--muted);font-size:13px;word-break:break-all}.link a{color:var(--aqua)}
.blank{max-width:420px;margin:24px;text-align:center;color:var(--muted);line-height:1.5}
.blank b{color:var(--text);font-size:18px}
</style></head><body>
<header><a class="back" href="/gallery" title="Back (Circle / Esc)">&#8592;</a>
<div><h1>@TITLE@</h1><small>@WHEN@ &middot; @SIZE@ &middot; @KIND@</small></div>
<div class="brand">P<b>Share</b><small>Made by StonedModder</small></div></header>
<div class="stage" id="stage">@MEDIA@</div>
<div class="bar" id="bar" hidden><span class="time" id="now">0:00</span>
<div class="track" id="track"><i id="fill"></i></div><span class="time" id="len">0:00</span></div>
<p class="link" id="last">@LINK@</p>
<div class="hints" id="hints"></div>
<div class="sheet" id="sheet"><h2>Send to&hellip;</h2><div id="list"></div></div>
<div class="toast" id="toast"></div>
<script>
const CAP=@DATA@;
const $=id=>document.getElementById(id);
const media=$('media'),video=CAP.video?media:null;
let sheet=false,focus=0,toastTimer=0;
function clock(t){t=Math.max(0,t|0);return (t/60|0)+':'+String(t%60).padStart(2,'0')}
function toast(html,err){const t=$('toast');t.innerHTML=html;t.className='toast on'+(err?' err':'');
clearTimeout(toastTimer);toastTimer=setTimeout(()=>t.className='toast'+(err?' err':''),err?6000:4500)}
function go(path){if(path)location.href='/view?p='+encodeURIComponent(path)}
function back(){if(sheet)return closeSheet();location.href='/gallery'}
function playPause(){if(!video){toggleFull();return}video.paused?video.play():video.pause()}
function toggleFull(){const s=$('stage');document.fullscreenElement?document.exitFullscreen():
(s.requestFullscreen&&s.requestFullscreen())}
function jump(s){if(video){video.currentTime=Math.min(Math.max(0,video.currentTime+s),video.duration||1e9)}}
function left(){video?jump(-10):go(CAP.prev)}function right(){video?jump(10):go(CAP.next)}
function send(id){const d=CAP.dests.find(x=>x.id===id)||CAP.dests.find(x=>x.def)||CAP.dests[0];
if(!d){toast('No destination is set up yet. Open Settings first.',1);return}
toast('Sending to '+esc(d.name)+'&hellip;');
fetch('/send',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
body:'p='+encodeURIComponent(CAP.path)+'&d='+encodeURIComponent(d.id)}).then(r=>r.json()).then(j=>{
if(j.error){toast(esc(j.error),1);return}poll(j.id,d.name)}).catch(()=>toast('PShare did not answer',1))}
function poll(id,name){fetch('/result?id='+id).then(r=>r.json()).then(j=>{
if(j.state==='pending'){setTimeout(()=>poll(id,name),800);return}
if(j.state==='ok'){toast('Sent to '+esc(j.dest||name)+(j.link?'<br><a href="'+esc(j.link)+'" target="_blank">'+esc(j.link)+'</a>':''));
if(j.link)$('last').innerHTML='Last upload: <a href="'+esc(j.link)+'" target="_blank">'+esc(j.link)+'</a>'}
else toast('Upload failed: '+esc(j.error||'unknown'),1)}).catch(()=>setTimeout(()=>poll(id,name),1500))}
function esc(s){return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
function openSheet(){if(!CAP.dests.length){toast('No destination is set up yet.',1);return}
sheet=true;focus=Math.max(0,CAP.dests.findIndex(d=>d.def));drawSheet();$('sheet').className='sheet on'}
function closeSheet(){sheet=false;$('sheet').className='sheet'}
function drawSheet(){$('list').innerHTML=CAP.dests.map((d,i)=>'<div class="it'+(i===focus?' f':'')+
'" data-i="'+i+'">'+esc(d.name)+(d.def?'<small>default</small>':'')+'</div>').join('');
[...$('list').children].forEach(el=>el.onclick=()=>{focus=+el.dataset.i;pick()})}
function pick(){const d=CAP.dests[focus];closeSheet();if(d)send(d.id)}
function moveSheet(n){focus=(focus+n+CAP.dests.length)%CAP.dests.length;drawSheet()}
const H=video?[['x','&#10005;','Play / pause',playPause],['','&#9664;&#9654;','10 s',null],
['','L2 R2','30 s',null],['s','&#9633;','Send',()=>send()],['t','&#9651;','Send to&hellip;',openSheet],
['','L1 R1','Newer / older',null],['o','&#9675;','Back',back]]
:[['x','&#10005;','Full screen',toggleFull],['','&#9664;&#9654;','Newer / older',null],
['s','&#9633;','Send',()=>send()],['t','&#9651;','Send to&hellip;',openSheet],['o','&#9675;','Back',back]];
$('hints').innerHTML=H.map((h,i)=>'<button data-i="'+i+'"><span class="g '+h[0]+'">'+h[1]+'</span>'+h[2]+'</button>').join('');
[...$('hints').children].forEach(b=>{const h=H[+b.dataset.i];
if(h[2]==='10 s')b.onclick=()=>jump(10);else if(h[2]==='30 s')b.onclick=()=>jump(30);
else if(h[2]==='Newer / older')b.onclick=()=>go(CAP.next||CAP.prev);else b.onclick=h[3]});
if(video){$('bar').hidden=false;const big=$('big');
video.addEventListener('timeupdate',()=>{$('now').textContent=clock(video.currentTime);
$('len').textContent=clock(video.duration||0);$('fill').style.width=(100*video.currentTime/(video.duration||1))+'%'});
video.addEventListener('loadedmetadata',()=>$('len').textContent=clock(video.duration||0));
video.addEventListener('play',()=>big.hidden=true);video.addEventListener('pause',()=>big.hidden=false);
big.onclick=playPause;video.onclick=playPause;
$('track').onclick=e=>{const r=e.currentTarget.getBoundingClientRect();
video.currentTime=(e.clientX-r.left)/r.width*(video.duration||0)};
video.play().catch(()=>{})}
document.addEventListener('keydown',e=>{const k=e.key;if(sheet){if(k==='ArrowUp')moveSheet(-1);
else if(k==='ArrowDown')moveSheet(1);else if(k==='Enter'||k===' ')pick();else if(k==='Escape'||k==='Backspace')closeSheet();
else return;e.preventDefault();return}
if(k===' '||k==='Enter'||k==='k'||k==='x')playPause();else if(k==='ArrowLeft')e.shiftKey?jump(-30):left();
else if(k==='ArrowRight')e.shiftKey?jump(30):right();else if(k==='['||k==='j')jump(-30);else if(k===']'||k==='l')jump(30);
else if(k==='s')send();else if(k==='t')openSheet();else if(k===','||k==='q')go(CAP.prev);
else if(k==='.'||k==='e')go(CAP.next);else if(k==='Escape'||k==='Backspace'||k==='o')back();else return;e.preventDefault()});
// A controller: the same buttons as on the console.
const held={};
function pad(){const p=[...(navigator.getGamepads?navigator.getGamepads():[])].find(g=>g);
if(p){const on=i=>p.buttons[i]&&p.buttons[i].pressed;
const edge=i=>{const v=on(i),was=held[i];held[i]=v;return v&&!was};
if(sheet){if(edge(12))moveSheet(-1);if(edge(13))moveSheet(1);if(edge(0))pick();if(edge(1))closeSheet()}
else{if(edge(0))playPause();if(edge(1))back();if(edge(2))send();if(edge(3))openSheet();
if(edge(14))left();if(edge(15))right();if(edge(6))jump(-30);if(edge(7))jump(30);
if(edge(4))go(CAP.prev);if(edge(5))go(CAP.next)}}requestAnimationFrame(pad)}
requestAnimationFrame(pad);
</script></body></html>)HTML";

void replace_all(std::string *text, std::string_view key, std::string_view value)
{
    for (std::size_t at = text->find(key); at != std::string::npos;
         at = text->find(key, at + value.size()))
        text->replace(at, key.size(), value);
}
} // namespace

std::string view_page(const ViewInfo &info)
{
    const GalleryItem &item = info.item;
    const std::string source = "/file?p=" + url_encode(item.path);
    std::string media;
    if (info.blank)
        media = "<p class=\"blank\"><b>This capture can't be read right now.</b><br>The console "
                "hands it back as empty (all zeros), so it can't play or download. If it still "
                "plays in the PS5's Media Gallery, it is fine there.</p>";
    else if (item.video)
        media = "<video id=\"media\" playsinline preload=\"auto\" src=\"" + html_escape(source) +
                "\"></video><button class=\"big\" id=\"big\" aria-label=\"Play\">&#9654;</button>";
    else
        media = "<img id=\"media\" alt=\"\" src=\"" + html_escape(source) + "\">";
    // JSON for the script; "</" is broken up so no string can end the <script>.
    std::string data = "{\"path\":\"" + json_escape(item.path) +
                       "\",\"video\":" + (item.video && !info.blank ? "true" : "false") +
                       ",\"prev\":\"" + json_escape(info.prev) + "\",\"next\":\"" +
                       json_escape(info.next) + "\",\"dests\":[";
    for (std::size_t i = 0; i < info.destinations.size(); ++i)
    {
        const ViewDestination &d = info.destinations[i];
        data += (i ? "," : "") + std::string("{\"id\":\"") + json_escape(d.id) + "\",\"name\":\"" +
                json_escape(d.name) + "\",\"def\":" + (d.is_default ? "true" : "false") + "}";
    }
    data += "]}";
    replace_all(&data, "</", "<\\/");
    std::string link;
    if (!info.link.empty())
        link = "Last upload: <a href=\"" + html_escape(info.link) + "\" target=\"_blank\">" +
               html_escape(info.link) + "</a>";
    std::string out = kViewPage;
    replace_all(&out, "@TITLE@", html_escape(item.game));
    replace_all(&out, "@WHEN@", html_escape(item.when));
    replace_all(&out, "@SIZE@", html_escape(item.size));
    replace_all(&out, "@KIND@", item.video ? "Clip" : "Screenshot");
    replace_all(&out, "@MEDIA@", media);
    replace_all(&out, "@LINK@", link);
    replace_all(&out, "@DATA@", data);
    return out;
}

bool parse_range(std::string_view header, std::uint64_t size, std::uint64_t *first,
                 std::uint64_t *last)
{
    // "bytes=START-[END]" or "bytes=-SUFFIX"; one range only.
    header = trim(header);
    if (!starts_with(header, "bytes=") || size == 0)
        return false;
    header.remove_prefix(6);
    if (header.find(',') != std::string_view::npos)
        return false;
    const std::size_t dash = header.find('-');
    if (dash == std::string_view::npos)
        return false;
    const std::string_view left = trim(header.substr(0, dash));
    const std::string_view right = trim(header.substr(dash + 1));
    std::uint64_t start = 0;
    std::uint64_t end = size - 1;
    if (left.empty())
    {
        const long long suffix = parse_int(right, -1);
        if (suffix <= 0)
            return false;
        start = static_cast<std::uint64_t>(suffix) >= size
                    ? 0
                    : size - static_cast<std::uint64_t>(suffix);
    }
    else
    {
        const long long value = parse_int(left, -1);
        if (value < 0 || static_cast<std::uint64_t>(value) >= size)
            return false;
        start = static_cast<std::uint64_t>(value);
        if (!right.empty())
        {
            const long long to = parse_int(right, -1);
            if (to < value)
                return false;
            end = std::min<std::uint64_t>(static_cast<std::uint64_t>(to), size - 1);
        }
    }
    *first = start;
    *last = end;
    return true;
}

std::string file_response_head(int status, std::string_view content_type, std::uint64_t size,
                               std::uint64_t first, std::uint64_t last,
                               std::string_view download_name)
{
    std::string out =
        "HTTP/1.1 " + std::to_string(status) + (status == 206 ? " Partial Content" : " OK");
    out += "\r\nContent-Type: ";
    out += content_type;
    out += "\r\nAccept-Ranges: bytes\r\nContent-Length: " + std::to_string(last - first + 1);
    if (status == 206)
        out += "\r\nContent-Range: bytes " + std::to_string(first) + "-" + std::to_string(last) +
               "/" + std::to_string(size);
    if (!download_name.empty())
    {
        out += "\r\nContent-Disposition: attachment; filename=\"";
        for (const char c : download_name)
            out.push_back(c == '"' || c == '\\' ? '_' : c);
        out += "\"";
    }
    out += "\r\nCache-Control: private, max-age=3600\r\nConnection: close\r\n\r\n";
    return out;
}

namespace
{

void text_field(std::string *out, std::string_view name, std::string_view label,
                std::string_view value, std::string_view type = "text", std::string_view hint = {})
{
    *out += "<label>";
    *out += html_escape(label);
    *out += "</label><input type=\"";
    *out += type;
    *out += "\" name=\"";
    *out += html_escape(name);
    *out += "\" value=\"";
    *out += html_escape(value);
    *out += "\" autocomplete=\"off\" autocapitalize=\"off\" spellcheck=\"false\"";
    if (!hint.empty())
    {
        *out += " placeholder=\"";
        *out += html_escape(hint);
        *out += "\"";
    }
    *out += ">";
}

// A webhook, token or password: anyone on the network can open this page,
// so a saved one is never sent back. Left empty, it stays as it is.
void secret_field(std::string *out, std::string_view name, std::string_view label,
                  std::string_view value, std::string_view hint = {})
{
    text_field(out, name, label, {}, "password",
               value.empty() ? hint : std::string_view("Saved. Type a new one to replace it."));
}

void yes_no(std::string *out, std::string_view name, std::string_view label, bool value)
{
    *out += "<label>";
    *out += html_escape(label);
    *out += "</label><select name=\"";
    *out += html_escape(name);
    *out += "\"><option value=\"true\"";
    if (value)
        *out += " selected";
    *out += ">Yes</option><option value=\"false\"";
    if (!value)
        *out += " selected";
    *out += ">No</option></select>";
}

bool set_text(std::string *target, const std::map<std::string, std::string> &form,
              const std::string &key)
{
    const auto found = form.find(key);
    if (found == form.end())
        return false;
    const std::string value(trim(found->second));
    if (*target == value)
        return false;
    *target = value;
    return true;
}

// A secret_field: empty means "keep the saved one".
bool set_secret(std::string *target, const std::map<std::string, std::string> &form,
                const std::string &key)
{
    const auto found = form.find(key);
    if (found == form.end() || trim(found->second).empty())
        return false;
    return set_text(target, form, key);
}

bool set_bool(bool *target, const std::map<std::string, std::string> &form, const std::string &key)
{
    const auto found = form.find(key);
    if (found == form.end())
        return false;
    const bool value = parse_bool(found->second, *target);
    if (*target == value)
        return false;
    *target = value;
    return true;
}

bool set_int(int *target, const std::map<std::string, std::string> &form, const std::string &key,
             int low, int high, std::vector<std::string> *errors)
{
    const auto found = form.find(key);
    if (found == form.end())
        return false;
    const long long value = parse_int(trim(found->second), -1);
    if (value < low || value > high)
    {
        if (errors)
            errors->push_back(key + ": expected a number from " + std::to_string(low) + " to " +
                              std::to_string(high));
        return false;
    }
    if (*target == static_cast<int>(value))
        return false;
    *target = static_cast<int>(value);
    return true;
}
} // namespace

std::string url_decode(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const char c = text[i];
        if (c == '+')
        {
            out.push_back(' ');
        }
        else if (c == '%' && i + 2 < text.size() && hex_value(text[i + 1]) >= 0 &&
                 hex_value(text[i + 2]) >= 0)
        {
            out.push_back(static_cast<char>(hex_value(text[i + 1]) * 16 + hex_value(text[i + 2])));
            i += 2;
        }
        else
        {
            out.push_back(c);
        }
    }
    return out;
}

std::map<std::string, std::string> parse_form(std::string_view body)
{
    std::map<std::string, std::string> out;
    for (const std::string &pair : split(body, '&'))
    {
        if (pair.empty())
            continue;
        const std::size_t equals = pair.find('=');
        if (equals == std::string::npos)
            out[url_decode(pair)] = {};
        else
            out[url_decode(std::string_view(pair).substr(0, equals))] =
                url_decode(std::string_view(pair).substr(equals + 1));
    }
    return out;
}

std::string html_escape(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const char c : text)
    {
        switch (c)
        {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&#39;";
            break;
        default:
            out.push_back(c);
        }
    }
    return out;
}

long web_head_length(std::string_view raw)
{
    const std::size_t end = raw.find("\r\n\r\n");
    if (end == std::string_view::npos)
        return raw.size() > 16384 ? -1 : 0;
    return static_cast<long>(end + 4);
}

std::size_t web_content_length(std::string_view head)
{
    for (const std::string &line : split(head, '\n'))
    {
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        if (iequals(trim(std::string_view(line).substr(0, colon)), "content-length"))
        {
            const long long value = parse_int(trim(std::string_view(line).substr(colon + 1)), 0);
            return value > 0 ? static_cast<std::size_t>(value) : 0;
        }
    }
    return 0;
}

bool parse_web_request(std::string_view raw, WebRequest *request)
{
    const long head_length = web_head_length(raw);
    if (head_length <= 0)
        return false;
    const std::string_view head = raw.substr(0, static_cast<std::size_t>(head_length));
    const std::vector<std::string> lines = split(head, '\n');
    if (lines.empty())
        return false;
    const std::vector<std::string> first = split(trim(lines[0]), ' ');
    if (first.size() < 2)
        return false;
    WebRequest out;
    out.method = upper(first[0]);
    const std::string &target = first[1];
    const std::size_t question = target.find('?');
    out.path = url_decode(std::string_view(target).substr(0, question));
    if (question != std::string::npos)
        out.query = parse_form(std::string_view(target).substr(question + 1));
    for (std::size_t i = 1; i < lines.size(); ++i)
    {
        const std::string_view line = trim(lines[i]);
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos)
            continue;
        out.headers[lower(trim(line.substr(0, colon)))] = std::string(trim(line.substr(colon + 1)));
    }
    const std::size_t length = web_content_length(head);
    const std::string_view rest = raw.substr(static_cast<std::size_t>(head_length));
    out.body.assign(rest.substr(0, std::min(length, rest.size())));
    *request = std::move(out);
    return true;
}

std::string web_response(int status, std::string_view content_type, std::string_view body,
                         std::string_view extra_headers)
{
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " + status_text(status) + "\r\n";
    out += "Content-Type: ";
    out += content_type;
    out += "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    out += "Cache-Control: no-store\r\nConnection: close\r\n"
           "X-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\n";
    out += extra_headers;
    out += "\r\n";
    out += body;
    return out;
}

std::string setup_page(const Config &config, std::string_view notice, bool notice_is_error)
{
    std::string body;
    notice_html(&body, notice, notice_is_error);
    body += nav_html(false);
    body += "<form method=\"post\" action=\"/save\">";

    body += "<section class=\"card\"><h2>Uploading</h2>";
    yes_no(&body, "g.auto_upload", "Send new captures automatically", config.auto_upload);
    body += "<div class=\"row\"><div>";
    yes_no(&body, "g.upload_screenshots", "Screenshots", config.upload_screenshots);
    body += "</div><div>";
    yes_no(&body, "g.upload_videos", "Video clips", config.upload_videos);
    body += "</div></div>";
    body += "<label>Default destination</label><select name=\"g.default_destination\">";
    for (const Destination &destination : config.destinations)
    {
        body += "<option value=\"";
        body += html_escape(destination.id);
        body += "\"";
        if (destination.id == config.default_destination)
            body += " selected";
        body += ">";
        body += html_escape(destination.display_name());
        body += "</option>";
    }
    body += "</select>";
    body += "<div class=\"row\"><div>";
    yes_no(&body, "g.notify", "Console notifications", config.notify);
    body += "</div><div>";
    text_field(&body, "g.retries", "Tries per upload", std::to_string(config.retries), "number");
    body += "</div></div>";
    yes_no(&body, "g.upload_existing", "Also send older captures", config.upload_existing);
    body += "</section>";

    for (const Destination &d : config.destinations)
    {
        const std::string prefix = "d." + d.id + ".";
        // Each destination folds up under its name, so the page stays short. The
        // fields of a closed one are still sent, and an empty secret keeps its value.
        body += d.enabled ? "<details class=\"card\">" : "<details class=\"card off\">";
        body += "<summary><h2>";
        body += html_escape(d.display_name());
        body += " <span class=\"tag\">";
        body += html_escape(type_label(d.type));
        body += "</span></h2></summary>";
        yes_no(&body, prefix + "enabled", "Enabled", d.enabled);
        yes_no(&body, prefix + "also", "Also gets every new capture",
               std::find(config.also_send.begin(), config.also_send.end(), d.id) !=
                   config.also_send.end());
        text_field(&body, prefix + "name", "Name", d.name);
        switch (d.type)
        {
        case DestinationType::discord:
            secret_field(&body, prefix + "url", "Webhook URL", d.url,
                         "https://discord.com/api/webhooks/...");
            text_field(&body, prefix + "username", "Post as", d.username);
            text_field(&body, prefix + "message", "Message", d.message);
            break;
        case DestinationType::telegram:
            secret_field(&body, prefix + "token", "Bot token", d.token);
            text_field(&body, prefix + "chat_id", "Chat ID", d.chat_id);
            text_field(&body, prefix + "message", "Message", d.message);
            break;
        case DestinationType::catbox:
            secret_field(&body, prefix + "token", "User hash (optional)", d.token);
            break;
        case DestinationType::litterbox:
            text_field(&body, prefix + "litterbox_time", "Keep for (1h, 12h, 24h, 72h)",
                       d.litterbox_time);
            break;
        case DestinationType::folder:
            text_field(&body, prefix + "path", "Folder (usb: the first USB drive)", d.path, "text",
                       "usb");
            break;
        case DestinationType::smb:
            text_field(&body, prefix + "url", "Share (smb://pc/Share/folder)", d.url, "text",
                       "smb://192.168.1.10/Captures");
            text_field(&body, prefix + "username", "Account", d.username);
            secret_field(&body, prefix + "token", "Password", d.token);
            break;
        case DestinationType::onedrive:
            text_field(&body, prefix + "client_id", "Client ID", d.client_id);
            secret_field(&body, prefix + "token", "Refresh token", d.token);
            text_field(&body, prefix + "path", "Folder in OneDrive", d.path, "text", "PShare");
            break;
        case DestinationType::http:
        case DestinationType::count:
            text_field(&body, prefix + "url", "Upload URL", d.url, "url");
            text_field(&body, prefix + "field", "File field", d.field);
            // Headers often carry a key, so they are not sent back either.
            body += "<label>Headers (one per line, Name: value)</label><textarea name=\"";
            body += html_escape(prefix + "headers");
            body += "\"";
            if (!d.headers.empty())
                body += " placeholder=\"Saved. Type new ones to replace them.\"";
            body += "></textarea>";
            break;
        }
        if (d.has_size_limit())
        {
            const int ceiling = d.service_ceiling_mb();
            body += "<div class=\"row\"><div>";
            text_field(&body, prefix + "max_mb",
                       ceiling > 0 ? "Size limit (MB, up to " + std::to_string(ceiling) + ")"
                                   : std::string("Size limit (MB, 0: none)"),
                       std::to_string(d.max_mb), "number");
            body += "</div><div>";
            text_field(&body, prefix + "fallback", "Bigger files go to", d.fallback, "text",
                       "catbox");
            body += "</div></div>";
        }
        body += "</details>";
    }
    body += "<p><small>Placeholders for messages: {type} {game} {title_id} {date} {time} "
            "{file} {size} {link}</small></p>";
    body += "<button type=\"submit\">Save to the console</button></form>";
    return page("PShare setup", body);
}

int apply_setup_form(const std::map<std::string, std::string> &form, Config *config,
                     std::vector<std::string> *errors)
{
    int changed = 0;
    changed += set_bool(&config->auto_upload, form, "g.auto_upload");
    changed += set_bool(&config->upload_screenshots, form, "g.upload_screenshots");
    changed += set_bool(&config->upload_videos, form, "g.upload_videos");
    changed += set_bool(&config->upload_existing, form, "g.upload_existing");
    changed += set_bool(&config->notify, form, "g.notify");
    changed += set_int(&config->retries, form, "g.retries", 1, 10, errors);
    {
        const auto found = form.find("g.default_destination");
        if (found != form.end() && found->second != config->default_destination)
        {
            if (config->find(found->second) != nullptr)
            {
                config->default_destination = found->second;
                ++changed;
            }
            else if (errors)
            {
                errors->push_back("Unknown destination: " + found->second);
            }
        }
    }
    for (Destination &d : config->destinations)
    {
        const std::string prefix = "d." + d.id + ".";
        changed += set_bool(&d.enabled, form, prefix + "enabled");
        changed += set_text(&d.name, form, prefix + "name");
        if (d.type == DestinationType::discord)
            changed += set_secret(&d.url, form, prefix + "url");
        else
            changed += set_text(&d.url, form, prefix + "url");
        changed += set_secret(&d.token, form, prefix + "token");
        changed += set_text(&d.chat_id, form, prefix + "chat_id");
        changed += set_text(&d.client_id, form, prefix + "client_id");
        changed += set_text(&d.username, form, prefix + "username");
        changed += set_text(&d.message, form, prefix + "message");
        changed += set_text(&d.field, form, prefix + "field");
        changed += set_text(&d.path, form, prefix + "path");
        {
            const auto also = form.find(prefix + "also");
            if (also != form.end())
            {
                const auto at = std::find(config->also_send.begin(), config->also_send.end(), d.id);
                const bool wanted = parse_bool(also->second, at != config->also_send.end());
                if (wanted && at == config->also_send.end())
                {
                    config->also_send.push_back(d.id);
                    ++changed;
                }
                else if (!wanted && at != config->also_send.end())
                {
                    config->also_send.erase(at);
                    ++changed;
                }
            }
        }
        changed += set_int(&d.max_mb, form, prefix + "max_mb", 0,
                           d.service_ceiling_mb() > 0 ? d.service_ceiling_mb() : 100000, errors);
        {
            std::string fallback = d.fallback;
            if (set_text(&fallback, form, prefix + "fallback"))
            {
                if (fallback.empty() || (config->find(fallback) != nullptr && fallback != d.id))
                {
                    d.fallback = fallback;
                    ++changed;
                }
                else if (errors)
                {
                    errors->push_back(d.display_name() + ": no destination called " + fallback);
                }
            }
        }
        {
            std::string time = d.litterbox_time;
            if (set_text(&time, form, prefix + "litterbox_time"))
            {
                if (time == "1h" || time == "12h" || time == "24h" || time == "72h")
                {
                    d.litterbox_time = time;
                    ++changed;
                }
                else if (errors)
                {
                    errors->push_back("Litterbox keeps files for 1h, 12h, 24h or 72h");
                }
            }
        }
        const auto headers = form.find(prefix + "headers");
        if (headers != form.end() && !trim(headers->second).empty())
        {
            std::vector<std::string> lines;
            for (const std::string &line : split(headers->second, '\n'))
            {
                const std::string_view cleaned = trim(line);
                if (!cleaned.empty() && cleaned.find(':') != std::string_view::npos)
                    lines.emplace_back(cleaned);
            }
            if (lines != d.headers)
            {
                d.headers = std::move(lines);
                ++changed;
            }
        }
    }
    return changed;
}

} // namespace pshare
