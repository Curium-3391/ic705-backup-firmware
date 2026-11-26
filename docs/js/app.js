// IC-705 Full-Storage Read-Only Backup Firmware Web Patcher
// Runs 100% locally in your browser via Web Worker. Zero server uploads.

import { reconstructLogicalFromPhysical, splitLogicalToPhysical, inspectLogicalAnchors } from './eeprom_tools.js';

const $ = (id) => document.getElementById(id);

const STAGES = {
  hash: ['Verifying input MD5 & SHA-256', 0, 6],
  decode: ['Decoding official container', 6, 20],
  patch: ['Applying 4-byte hook + open-source payload', 20, 24],
  compress: ['Compressing image (Okumura LZSS)', 24, 92],
  seal: ['Re-sealing container & verifying MD5 + SHA-256', 92, 99],
  done: ['Done', 100, 100],
};

let index = null;
let fileBuf = null;
let fileName = '';
let current = null;
let worker = null;
let jobId = 0;
let busy = false;

function getWorker() {
  if (!worker) worker = new Worker(new URL('./worker.js', import.meta.url), { type: 'module' });
  return worker;
}

function runWorker(msg, onProgress) {
  return new Promise((resolve, reject) => {
    const w = getWorker();
    const id = ++jobId;
    const handler = (ev) => {
      const m = ev.data;
      if (m.id !== id) return;
      if (m.type === 'progress') {
        if (onProgress) onProgress(m.stage, m.frac);
        return;
      }
      w.removeEventListener('message', handler);
      if (m.type === 'error') reject(m);
      else resolve(m);
    };
    w.addEventListener('message', handler);
    w.onerror = (e) => {
      w.removeEventListener('message', handler);
      worker = null;
      reject({ code: 'worker', message: e.message || 'Web Worker failed to start' });
    };
    w.postMessage({ ...msg, id });
  });
}

function el(tag, attrs = {}, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === 'class') e.className = v;
    else e.setAttribute(k, v);
  }
  for (const k of kids) e.append(k);
  return e;
}

function setStatus(node, kind, title, text) {
  node.hidden = false;
  node.className = 'status ' + kind;
  node.replaceChildren();
  node.append(el('strong', {}, title));
  if (text) node.append(el('div', {}, text));
}

function fmtSize(n) {
  return n.toLocaleString('en') + ' bytes (' + (n / 1048576).toFixed(2) + ' MiB)';
}

async function loadIndex() {
  const r = await fetch('patches/index.json', { cache: 'no-cache' });
  if (!r.ok) throw new Error('Cannot load patches/index.json (' + r.status + ')');
  index = await r.json();
  const tb = $('supported').tBodies[0];
  tb.replaceChildren();

  for (const e of index.releases) {
    const rel = e.release;
    tb.append(
      el(
        'tr',
        {},
        el('td', {}, `${e.input.radio} — ${e.input.region}`),
        el('td', {}, el('code', {}, e.input.file), el('br'), el('code', { class: 'hash small' }, 'SHA-256: ' + e.input.sha256)),
        el('td', {}, el('strong', {}, rel.label), el('br'), el('span', { class: 'badge ok' }, 'Hardware Verified (JP Only)')),
        el('td', {}, el('code', { class: 'hash small' }, rel.output_sha256))
      )
    );
  }
  for (const u of index.unsupported_regions_seeking_contributors || []) {
    tb.append(
      el(
        'tr',
        { class: 'muted-row' },
        el('td', {}, `${u.radio} — ${u.region}`),
        el('td', {}, 'Not yet supported'),
        el('td', {}, el('span', { class: 'badge warn' }, 'Seeking Contributors')),
        el('td', {}, u.note)
      )
    );
  }
}

function resetRelease() {
  current = null;
  $('release').hidden = true;
  $('result').hidden = true;
  $('progress').hidden = true;
  $('accept').checked = false;
  updateBuildButton();
}

function updateBuildButton() {
  const ok = !!current && $('accept').checked && !busy;
  $('build').disabled = !ok;
  $('build-hint').textContent = busy
    ? 'Working locally in browser…'
    : current && !$('accept').checked
      ? 'Tick the confirmation checkbox above to enable.'
      : '';
}

async function onFile(file) {
  if (!file || busy) return;
  resetRelease();
  fileName = file.name;
  $('fileinfo').hidden = false;
  $('fi-name').textContent = file.name;
  $('fi-size').textContent = fmtSize(file.size);
  $('fi-md5').textContent = 'computing…';
  $('fi-sha256').textContent = 'computing…';
  $('fi-status').hidden = true;

  if (file.size > 64 * 1048576) {
    setStatus($('fi-status'), 'err', 'Not an Icom firmware file', 'File exceeds 64 MiB.');
    return;
  }

  try {
    fileBuf = await file.arrayBuffer();
    const { md5, sha256 } = await runWorker({ type: 'hash', buf: fileBuf.slice(0) });
    if (file.name !== fileName) return;
    $('fi-md5').textContent = md5;
    $('fi-sha256').textContent = sha256;
    await indexReady;

    const entry = index && index.releases.find((e) => e.input.md5 === md5 && e.input.sha256 === sha256);
    if (!entry) {
      setStatus(
        $('fi-status'),
        'err',
        'Unsupported or non-matching firmware file',
        'This digest does not match a currently supported official Icom .dat file. ' +
          'Right now ONLY the Japanese domestic IC-705 v1.40 file (705J140.dat) is supported and hardware-verified. ' +
          'If you have an EU/Global/US IC-705 (e.g. 705E140.dat) and want to help port or verify it, please open an issue or PR on GitHub.'
      );
      return;
    }

    const nameNote =
      entry.input.file !== file.name
        ? ` (filename differs, but bytes are identical to official ${entry.input.file})`
        : '';
    setStatus(
      $('fi-status'),
      'ok',
      `Recognised: official ${entry.input.radio} ${entry.input.region} v${entry.input.version} (${entry.input.file})`,
      nameNote.trim()
    );
    showRelease(entry);
  } catch (e) {
    setStatus($('fi-status'), 'err', 'Could not read the file', String(e.message || e));
  }
}

function showRelease(entry) {
  current = entry;
  const r = entry.release;
  $('rel-title').textContent = r.title;
  $('rel-meta').textContent = `Build ${r.build_id} · ${r.date} · patch ${r.patch_file}`;
  const badges = $('rel-badges');
  badges.replaceChildren(
    el('span', { class: 'badge ok' }, 'Zero Storage Writes'),
    el('span', { class: 'badge ok' }, 'Stock Bootloader Untouched'),
    el('span', { class: 'badge warn' }, 'JP J140 Verified Only')
  );
  const ul = $('rel-features');
  ul.replaceChildren();
  for (const f of r.features || []) ul.append(el('li', {}, f));
  $('rel-in').textContent = entry.input.file;
  $('rel-in-sha256').textContent = entry.input.sha256;
  $('rel-out-md5').textContent = r.output_md5;
  $('rel-out-sha256').textContent = r.output_sha256;
  $('release').hidden = false;
  updateBuildButton();
}

function showProgress(stage, frac) {
  const s = STAGES[stage] || [stage, 0, 0];
  const pct = Math.round(s[1] + (s[2] - s[1]) * (frac || 0));
  $('bar').style.width = pct + '%';
  $('progress').querySelector('.bar').setAttribute('aria-valuenow', String(pct));
  $('stage').textContent =
    s[0] + (stage === 'compress' ? ` — ${Math.round((frac || 0) * 100)}%` : '') + (stage === 'done' ? '' : '…');
}

async function build() {
  if (!current || busy || !fileBuf) return;
  busy = true;
  updateBuildButton();
  $('result').hidden = true;
  $('progress').hidden = false;
  showProgress('hash', 0);
  const t0 = performance.now();
  const tick = setInterval(() => {
    $('elapsed').textContent = ((performance.now() - t0) / 1000).toFixed(1) + ' s';
  }, 200);

  try {
    const pr = await fetch('patches/' + encodeURIComponent(current.release.patch_file), { cache: 'no-cache' });
    if (!pr.ok) throw { code: 'fetch', message: 'Cannot load patch JSON (' + pr.status + ')' };
    const patch = await pr.json();
    if (
      patch.input.sha256 !== current.input.sha256 ||
      patch.output.sha256 !== current.release.output_sha256 ||
      patch.output.md5 !== current.release.output_md5
    ) {
      throw { code: 'bad_patch', message: 'Patch JSON metadata does not match release index' };
    }

    const res = await runWorker({ type: 'build', buf: fileBuf.slice(0), patch }, showProgress);
    const secs = ((performance.now() - t0) / 1000).toFixed(1);
    if (res.sha256 !== current.release.output_sha256 || res.md5 !== current.release.output_md5) {
      throw { code: 'output_mismatch', message: `Unexpected digest MD5=${res.md5}, SHA-256=${res.sha256}` };
    }

    showProgress('done', 1);
    const outName = current.release.name + '.dat';
    const url = URL.createObjectURL(new Blob([res.out], { type: 'application/octet-stream' }));
    const dl = el('a', { href: url, download: outName, class: 'button primary' }, 'Download ' + outName);
    const box = $('result');
    setStatus(
      box,
      'ok',
      `Built in ${secs} s — MD5 & SHA-256 match the hardware-verified build byte-for-byte.`,
      `SHA-256: ${res.sha256}`
    );
    box.append(el('div', { class: 'actions' }, dl));
    box.append(
      el(
        'p',
        { class: 'muted small' },
        'Important: Flash this .dat FIRST with NO armed .TRG file on the SD card. Only add RB3_FULL_BACKUP.TRG after flashing completes. Keep your official 705J140.dat so you can restore stock firmware afterwards.'
      )
    );
    dl.click();
  } catch (e) {
    const msgs = {
      wrong_input: 'The selected file does not match the required official Icom .dat.',
      output_mismatch: 'The built file is NOT byte-identical to the hardware-verified build, so download was blocked.',
      decode_mismatch: 'Decoded ARM image did not match expected official signature.',
      patch_mismatch: 'Patched ARM image digest mismatch.',
    };
    setStatus(
      $('result'),
      'err',
      'Build failed — nothing to download',
      (msgs[e.code] ? msgs[e.code] + ' ' : '') + 'Details: ' + (e.message || e)
    );
    $('progress').hidden = true;
  } finally {
    clearInterval(tick);
    busy = false;
    updateBuildButton();
  }
}

function downloadTrigger(filename) {
  const blob = new Blob(['ARMED\r\n'], { type: 'text/plain' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

async function runLocalEepromReconstruct() {
  const f705 = $('ee-705').files[0];
  const f706 = $('ee-706').files[0];
  const outBox = $('ee-result');
  if (!f705 || !f706) {
    setStatus(outBox, 'err', 'Select both files', 'Choose IC705_EEPROM.bin (131,072 B) and IC706_EEPROM.bin (131,072 B) from the same backup session.');
    return;
  }
  try {
    const b705 = new Uint8Array(await f705.arrayBuffer());
    const b706 = new Uint8Array(await f706.arrayBuffer());
    const logical = reconstructLogicalFromPhysical(b705, b706);
    const [rt705, rt706] = splitLogicalToPhysical(logical);
    let exact = rt705.length === b705.length && rt706.length === b706.length;
    for (let i = 0; exact && i < b705.length; i++) {
      if (rt705[i] !== b705[i] || rt706[i] !== b706[i]) exact = false;
    }
    const anchors = await inspectLogicalAnchors(logical);
    const allOk = exact && anchors.sum8_domain1.valid && anchors.sum8_domain2.valid;
    const url = URL.createObjectURL(new Blob([logical], { type: 'application/octet-stream' }));
    const dl = el('a', { href: url, download: 'EEPROM_logical.bin', class: 'button primary' }, 'Download EEPROM_logical.bin (256 KiB)');
    setStatus(
      outBox,
      allOk ? 'ok' : 'err',
      allOk
        ? 'EEPROM_logical.bin reconstructed locally — bijection & SUM8 checksums valid'
        : 'Reconstructed, but one or more checksum checks did not match J140 anchors',
      `SHA-256: ${anchors.logical_sha256} | SUM8 #1: ${anchors.sum8_domain1.calculated}/${anchors.sum8_domain1.stored} (${anchors.sum8_domain1.valid ? 'OK' : 'MISMATCH'}) | SUM8 #2: ${anchors.sum8_domain2.calculated}/${anchors.sum8_domain2.stored} (${anchors.sum8_domain2.valid ? 'OK' : 'MISMATCH'}) | Touch marker: "${anchors.touch_marker_ascii}"`
    );
    outBox.append(el('div', { class: 'actions' }, dl));
  } catch (e) {
    setStatus(outBox, 'err', 'EEPROM reconstruction failed', String(e.message || e));
  }
}

// ---- Wire up DOM events --------------------------------------------------------------
const indexReady = loadIndex().catch((e) => {
  const tb = $('supported').tBodies[0];
  tb.replaceChildren(el('tr', {}, el('td', { colspan: '4' }, 'Could not load patches/index.json: ' + e.message)));
});

$('file').addEventListener('change', (e) => onFile(e.target.files[0]));
const drop = $('drop');
['dragenter', 'dragover'].forEach((t) =>
  drop.addEventListener(t, (e) => {
    e.preventDefault();
    drop.classList.add('over');
  })
);
['dragleave', 'drop'].forEach((t) =>
  drop.addEventListener(t, (e) => {
    e.preventDefault();
    drop.classList.remove('over');
  })
);
drop.addEventListener('drop', (e) => onFile(e.dataTransfer.files[0]));
drop.addEventListener('keydown', (e) => {
  if (e.key === 'Enter' || e.key === ' ') {
    e.preventDefault();
    $('file').click();
  }
});
window.addEventListener('dragover', (e) => e.preventDefault());
window.addEventListener('drop', (e) => e.preventDefault());
$('accept').addEventListener('change', updateBuildButton);
$('build').addEventListener('click', build);

document.querySelectorAll('[data-trigger]').forEach((btn) => {
  btn.addEventListener('click', () => downloadTrigger(btn.getAttribute('data-trigger')));
});
$('ee-run').addEventListener('click', runLocalEepromReconstruct);
