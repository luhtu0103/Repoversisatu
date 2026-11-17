#include "speaker_id.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "SPEAKER_WEB";

#define REC_MS      2500
#define REC_SAMPLES (SPEAKER_ID_SAMPLE_RATE * REC_MS / 1000)

static int16_t *s_rec = nullptr;

/* Dari audio_hal.h (C++ linkage). */
void audio_hal_afe_init(void);

static const char *VOICE_HTML = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Profil Suara</title>
<style>
  body { font-family: sans-serif; background: #1e1e24; color: #f0f0f5; padding: 20px; max-width: 520px; margin: auto; }
  input { width: 100%; padding: 10px; margin: 6px 0; border-radius: 6px; border: 1px solid #333; background: #121216; color: #fff; box-sizing: border-box; }
  button { width: 100%; padding: 12px; margin: 6px 0; background: #4338ca; color: white; border: none; border-radius: 6px; font-weight: bold; }
  button.del { background: #b91c1c; width: auto; padding: 6px 12px; margin: 0 0 0 8px; }
  .box { background: #121216; border-radius: 6px; padding: 10px; margin: 8px 0; }
  .row { display: flex; justify-content: space-between; align-items: center; padding: 6px 0; border-bottom: 1px solid #2a2a33; }
  #msg, #tr { min-height: 24px; }
  a { color: #a5b4fc; }
  .chip { display: inline-block; padding: 4px 10px; margin: 3px 4px 3px 0; border-radius: 14px; background: #2a2a33; color: #f0f0f5; font-size: 13px; cursor: pointer; }
  .chip.on { background: #166534; }
  .chip small { color: #fca5a5; margin-left: 4px; }
</style>
</head>
<body>
<h2>Profil Suara</h2>
<p>Ucapkan <b>"Hi ESP"</b> 3 kali ke <b>mic robot</b> (bukan HP). Jarak sekitar 20-30 cm, suasana tenang.</p>
<div class="box">
  <input id="nm" maxlength="24" placeholder="Nama (mis. Budi)">
  <button id="bt" onclick="rec()">Rekam 1/3</button>
  <div id="msg"></div>
</div>
<h3>Profil tersimpan</h3>
<div id="ls" class="box">...</div>
<h3>Kata harian (dikenali saat ngobrol)</h3>
<div class="box">
  <small>Ucapkan tiap kata 2 kali ke <b>mic robot</b>. Robot lalu mengenalimu saat kamu memakai kata itu di tengah obrolan.</small>
  <input id="wn" maxlength="24" placeholder="Nama (sama seperti di atas)">
  <input id="ww" maxlength="15" placeholder="Kata (mis. nyalakan)">
  <div id="chips"></div>
  <button id="wbt" onclick="wrec()">Rekam kata 1/2</button>
  <button id="wall" onclick="wauto()">Daftar semua kata saran (otomatis)</button>
  <button id="wstop" onclick="wstopNow()" style="background:#6b7280;display:none">Berhenti</button>
  <div id="wmsg"></div>
</div>
<div id="wls" class="box">...</div>
<div class="box">
  <button onclick="wtst()">Tes kata (ucapkan satu kalimat)</button>
  <div id="wtr"></div>
  <small>Batas rasio kata (1.0 - 3.0). Naikkan jika sering tidak dikenali, turunkan jika orang lain ikut dikenali.</small>
  <input id="wtol" type="number" min="1" max="3" step="0.1" value="1.5">
  <button onclick="wsetTol()">Simpan batas kata</button>
</div>
<h3>Tes suara</h3>
<div class="box">
  <button onclick="tst()">Tes (ucapkan "Hi ESP")</button>
  <div id="tr"></div>
</div>
<h3>Toleransi</h3>
<div class="box">
  <small>Naikkan jika suaramu sering tidak dikenali. Turunkan jika orang lain ikut dikenali. 1.0 - 3.0</small>
  <input id="tol" type="number" min="1" max="3" step="0.1" value="1.5">
  <button onclick="setTol()">Simpan toleransi</button>
</div>
<p><a href="/">&larr; Kembali ke pengaturan</a></p>
<script>
var step = 1;
function sleep(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }
function $(id) { return document.getElementById(id); }
async function countdown(el) {
  for (var i = 3; i > 0; i--) { el.textContent = 'Siap... ' + i; await sleep(1000); }
  el.textContent = 'Ucapkan "Hi ESP" SEKARANG!';
}
async function post(url) {
  try { var r = await fetch(url, { method: 'POST' }); return await r.json(); }
  catch (e) { return { ok: false, msg: 'Koneksi ke robot gagal.' }; }
}
async function load() {
  try {
    var r = await fetch('/voice/list'); var d = await r.json();
    $('tol').value = d.tol;
    var ls = $('ls'); ls.textContent = '';
    if (!d.profiles.length) { ls.textContent = 'Belum ada profil.'; return; }
    d.profiles.forEach(function (p) {
      var row = document.createElement('div'); row.className = 'row';
      var t = document.createElement('span'); t.textContent = p.name + ' (' + p.samples + ' rekaman)';
      var b = document.createElement('button'); b.className = 'del'; b.textContent = 'Hapus';
      b.onclick = async function () {
        if (!confirm('Hapus profil ' + p.name + '?')) return;
        await post('/voice/del?name=' + encodeURIComponent(p.name)); load();
      };
      row.appendChild(t); row.appendChild(b); ls.appendChild(row);
    });
  } catch (e) { $('ls').textContent = 'Gagal membaca daftar.'; }
}
async function rec() {
  var name = $('nm').value.trim();
  if (!name) { $('msg').textContent = 'Isi nama dulu.'; return; }
  $('bt').disabled = true;
  await countdown($('msg'));
  var d = await post('/voice/rec?name=' + encodeURIComponent(name) + '&step=' + step);
  $('msg').textContent = d.msg;
  if (d.ok) {
    if (d.done) { step = 1; $('bt').textContent = 'Rekam 1/3'; load(); }
    else { step = d.count + 1; $('bt').textContent = 'Rekam ' + step + '/3'; }
  } else if (step > 1 && /Urutan/.test(d.msg)) { step = 1; $('bt').textContent = 'Rekam 1/3'; }
  $('bt').disabled = false;
}
async function tst() {
  await countdown($('tr'));
  var d = await post('/voice/test');
  if (!d.ok) { $('tr').textContent = d.msg; return; }
  var s = (d.known ? 'DIKENAL: ' + d.name : (d.ambiguous ? 'RAGU (mirip lebih dari satu profil) = tidak disapa' : 'TIDAK DIKENAL')) + ' (batas rasio ' + d.tol + ')';
  d.scores.forEach(function (x) { s += '\n' + x.name + ': rasio ' + x.ratio.toFixed(2) + ' (jarak ' + x.dist.toFixed(1) + ')'; });
  $('tr').style.whiteSpace = 'pre-line'; $('tr').textContent = s;
}
async function setTol() {
  var d = await post('/voice/tol?v=' + encodeURIComponent($('tol').value));
  alert(d.msg);
}
load();

var WORDS = ['nyalakan','matikan','siapa','baca','tolong','oke','kamu','jam berapa','tanggal berapa','saya','coba','lampu','buka','tutup','putar','terima kasih'];
var wdata = {}; var wstop = false; var wbusy = false;
function wstopNow() { wstop = true; $('wmsg').textContent = 'Berhenti setelah rekaman ini...'; }
async function wload() {
  try {
    var r = await fetch('/voice/wlist'); var d = await r.json();
    $('wtol').value = d.tol; wdata = {};
    var ls = $('wls'); ls.textContent = '';
    if (!d.profiles.length) ls.textContent = 'Belum ada kata harian.';
    d.profiles.forEach(function (p) {
      wdata[p.name.toLowerCase()] = p.words;
      var row = document.createElement('div'); row.className = 'row'; row.style.display = 'block';
      var t = document.createElement('div'); t.textContent = p.name + ' (' + p.words.length + ' kata)';
      row.appendChild(t);
      p.words.forEach(function (w) {
        var c = document.createElement('span'); c.className = 'chip on'; c.textContent = w;
        var x = document.createElement('small'); x.textContent = 'x';
        x.onclick = async function () {
          if (!confirm('Hapus kata ' + w + ' milik ' + p.name + '?')) return;
          await post('/voice/wdel?name=' + encodeURIComponent(p.name) + '&word=' + encodeURIComponent(w)); wload();
        };
        c.appendChild(x); row.appendChild(c);
      });
      var b = document.createElement('button'); b.className = 'del'; b.style.marginTop = '6px'; b.textContent = 'Hapus semua kata ' + p.name;
      b.onclick = async function () {
        if (!confirm('Hapus semua kata ' + p.name + '?')) return;
        await post('/voice/wdel?name=' + encodeURIComponent(p.name)); wload();
      };
      row.appendChild(b); ls.appendChild(row);
    });
    wchips();
  } catch (e) { $('wls').textContent = 'Gagal membaca daftar kata.'; }
}
function wchips() {
  var box = $('chips'); box.textContent = '';
  var have = wdata[$('wn').value.trim().toLowerCase()] || [];
  WORDS.forEach(function (w) {
    var c = document.createElement('span'); c.className = 'chip' + (have.indexOf(w) >= 0 ? ' on' : ''); c.textContent = w;
    c.onclick = function () { $('ww').value = w; };
    box.appendChild(c);
  });
}
async function wcount(word) {
  var el = $('wmsg');
  for (var i = 3; i > 0; i--) { el.textContent = 'Siap... ' + i; await sleep(900); }
  el.textContent = 'Ucapkan "' + word + '" SEKARANG!';
}
async function wrecStep(name, word, step) {
  await wcount(word);
  var d = await post('/voice/wrec?name=' + encodeURIComponent(name) + '&word=' + encodeURIComponent(word) + '&step=' + step);
  $('wmsg').textContent = word + ': ' + d.msg;
  return d;
}
var wst = 1;
async function wrec() {
  var name = $('wn').value.trim(), word = $('ww').value.trim();
  if (!name || !word) { $('wmsg').textContent = 'Isi nama dan kata dulu.'; return; }
  if (wbusy) return; wbusy = true; $('wbt').disabled = true;
  var d = await wrecStep(name, word, wst);
  if (d.ok) {
    if (d.done) { wst = 1; $('wbt').textContent = 'Rekam kata 1/2'; wload(); }
    else { wst = d.count + 1; $('wbt').textContent = 'Rekam kata ' + wst + '/2'; }
  } else if (wst > 1 && /Urutan/.test(d.msg)) { wst = 1; $('wbt').textContent = 'Rekam kata 1/2'; }
  $('wbt').disabled = false; wbusy = false;
}
async function wauto() {
  var name = $('wn').value.trim();
  if (!name) { $('wmsg').textContent = 'Isi nama dulu.'; return; }
  if (wbusy) return; wbusy = true; wstop = false;
  $('wall').disabled = true; $('wbt').disabled = true; $('wstop').style.display = 'block';
  var have = (wdata[name.toLowerCase()] || []).slice();
  for (var i = 0; i < WORDS.length && !wstop; i++) {
    var w = WORDS[i]; if (have.indexOf(w) >= 0) continue;
    var s = 1, fails = 0;
    while (s <= 2 && !wstop) {
      var d = await wrecStep(name, w, s);
      if (d.ok) { s = d.done ? 3 : s + 1; fails = 0; }
      else if (/penuh/.test(d.msg)) { wstop = true; }
      else if (/Urutan/.test(d.msg)) { s = 1; }
      else { fails++; if (fails >= 4) { wstop = true; } await sleep(1200); }
    }
    if (s > 2) { have.push(w); await wload(); }
  }
  $('wmsg').textContent = wstop ? 'Berhenti. Kata yang sudah selesai tetap tersimpan.' : 'Semua kata saran selesai.';
  $('wall').disabled = false; $('wbt').disabled = false; $('wstop').style.display = 'none'; wbusy = false;
  wload();
}
async function wtst() {
  await wcount('kalimat apa saja');
  var d = await post('/voice/wtest');
  var el = $('wtr');
  if (!d.ok) { el.textContent = d.msg; return; }
  var s = (d.known ? 'DIKENAL: ' + d.name + ' (kata "' + d.word + '")' : (d.ambiguous ? 'RAGU (mirip lebih dari satu profil) = tidak dikenali' : 'TIDAK DIKENAL')) + ' (batas rasio ' + d.tol + ')';
  d.scores.forEach(function (x) { s += '\n' + x.name + ': kata "' + x.word + '" rasio ' + x.ratio.toFixed(2); });
  el.style.whiteSpace = 'pre-line'; el.textContent = s;
}
async function wsetTol() {
  var d = await post('/voice/wtol?v=' + encodeURIComponent($('wtol').value));
  alert(d.msg);
}
$('wn').addEventListener('input', wchips);
fetch('/voice/wprep', { method: 'POST' }).catch(function () {});
wload();
</script>
</body>
</html>
)rawliteral";

static void url_decode(char *dst, size_t cap, const char *src)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < cap; ++i) {
        if (src[i] == '%' && src[i + 1] && src[i + 2]) {
            unsigned int h = 0;
            if (sscanf(src + i + 1, "%2x", &h) == 1) {
                dst[j++] = (char)h;
                i += 2;
                continue;
            }
        }
        dst[j++] = (src[i] == '+') ? ' ' : src[i];
    }
    dst[j] = '\0';
}

static bool query_value(httpd_req_t *req, const char *key, char *out, size_t cap)
{
    char query[200];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return false;
    char raw[128];
    if (httpd_query_key_value(query, key, raw, sizeof(raw)) != ESP_OK) return false;
    url_decode(out, cap, raw);
    return true;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = text ? httpd_resp_sendstr(req, text) : httpd_resp_send_500(req);
    if (text) cJSON_free(text);
    cJSON_Delete(root);
    return err;
}

static esp_err_t send_simple(httpd_req_t *req, bool ok, const char *msg)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", ok);
    cJSON_AddStringToObject(r, "msg", msg ? msg : "");
    return send_json(req, r);
}

static int16_t *rec_buffer(void)
{
    if (!s_rec)
        s_rec = (int16_t *)heap_caps_malloc(REC_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s_rec;
}

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, VOICE_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t list_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "tol", (double)speaker_id_get_tolerance());
    cJSON_AddNumberToObject(root, "max", SPEAKER_ID_MAX_PROFILES);
    cJSON *arr = cJSON_AddArrayToObject(root, "profiles");
    for (int i = 0; i < speaker_id_profile_count(); ++i) {
        char name[SPEAKER_ID_NAME_MAX + 1];
        int samples = 0;
        if (!speaker_id_get_profile(i, name, sizeof(name), &samples)) continue;
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "name", name);
        cJSON_AddNumberToObject(p, "samples", samples);
        cJSON_AddItemToArray(arr, p);
    }
    return send_json(req, root);
}

static esp_err_t rec_post(httpd_req_t *req)
{
    char name[96] = "";
    char step_text[8] = "";
    if (!query_value(req, "name", name, sizeof(name)))
        return send_simple(req, false, speaker_id_status_text(SPEAKER_ID_ERR_NAME));
    int step = 1;
    if (query_value(req, "step", step_text, sizeof(step_text))) step = atoi(step_text);

    int16_t *buf = rec_buffer();
    if (!buf) return send_simple(req, false, speaker_id_status_text(SPEAKER_ID_ERR_NOMEM));

    const size_t got = speaker_id_record(buf, REC_SAMPLES, REC_MS + 1500);
    int count = 0;
    bool done = false;
    const speaker_id_status_t st = speaker_id_enroll_sample(name, step, buf, got, &count, &done);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", st == SPEAKER_ID_OK);
    cJSON_AddNumberToObject(r, "count", count);
    cJSON_AddBoolToObject(r, "done", done);
    char msg[160];
    if (st != SPEAKER_ID_OK) {
        snprintf(msg, sizeof(msg), "%s", speaker_id_status_text(st));
    } else if (done) {
        snprintf(msg, sizeof(msg), "Profil %s tersimpan!", name);
    } else {
        snprintf(msg, sizeof(msg), "Rekaman %d/%d berhasil. Lanjut rekaman berikutnya.",
                 count, SPEAKER_ID_SAMPLES_PER_PROFILE);
    }
    cJSON_AddStringToObject(r, "msg", msg);
    ESP_LOGI(TAG, "Rekam name=%s step=%d sampel=%u -> %s", name, step, (unsigned)got, msg);
    return send_json(req, r);
}

static esp_err_t test_post(httpd_req_t *req)
{
    int16_t *buf = rec_buffer();
    if (!buf) return send_simple(req, false, speaker_id_status_text(SPEAKER_ID_ERR_NOMEM));

    const size_t got = speaker_id_record(buf, REC_SAMPLES, REC_MS + 1500);
    speaker_id_result_t res;
    const speaker_id_status_t st = speaker_id_identify(buf, got, &res);
    if (st != SPEAKER_ID_OK) return send_simple(req, false, speaker_id_status_text(st));

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddBoolToObject(r, "known", res.known);
    cJSON_AddBoolToObject(r, "ambiguous", res.ambiguous);
    cJSON_AddStringToObject(r, "name", res.name);
    cJSON_AddNumberToObject(r, "tol", (double)speaker_id_get_tolerance());
    cJSON *arr = cJSON_AddArrayToObject(r, "scores");
    for (int i = 0; i < res.score_count; ++i) {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "name", res.scores[i].name);
        cJSON_AddNumberToObject(s, "ratio", (double)res.scores[i].ratio);
        cJSON_AddNumberToObject(s, "dist", (double)res.scores[i].distance);
        cJSON_AddItemToArray(arr, s);
    }
    return send_json(req, r);
}

static esp_err_t del_post(httpd_req_t *req)
{
    char name[96] = "";
    if (!query_value(req, "name", name, sizeof(name))) return send_simple(req, false, "Nama kosong.");
    const bool ok = speaker_id_delete(name);
    return send_simple(req, ok, ok ? "Profil dihapus." : "Profil tidak ditemukan.");
}

static esp_err_t tol_post(httpd_req_t *req)
{
    char value[16] = "";
    if (!query_value(req, "v", value, sizeof(value))) return send_simple(req, false, "Nilai kosong.");
    const float v = strtof(value, nullptr);
    const bool ok = speaker_id_set_tolerance(v);
    return send_simple(req, ok, ok ? "Toleransi tersimpan." : "Nilai harus 1.0 sampai 3.0.");
}

/* ---------- Kata harian ---------- */
#define WREC_MS      2000
#define WREC_SAMPLES (SPEAKER_ID_SAMPLE_RATE * WREC_MS / 1000)

static esp_err_t wprep_post(httpd_req_t *req)
{
    audio_hal_afe_init();   /* siapkan AFE sekali, supaya rekaman pertama tidak terlambat */
    return send_simple(req, true, "siap");
}

static esp_err_t wlist_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "tol", (double)speaker_id_word_get_tolerance());
    cJSON *arr = cJSON_AddArrayToObject(root, "profiles");
    for (int i = 0; i < speaker_id_word_profile_count(); ++i) {
        char name[SPEAKER_ID_NAME_MAX + 1];
        int n = 0;
        if (!speaker_id_word_get_profile(i, name, sizeof(name), &n)) continue;
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "name", name);
        cJSON *words = cJSON_AddArrayToObject(p, "words");
        for (int w = 0; w < n; ++w) {
            char word[SPEAKER_WORD_NAME_MAX + 1];
            if (speaker_id_word_get_word(i, w, word, sizeof(word)))
                cJSON_AddItemToArray(words, cJSON_CreateString(word));
        }
        cJSON_AddItemToArray(arr, p);
    }
    return send_json(req, root);
}

static esp_err_t wrec_post(httpd_req_t *req)
{
    char name[96] = "";
    char word[64] = "";
    char step_text[8] = "";
    if (!query_value(req, "name", name, sizeof(name)) || !query_value(req, "word", word, sizeof(word)))
        return send_simple(req, false, speaker_id_status_text(SPEAKER_ID_ERR_NAME));
    int step = 1;
    if (query_value(req, "step", step_text, sizeof(step_text))) step = atoi(step_text);

    int16_t *buf = rec_buffer();
    if (!buf) return send_simple(req, false, speaker_id_status_text(SPEAKER_ID_ERR_NOMEM));

    const size_t got = speaker_id_record_conv(buf, WREC_SAMPLES, WREC_MS + 2500);
    int count = 0;
    bool done = false;
    const speaker_id_status_t st = speaker_id_word_enroll(name, word, step, buf, got, &count, &done);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", st == SPEAKER_ID_OK);
    cJSON_AddNumberToObject(r, "count", count);
    cJSON_AddBoolToObject(r, "done", done);
    char msg[160];
    if (st != SPEAKER_ID_OK) snprintf(msg, sizeof(msg), "%s", speaker_id_status_text(st));
    else if (done) snprintf(msg, sizeof(msg), "tersimpan!");
    else snprintf(msg, sizeof(msg), "rekaman %d/%d berhasil.", count, SPEAKER_WORD_SAMPLES);
    cJSON_AddStringToObject(r, "msg", msg);
    ESP_LOGI(TAG, "Rekam kata name=%s word=%s step=%d sampel=%u -> %s", name, word, step, (unsigned)got, msg);
    return send_json(req, r);
}

static esp_err_t wdel_post(httpd_req_t *req)
{
    char name[96] = "";
    char word[64] = "";
    if (!query_value(req, "name", name, sizeof(name))) return send_simple(req, false, "Nama kosong.");
    const bool has_word = query_value(req, "word", word, sizeof(word));
    const bool ok = speaker_id_word_delete(name, has_word ? word : nullptr);
    return send_simple(req, ok, ok ? "Dihapus." : "Tidak ditemukan.");
}

static esp_err_t wtest_post(httpd_req_t *req)
{
    int16_t *buf = rec_buffer();
    if (!buf) return send_simple(req, false, speaker_id_status_text(SPEAKER_ID_ERR_NOMEM));

    const size_t got = speaker_id_record_conv(buf, REC_SAMPLES, REC_MS + 2500);
    speaker_word_result_t res;
    const speaker_id_status_t st = speaker_id_word_identify(buf, got, &res);
    if (st != SPEAKER_ID_OK) return send_simple(req, false, speaker_id_status_text(st));

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddBoolToObject(r, "known", res.known);
    cJSON_AddBoolToObject(r, "ambiguous", res.ambiguous);
    cJSON_AddStringToObject(r, "name", res.name);
    cJSON_AddStringToObject(r, "word", res.word);
    cJSON_AddNumberToObject(r, "tol", (double)speaker_id_word_get_tolerance());
    cJSON *arr = cJSON_AddArrayToObject(r, "scores");
    for (int i = 0; i < res.score_count; ++i) {
        cJSON *x = cJSON_CreateObject();
        cJSON_AddStringToObject(x, "name", res.scores[i].name);
        cJSON_AddStringToObject(x, "word", res.scores[i].word);
        cJSON_AddNumberToObject(x, "ratio", (double)res.scores[i].ratio);
        cJSON_AddItemToArray(arr, x);
    }
    return send_json(req, r);
}

static esp_err_t wtol_post(httpd_req_t *req)
{
    char value[16] = "";
    if (!query_value(req, "v", value, sizeof(value))) return send_simple(req, false, "Nilai kosong.");
    const bool ok = speaker_id_word_set_tolerance(strtof(value, nullptr));
    return send_simple(req, ok, ok ? "Batas kata tersimpan." : "Nilai harus 1.0 sampai 3.0.");
}

extern "C" void speaker_id_web_register(void *httpd_handle)
{
    httpd_handle_t server = (httpd_handle_t)httpd_handle;
    if (!server) return;

    struct Route { const char *uri; httpd_method_t method; esp_err_t (*handler)(httpd_req_t *); };
    static const Route routes[] = {
        {"/voice",      HTTP_GET,  page_get},
        {"/voice/list", HTTP_GET,  list_get},
        {"/voice/rec",  HTTP_POST, rec_post},
        {"/voice/test", HTTP_POST, test_post},
        {"/voice/del",  HTTP_POST, del_post},
        {"/voice/tol",  HTTP_POST, tol_post},
        {"/voice/wprep", HTTP_POST, wprep_post},
        {"/voice/wlist", HTTP_GET,  wlist_get},
        {"/voice/wrec",  HTTP_POST, wrec_post},
        {"/voice/wdel",  HTTP_POST, wdel_post},
        {"/voice/wtest", HTTP_POST, wtest_post},
        {"/voice/wtol",  HTTP_POST, wtol_post},
    };
    for (const Route &r : routes) {
        httpd_uri_t uri = {};
        uri.uri = r.uri;
        uri.method = r.method;
        uri.handler = r.handler;
        uri.user_ctx = nullptr;
        if (httpd_register_uri_handler(server, &uri) != ESP_OK)
            ESP_LOGE(TAG, "Gagal mendaftarkan %s", r.uri);
    }
    ESP_LOGI(TAG, "Halaman profil suara aktif di /voice");
}
