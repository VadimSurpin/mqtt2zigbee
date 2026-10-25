#!/usr/bin/env python3
"""
Mock server — Zigbee MQTT Gateway configuration wizard.
Multi-page server-rendered HTML with progressive enhancement.
PRG pattern: POST → redirect → GET. Works without JavaScript.
"""
import json, os, time, secrets
from urllib.parse import urlencode
from functools import wraps
from flask import Flask, request, redirect, send_from_directory, Response, session

app = Flask(__name__)
app.secret_key = secrets.token_hex(16)
FRONTEND = os.path.join(os.path.dirname(__file__), "..", "components/web_ui/frontend")

# ── State ─────────────────────────────────────────────────────────────────────
_config       = {"wifi": {}, "mqtt": {}, "devices": []}
_wifi_networks  = []
_wifi_scan_done = False
_z2m_devices    = []
_z2m_scan_done  = False
_pair_until     = 0   # epoch seconds; 0 = closed
_paired_with    = None  # dict when a remote device is paired with us, else None
_user           = {"username": "admin", "password": "gateway"}
_DEFAULT_CREDS  = ("admin", "gateway")
_wifi_sta_ip    = None  # STA-mode IP assigned after connecting to router

# ── Mock data ─────────────────────────────────────────────────────────────────
MOCK_NETWORKS = [
    {"ssid": "HomeNetwork", "rssi": -45, "auth": "WPA2"},
    {"ssid": "OfficeWiFi",  "rssi": -62, "auth": "WPA2"},
    {"ssid": "GuestNet",    "rssi": -75, "auth": "WPA"},
    {"ssid": "OpenSpot",    "rssi": -80, "auth": "OPEN"},
]

MOCK_Z2M_DEVICES = [
    {"ieee_address":"0x001","friendly_name":"living_room_bulb",
     "vendor":"IKEA","model":"LED1545G12","description":"TRADFRI bulb E26/E27, white spectrum",
     "exposes":[
       {"type":"light","label":"Light","features":[
         {"name":"state",      "property":"state",      "access":7,"description":"On / Off"},
         {"name":"brightness", "property":"brightness", "access":7,"description":"Brightness 0-254"},
         {"name":"color_temp", "property":"color_temp", "access":7,"description":"Colour temperature (mired)"},
       ]},
       {"type":"numeric","name":"linkquality","property":"linkquality","access":1,"description":"Link quality"},
     ]},
    {"ieee_address":"0x002","friendly_name":"door_sensor",
     "vendor":"Aqara","model":"MCCGQ11LM","description":"Door and window sensor",
     "exposes":[
       {"type":"binary", "name":"contact",    "property":"contact",    "access":1,"description":"Contact open/closed"},
       {"type":"numeric","name":"battery",    "property":"battery",    "access":1,"description":"Battery %"},
       {"type":"numeric","name":"temperature","property":"temperature","access":1,"description":"Temperature °C"},
       {"type":"numeric","name":"linkquality","property":"linkquality","access":1,"description":"Link quality"},
     ]},
    {"ieee_address":"0x003","friendly_name":"thermostat",
     "vendor":"Danfoss","model":"014G2461","description":"Ally radiator thermostat",
     "exposes":[
       {"type":"numeric","name":"temperature",              "property":"local_temperature",         "access":1,"description":"Current temperature"},
       {"type":"numeric","name":"occupied_heating_setpoint","property":"occupied_heating_setpoint", "access":7,"description":"Setpoint 5-32 °C"},
       {"type":"binary", "name":"system_mode",             "property":"system_mode",               "access":7,"description":"heat / off"},
       {"type":"numeric","name":"linkquality",             "property":"linkquality",               "access":1,"description":"Link quality"},
     ]},
    {"ieee_address":"0x004","friendly_name":"motion_sensor",
     "vendor":"Aqara","model":"RTCGQ11LM","description":"Motion and illuminance sensor",
     "exposes":[
       {"type":"binary", "name":"occupancy",  "property":"occupancy",  "access":1,"description":"Motion detected"},
       {"type":"numeric","name":"illuminance","property":"illuminance","access":1,"description":"Illuminance (lux)"},
       {"type":"numeric","name":"battery",    "property":"battery",    "access":1,"description":"Battery %"},
       {"type":"numeric","name":"linkquality","property":"linkquality","access":1,"description":"Link quality"},
     ]},
    {"ieee_address":"0x005","friendly_name":"smart_plug",
     "vendor":"Tuya","model":"SP-EUC01","description":"Smart plug with energy monitoring",
     "exposes":[
       {"type":"binary", "name":"state",      "property":"state",      "access":7,"description":"On / Off"},
       {"type":"numeric","name":"power",      "property":"power",      "access":1,"description":"Power (W)"},
       {"type":"numeric","name":"current",    "property":"current",    "access":1,"description":"Current (A)"},
       {"type":"numeric","name":"voltage",    "property":"voltage",    "access":1,"description":"Voltage (V)"},
       {"type":"numeric","name":"linkquality","property":"linkquality","access":1,"description":"Link quality"},
     ]},
]

# ── HTML helpers ──────────────────────────────────────────────────────────────
DASH_PAGES   = [("/status",          "bi-speedometer2","Status"),
                ("/wifi",            "bi-wifi",         "WiFi"),
                ("/mqtt",            "bi-hdd-network",  "MQTT"),
                ("/devices",         "bi-diagram-3",    "Devices"),
                ("/pair",            "bi-broadcast",    "Pair"),
                ("/change-password", "bi-key",          "Security")]

COMMON_CSS = """
body{background:#f4f5f7}
.card{border-radius:12px;box-shadow:0 2px 12px rgba(0,0,0,.1);border:none}
.card-header{background:linear-gradient(135deg,#667eea,#764ba2);color:#fff;
             border-radius:12px 12px 0 0!important;padding:.75rem 1.25rem}
.wifi-item{cursor:pointer;transition:background .15s;border:1px solid #dee2e6}
.wifi-item:hover{background:#f0f0ff}
.wifi-item.chosen{background:#e8eeff;border-color:#667eea!important}
.feature-group{border-left:3px solid #667eea;padding-left:.75rem;margin:.3rem 0}
.device-card{border:1px solid #dee2e6!important;transition:box-shadow .15s}
.device-card:hover{box-shadow:0 3px 14px rgba(102,126,234,.2)}
.status-badge{font-size:.8rem;padding:.35em .7em}
.nav-link.active{font-weight:600}
"""

def _rssi_icon(rssi):
    if rssi > -55: return "bi-wifi"
    if rssi > -70: return "bi-wifi-2"
    return "bi-wifi-1"

def _expose_icon(t):
    return {"light":"bi-lightbulb","binary":"bi-toggle-on","numeric":"bi-bar-chart-line",
            "switch":"bi-toggles","cover":"bi-window","climate":"bi-thermometer-half"}.get(t,"bi-circle")

def _access_badge(a):
    b = []
    if a&1: b.append('<span class="badge bg-secondary" style="font-size:.6rem">R</span>')
    if a&2: b.append('<span class="badge bg-primary"   style="font-size:.6rem">W</span>')
    return " ".join(b)

def _alert(msg, kind="danger"):
    return f'<div class="alert alert-{kind} py-2 small mb-3">{msg}</div>' if msg else ""

def _is_default_creds():
    return _user["username"] == _DEFAULT_CREDS[0] and _user["password"] == _DEFAULT_CREDS[1]

def require_login(f):
    @wraps(f)
    def decorated(*args, **kwargs):
        if not session.get("logged_in"):
            return redirect("/login?" + urlencode({"next": request.path}))
        if _is_default_creds() and request.path not in ("/change-password", "/logout"):
            return redirect("/change-password")
        return f(*args, **kwargs)
    return decorated

def _auth_page(title, body):
    return f"""<!DOCTYPE html>
<html lang="en"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>{title} – Zigbee Gateway</title>
<link rel="stylesheet" href="/css/bootstrap.min.css">
<link rel="stylesheet" href="/css/bootstrap-icons.css">
<style>{COMMON_CSS}</style>
</head><body>
<div class="container py-5" style="max-width:400px">
  <div class="text-center mb-4">
    <h5 class="fw-bold mb-0"><i class="bi bi-broadcast me-2"></i>Zigbee MQTT Gateway</h5>
  </div>
  {body}
</div>
</body></html>"""

def _dash_page(title, body, active_path):
    nav_items = "".join(
        f'<a href="{href}" class="nav-link px-3 {"active" if href==active_path else ""}">'
        f'<i class="bi {icon} me-1"></i>{label}</a>'
        for href,icon,label in DASH_PAGES
    )
    return f"""<!DOCTYPE html>
<html lang="en"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>{title} – Zigbee Gateway</title>
<link rel="stylesheet" href="/css/bootstrap.min.css">
<link rel="stylesheet" href="/css/bootstrap-icons.css">
<style>{COMMON_CSS}</style>
</head><body>
<nav class="navbar navbar-light bg-white border-bottom mb-4 px-2">
  <div class="container-fluid justify-content-between" style="max-width:640px">
    <div class="d-flex align-items-center gap-2">
      <span class="navbar-brand fw-bold mb-0 small">[Z] Zigbee MQTT Gateway</span>
      <form method="post" action="/logout" class="mb-0">
        <button class="btn btn-sm btn-outline-secondary py-0 px-2">
          <i class="bi bi-box-arrow-right me-1"></i>Logout
        </button>
      </form>
    </div>
    <div class="d-flex flex-wrap align-items-center gap-1">{nav_items}</div>
  </div>
</nav>
<div class="container pb-4" style="max-width:640px">
  {body}
</div>
<script src="/js/enhance.js"></script>
</body></html>"""

# ── Static files ──────────────────────────────────────────────────────────────
@app.route("/css/<path:f>")
def css_file(f): return send_from_directory(os.path.join(FRONTEND,"css"), f)

@app.route("/js/enhance.js")
def enhance_js(): return Response(ENHANCE_JS, mimetype="application/javascript")

@app.route("/js/<path:f>")
def js_file(f): return send_from_directory(os.path.join(FRONTEND,"js"), f)

@app.route("/favicon.ico")
def favicon(): return Response(status=204)

# ── Auth routes ───────────────────────────────────────────────────────────────
@app.route("/login", methods=["GET"])
def login_get():
    if session.get("logged_in"):
        return redirect("/status")
    err = request.args.get("error", "")
    body = f"""
    <div class="card">
      <div class="card-header">
        <h6 class="mb-0"><i class="bi bi-lock me-2"></i>Sign in</h6>
      </div>
      <div class="card-body">
        {_alert(err)}
        <form method="post" action="/login">
          <div class="mb-3">
            <label class="form-label fw-semibold">Username</label>
            <input type="text" name="username" class="form-control" autocomplete="username" required autofocus>
          </div>
          <div class="mb-4">
            <label class="form-label fw-semibold">Password</label>
            <input type="password" name="password" class="form-control" autocomplete="current-password" required>
          </div>
          <button type="submit" class="btn btn-primary w-100">
            <i class="bi bi-box-arrow-in-right me-1"></i>Sign in
          </button>
        </form>
      </div>
    </div>"""
    return _auth_page("Sign in", body)

@app.route("/login", methods=["POST"])
def login_post():
    username = request.form.get("username", "").strip()
    password = request.form.get("password", "")
    if username == _user["username"] and password == _user["password"]:
        session["logged_in"] = True
        return redirect("/status")
    return redirect("/login?" + urlencode({"error": "Invalid username or password."}))

@app.route("/logout", methods=["POST"])
def logout():
    session.clear()
    return redirect("/login")

@app.route("/change-password", methods=["GET"])
@require_login
def change_password_get():
    err        = request.args.get("error", "")
    is_default = _is_default_creds()
    notice     = (_alert("You are using the default credentials. Please set a new username and password before continuing.", "warning")
                  if is_default else "")
    body = f"""
    <div class="card">
      <div class="card-header">
        <h6 class="mb-0"><i class="bi bi-key me-2"></i>Change credentials</h6>
      </div>
      <div class="card-body">
        {notice}
        {_alert(err)}
        <form method="post" action="/change-password">
          <div class="mb-3">
            <label class="form-label fw-semibold">Username</label>
            <input type="text" name="username" class="form-control" autocomplete="username"
                   value="{_user['username']}" required>
          </div>
          <div class="mb-3">
            <label class="form-label fw-semibold">New password</label>
            <input type="password" name="password" class="form-control" autocomplete="new-password" required>
          </div>
          <div class="mb-4">
            <label class="form-label fw-semibold">Confirm password</label>
            <input type="password" name="password2" class="form-control" autocomplete="new-password" required>
          </div>
          <button type="submit" class="btn btn-primary w-100">
            <i class="bi bi-check-lg me-1"></i>Save credentials
          </button>
        </form>
      </div>
    </div>"""
    return _dash_page("Security", body, "/change-password")

@app.route("/change-password", methods=["POST"])
@require_login
def change_password_post():
    username  = request.form.get("username",  "").strip()
    password  = request.form.get("password",  "")
    password2 = request.form.get("password2", "")
    if not username:
        return redirect("/change-password?" + urlencode({"error": "Username cannot be empty."}))
    if len(password) < 4:
        return redirect("/change-password?" + urlencode({"error": "Password must be at least 4 characters."}))
    if password != password2:
        return redirect("/change-password?" + urlencode({"error": "Passwords do not match."}))
    _user["username"] = username
    _user["password"] = password
    return redirect("/status")

# ── Enhancement JS ────────────────────────────────────────────────────────────
ENHANCE_JS = r"""
(function(){
'use strict';

/* ── WiFi: open network hides password; blank pw disables submit ─────────── */
function applyNetworkAuth(radio){
  var auth   = radio.dataset.auth || 'WPA2';
  var pwSec  = document.getElementById('pw-section');
  var pwNote = document.getElementById('pw-note');
  var pwIn   = document.getElementById('pw-input');
  var subBtn = document.getElementById('connect-btn');
  if(!pwSec) return;
  if(auth==='OPEN'){
    pwSec.hidden  = true;
    pwNote.hidden = false;
    if(pwIn) pwIn.required = false;
    if(subBtn) subBtn.disabled = false;
  } else {
    pwSec.hidden  = false;
    pwNote.hidden = true;
    if(pwIn) pwIn.required = true;
    if(subBtn) subBtn.disabled = !(pwIn && pwIn.value.length > 0);
  }
  document.querySelectorAll('.wifi-item').forEach(function(el){
    el.classList.toggle('chosen', el.querySelector('input[name=ssid]')===radio);
  });
}

document.querySelectorAll('input[name=ssid]').forEach(function(r){
  r.addEventListener('change', function(){ applyNetworkAuth(r); });
  if(r.checked) applyNetworkAuth(r);
});

var pwInput = document.getElementById('pw-input');
if(pwInput){
  pwInput.addEventListener('input', function(){
    var selected = document.querySelector('input[name=ssid]:checked');
    if(!selected || selected.dataset.auth==='OPEN') return;
    var btn = document.getElementById('connect-btn');
    if(btn) btn.disabled = pwInput.value.length===0;
  });
}

/* ── WiFi async scan ────────────────────────────────────────────────────────*/
var scanBtn = document.getElementById('scan-btn');
if(scanBtn){
  scanBtn.addEventListener('click', function(e){
    e.preventDefault();
    scanBtn.disabled=true;
    scanBtn.innerHTML='<span class="spinner-border spinner-border-sm me-1"></span>Scanning…';
    fetch('/wifi/scan',{method:'POST'}).then(poll);
    function poll(){
      return fetch('/api/wifi/networks').then(function(r){return r.json();}).then(function(d){
        if(!d.done) return new Promise(function(res){setTimeout(res,800);}).then(poll);
        renderNets(d.networks);
        scanBtn.disabled=false;
        scanBtn.innerHTML='<i class="bi bi-arrow-clockwise me-1"></i>Rescan';
      });
    }
    function renderNets(nets){
      var list=document.getElementById('network-list');
      list.innerHTML=nets.map(function(n){
        return '<label class="d-flex align-items-center gap-2 wifi-item p-2 rounded mb-2"'+
          ' data-auth="'+n.auth+'">'+
          '<input type="radio" name="ssid" value="'+n.ssid+
          '" class="form-check-input" data-auth="'+n.auth+'" required> '+
          '<i class="bi '+n.icon+'"></i>'+
          '<strong class="flex-grow-1">'+n.ssid+'</strong>'+
          '<span class="badge bg-secondary">'+n.auth+'</span>'+
          '<small class="text-muted">'+n.rssi+' dBm</small></label>';
      }).join('');
      document.querySelectorAll('input[name=ssid]').forEach(function(r){
        r.addEventListener('change',function(){applyNetworkAuth(r);});
        if(r.checked) applyNetworkAuth(r);
      });
      document.getElementById('pw-section').hidden=false;
    }
  });
}

/* ── Device checkboxes: parent ↔ feature sync ───────────────────────────── */
function syncParent(devName){
  var all=document.querySelectorAll('.feat-cb[data-dev="'+devName+'"]');
  var chk=document.querySelectorAll('.feat-cb[data-dev="'+devName+'"]:checked');
  var par=document.querySelector('.dev-cb[data-dev="'+devName+'"]');
  if(!par) return;
  par.indeterminate=chk.length>0&&chk.length<all.length;
  if(!par.indeterminate) par.checked=chk.length===all.length;
}
document.querySelectorAll('.dev-cb').forEach(function(cb){
  cb.addEventListener('change',function(){
    document.querySelectorAll('.feat-cb[data-dev="'+cb.dataset.dev+'"]')
      .forEach(function(f){f.checked=cb.checked;});
  });
});
document.querySelectorAll('.feat-cb').forEach(function(cb){
  cb.addEventListener('change',function(){syncParent(cb.dataset.dev);});
  syncParent(cb.dataset.dev);
});

/* ── Pair: live countdown ───────────────────────────────────────────────────*/
var countdown=document.getElementById('pair-countdown');
if(countdown){
  var until=parseInt(countdown.dataset.until,10);
  function tick(){
    var left=until-Math.floor(Date.now()/1000);
    if(left<=0){countdown.textContent='Pairing closed.';return;}
    countdown.textContent='Pairing open — '+left+'s remaining…';
    setTimeout(tick,1000);
  }
  tick();
}

})();
"""

# ── Root ──────────────────────────────────────────────────────────────────────
@app.route("/")
@require_login
def index():
    if _config["wifi"].get("ssid") and _config["mqtt"].get("host"):
        return redirect("/status")
    return redirect("/wifi")

# ── Step 1: WiFi ──────────────────────────────────────────────────────────────
@app.route("/wifi", methods=["GET"])
@require_login
def wifi_get():
    err   = request.args.get("error","")
    saved = _config["wifi"].get("ssid","")

    if _wifi_scan_done and _wifi_networks:
        items = ""
        for n in _wifi_networks:
            chosen  = "chosen" if n["ssid"]==saved else ""
            checked = "checked" if n["ssid"]==saved else ""
            items  += (
                f'<label class="d-flex align-items-center gap-2 wifi-item p-2 rounded mb-2 {chosen}"'
                f' data-auth="{n["auth"]}">'
                f'<input type="radio" name="ssid" value="{n["ssid"]}" class="form-check-input"'
                f' data-auth="{n["auth"]}" {checked} required>'
                f'<i class="bi {_rssi_icon(n["rssi"])}"></i>'
                f'<strong class="flex-grow-1">{n["ssid"]}</strong>'
                f'<span class="badge bg-secondary">{n["auth"]}</span>'
                f'<small class="text-muted">{n["rssi"]} dBm</small></label>'
            )
        net_html   = f'<div id="network-list" class="mb-2">{items}</div>'
        pw_hidden  = "" if saved and _config["wifi"].get("auth","WPA2")!="OPEN" else "hidden"
        note_hidden= "hidden" if _config["wifi"].get("auth","WPA2")!="OPEN" else ""
    else:
        net_html   = '<div id="network-list" class="text-muted small mb-2">No scan yet — click Scan Networks.</div>'
        pw_hidden  = "hidden"
        note_hidden= "hidden"

    body = f"""
    <div class="card">
      <div class="card-header">
        <h6 class="mb-0"><i class="bi bi-wifi me-2"></i>WiFi Network</h6>
      </div>
      <div class="card-body">
        {_alert(err)}
        <form method="post" action="/wifi/scan" class="mb-3">
          <button id="scan-btn" class="btn btn-outline-primary btn-sm" type="submit">
            <i class="bi bi-search me-1"></i>Scan Networks
          </button>
        </form>
        <form method="post" action="/wifi">
          {net_html}
          <div id="pw-note" class="alert alert-info py-2 small mb-3" {note_hidden}>
            <i class="bi bi-info-circle me-1"></i>Open network — no password required.
          </div>
          <div id="pw-section" {pw_hidden}>
            <div class="mb-3">
              <label class="form-label fw-semibold">Password</label>
              <input type="password" id="pw-input" name="password" class="form-control"
                     placeholder="WiFi password" autocomplete="current-password">
            </div>
          </div>
          <div class="d-flex justify-content-end mt-3">
            <button id="connect-btn" type="submit" class="btn btn-primary"
                    {"disabled" if not saved else ""}>
              Connect <i class="bi bi-arrow-right ms-1"></i>
            </button>
          </div>
        </form>
      </div>
    </div>"""
    return _dash_page("WiFi Settings", body, "/wifi")

@app.route("/wifi/scan", methods=["GET","POST"])
@require_login
def wifi_scan():
    global _wifi_networks, _wifi_scan_done
    _wifi_networks = MOCK_NETWORKS
    _wifi_scan_done = True
    return redirect("/wifi")

@app.route("/wifi", methods=["POST"])
@require_login
def wifi_post():
    global _wifi_sta_ip
    ssid     = request.form.get("ssid","").strip()
    password = request.form.get("password","")
    if not ssid:
        return redirect("/wifi?"+urlencode({"error":"Please select a network."}))
    net  = next((n for n in _wifi_networks if n["ssid"]==ssid), None)
    auth = net["auth"] if net else "WPA2"
    if auth != "OPEN" and not password:
        return redirect("/wifi?"+urlencode({"error":f"Password is required for {ssid} ({auth})."}))
    _config["wifi"] = {"ssid": ssid, "password": password, "auth": auth}
    # Derive a mock STA IP from the current host's subnet
    parts = request.host.split(":")[0].split(".")
    prefix = ".".join(parts[:3]) if len(parts) == 4 else "192.168.1"
    _wifi_sta_ip = prefix + ".100"
    return redirect("/wifi/connected")

@app.route("/wifi/connected", methods=["GET"])
@require_login
def wifi_connected():
    ssid = _config["wifi"].get("ssid", "?")
    ip   = _wifi_sta_ip or "192.168.1.100"
    next_url = f"http://{ip}/mqtt"
    body = f"""
    <div class="card">
      <div class="card-header" style="background:linear-gradient(135deg,#28a745,#20c997)">
        <h6 class="mb-0"><i class="bi bi-check-circle me-2"></i>Connected to WiFi</h6>
      </div>
      <div class="card-body">
        <p class="mb-1">Successfully joined <strong>{ssid}</strong>.</p>
        <p class="text-muted small mb-4">
          The gateway has switched to station mode and turned off its access point.
          It is now reachable on your local network.
        </p>
        <div class="card bg-light border mb-4">
          <div class="card-body py-3">
            <div class="text-muted small mb-1">Gateway IP address</div>
            <div class="font-monospace fw-semibold fs-5">{ip}</div>
          </div>
        </div>
        <p class="small text-muted mb-3">
          Connect your device to <strong>{ssid}</strong>, then continue setup at the link below.
        </p>
        <a href="{next_url}" class="btn btn-primary w-100 mb-2">
          Continue setup at {ip} <i class="bi bi-arrow-right ms-1"></i>
        </a>
        <a href="/mqtt" class="btn btn-outline-secondary w-100 btn-sm">
          Continue here (mock only)
        </a>
      </div>
    </div>"""
    return _dash_page("Connected", body, "/wifi")

@app.route("/api/wifi/networks")
@require_login
def api_wifi_networks():
    return Response(json.dumps({
        "done": _wifi_scan_done,
        "networks": [{**n, "icon": _rssi_icon(n["rssi"])} for n in _wifi_networks],
    }), mimetype="application/json")

# ── Step 2: MQTT ──────────────────────────────────────────────────────────────
@app.route("/mqtt", methods=["GET"])
@require_login
def mqtt_get():
    err      = request.args.get("error","")
    m = _config.get("mqtt",{})

    # Derive network prefix hint from request host (same subnet as mock = same as device would use)
    host_ip = request.host.split(":")[0]
    parts   = host_ip.split(".")
    prefix  = ".".join(parts[:3])+"." if len(parts)==4 else "192.168.1."
    placeholder = prefix + "x  (e.g. " + prefix + "100)"

    body = f"""
    <div class="card">
      <div class="card-header">
        <h6 class="mb-0"><i class="bi bi-hdd-network me-2"></i>MQTT Broker</h6>
      </div>
      <div class="card-body">
        {_alert(err)}
        <form method="post" action="/mqtt">
          <div class="row g-3 mb-3">
            <div class="col-8">
              <label class="form-label fw-semibold">Broker Host</label>
              <input type="text" name="host" class="form-control" required
                     value="{m.get('host','')}" placeholder="{placeholder}">
            </div>
            <div class="col-4">
              <label class="form-label fw-semibold">Port</label>
              <input type="number" name="port" class="form-control" value="{m.get('port',1883)}">
            </div>
          </div>
          <div class="mb-3">
            <label class="form-label fw-semibold">Client ID</label>
            <input type="text" name="client_id" class="form-control"
                   value="{m.get('client_id','zigbee_gw_01')}">
          </div>
          <div class="row g-3 mb-4">
            <div class="col">
              <label class="form-label fw-semibold">
                Username <small class="fw-normal text-muted">(optional)</small>
              </label>
              <input type="text" name="username" class="form-control" value="{m.get('username','')}">
            </div>
            <div class="col">
              <label class="form-label fw-semibold">
                Password <small class="fw-normal text-muted">(optional)</small>
              </label>
              <input type="password" name="password" class="form-control">
            </div>
          </div>
          <div class="d-flex justify-content-end">
            <button type="submit" class="btn btn-primary">
              Test &amp; Continue <i class="bi bi-arrow-right ms-1"></i>
            </button>
          </div>
        </form>
      </div>
    </div>"""

    return _dash_page("MQTT Settings", body, "/mqtt")

@app.route("/mqtt", methods=["POST"])
@require_login
def mqtt_post():
    host = request.form.get("host","").strip()
    if not host:
        return redirect("/mqtt?"+urlencode({"error":"Broker host is required."}))
    _config["mqtt"] = {
        "host":      host,
        "port":      int(request.form.get("port",1883) or 1883),
        "client_id": request.form.get("client_id","zigbee_gw_01"),
        "username":  request.form.get("username",""),
        "password":  request.form.get("password",""),
    }
    return redirect("/devices")

# ── Step 3: Devices ───────────────────────────────────────────────────────────
def _device_cards(selected_names):
    cards = ""
    for dev in _z2m_devices:
        name = dev["friendly_name"]
        sel  = name in selected_names
        rows = ""
        for exp in dev["exposes"]:
            icon = _expose_icon(exp.get("type",""))
            if "features" in exp:
                feats = "".join(
                    f'<div class="d-flex align-items-center gap-2 py-1 ps-3">'
                    f'<input type="checkbox" name="feat_{name}" value="{f["property"]}"'
                    f' class="form-check-input feat-cb" data-dev="{name}" {"checked" if sel else ""}>'
                    f'<code class="small">{f["property"]}</code>'
                    f'<small class="text-muted flex-grow-1">{f.get("description","")}</small>'
                    f'{_access_badge(f.get("access",1))}</div>'
                    for f in exp["features"]
                )
                rows += (
                    f'<div class="feature-group mb-2">'
                    f'<div class="d-flex align-items-center gap-1 mb-1">'
                    f'<i class="bi {icon} text-primary small"></i>'
                    f'<strong class="small">{exp.get("label",exp["type"].title())}</strong>'
                    f'</div>{feats}</div>'
                )
            else:
                prop = exp.get("property", exp.get("name",""))
                rows += (
                    f'<div class="d-flex align-items-center gap-2 py-1">'
                    f'<input type="checkbox" name="feat_{name}" value="{prop}"'
                    f' class="form-check-input feat-cb" data-dev="{name}" {"checked" if sel else ""}>'
                    f'<i class="bi {icon} text-secondary small"></i>'
                    f'<code class="small">{prop}</code>'
                    f'<small class="text-muted flex-grow-1">{exp.get("description","")}</small>'
                    f'{_access_badge(exp.get("access",1))}</div>'
                )
        cards += (
            f'<div class="card device-card mb-3"><div class="card-body py-3">'
            f'<div class="d-flex align-items-start gap-3">'
            f'<input type="checkbox" name="device" value="{name}"'
            f' class="form-check-input dev-cb mt-1" data-dev="{name}"'
            f' {"checked" if sel else ""} style="width:1.2em;height:1.2em;flex-shrink:0">'
            f'<div class="flex-grow-1 overflow-hidden">'
            f'<div class="d-flex align-items-center flex-wrap gap-2 mb-1">'
            f'<strong>{name.replace("_"," ").title()}</strong>'
            f'<span class="badge bg-light text-dark border" style="font-size:.7rem">'
            f'{dev["vendor"]} {dev["model"]}</span></div>'
            f'<small class="text-muted d-block mb-2">{dev.get("description","")}</small>'
            f'{rows}</div></div></div></div>'
        )
    return cards

@app.route("/devices", methods=["GET"])
@require_login
def devices_get():
    err      = request.args.get("error","")
    selected = {d["friendly_name"] for d in _config.get("devices",[])}

    if not _z2m_scan_done:
        body = f"""
        <div class="card">
          <div class="card-header">
            <h6 class="mb-0"><i class="bi bi-diagram-3 me-2"></i>Select Devices</h6>
          </div>
          <div class="card-body text-center py-5">
            {_alert(err)}
            <i class="bi bi-diagram-3" style="font-size:3rem;color:#667eea"></i>
            <p class="mt-3 text-muted">Discover devices from zigbee2mqtt.</p>
            <form method="post" action="/devices/discover">
              <button class="btn btn-success"><i class="bi bi-search me-1"></i>Discover Devices</button>
            </form>
          </div>
        </div>"""
    else:
        cards = _device_cards(selected)
        body  = f"""
        <form method="post" action="/devices">
          <div class="d-flex justify-content-between align-items-center mb-3">
            <div class="card-header w-100 rounded-top" style="background:linear-gradient(135deg,#667eea,#764ba2);color:#fff;padding:.75rem 1.25rem">
              <h6 class="mb-0"><i class="bi bi-diagram-3 me-2"></i>Select Devices
              <span class="float-end">
                <a href="/devices/discover" class="btn btn-sm btn-light btn-outline-light py-0">
                  <i class="bi bi-arrow-clockwise me-1"></i>Rescan
                </a>
              </span></h6>
            </div>
          </div>
          {_alert(err)}
          {cards}
          <div class="d-flex justify-content-end mt-2">
            <button type="submit" class="btn btn-primary">
              Save &amp; Continue <i class="bi bi-arrow-right ms-1"></i>
            </button>
          </div>
        </form>"""

    return _dash_page("Device Settings", body, "/devices")

@app.route("/devices/discover", methods=["GET","POST"])
@require_login
def devices_discover():
    global _z2m_devices, _z2m_scan_done
    _z2m_devices   = MOCK_Z2M_DEVICES
    _z2m_scan_done = True
    return redirect("/devices")

@app.route("/devices", methods=["POST"])
@require_login
def devices_post():
    selected = request.form.getlist("device")
    if not selected:
        return redirect("/devices?"+urlencode({"error":"Select at least one device."}))
    _config["devices"] = [
        {"friendly_name": d["friendly_name"], "ieee_address": d["ieee_address"],
         "topic": f'zigbee2mqtt/{d["friendly_name"]}',
         "vendor": d.get("vendor",""), "model": d.get("model",""),
         "features": request.form.getlist(f'feat_{d["friendly_name"]}')}
        for d in _z2m_devices if d["friendly_name"] in selected
    ]
    print(f"[SAVE] {json.dumps(_config, indent=2)}")
    return redirect("/pair")

# ── Pair Zigbee devices ───────────────────────────────────────────────────────
PAIR_WINDOW = 60  # seconds

@app.route("/pair", methods=["GET"])
@require_login
def pair_get():
    global _pair_until, _paired_with
    now   = int(time.time())
    open_ = _pair_until > now
    left  = max(0, _pair_until - now)

    # ── Gateway Zigbee identity (mock values; real firmware reads from NVS/stack) ──
    identity_html = """
    <div class="card mb-4">
      <div class="card-header py-2">
        <h6 class="mb-0"><i class="bi bi-cpu me-2"></i>This Gateway's Zigbee Identity</h6>
      </div>
      <div class="card-body p-0">
        <table class="table table-sm mb-0">
          <tbody>
            <tr><th class="ps-3 text-muted fw-normal" style="width:40%">Device name</th>
                <td class="fw-semibold">ZigbeeMQTT Gateway</td></tr>
            <tr><th class="ps-3 text-muted fw-normal">IEEE address</th>
                <td class="font-monospace">40:4c:ca:57:8b:b4</td></tr>
            <tr><th class="ps-3 text-muted fw-normal">PAN ID</th>
                <td class="font-monospace">0x1A2B</td></tr>
            <tr><th class="ps-3 text-muted fw-normal">Channel</th>
                <td>15</td></tr>
          </tbody>
        </table>
      </div>
    </div>"""

    # ── Exposed Zigbee endpoints — one per configured MQTT device ──
    devices = _config.get("devices", [])
    def _z_type(d):
        feats = " ".join(
            f.get("name","") for f in d.get("exposes", []) if isinstance(f, dict)
        ).lower()
        if "occupancy"   in feats: return "bi-person-walking",   "Occupancy sensor"
        if "temperature" in feats: return "bi-thermometer-half", "Temperature sensor"
        if "illuminance" in feats: return "bi-brightness-high",  "Light sensor"
        if "contact"     in feats: return "bi-door-closed",      "Contact sensor"
        if "brightness"  in feats: return "bi-lightbulb",        "Dimmable light"
        if "state"       in feats: return "bi-toggle-on",        "On/Off device"
        return "bi-diagram-3", "Generic device"

    if devices:
        ep_rows = ""
        for i, d in enumerate(devices, start=1):
            icon, ztype = _z_type(d)
            name = d.get("friendly_name", d.get("ieee_address","?")).replace("_"," ").title()
            ep_rows += (
                f'<tr>'
                f'<td class="ps-3 text-muted font-monospace">{i}</td>'
                f'<td class="fw-semibold">{name}</td>'
                f'<td><i class="bi {icon} me-1"></i>{ztype}</td>'
                f'</tr>'
            )
        endpoints_html = f"""
        <div class="card mb-4">
          <div class="card-header py-2">
            <h6 class="mb-0"><i class="bi bi-diagram-3 me-2"></i>Exposed Zigbee Endpoints</h6>
          </div>
          <div class="card-body p-0">
            <table class="table table-sm mb-0">
              <thead class="table-light">
                <tr>
                  <th class="ps-3" style="width:60px">EP#</th>
                  <th>MQTT device</th>
                  <th>Zigbee type</th>
                </tr>
              </thead>
              <tbody>{ep_rows}</tbody>
            </table>
          </div>
        </div>"""
    else:
        endpoints_html = """
        <div class="alert alert-warning mb-4">
          <i class="bi bi-exclamation-triangle me-2"></i>
          No MQTT devices configured yet — <a href="/devices">add devices</a> first.
        </div>"""

    # ── Three states: paired / pairing-open / idle ──
    if _paired_with:
        p = _paired_with
        paired_at = time.strftime("%Y-%m-%d %H:%M", time.localtime(p["paired_at"]))
        intro_or_paired = f"""
        <div class="card mb-4 border-success">
          <div class="card-header py-2" style="background:#d1e7dd;color:#0a3622">
            <h6 class="mb-0"><i class="bi bi-check-circle-fill me-2"></i>Paired with a remote device</h6>
          </div>
          <div class="card-body p-0">
            <table class="table table-sm mb-0">
              <tbody>
                <tr><th class="ps-3 text-muted fw-normal" style="width:40%">Name</th>
                    <td class="fw-semibold">{p['name']}</td></tr>
                <tr><th class="ps-3 text-muted fw-normal">IEEE address</th>
                    <td class="font-monospace">{p['ieee']}</td></tr>
                <tr><th class="ps-3 text-muted fw-normal">Model</th>
                    <td>{p['vendor']} {p['model']}</td></tr>
                <tr><th class="ps-3 text-muted fw-normal">Paired at</th>
                    <td>{paired_at}</td></tr>
              </tbody>
            </table>
          </div>
        </div>"""
        action_form = """
        <form method="post" action="/pair/unpair" class="mb-4">
          <button class="btn btn-outline-danger">
            <i class="bi bi-x-circle me-1"></i>Unpair
          </button>
        </form>"""
        meta_refresh = ""

    elif open_:
        intro_or_paired = """
        <p class="text-muted small mb-3">
          This gateway appears as a single Zigbee device with one endpoint per MQTT device.
          Other Zigbee coordinators or hubs (e.g. Home Assistant) can pair with it below.
        </p>"""
        countdown = (f'<span id="pair-countdown" data-until="{_pair_until}">'
                     f'— {left}s remaining…</span>')
        action_form = f"""
        <form method="post" action="/pair/stop" class="mb-4">
          <button class="btn btn-danger">
            <i class="bi bi-broadcast me-1"></i>Stop pairing {countdown}
          </button>
        </form>"""
        meta_refresh = '<meta http-equiv="refresh" content="5">'

    else:
        intro_or_paired = """
        <p class="text-muted small mb-3">
          This gateway appears as a single Zigbee device with one endpoint per MQTT device.
          Other Zigbee coordinators or hubs (e.g. Home Assistant) can pair with it below.
        </p>"""
        action_form = """
        <form method="post" action="/pair/open" class="mb-4">
          <button class="btn btn-success">
            <i class="bi bi-broadcast me-1"></i>Start pairing
          </button>
        </form>"""
        meta_refresh = ""

    body = f"""
    {meta_refresh}
    <div class="card">
      <div class="card-header">
        <h6 class="mb-0"><i class="bi bi-broadcast me-2"></i>Zigbee Pairing</h6>
      </div>
      <div class="card-body">
        {intro_or_paired}
        {action_form}
        {identity_html}
        {endpoints_html}
        <a href="/status" class="btn btn-primary">
          Go to Dashboard <i class="bi bi-speedometer2 ms-1"></i>
        </a>
      </div>
    </div>"""
    return _dash_page("Pair Devices", body, "/pair")

@app.route("/pair/open", methods=["POST"])
@require_login
def pair_open():
    global _pair_until
    _pair_until = int(time.time()) + PAIR_WINDOW
    return redirect("/pair")

@app.route("/pair/stop", methods=["POST"])
@require_login
def pair_stop():
    global _pair_until, _paired_with
    _pair_until  = 0
    # Simulate a remote device having joined during the pairing window
    _paired_with = {
        "name":      "Home Assistant",
        "ieee":      "00:21:2e:ff:fe:a8:3c:b2",
        "vendor":    "Nabu Casa",
        "model":     "SkyConnect",
        "paired_at": int(time.time()),
    }
    return redirect("/pair")

@app.route("/pair/unpair", methods=["POST"])
@require_login
def pair_unpair():
    global _paired_with, _pair_until
    _paired_with = None
    _pair_until  = 0
    return redirect("/pair")

# ── Status dashboard ──────────────────────────────────────────────────────────
@app.route("/status")
@require_login
def status():
    wifi = _config.get("wifi",{})
    mqtt = _config.get("mqtt",{})
    devs = _config.get("devices",[])
    now  = int(time.time())
    pair_open = _pair_until > now

    def badge(ok, yes="Connected", no="Not connected"):
        cls = "bg-success" if ok else "bg-secondary"
        return f'<span class="badge {cls} status-badge">{yes if ok else no}</span>'

    wifi_ok = bool(wifi.get("ssid"))
    mqtt_ok = bool(mqtt.get("host"))
    zb_ok   = wifi_ok and mqtt_ok  # mock: assume Zigbee up if configured

    body = f"""
    <div class="row g-3 mb-4">
      <div class="col-12 col-md-4">
        <div class="card h-100">
          <div class="card-body">
            <div class="d-flex justify-content-between align-items-start">
              <div>
                <div class="text-muted small mb-1"><i class="bi bi-wifi me-1"></i>WiFi</div>
                {badge(wifi_ok)}
              </div>
              <i class="bi bi-wifi" style="font-size:1.8rem;color:{'#28a745' if wifi_ok else '#adb5bd'}"></i>
            </div>
            <div class="mt-2 small text-muted">
              {f'SSID: <strong>{wifi["ssid"]}</strong>' if wifi_ok else 'Not configured'}
            </div>
          </div>
        </div>
      </div>
      <div class="col-12 col-md-4">
        <div class="card h-100">
          <div class="card-body">
            <div class="d-flex justify-content-between align-items-start">
              <div>
                <div class="text-muted small mb-1"><i class="bi bi-hdd-network me-1"></i>MQTT</div>
                {badge(mqtt_ok)}
              </div>
              <i class="bi bi-hdd-network" style="font-size:1.8rem;color:{'#28a745' if mqtt_ok else '#adb5bd'}"></i>
            </div>
            <div class="mt-2 small text-muted">
              {f'<strong>{mqtt["host"]}</strong>:{mqtt.get("port",1883)}' if mqtt_ok else 'Not configured'}
            </div>
          </div>
        </div>
      </div>
      <div class="col-12 col-md-4">
        <div class="card h-100">
          <div class="card-body">
            <div class="d-flex justify-content-between align-items-start">
              <div>
                <div class="text-muted small mb-1"><i class="bi bi-diagram-3 me-1"></i>Zigbee</div>
                {badge(zb_ok, "Coordinator active", "Not started")}
              </div>
              <i class="bi bi-diagram-3" style="font-size:1.8rem;color:{'#28a745' if zb_ok else '#adb5bd'}"></i>
            </div>
            <div class="mt-2 small text-muted">
              {f'{len(devs)} device(s) bridged' if zb_ok else 'Waiting for WiFi &amp; MQTT'}
            </div>
          </div>
        </div>
      </div>
    </div>

    {"" if not pair_open else
     '<div class="alert alert-success mb-4"><i class="bi bi-broadcast me-2"></i>'
     f'Pairing window open — <strong>{max(0,_pair_until-now)}s</strong> remaining. '
     f'<a href="/pair" class="alert-link">Manage pairing</a></div>'}

    <div class="card">
      <div class="card-header"><h6 class="mb-0">Bridged Devices ({len(devs)})</h6></div>
      {'<ul class="list-group list-group-flush rounded-bottom">' +
       "".join(f'<li class="list-group-item d-flex justify-content-between align-items-center small">'
               f'<span><strong>{d["friendly_name"].replace("_"," ").title()}</strong>'
               f' <span class="text-muted">{d["vendor"]} {d["model"]}</span></span>'
               f'<span class="badge bg-success">active</span></li>'
               for d in devs) +
       '</ul>'
       if devs else
       '<div class="card-body text-muted small">No devices configured. '
       '<a href="/devices">Configure devices →</a></div>'}
    </div>"""

    return _dash_page("Dashboard", body, "/status")

# ── Legacy API (keep for any stale references) ────────────────────────────────
@app.route("/api/status")
@require_login
def api_status():
    return Response(json.dumps({"wifi_connected": bool(_config["wifi"].get("ssid")),
                                "ip":"","mqtt_connected":False}), mimetype="application/json")

if __name__ == "__main__":
    print("Mock server → http://localhost:8080")
    app.run(host="0.0.0.0", port=8080, debug=True)
