'use strict';
/* Room EQ web page. Talks to Kodi over JSON-RPC (same origin) and reads the JSON files
 * that roomeq.sh publishes into ./data/. No dependencies. MIT License. */
const $ = s => document.querySelector(s), $$ = s => [...document.querySelectorAll(s)];

/* ---------------------------------------------------------------- i18n */
const T = {
  en: {},
  ru: {
    output: 'Выход', measure: 'Замер', response: 'АЧХ', settings: 'Настройки', save: 'Сохранить',
    step1: 'Подключите измерительный микрофон к входу интерфейса, указанному в настройках (для конденсаторного включите +48 В).',
    step2: 'Поставьте микрофон на уровне ушей в точке прослушивания. Громкость: комфортно громко: свип идёт 8 с на −10 dBFS.',
    step3: 'Запишите 3-5 точек на расстоянии 30-50 см друг от друга вокруг места слушателя. Чем больше точек, тем шире и безопаснее фильтры.',
    step4: 'Посмотрите на кривые и нажмите «Применить».',
    measure_point: 'Записать точку', undo: 'Убрать последнюю', new_session: 'Новая серия', apply: 'Применить',
    preview: 'Расчёт', active_t: 'Активные', lg_points: 'точки', lg_avg: 'среднее', lg_corr: 'с EQ (расчёт)', lg_target: 'цель',
    s_mic: 'Канал микрофона', s_card: 'Звуковая карта (ALSA id)', s_format: 'Формат карты', s_fmax: 'Корректировать до, Гц',
    s_bass: 'Подъём баса в цели, дБ', s_maxf: 'Макс. фильтров', s_boost: 'Макс. подъём, дБ', s_bmin: 'Не поднимать ниже, Гц',
    s_cal: 'Файл калибровки микрофона (необязательно)',
    s_hint: 'Измерительный микрофон (ECM8000, UMIK-1 через интерфейс и т.п.): порог подъёма 20-30 Гц. Вокальный/динамический: оставьте 150 Гц, иначе «исправлялся» бы завал самого микрофона.',
    reset: 'Удалить активные фильтры',
    // dynamic
    eq_on: 'Room EQ включён', eq_off: 'Room EQ выключен', no_card: 'интерфейс не найден',
    inputs: n => `входов: ${n}`, filters_active: n => `активных фильтров: ${n}`, no_filters: 'фильтров нет, EQ прозрачен',
    points_none: 'точек пока нет', point: (i, snr) => `точка ${i} · SNR ${snr} дБ`, point_bad: (i, e) => `точка ${i}: ${e}`,
    st_measuring: 'Идёт замер, тишина…', st_recorded: n => `Записано точек: ${n}`, st_applied: n => `Применено фильтров: ${n}`,
    hz: 'Гц', db: 'дБ',
    errors: {
      'clipping': 'перегруз: уменьшите усиление микрофона или громкость',
      'signal too low': 'слишком тихо: прибавьте усиление микрофона или громкость, проверьте микрофон и фантомное питание',
      'SNR too low': 'низкий SNR: шумно или микрофон не слышит свип',
      'the audio device is busy': 'звуковая карта занята другим приложением; повторите через минуту',
      'audio interface not found': 'аудиоинтерфейс не найден',
      'the interface has no input': 'у интерфейса нет входа',
      'nothing to apply': 'нечего применять: запишите хотя бы одну удачную точку',
      'recording failed': 'запись не удалась',
    },
    st_error: m => `Ошибка: ${m}`, confirm_measure: 'Сейчас прозвучит свип ~8 с. Микрофон на месте?',
    confirm_new: 'Удалить записанные точки и начать заново?', confirm_reset: 'Удалить активные фильтры?',
    saved: 'Сохранено', no_data: 'нет данных', device_missing: 'Устройство недоступно. Интерфейс подключён?',
    summary: (u, b0, b1, x0, x1, pre) => `${u} точ. · полоса ${b0}-${b1} Гц · превышение над целью ${x0} → ${x1} дБ · преамп ${pre} дБ`,
    th: ['Частота', 'Усиление', 'Q'], nothing_apply: 'Нечего применять',
  },
};
const EN = {
  eq_on: 'Room EQ is on', eq_off: 'Room EQ is off', no_card: 'no audio interface found',
  inputs: n => `${n} inputs`, filters_active: n => `${n} filters active`, no_filters: 'no filters, EQ is transparent',
  points_none: 'no points yet', point: (i, snr) => `point ${i} · SNR ${snr} dB`, point_bad: (i, e) => `point ${i}: ${e}`,
  st_measuring: 'Measuring, keep quiet…', st_recorded: n => `Points recorded: ${n}`, st_applied: n => `Filters applied: ${n}`,
  hz: 'Hz', db: 'dB', errors: {},
  st_error: m => `Error: ${m}`, confirm_measure: 'A sweep will play for ~8 s. Is the mic in place?',
  confirm_new: 'Delete the recorded points and start over?', confirm_reset: 'Remove the active filters?',
  saved: 'Saved', no_data: 'no data', device_missing: 'Device not available. Is the interface connected?',
  summary: (u, b0, b1, x0, x1, pre) => `${u} point(s) · band ${b0}-${b1} Hz · peaks above target ${x0} → ${x1} dB · preamp ${pre} dB`,
  th: ['Frequency', 'Gain', 'Q'], nothing_apply: 'Nothing to apply',
};
let lang = (() => { try { return localStorage.getItem('lang') || (navigator.language || 'en').slice(0, 2); } catch (e) { return 'en'; } })();
if (!T[lang]) lang = 'en';
const t = (k, ...a) => { const v = (T[lang] && T[lang][k]) ?? EN[k]; return typeof v === 'function' ? v(...a) : v; };
function translate() {
  document.documentElement.lang = lang;
  $$('[data-t]').forEach(el => {
    if (!el.dataset.en) el.dataset.en = el.textContent;
    el.textContent = lang === 'en' ? el.dataset.en : (T[lang][el.dataset.t] || el.dataset.en);
  });
  $('#lang').textContent = lang === 'en' ? 'RU' : 'EN';
}
$('#lang').onclick = () => { lang = lang === 'en' ? 'ru' : 'en'; try { localStorage.setItem('lang', lang); } catch (e) {} translate(); refresh(); };

/* ---------------------------------------------------------------- plumbing */
let rid = 0;
async function rpc(method, params) {
  const r = await fetch('/jsonrpc', { method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ jsonrpc: '2.0', id: ++rid, method, params: params || {} }) });
  const j = await r.json();
  if (j.error) throw new Error(j.error.message);
  return j.result;
}
const helper = params => rpc('Addons.ExecuteAddon', { addonid: 'script.roomeq', params, wait: false });
const data = name => fetch('data/' + name + '?t=' + Date.now()).then(r => r.ok ? r.json() : null).catch(() => null);
const sleep = ms => new Promise(r => setTimeout(r, ms));
function toast(m) { const e = $('#toast'); e.textContent = m; e.classList.add('on'); clearTimeout(toast.h); toast.h = setTimeout(() => e.classList.remove('on'), 2800); }
const esc = s => String(s ?? '').replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

/* ---------------------------------------------------------------- output device */
let devOptions = [];
async function loadDevices() {
  const r = await rpc('Settings.GetSettings', { level: 'expert', filter: { section: 'system', category: 'audio' } });
  const s = r.settings.find(x => x.id === 'audiooutput.audiodevice');
  devOptions = (s && s.options || []).map(o => o.value);
  return s ? s.value : '';
}
const devFor = name => devOptions.find(v => v.split('|')[0] === 'ALSA:' + name) || 'ALSA:' + name;
async function setEq(on) {
  try { await rpc('Settings.SetSettingValue', { setting: 'audiooutput.audiodevice', value: devFor(on ? 'roomeq' : 'roomeq_direct') }); }
  catch (e) { toast(t('device_missing')); }
  await refresh();
}
$('#eq').onchange = e => setEq(e.target.checked);

/* ---------------------------------------------------------------- status */
async function refresh() {
  let dev = '';
  try { dev = await loadDevices(); } catch (e) { /* Kodi busy */ }
  const name = dev.split('|')[0];
  $('#eq').checked = name === 'ALSA:roomeq';
  $('#dev').textContent = name === 'ALSA:roomeq' ? t('eq_on') : name === 'ALSA:roomeq_direct' ? t('eq_off') : (dev.split('|')[1] || dev || '-');
  await helper(['status']); await sleep(900);
  const st = await data('status.json');
  if (st) {
    $('#card').textContent = st.card ? `${st.name || st.card} · ${t('inputs', st.inputs)}` : t('no_card');
    $('#active').textContent = st.filters ? t('filters_active', (await data('report.active.json') || { filters: [] }).filters.length) : t('no_filters');
    const f = $('#settings');
    for (const [k, v] of Object.entries(st.settings || {})) if (f.elements[k]) f.elements[k].value = v;
  }
  showState(await data('state.json'));
  await showReport();
}

// script/analyzer errors arrive in English; translate the known ones by prefix
function trErr(m) {
  const d = t('errors') || {};
  for (const k in d) if (String(m).startsWith(k)) return d[k];
  return m;
}
function showState(s) {
  const el = $('#state');
  el.classList.toggle('err', !!s && s.state === 'error');
  if (!s) { el.textContent = ''; return; }
  el.textContent = s.state === 'measuring' ? t('st_measuring') : s.state === 'recorded' ? t('st_recorded', s.points)
    : s.state === 'applied' ? t('st_applied', parseInt(s.msg, 10) || 0) : s.state === 'error' ? t('st_error', trErr(s.msg)) : '';
  const busy = s.state === 'measuring' && Date.now() / 1000 - s.time < 60;
  $$('.buttons button').forEach(b => b.disabled = busy);
}

/* ---------------------------------------------------------------- chart */
let view = 'preview';
$$('#view button').forEach(b => b.onclick = () => { view = b.dataset.v; $$('#view button').forEach(x => x.classList.toggle('on', x === b)); showReport(); });

async function showReport() {
  const prev = await data('report.json'), act = await data('report.active.json');
  const pts = (prev && prev.points) || [];
  $('#points').innerHTML = pts.length ? pts.map((p, i) => p.ok
    ? `<span class="pt">${esc(t('point', i + 1, p.snr_db))}</span>`
    : `<span class="pt bad">${esc(t('point_bad', i + 1, p.error))}</span>`).join('') : `<span class="muted small">${t('points_none')}</span>`;
  const r = view === 'active' ? act : prev;
  draw(r);
  if (!r || !r.f) { $('#summary').textContent = t('no_data'); $('#filters').innerHTML = ''; return; }
  $('#summary').textContent = t('summary', r.used, r.band[0], r.band[1], r.excess_before ?? '-', r.excess_after ?? '-', r.preamp.toFixed(1));
  const th = t('th');
  $('#filters').innerHTML = `<tr><th>${th[0]}</th><th>${th[1]}</th><th>${th[2]}</th></tr>` +
    r.filters.map(f => `<tr><td>${f.fc.toFixed(1)} ${t('hz')}</td><td>${f.g > 0 ? '+' : ''}${f.g.toFixed(1)} ${t('db')}</td><td>${f.q.toFixed(2)}</td></tr>`).join('');
}

function draw(r) {
  const svg = $('#chart'), W = 640, H = 300, L = 30, B = 18;
  if (!r || !r.f) { svg.innerHTML = `<text x="${W / 2}" y="${H / 2}" fill="#8b919c" text-anchor="middle" font-size="14">${t('no_data')}</text>`; return; }
  const f0 = Math.log10(20), f1 = Math.log10(20000), lo = -20, hi = 15;
  const X = f => L + (Math.log10(f) - f0) / (f1 - f0) * (W - L - 6);
  const Y = d => 6 + (hi - Math.max(lo, Math.min(hi, d))) / (hi - lo) * (H - B - 6);
  const path = a => a.map((d, i) => (i ? 'L' : 'M') + X(r.f[i]).toFixed(1) + ',' + Y(d).toFixed(1)).join('');
  let g = `<rect x="${X(r.band[0])}" y="6" width="${X(r.band[1]) - X(r.band[0])}" height="${H - B - 6}" fill="#4fb3a9" opacity=".07"/>`;
  for (const f of [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000])
    g += `<line x1="${X(f)}" x2="${X(f)}" y1="6" y2="${H - B}" stroke="#20242b"/><text x="${X(f)}" y="${H - 4}" fill="#5f6672" font-size="10" text-anchor="middle">${f >= 1000 ? f / 1000 + 'k' : f}</text>`;
  for (let d = -15; d <= 10; d += 5)
    g += `<line x1="${L}" x2="${W - 6}" y1="${Y(d)}" y2="${Y(d)}" stroke="${d ? '#20242b' : '#363b45'}"/><text x="${L - 4}" y="${Y(d) + 3}" fill="#5f6672" font-size="10" text-anchor="end">${d}</text>`;
  for (const c of r.curves || []) g += `<path d="${path(c)}" fill="none" stroke="#4a505b" stroke-width="1"/>`;
  g += `<path d="${path(r.target)}" fill="none" stroke="#4fb3a9" stroke-width="1.5" stroke-dasharray="5 4"/>`;
  g += `<path d="${path(r.meas)}" fill="none" stroke="#c9ced8" stroke-width="1.6"/>`;
  g += `<path d="${path(r.corr)}" fill="none" stroke="#e3a535" stroke-width="2.2"/>`;
  svg.innerHTML = g;
}

/* ---------------------------------------------------------------- actions */
async function runAndFollow(cmd) {
  const t0 = Date.now() / 1000;
  await helper(['cmd=' + cmd]);
  for (let i = 0; i < 40; i++) {
    await sleep(1500);
    const s = await data('state.json');
    if (s && s.time >= t0 - 1) { showState(s); if (s.state !== 'measuring') break; }
  }
  await refresh();
}
$('#b-measure').onclick = () => confirm(t('confirm_measure')) && runAndFollow('measure');
$('#b-undo').onclick = () => runAndFollow('undo');
$('#b-new').onclick = () => confirm(t('confirm_new')) && runAndFollow('new');
$('#b-apply').onclick = () => runAndFollow('apply');
$('#b-reset').onclick = () => confirm(t('confirm_reset')) && runAndFollow('reset');
$('#settings').onsubmit = async e => {
  e.preventDefault();
  const pairs = [...new FormData(e.target)].map(([k, v]) => `${k}=${String(v).trim()}`);
  await helper(['settings', ...pairs]);
  toast(t('saved'));
  setTimeout(refresh, 1200);
};

translate();
refresh();
