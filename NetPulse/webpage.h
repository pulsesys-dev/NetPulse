#pragma once
// The page served at http://netpulse.local/ (or the board's IP). Self-contained: no internet needed.
// It reads /api/status and /api/files and links to the download endpoints in NetPulse.ino.
#include <Arduino.h>

static const char WEB_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>NetPulse</title>
<style>
:root{--bg:#05040c;--panel:#0d0b1c;--line:#342864;--dim:#5c5294;--grey:#9288c8;--white:#eceeff;
--cyan:#00f0ff;--pink:#ff00c8;--green:#05ffa1;--yellow:#fcee0a;--red:#ff2a6d;--violet:#966eff}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--white);font:15px/1.45 "Chakra Petch","Rajdhani","Segoe UI",system-ui,sans-serif;letter-spacing:.02em}
header{display:flex;align-items:center;gap:14px;padding:14px 22px;background:#0b0818;border-bottom:1px solid var(--line);position:relative}
header:after{content:"";position:absolute;left:0;bottom:-1px;width:33%;height:1px;background:var(--pink)}
.blk{width:12px;height:12px;background:var(--green)}
h1{margin:0;font-size:22px;letter-spacing:.06em}h1 b{color:var(--pink);margin-right:8px}
header .meta{margin-left:auto;color:var(--grey);font-size:13px;text-align:right}
main{max-width:1100px;margin:0 auto;padding:18px;display:grid;gap:16px}
.panel{background:var(--panel);border:1px solid var(--line);padding:16px 18px;position:relative;
clip-path:polygon(10px 0,100% 0,100% calc(100% - 10px),calc(100% - 10px) 100%,0 100%,0 10px)}
.panel:before{content:"";position:absolute;left:10px;top:0;width:40px;height:1px;background:var(--cyan)}
h2{margin:0 0 12px;font-size:14px;color:var(--pink);letter-spacing:.12em}h2:before{content:"// "}
.tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));gap:10px}
.tile{border:1px solid var(--line);padding:8px 10px}.tile small{display:block;color:var(--grey);font-size:12px;letter-spacing:.1em}
.tile span{font-size:24px;font-weight:700}.tile i{font-style:normal;color:var(--grey);font-size:13px;margin-left:3px}
.status{font-size:28px;font-weight:700;letter-spacing:.05em}
.row{display:flex;flex-wrap:wrap;gap:8px;align-items:center}
button,a.btn{background:transparent;color:var(--cyan);border:1px solid var(--cyan);padding:8px 14px;font:inherit;font-weight:600;
letter-spacing:.08em;cursor:pointer;text-decoration:none;display:inline-block}
button:hover,a.btn:hover{background:var(--cyan);color:var(--bg)}
a.btn.pink{color:var(--pink);border-color:var(--pink)}a.btn.pink:hover{background:var(--pink);color:var(--bg)}
input[type=date]{background:#120f26;color:var(--white);border:1px solid var(--line);padding:7px 8px;font:inherit;color-scheme:dark}
table{width:100%;border-collapse:collapse;font-size:14px}td,th{padding:6px 8px;border-bottom:1px solid #1e1940;text-align:left}
th{color:var(--grey);font-weight:600;font-size:12px;letter-spacing:.1em}td.r,th.r{text-align:right}
td a{color:var(--cyan)}.dim{color:var(--dim)}.note{color:var(--grey);font-size:13px;margin-top:8px}
.kv{display:grid;grid-template-columns:max-content 1fr;gap:4px 16px;font-size:14px}.kv b{color:var(--grey);font-weight:500}
.cols{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:16px}
</style></head><body>
<header><div class="blk" id="blk"></div><h1><b>//</b>NETPULSE</h1>
<div class="meta"><div id="board">connecting...</div><div id="clock" class="dim"></div></div></header>
<main>
<section class="panel"><h2>LIVE</h2>
<div class="row" style="justify-content:space-between;margin-bottom:12px"><div class="status" id="status">--</div><div id="health" class="dim"></div></div>
<div class="tiles">
<div class="tile"><small>PING</small><span id="ping">--</span><i>ms</i></div>
<div class="tile"><small>2ND TARGET</small><span id="ping2">--</span><i>ms</i></div>
<div class="tile"><small>LOSS (5 MIN)</small><span id="loss">--</span><i>%</i></div>
<div class="tile"><small>JITTER</small><span id="jit">--</span><i>ms</i></div>
<div class="tile"><small>ROUTER</small><span id="rtr" style="color:var(--violet)">--</span><i>ms</i></div>
<div class="tile"><small>ISP ROUTER</small><span id="isp" style="color:var(--cyan)">--</span><i>ms</i></div>
<div class="tile"><small>DNS</small><span id="dns">--</span><i>ms</i></div>
<div class="tile"><small>WI-FI</small><span id="rssi">--</span><i>dBm</i></div>
</div><div class="note" id="livenote"></div></section>

<section class="panel"><h2>EXPORT TESTS (MICROSD)</h2>
<div class="row" style="margin-bottom:10px">
<button onclick="quick(0)">TODAY</button><button onclick="quick(6)">LAST 7 DAYS</button>
<button onclick="quick(29)">LAST 30 DAYS</button><button onclick="quick(-1)">EVERYTHING</button></div>
<div class="row"><span class="dim">FROM</span><input type="date" id="from"><span class="dim">TO</span><input type="date" id="to">
<a class="btn pink" id="exp" href="#">DOWNLOAD CSV</a></div>
<div class="note">One CSV with every test (every 10 s) in the range: ping, loss, jitter, router / ISP / DNS times, web check, Wi-Fi signal. Opens in Excel.</div>
<div class="row" style="margin-top:14px"><a class="btn" href="/dl?f=outages.csv">OUTAGES.CSV</a><a class="btn" href="/dl?f=events.csv">EVENTS.CSV</a></div>
<div class="note" id="sdnote"></div></section>

<div class="cols">
<section class="panel"><h2>DAILY FILES</h2><table><thead><tr><th>DAY</th><th class="r">SIZE</th><th></th></tr></thead>
<tbody id="files"><tr><td class="dim" colspan="3">loading...</td></tr></tbody></table></section>
<section class="panel"><h2>ON-BOARD HISTORY</h2>
<div class="note" style="margin:0 0 10px">Kept in the board's flash (no card needed): averages per minute (24 h) or per 15 minutes (14 days).</div>
<div class="row"><a class="btn" href="/history.csv?r=24h">24 HOURS</a><a class="btn" href="/history.csv?r=7d">7 DAYS</a>
<a class="btn" href="/history.csv?r=14d">14 DAYS</a><a class="btn" href="/events-board.csv">EVENTS</a></div>
<h2 style="margin-top:18px">DEVICE</h2><div class="kv" id="dev"></div></section>
</div>
</main>
<script>
const $=id=>document.getElementById(id);
const ymd=d=>`${d.getFullYear()}-${String(d.getMonth()+1).padStart(2,"0")}-${String(d.getDate()).padStart(2,"0")}`;   // local date
let firstDay=null;
function setRange(){ $("exp").href=`/export.csv?from=${$("from").value}&to=${$("to").value}`; }
function quick(n){ const t=new Date(); const f=new Date(); if(n<0&&firstDay){ $("from").value=firstDay; } else { f.setDate(t.getDate()-Math.max(n,0)); $("from").value=ymd(f); }
  $("to").value=ymd(t); setRange(); }
$("from").onchange=$("to").onchange=setRange;
const col={0:"var(--green)",1:"var(--yellow)",2:"var(--red)"};
function v(x){ return x<0?"--":x; }
async function status(){
  try{ const s=await (await fetch("/api/status")).json();
    $("board").textContent=`${s.board} · ${s.fw}${s.upd?` · update ${s.upd} waiting`:""}`;
    $("clock").textContent=s.time?`${s.time} · up ${s.uptime}`:`up ${s.uptime}`;
    $("status").textContent=s.status; $("status").style.color=col[s.level]; $("blk").style.background=col[s.level];
    $("health").textContent=`HEALTH ${s.health}%`;
    $("ping").textContent=v(s.ping); $("ping2").textContent=v(s.ping2); $("loss").textContent=s.loss<0?"--":s.loss.toFixed(s.loss<10?1:0);
    $("jit").textContent=v(s.jitter); $("rtr").textContent=v(s.router); $("isp").textContent=v(s.isp); $("dns").textContent=v(s.dns);
    $("rssi").textContent=s.wifi?s.rssi:"--";
    $("livenote").textContent=`Targets ${s.host1} + ${s.host2}. ISP router ${s.ispip||"not found"}. Public IP ${s.wan||"--"}. Updates every 10 s.`;
    $("sdnote").textContent=s.sd==1?`microSD logging · ${s.sdrows} rows since start`:(s.sd==2?"microSD card error":"No microSD card - use ON-BOARD HISTORY below");
    $("dev").innerHTML=[["Wi-Fi",s.ssid],["IP",s.ip],["Tests run",s.tests],["Pings lost",`${s.lost} / ${s.sent}`],["Free memory",`${s.heap} KB`],["Stack left",`${s.stack} B`]]
      .map(([k,x])=>`<b>${k}</b><span>${x}</span>`).join("");
  }catch(e){ $("board").textContent="board not answering"; }
}
async function files(){
  try{ const f=await (await fetch("/api/files")).json();
    const days=f.filter(x=>/^\d{4}-\d{2}-\d{2}\.csv$/.test(x.n)).sort((a,b)=>b.n.localeCompare(a.n));
    firstDay=days.length?days[days.length-1].n.slice(0,10):null;
    $("files").innerHTML=days.length?days.map(x=>`<tr><td>${x.n.slice(0,10)}</td><td class="r dim">${(x.s/1024).toFixed(0)} KB</td><td class="r"><a href="/dl?f=tests/${x.n}">DOWNLOAD</a></td></tr>`).join("")
      :`<tr><td class="dim" colspan="3">No daily files${f.length?"":" (no microSD card)"}</td></tr>`;
  }catch(e){ $("files").innerHTML=`<tr><td class="dim" colspan="3">couldn't list files</td></tr>`; }
}
quick(0); status(); files(); setInterval(status,10000); setInterval(files,60000);
</script></body></html>)HTML";
