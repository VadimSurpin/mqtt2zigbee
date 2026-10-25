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
  var left=parseInt(countdown.dataset.seconds,10);
  function tick(){
    if(left<=0){countdown.textContent='Pairing closed.';return;}
    countdown.textContent='Pairing open — '+left+'s remaining…';
    left--;
    setTimeout(tick,1000);
  }
  tick();
}

})();
