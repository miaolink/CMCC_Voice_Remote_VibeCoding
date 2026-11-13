#include "wifi_web.h"

#include "ble_remote.h"
#include "display_ui.h"
#include "key_learn.h"
#include "sd_log.h"
#include "usb_hid_out.h"
#include "voice_probe.h"

#include <FS.h>
#include <Preferences.h>
#include <SD.h>
#include <WebServer.h>
#include <WiFi.h>

static constexpr const char *kApSsid = "ADV-Bridge";
static constexpr const char *kApPass = "bridge1234";
static constexpr const char *kDefaultStaSsid = "RongHe-wifi6";
static constexpr const char *kDefaultStaPass = "20202020";
static constexpr const char *kLegacyStaSsid = "iot";
static constexpr uint16_t kHttpPort = 80;
static constexpr uint32_t kStaRetryMs = 8000;

static WebServer server(kHttpPort);
static Preferences wifiPrefs;
static bool apUp = false;
static String lastApiNote = "";
static uint32_t lastStaRetryMs = 0;
static bool staAnnounced = false;

static String jsonEscape(const String &s) {
    String o;
    o.reserve(s.length() + 8);
    for (size_t i = 0; i < s.length(); ++i) {
        char c = s[i];
        if (c == '"' || c == '\\') {
            o += '\\';
            o += c;
        } else if (c == '\n') {
            o += "\\n";
        } else if ((uint8_t)c < 0x20) {
            continue;
        } else {
            o += c;
        }
    }
    return o;
}

static void sendJson(int code, const String &body) {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    server.send(code, "application/json", body);
}

static void handleOptions() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    server.send(204);
}

static String statusJson() {
    String ssid, pass;
    wifiLoadSta(ssid, pass);
    String devices = "[";
    for (int i = 0; i < bleDeviceCount(); ++i) {
        BleDeviceInfo d;
        if (!bleGetDevice(i, d)) continue;
        if (i) devices += ",";
        devices += "{\"i\":";
        devices += String(i);
        devices += ",\"name\":\"";
        devices += jsonEscape(d.name);
        devices += "\",\"addr\":\"";
        devices += jsonEscape(d.addr);
        devices += "\",\"rssi\":";
        devices += String(d.rssi);
        devices += "}";
    }
    devices += "]";

    String j = "{";
    j += "\"ok\":true,";
    j += "\"wifi\":{";
    j += "\"mode\":\"";
    j += wifiModeText();
    j += "\",\"sta\":";
    j += wifiStaConnected() ? "true" : "false";
    j += ",\"ap\":";
    j += wifiApUp() ? "true" : "false";
    j += ",\"sta_ip\":\"";
    j += wifiStaIp();
    j += "\",\"ap_ip\":\"";
    j += wifiApIp();
    j += "\",\"sta_ssid\":\"";
    j += jsonEscape(wifiStaSsid());
    j += "\",\"saved_ssid\":\"";
    j += jsonEscape(ssid);
    j += "\",\"ap_ssid\":\"";
    j += kApSsid;
    j += "\"},";
    j += "\"ble\":{";
    j += "\"connected\":";
    j += bleRemoteConnected() ? "true" : "false";
    j += ",\"scanning\":";
    j += bleIsScanning() ? "true" : "false";
    j += ",\"busy\":";
    j += bleIsBusy() ? "true" : "false";
    j += ",\"default_addr\":\"";
    j += kDefaultCmccAddr;
    j += "\"";
    j += ",\"peer\":\"";
    j += jsonEscape(bleRemotePeerName());
    j += "\",\"addr\":\"";
    j += jsonEscape(bleRemotePeerAddr());
    j += "\",\"devices\":";
    j += devices;
    j += "},";
    j += "\"hid\":";
    j += usbHidReady() ? "true" : "false";
    j += ",\"sd\":{";
    j += "\"mounted\":";
    j += sdMounted() ? "true" : "false";
    j += ",\"run\":\"";
    j += sdRunPath();
    j += "\",\"debug\":\"";
    j += sdDebugPath();
    j += "\"},";
    j += "\"note\":\"";
    j += jsonEscape(lastApiNote);
    j += "\",\"uptime_ms\":";
    j += String(millis());
    j += "}";
    return j;
}

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>ADV Bridge</title>
<style>
body{font-family:system-ui,sans-serif;margin:12px;background:#0f1419;color:#e7ecf3}
h1{font-size:1.2rem;margin:0 0 8px}
h2{font-size:1rem;margin:16px 0 8px;color:#8ec8ff}
.card{background:#1a2332;border-radius:10px;padding:12px;margin-bottom:12px}
label{display:block;font-size:.85rem;color:#9aa7b8;margin:6px 0 2px}
input,button,select{font:inherit;padding:8px 10px;border-radius:8px;border:1px solid #334}
input,select{width:100%;box-sizing:border-box;background:#0f1419;color:#e7ecf3}
button{background:#2b6cb0;color:#fff;border:none;margin:4px 4px 0 0;cursor:pointer}
button.sec{background:#334155}
button.danger{background:#b03a2b}
pre{white-space:pre-wrap;word-break:break-all;background:#0b1220;padding:8px;border-radius:8px;font-size:.75rem}
.row{display:flex;flex-wrap:wrap;gap:4px;margin-top:8px}
.ok{color:#3ddc97}.bad{color:#ff6b6b}
</style>
</head>
<body>
<h1>ADV CMCC Bridge</h1>
<p><a href="/learn" style="color:#8ec8ff">21键学习 →</a>
 &nbsp; <a href="/voice" style="color:#8ec8ff">MIC语音探测 →</a></p>
<div class="card">
  <div id="sum">loading...</div>
  <div class="row">
    <button onclick="refresh()">刷新状态</button>
    <button onclick="api('POST','/api/ble/autopair')">自动配对CMCC</button>
    <button class="sec" onclick="api('POST','/api/hid/run')">Run HID</button>
    <button class="sec" onclick="api('POST','/api/ble/scan',{ms:8000})">BLE扫描</button>
    <button class="danger" onclick="api('POST','/api/ble/disconnect')">断开BLE</button>
  </div>
</div>

<div class="card">
  <h2>WiFi 设置</h2>
  <label>SSID (接入模式 STA)</label><input id="ssid" placeholder="RongHe-wifi6"/>
  <label>密码</label><input id="pass" type="password" placeholder="20202020"/>
  <div class="row">
    <button onclick="saveWifi()">保存并接入</button>
    <button class="sec" onclick="api('POST','/api/wifi/sta')">仅STA</button>
    <button class="sec" onclick="api('POST','/api/wifi/ap')">开启AP</button>
    <button class="danger" onclick="api('POST','/api/wifi/clear')">恢复默认SSID</button>
  </div>
  <p style="font-size:.8rem;color:#9aa7b8;margin:8px 0 0">
    默认接入: RongHe-wifi6 / 20202020（STA）<br/>
    备用热点 AP: ADV-Bridge / bridge1234 → http://192.168.4.1
  </p>
</div>

<div class="card">
  <h2>BLE 设备</h2>
  <div id="devs">-</div>
  <div class="row">
    <button class="sec" onclick="api('POST','/api/ble/reconnect')">重连上次</button>
    <button class="sec" onclick="api('POST','/api/ble/stop')">停止扫描</button>
  </div>
</div>

<div class="card">
  <h2>HID 调试</h2>
  <div class="row">
    <button onclick="hid('up')">↑</button>
    <button onclick="hid('down')">↓</button>
    <button onclick="hid('left')">←</button>
    <button onclick="hid('right')">→</button>
  </div>
</div>

<div class="card">
  <h2>SD 日志</h2>
  <div class="row">
    <button class="sec" onclick="loadLog('run')">run.log</button>
    <button class="sec" onclick="loadLog('debug')">debug.log</button>
  </div>
  <pre id="logbox">SD: /logs/run.log  /logs/debug.log</pre>
</div>

<div class="card">
  <h2>原始 JSON</h2>
  <pre id="raw">{}</pre>
</div>

<script>
async function api(method, path, body){
  const opt={method, headers:{}};
  if(body!==undefined){ opt.headers['Content-Type']='application/json'; opt.body=JSON.stringify(body); }
  const r=await fetch(path, opt);
  const j=await r.json().catch(()=>({ok:false,error:'bad json'}));
  document.getElementById('raw').textContent=JSON.stringify(j,null,2);
  await refresh();
  return j;
}
async function refresh(){
  const j=await api('GET','/api/status');
  if(!j || !j.wifi) return;
  const w=j.wifi, b=j.ble;
  document.getElementById('sum').innerHTML =
    `WiFi: <b>${w.mode}</b> STA ${w.sta?'<span class=ok>ON '+w.sta_ip+'</span>':'<span class=bad>OFF</span>'} `+
    `AP ${w.ap?'<span class=ok>ON '+w.ap_ip+'</span>':'OFF'}<br/>`+
    `BLE: ${b.connected?'<span class=ok>已连接 '+esc(b.peer)+'</span>':'未连接'}`+
    (b.scanning?' · 扫描中':'')+
    ` · HID: ${j.hid?'<span class=ok>ON</span>':'OFF'}`;
  if(w.saved_ssid) document.getElementById('ssid').value=w.saved_ssid;
  let html='';
  (b.devices||[]).forEach(d=>{
    html += `<div style="margin:6px 0"><b>${esc(d.name)}</b> <small>${d.rssi}dBm</small><br/><code>${esc(d.addr)}</code>
      <button onclick="api('POST','/api/ble/connect',{index:${d.i}})">连接</button></div>`;
  });
  document.getElementById('devs').innerHTML = html || '无设备，先扫描';
}
function esc(s){return String(s||'').replace(/[&<>"']/g,c=>({ '&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;' }[c]));}
async function saveWifi(){
  await api('POST','/api/wifi',{ssid:document.getElementById('ssid').value, pass:document.getElementById('pass').value});
}
async function hid(key){ await api('POST','/api/hid/test',{key, ms:120}); }
async function loadLog(file){
  const r=await fetch('/api/logs?file='+file+'&n=80');
  const t=await r.text();
  document.getElementById('logbox').textContent=t;
}
refresh();
setInterval(refresh, 3000);
</script>
</body>
</html>
)HTML";

static String bodyJson() {
    if (server.hasArg("plain")) return server.arg("plain");
    return "";
}

static String jsonGetString(const String &body, const char *key) {
    String pat = String("\"") + key + "\"";
    int k = body.indexOf(pat);
    if (k < 0) return "";
    int c = body.indexOf(':', k + pat.length());
    if (c < 0) return "";
    int q1 = body.indexOf('"', c + 1);
    if (q1 < 0) return "";
    int q2 = q1 + 1;
    while (q2 < (int)body.length()) {
        if (body[q2] == '"' && body[q2 - 1] != '\\') break;
        q2++;
    }
    return body.substring(q1 + 1, q2);
}

static int jsonGetInt(const String &body, const char *key, int defV) {
    String pat = String("\"") + key + "\"";
    int k = body.indexOf(pat);
    if (k < 0) return defV;
    int c = body.indexOf(':', k + pat.length());
    if (c < 0) return defV;
    return body.substring(c + 1).toInt();
}

static bool jsonGetBool(const String &body, const char *key, bool defV) {
    String pat = String("\"") + key + "\"";
    int k = body.indexOf(pat);
    if (k < 0) return defV;
    int c = body.indexOf(':', k + pat.length());
    if (c < 0) return defV;
    String rest = body.substring(c + 1);
    rest.trim();
    if (rest.startsWith("true")) return true;
    if (rest.startsWith("false")) return false;
    return defV;
}

static const char LEARN_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>21键学习</title>
<style>
body{font-family:system-ui,sans-serif;margin:12px;background:#0f1419;color:#e7ecf3}
.card{background:#1a2332;border-radius:10px;padding:12px;margin-bottom:12px}
button{font:inherit;padding:8px 10px;border-radius:8px;border:0;background:#2b6cb0;color:#fff;margin:2px;cursor:pointer}
button.sec{background:#334155}button.danger{background:#b03a2b}button.on{background:#b08900}
.big{font-size:1.4rem;padding:14px;text-align:center;background:#0b1220;border-radius:12px;margin:10px 0}
.warn{color:#ffd166}table{width:100%;border-collapse:collapse;font-size:.8rem}
td,th{border-bottom:1px solid #334;padding:5px;text-align:left;vertical-align:middle}
code{color:#9cdcfe}a{color:#8ec8ff}
input[type=text]{width:110px;background:#0b1220;color:#e7ecf3;border:1px solid #445;border-radius:6px;padding:6px}
</style>
</head>
<body>
<a href="/">← 返回</a>
<h1>遥控器 21 键 + USB HID</h1>
<div class="card">
  <div class="big" id="prompt">未开始</div>
  <div id="hint" class="warn"></div>
  <button onclick="startLearn()">学习全部(21)</button>
  <button class="sec" onclick="startLearnNew()">只学新增5键</button>
  <button class="sec" onclick="learnSkip()">跳过/下一键</button>
  <button class="sec" onclick="saveMap()">保存</button>
  <button class="danger" onclick="clearMap()">清空</button>
  <div style="margin-top:10px;display:flex;gap:8px;flex-wrap:wrap;align-items:center">
    <label>录指定键</label>
    <select id="pickKey"></select>
    <button onclick="learnOne()">开始录此键</button>
  </div>
  <div style="margin-top:8px;font-size:.85rem;color:#9aa7b8">卡在「松开」时会约0.7s自动下跳；也可点「跳过」或表格行内「录此键」。USB列可后填。</div>
</div>
<div class="card" style="overflow:auto">
<table>
<thead><tr><th>#</th><th>遥控</th><th>BLE HEX</th><th>USB HID 输出</th></tr></thead>
<tbody id="tbl"></tbody>
</table>
</div>
<div class="card"><h3>JSON</h3>
<textarea id="jsonbox" rows="5" style="width:100%;background:#0b1220;color:#e7ecf3;border:1px solid #334;border-radius:8px"></textarea>
<button class="sec" onclick="uploadJson()">上传应用</button>
</div>
<script>
let step=0, captureUsb=-1;
const CODE_MAP={ArrowUp:'ArrowUp',ArrowDown:'ArrowDown',ArrowLeft:'ArrowLeft',ArrowRight:'ArrowRight',Enter:'Enter',Escape:'Escape',Backspace:'Backspace',Tab:'Tab',PageUp:'PageUp',PageDown:'PageDown',Home:'Home',End:'End',Insert:'Insert',Delete:'Delete',ContextMenu:'Menu',Space:'Space',F1:'F1',F2:'F2',F3:'F3',F4:'F4',F5:'F5',F6:'F6',F7:'F7',F8:'F8',F9:'F9',F10:'F10',F11:'F11',F12:'F12'};
async function jget(u){const r=await fetch(u);return r.json();}
async function jpost(u,b){const r=await fetch(u,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b||{})});return r.json();}
async function startLearn(){await jpost('/api/map/learn/start',{step:0,one:false});refresh();}
async function startLearnNew(){await jpost('/api/map/learn/start',{step:16,one:false});refresh();}
async function learnOne(i){
  if(i===undefined||i===null){const el=document.getElementById('pickKey');i=parseInt(el.value,10);}
  await jpost('/api/map/learn/start',{step:i,one:true});refresh();
}
async function learnSkip(){await jpost('/api/map/learn/skip');refresh();}
async function saveMap(){await jpost('/api/map/save');refresh();}
async function clearMap(){if(!confirm('清空?'))return;await jpost('/api/map/clear');refresh();}
async function uploadJson(){await fetch('/api/map/upload',{method:'POST',headers:{'Content-Type':'application/json'},body:document.getElementById('jsonbox').value});refresh();}
function fillPick(keys){
  const el=document.getElementById('pickKey'); if(!el) return;
  const cur=el.value;
  el.innerHTML=(keys||[]).map(k=>`<option value="${k.i}">${k.i+1}. ${k.zh} (${k.id})</option>`).join('');
  if(cur!=='') el.value=cur;
}
async function setUsb(i,label){
  if(!label) return;
  const r=await jpost('/api/map/usb',{i,label});
  if(r && r.error){document.getElementById('hint').textContent='无效: '+label;return;}
  captureUsb=-1; document.getElementById('hint').textContent=''; refresh();
}
function cancelCapture(){captureUsb=-1;document.getElementById('hint').textContent='';refresh();}
function armUsb(i){
  captureUsb=i;
  document.getElementById('hint').textContent='录入 USB['+(i+1)+']：按目标键/组合键；点「取消」退出（Escape 可录入）';
  refreshTable(window.__keys||[]);
}
function manualUsb(i){
  const v=document.getElementById('usb'+i).value.trim();
  if(v) setUsb(i,v);
}
function keyLabelFromEvent(e){
  if(CODE_MAP[e.code]) return CODE_MAP[e.code];
  if(e.code && e.code.startsWith('Digit')) return e.code.slice(5);
  if(e.code && e.code.startsWith('Numpad') && e.code.length===7) return e.code.slice(6);
  if(e.key && e.key.length===1) return e.key;
  return e.code || e.key;
}
window.addEventListener('keydown',e=>{
  if(captureUsb<0) return;
  if(e.key==='Control'||e.key==='Shift'||e.key==='Alt'||e.key==='Meta') return;
  e.preventDefault(); e.stopPropagation();
  const parts=[];
  if(e.ctrlKey) parts.push('Ctrl');
  if(e.shiftKey) parts.push('Shift');
  if(e.altKey) parts.push('Alt');
  if(e.metaKey) parts.push('Meta');
  parts.push(keyLabelFromEvent(e));
  setUsb(captureUsb, parts.join('+'));
}, true);
function refreshTable(keys){
  document.getElementById('tbl').innerHTML=keys.map(k=>{
    const on=captureUsb===k.i?' on':'';
    const cancel=captureUsb===k.i?`<button class="danger" onclick="cancelCapture()">取消</button>`:'';
    return `<tr>
      <td>${k.i+1}</td>
      <td>${k.zh}<br/><small>${k.id}</small></td>
      <td><code>${k.learned?k.hex:'—'}</code>
        <button class="sec" onclick="learnOne(${k.i})">录此键</button></td>
      <td>
        <input id="usb${k.i}" type="text" value="${k.usb_label||''}" placeholder="Home / Ctrl+0"/>
        <button class="sec${on}" onclick="armUsb(${k.i})">${captureUsb===k.i?'录入中…':'录入'}</button>
        <button class="sec" onclick="manualUsb(${k.i})">应用</button>
        ${cancel}
      </td></tr>`;
  }).join('');
}
function editingUsb(){
  const el=document.activeElement;
  return el && el.id && el.id.startsWith('usb');
}
async function refresh(){
  const m=await jget('/api/map');
  step=m.step||0; window.__keys=m.keys||[];
  const keys=m.keys||[]; const cur=keys[step];
  let prompt='遥控报告学习 / USB 输出编辑';
  if(m.active){
    if(m.wait_release){prompt='已录遥控: '+(cur?cur.zh:'')+' — 松开(无包约0.7s自动下跳)';}
    else if(m.armed){prompt='请按遥控: ['+(step+1)+'/'+(keys.length||21)+'] '+(cur?cur.zh:'')+(m.one_shot?' [单键]':'');}
  }
  document.getElementById('prompt').textContent=prompt;
  if(captureUsb<0 && !editingUsb()){
    if(!document.getElementById('hint').textContent.startsWith('无效'))
      document.getElementById('hint').textContent='';
    fillPick(keys);
    refreshTable(keys);
    if(document.activeElement!==document.getElementById('jsonbox'))
      document.getElementById('jsonbox').value=JSON.stringify(m,null,2);
  } else if(captureUsb>=0) {
    refreshTable(keys);
  }
}
refresh(); setInterval(()=>{ if(captureUsb<0 && !editingUsb()) refresh(); }, 1200);
</script>
</body></html>
)HTML";

static const char VOICE_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>MIC 语音探测</title>
<style>
body{font-family:system-ui,sans-serif;margin:12px;background:#0f1419;color:#e7ecf3}
.card{background:#1a2332;border-radius:10px;padding:12px;margin-bottom:12px}
button{font:inherit;padding:10px 12px;border-radius:8px;border:0;background:#2b6cb0;color:#fff;margin:4px 4px 0 0;cursor:pointer}
button.sec{background:#334155}button.danger{background:#b03a2b}button.on{background:#b08900}button.mic{background:#0d9488}
.big{font-size:1.3rem;padding:12px;background:#0b1220;border-radius:10px;margin:8px 0}
.ok{color:#3ddc97}.warn{color:#ffd166}.bad{color:#ff6b6b}
pre{white-space:pre-wrap;word-break:break-all;background:#0b1220;padding:8px;border-radius:8px;font-size:.72rem;max-height:340px;overflow:auto}
a{color:#8ec8ff}table{width:100%;border-collapse:collapse;font-size:.75rem}
td,th{border-bottom:1px solid #334;padding:4px;text-align:left;vertical-align:top}
code{color:#9cdcfe}
</style>
</head>
<body>
<a href="/">← 返回</a>
<h1>MIC / 语音探测</h1>
<div class="card">
  <div>流程：1) 自动配对 2) <strong>开始抓包</strong>（不主动开麦）3) <strong>按住语音键</strong>→固件收到按键后再写 0xFB=01 开麦；松开关麦。看 <code>fc_like</code> / pps。</div>
  <div class="big" id="verdict">未开始</div>
  <div id="stats" class="warn"></div>
  <div id="micnote" class="ok"></div>
  <div class="row">
    <button id="btnStart" onclick="startProbe()">开始抓包</button>
    <button class="danger" onclick="stopProbe()">停止</button>
    <button class="mic" onclick="mic(true)">强制开麦</button>
    <button class="sec" onclick="mic(false)">强制停麦</button>
    <button class="sec" onclick="loadCatalog()">Report Map</button>
    <button class="sec" onclick="loadGatt()">刷新GATT</button>
    <button class="sec" onclick="api('POST','/api/ble/autopair')">自动配对</button>
  </div>
</div>
<div class="card">
  <h3>实时包（新→旧）</h3>
  <pre id="ev"></pre>
</div>
<div class="card">
  <h3>HID Report Map / 0xFC·0xFB</h3>
  <pre id="catalog">点「Report Map」</pre>
</div>
<div class="card">
  <h3>GATT 服务</h3>
  <pre id="gatt">点「刷新GATT」</pre>
</div>
<script>
async function jget(u){const r=await fetch(u);return r.json();}
async function jpost(u,b){const r=await fetch(u,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b||{})});return r.json();}
async function api(m,u,b){await fetch(u,{method:m,headers:{'Content-Type':'application/json'},body:JSON.stringify(b||{})});}
async function mic(on){
  const r=await jpost(on?'/api/voice/mic_on':'/api/voice/mic_off');
  document.getElementById('micnote').textContent=(r.ok?'✓ ':'✗ ')+(r.detail||JSON.stringify(r));
}
let starting=false;
async function startProbe(){
  if(starting) return;
  starting=true;
  try{
    const r=await jpost('/api/voice/start');
    if(r.error){document.getElementById('micnote').textContent='✗ '+r.error;return;}
    document.getElementById('btnStart').classList.add('on');
    document.getElementById('verdict').textContent='抓包中 — 请按住语音键（收到按键后才开麦）';
    document.getElementById('micnote').textContent='等待语音键 PTT…';
  } finally { starting=false; }
}
async function stopProbe(){
  await jpost('/api/voice/stop');
  document.getElementById('btnStart').classList.remove('on');
  await refresh();
}
async function subAll(){const r=await jpost('/api/voice/subscribe_all');document.getElementById('stats').textContent='已订阅 notify: '+(r.subscribed||0);}
async function loadCatalog(){
  const c=await jget('/api/voice/hid_catalog');
  if(!c.connected){document.getElementById('catalog').textContent='未连接 BLE';return;}
  let t='map_len='+c.map_len+' has_FC='+c.has_fc+' has_FB='+c.has_fb+'\n';
  (c.reports||[]).forEach(r=>{
    t+=' id=0x'+r.id_hex+' '+r.type_name+' h=0x'+r.handle.toString(16)+' N='+r.notify+' W='+r.write+'\n';
  });
  t+='\nmap_hex:\n'+(c.map_hex||'(empty)');
  document.getElementById('catalog').textContent=t;
}
async function loadGatt(){
  const g=await jget('/api/voice/gatt');
  if(!g.connected){document.getElementById('gatt').textContent='未连接 BLE';return;}
  let t='peer '+g.peer+' '+g.addr+'\n';
  (g.services||[]).forEach(s=>{
    t+= '\nSVC '+s.uuid+'\n';
    (s.chars||[]).forEach(c=>{
      const f=[c.notify?'N':'',c.indicate?'I':'',c.read?'R':'',c.write?'W':''].filter(Boolean).join('');
      t+='  '+c.uuid+' ['+f+']\n';
    });
  });
  document.getElementById('gatt').textContent=t;
}
async function refresh(){
  const s=await jget('/api/voice/status');
  const e=await jget('/api/voice/events?n=50');
  let v='未开始';
  if(s.active) v='抓包中… 请按住 MIC';
  else if(s.likely_audio) v='✓ 疑似 0xFC 音频流';
  else if(s.pkt>0) v='已停止 — 多为按键/低频包';
  document.getElementById('verdict').innerHTML=s.likely_audio?'<span class=ok>'+v+'</span>':(s.active?'<span class=warn>'+v+'</span>':v);
  document.getElementById('stats').textContent=
    `pkt=${s.pkt||0} fc_like=${s.fc_like||0} bytes=${s.bytes||0} dur=${s.dur_ms||0}ms pps≈${s.pps||0} avg_len=${s.avg_len||0} · ${s.hint||''}`;
  const lines=(e.events||[]).map(x=>`#${x.seq} t=${x.ms} len=${x.len} ${x.uuid}\n  ${x.hex}`).join('\n');
  document.getElementById('ev').textContent=lines||'(空 — 开麦后开始抓包并按住 MIC)';
  if(s.active) document.getElementById('btnStart').classList.add('on');
  else document.getElementById('btnStart').classList.remove('on');
}
refresh(); setInterval(refresh, 800);
</script>
</body></html>
)HTML";

static void handleRoot() { server.send_P(200, "text/html", INDEX_HTML); }
static void handleLearnPage() { server.send_P(200, "text/html", LEARN_HTML); }
static void handleVoicePage() { server.send_P(200, "text/html", VOICE_HTML); }

static void handleVoiceStatus() {
    sendJson(200, voiceProbeStatusJson());
}
static void handleVoiceEvents() {
    int n = 40;
    if (server.hasArg("n")) n = server.arg("n").toInt();
    sendJson(200, voiceProbeEventsJson(n));
}
static uint32_t gVoiceStartMs = 0;

static void handleVoiceStart() {
    if (!bleRemoteConnected()) {
        sendJson(409, "{\"ok\":false,\"error\":\"ble not connected\"}");
        return;
    }
    // Debounce: page refresh / double-click used to flood ATT and drop link
    if (voiceProbeActive() && (millis() - gVoiceStartMs) < 2500) {
        sendJson(200, voiceProbeStatusJson());
        return;
    }
    // Arm capture only — do NOT open mic here.
    // PTT: voice-key notify → deferred FB=01; release → FB=00 (see ble_remote).
    voiceProbeStart();
    gVoiceStartMs = millis();
    lastApiNote = "voice probe on (PTT wait key)";
    uiSetStatus("voice probe", "hold voice key");
    sendJson(200, voiceProbeStatusJson());
}
static void handleVoiceStop() {
    voiceProbeStop();
    lastApiNote = "voice probe off";
    uiSetStatus("voice stop", "");
    sendJson(200, voiceProbeStatusJson());
}
static void handleVoiceGatt() { sendJson(200, bleGattDumpJson()); }
static void handleVoiceHidCatalog() { sendJson(200, bleHidReportCatalogJson()); }
static void handleVoiceMicOn() {
    String d;
    bool ok = bleVoiceMicEnable(true, d);
    sendJson(200, String("{\"ok\":") + (ok ? "true" : "false") + ",\"detail\":\"" + d + "\"}");
}
static void handleVoiceMicOff() {
    String d;
    bool ok = bleVoiceMicEnable(false, d);
    sendJson(200, String("{\"ok\":") + (ok ? "true" : "false") + ",\"detail\":\"" + d + "\"}");
}
static void handleVoiceSubAll() {
    int n = bleSubscribeAllNotifies();
    sendJson(200, String("{\"ok\":true,\"subscribed\":") + n + "}");
}

static void handleMapGet() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", keyLearnMapJson());
}

static void handleMapLearnStart() {
    String body = bodyJson();
    int step = jsonGetInt(body, "step", 0);
    bool one = jsonGetBool(body, "one", false);
    keyLearnStart(step, one);
    lastApiNote = String("learn step ") + step + (one ? " one" : "");
    sendJson(200, keyLearnMapJson());
}

static void handleMapLearnSkip() {
    keyLearnSkip();
    lastApiNote = "learn skip";
    sendJson(200, keyLearnMapJson());
}

static void handleMapSave() {
    keyLearnSave();
    lastApiNote = "map saved";
    sendJson(200, keyLearnMapJson());
}

static void handleMapClear() {
    keyLearnClearAll();
    lastApiNote = "map cleared";
    sendJson(200, keyLearnMapJson());
}

static void handleMapUpload() {
    String body = bodyJson();
    bool ok = keyLearnApplyJson(body);
    lastApiNote = ok ? "map uploaded" : "map upload fail";
    sendJson(ok ? 200 : 400, keyLearnMapJson());
}

static void handleMapUsb() {
    String body = bodyJson();
    int idx = jsonGetInt(body, "i", -1);
    String label = jsonGetString(body, "label");
    if (idx < 0 || idx >= kRemoteKeyCount || !label.length()) {
        sendJson(400, "{\"ok\":false,\"error\":\"i+label required\"}");
        return;
    }
    UsbOutKind kind;
    uint16_t code;
    uint8_t mods = 0;
    if (!keyLearnParseUsbLabel(label.c_str(), kind, code, mods)) {
        sendJson(400, "{\"ok\":false,\"error\":\"unknown label\"}");
        return;
    }
    keyLearnSetUsb(idx, kind, code, mods, label.c_str());
    keyLearnSave();
    lastApiNote = String("usb ") + idx + "=" + label;
    sendJson(200, keyLearnMapJson());
}

static void handleHidReports() {
    String j = "{\"reports\":[";
    int n = keyLearnRecentCount();
    if (n > 12) n = 12;
    for (int i = 0; i < n; ++i) {
        HidReportSample s;
        if (!keyLearnRecentAt(i, s)) continue;
        if (i) j += ',';
        j += "{\"ms\":";
        j += String(s.ms);
        j += ",\"len\":";
        j += String((int)s.len);
        j += ",\"zero\":";
        j += s.isZero ? "true" : "false";
        j += ",\"hex\":\"";
        for (int b = 0; b < s.len; ++b) {
            if (b) j += ' ';
            char buf[4];
            snprintf(buf, sizeof(buf), "%02X", s.data[b]);
            j += buf;
        }
        j += "\"}";
    }
    j += "]}";
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", j);
}

static void handleStatus() { sendJson(200, statusJson()); }

static void handleWifiSave() {
    String body = bodyJson();
    String ssid = jsonGetString(body, "ssid");
    String pass = jsonGetString(body, "pass");
    if (!ssid.length()) {
        sendJson(400, "{\"ok\":false,\"error\":\"ssid required\"}");
        return;
    }
    wifiSaveSta(ssid.c_str(), pass.c_str());
    lastApiNote = "wifi saved, connecting...";
    uiSetStatus("wifi join", ssid.c_str());
    slog("wifi save ssid=%s", ssid.c_str());
    sendJson(200, "{\"ok\":true,\"msg\":\"saved, connecting\"}");
    // connect after response
    wifiReconnectSta();
}

static void handleWifiClear() {
    wifiSaveSta(kDefaultStaSsid, kDefaultStaPass);
    lastApiNote = String("reset to ") + kDefaultStaSsid;
    uiSetStatus("wifi", kDefaultStaSsid);
    sendJson(200, String("{\"ok\":true,\"ssid\":\"") + kDefaultStaSsid + "\"}");
    wifiReconnectSta();
}

static void ensureAp() {
    wifi_mode_t mode = WiFi.getMode();
    if (mode == WIFI_STA) WiFi.mode(WIFI_AP_STA);
    else if (mode == WIFI_OFF) WiFi.mode(WIFI_AP_STA);
    if (!apUp) {
        apUp = WiFi.softAP(kApSsid, kApPass);
        Serial.printf("[WiFi] AP on %s ip=%s\n", kApSsid, WiFi.softAPIP().toString().c_str());
    }
}

static void handleWifiAp() {
    ensureAp();
    lastApiNote = String("AP ") + kApSsid;
    uiSetStatus("AP on", WiFi.softAPIP().toString().c_str());
    sendJson(200, statusJson());
}

static void handleWifiStaOnly() {
    if (apUp) {
        WiFi.softAPdisconnect(true);
        apUp = false;
    }
    WiFi.mode(WIFI_STA);
    wifiReconnectSta();
    lastApiNote = "STA only";
    uiSetStatus("STA only", kDefaultStaSsid);
    sendJson(200, statusJson());
}

static void handleBleScan() {
    String body = bodyJson();
    int ms = jsonGetInt(body, "ms", 8000);
    if (server.hasArg("ms")) ms = server.arg("ms").toInt();
    bleStartScan(ms > 0 ? ms : 8000);
    lastApiNote = "scan started";
    uiSetStatus("scanning", String(ms).c_str());
    sendJson(200, "{\"ok\":true,\"scanning\":true}");
}

static void handleBleStop() {
    bleStopScan();
    lastApiNote = "scan stop";
    sendJson(200, statusJson());
}

static void handleBleConnect() {
    String body = bodyJson();
    int index = jsonGetInt(body, "index", -1);
    String addr = jsonGetString(body, "addr");
    String name = jsonGetString(body, "name");
    if (index >= 0) {
        bleRequestConnectIndex(index);
        lastApiNote = String("queue index ") + index;
    } else if (addr.length()) {
        bleRequestConnectAddr(addr.c_str(), name.c_str());
        lastApiNote = String("queue ") + addr;
    } else {
        sendJson(400, "{\"ok\":false,\"error\":\"index or addr required\"}");
        return;
    }
    uiSetStatus("queue connect", lastApiNote.c_str());
    sendJson(202, statusJson());
}

static void handleBleAutopair() {
    bleAutoPairCmcc();
    lastApiNote = String("autopair ") + kDefaultCmccAddr;
    uiSetStatus("autopair", kDefaultCmccName);
    sendJson(202, statusJson());
}

static void handleBleDisconnect() {
    bleDisconnect();
    lastApiNote = "ble disconnect";
    uiSetStatus("disconnected", "");
    sendJson(200, statusJson());
}

static void handleBleReconnect() {
    String addr, name;
    if (!bleLoadLastPeer(addr, name)) {
        addr = kDefaultCmccAddr;
        name = kDefaultCmccName;
    }
    bleRequestConnectAddr(addr.c_str(), name.c_str());
    lastApiNote = "queue reconnect " + name;
    sendJson(202, statusJson());
}

static void handleHidRun() {
    if (!usbHidReady()) usbHidBegin();
    lastApiNote = "HID ON";
    uiSetStatus("HID ON", "via web");
    sendJson(200, statusJson());
}

static void handleLogs() {
    if (!sdMounted()) {
        server.send(503, "text/plain", "SD not mounted");
        return;
    }
    sdLogFlush();
    String which = server.hasArg("file") ? server.arg("file") : "run";
    int n = server.hasArg("n") ? server.arg("n").toInt() : 60;
    if (n < 1) n = 1;
    if (n > 200) n = 200;
    const char *path = (which == "debug") ? sdDebugPath() : sdRunPath();
    File f = SD.open(path, FILE_READ);
    if (!f) {
        server.send(404, "text/plain", "log missing");
        return;
    }
    // Read all into buffer if small; else keep last ~32KB then split lines
    size_t sz = f.size();
    size_t start = 0;
    const size_t cap = 24 * 1024;
    if (sz > cap) start = sz - cap;
    f.seek(start);
    String data;
    data.reserve(cap + 16);
    while (f.available()) {
        data += (char)f.read();
    }
    f.close();

    // Keep last n lines
    int lines = 0;
    int idx = data.length();
    while (idx > 0 && lines < n) {
        idx--;
        if (data[idx] == '\n') lines++;
    }
    if (idx > 0 && data[idx] == '\n') idx++;
    String tail = data.substring(idx);
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "text/plain; charset=utf-8", tail);
}

static void handleHidTest() {
    if (!usbHidReady()) {
        sendJson(409, "{\"ok\":false,\"error\":\"HID not started; POST /api/hid/run first\"}");
        return;
    }
    String body = bodyJson();
    String key = jsonGetString(body, "key");
    if (!key.length() && server.hasArg("key")) key = server.arg("key");
    int ms = jsonGetInt(body, "ms", 100);
    ArrowKey ak = ArrowKey::None;
    if (key == "up") ak = ArrowKey::Up;
    else if (key == "down") ak = ArrowKey::Down;
    else if (key == "left") ak = ArrowKey::Left;
    else if (key == "right") ak = ArrowKey::Right;
    if (ak == ArrowKey::None) {
        sendJson(400, "{\"ok\":false,\"error\":\"key=up|down|left|right\"}");
        return;
    }
    usbHidPressArrow(ak);
    uiSetLastKey(ak);
    delay(ms > 0 ? ms : 100);
    usbHidReleaseArrow();
    uiSetLastKey(ArrowKey::None);
    lastApiNote = String("hid test ") + key;
    sendJson(200, "{\"ok\":true,\"key\":\"" + key + "\"}");
}

void wifiSaveSta(const char *ssid, const char *pass) {
    wifiPrefs.putString("ssid", ssid ? ssid : "");
    wifiPrefs.putString("pass", pass ? pass : "");
}

void wifiLoadSta(String &ssid, String &pass) {
    ssid = wifiPrefs.getString("ssid", "");
    pass = wifiPrefs.getString("pass", "");
    if (!ssid.length()) {
        ssid = kDefaultStaSsid;
        pass = kDefaultStaPass;
    }
}

void wifiClearSta() {
    wifiSaveSta(kDefaultStaSsid, kDefaultStaPass);
}

void wifiReconnectSta() {
    String ssid, pass;
    wifiLoadSta(ssid, pass);
    if (!ssid.length()) {
        ssid = kDefaultStaSsid;
        pass = kDefaultStaPass;
    }
    // Keep current AP if already up; otherwise STA-only (default).
    if (apUp) WiFi.mode(WIFI_AP_STA);
    else WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, false);
    delay(50);
    WiFi.begin(ssid.c_str(), pass.c_str());
    Serial.printf("[WiFi] STA connecting %s\n", ssid.c_str());
}

bool wifiStaConnected() { return WiFi.status() == WL_CONNECTED; }
bool wifiApUp() { return apUp; }
String wifiStaIp() { return wifiStaConnected() ? WiFi.localIP().toString() : String("-"); }
String wifiApIp() { return apUp ? WiFi.softAPIP().toString() : String("-"); }
String wifiStaSsid() { return wifiStaConnected() ? WiFi.SSID() : String(""); }

const char *wifiModeText() {
    if (wifiStaConnected() && apUp) return "AP+STA";
    if (wifiStaConnected()) return "STA";
    if (apUp) return "AP";
    return "OFF";
}

void wifiWebBegin() {
    wifiPrefs.begin("wifi", false);

    String savedSsid = wifiPrefs.getString("ssid", "");
    // empty or legacy "iot" → new default
    if (!savedSsid.length() || savedSsid.equals(kLegacyStaSsid)) {
        wifiSaveSta(kDefaultStaSsid, kDefaultStaPass);
    }

    // STA only, non-blocking — never wait here (keeps boot UI responsive)
    if (apUp) {
        WiFi.softAPdisconnect(true);
        apUp = false;
    }
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    staAnnounced = false;
    wifiReconnectSta();
    lastStaRetryMs = millis();
    uiSetStatus("STA join", kDefaultStaSsid);
    Serial.printf("[WiFi] STA start %s (async, no auto-AP)\n", kDefaultStaSsid);
    slog("STA start ssid=%s", kDefaultStaSsid);

    server.on("/", HTTP_GET, handleRoot);
    server.on("/learn", HTTP_GET, handleLearnPage);
    server.on("/voice", HTTP_GET, handleVoicePage);
    server.on("/api/voice/status", HTTP_GET, handleVoiceStatus);
    server.on("/api/voice/events", HTTP_GET, handleVoiceEvents);
    server.on("/api/voice/start", HTTP_POST, handleVoiceStart);
    server.on("/api/voice/stop", HTTP_POST, handleVoiceStop);
    server.on("/api/voice/gatt", HTTP_GET, handleVoiceGatt);
    server.on("/api/voice/hid_catalog", HTTP_GET, handleVoiceHidCatalog);
    server.on("/api/voice/mic_on", HTTP_POST, handleVoiceMicOn);
    server.on("/api/voice/mic_off", HTTP_POST, handleVoiceMicOff);
    server.on("/api/voice/subscribe_all", HTTP_POST, handleVoiceSubAll);
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/map", HTTP_GET, handleMapGet);
    server.on("/api/map/learn/start", HTTP_POST, handleMapLearnStart);
    server.on("/api/map/learn/skip", HTTP_POST, handleMapLearnSkip);
    server.on("/api/map/save", HTTP_POST, handleMapSave);
    server.on("/api/map/clear", HTTP_POST, handleMapClear);
    server.on("/api/map/upload", HTTP_POST, handleMapUpload);
    server.on("/api/map/usb", HTTP_POST, handleMapUsb);
    server.on("/api/hid/reports", HTTP_GET, handleHidReports);
    server.on("/api/wifi", HTTP_POST, handleWifiSave);
    server.on("/api/wifi/clear", HTTP_POST, handleWifiClear);
    server.on("/api/wifi/ap", HTTP_POST, handleWifiAp);
    server.on("/api/wifi/sta", HTTP_POST, handleWifiStaOnly);
    server.on("/api/ble/scan", HTTP_POST, handleBleScan);
    server.on("/api/ble/stop", HTTP_POST, handleBleStop);
    server.on("/api/ble/connect", HTTP_POST, handleBleConnect);
    server.on("/api/ble/disconnect", HTTP_POST, handleBleDisconnect);
    server.on("/api/ble/reconnect", HTTP_POST, handleBleReconnect);
    server.on("/api/ble/autopair", HTTP_POST, handleBleAutopair);
    server.on("/api/hid/run", HTTP_POST, handleHidRun);
    server.on("/api/hid/test", HTTP_POST, handleHidTest);
    server.on("/api/logs", HTTP_GET, handleLogs);
    server.onNotFound([]() {
        if (server.method() == HTTP_OPTIONS) {
            handleOptions();
            return;
        }
        sendJson(404, "{\"ok\":false,\"error\":\"not found\"}");
    });
    server.on("/api/status", HTTP_OPTIONS, handleOptions);
    server.on("/api/wifi", HTTP_OPTIONS, handleOptions);
    server.on("/api/wifi/sta", HTTP_OPTIONS, handleOptions);
    server.on("/api/wifi/ap", HTTP_OPTIONS, handleOptions);
    server.on("/api/ble/scan", HTTP_OPTIONS, handleOptions);
    server.on("/api/ble/connect", HTTP_OPTIONS, handleOptions);
    server.on("/api/hid/run", HTTP_OPTIONS, handleOptions);
    server.on("/api/hid/test", HTTP_OPTIONS, handleOptions);

    server.begin();
    if (wifiStaConnected()) {
        Serial.printf("[HTTP] http://%s/\n", WiFi.localIP().toString().c_str());
    }
    if (apUp) {
        Serial.printf("[HTTP] AP http://%s/\n", WiFi.softAPIP().toString().c_str());
    }
}

void wifiWebLoop() {
    server.handleClient();

    if (wifiStaConnected()) {
        if (!staAnnounced) {
            staAnnounced = true;
            Serial.printf("[WiFi] STA OK ip=%s\n", WiFi.localIP().toString().c_str());
            Serial.printf("[HTTP] http://%s/\n", WiFi.localIP().toString().c_str());
            uiSetStatus("STA OK", WiFi.localIP().toString().c_str());
            slog("STA OK ip=%s", WiFi.localIP().toString().c_str());
            uiInvalidate();
        }
        return;
    }

    staAnnounced = false;
    if (apUp) return;
    uint32_t now = millis();
    if (now - lastStaRetryMs >= kStaRetryMs) {
        lastStaRetryMs = now;
        wifiReconnectSta();
        uiSetStatus("STA retry", kDefaultStaSsid);
        Serial.println("[WiFi] STA retry");
    }
}