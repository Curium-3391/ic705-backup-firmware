#!/usr/bin/env node
/**
 * Automated Node.js test suite for the browser ES modules:
 *   - docs/js/md5.js
 *   - docs/js/codec.js (autokey & Okumura LZSS round-trip)
 *   - docs/js/eeprom_tools.js (64-byte stripe bijection & SUM8 anchors)
 *   - docs/patches/index.json & 705J140_rb3_b5.json schema checks
 *   - Optional end-to-end real .dat build test when IC705_J140_DAT env var is set
 *     (proves browser JS produces byte-identical hardware-verified 705RB3B5.dat!)
 */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import { md5hex } from '../docs/js/md5.js';
import { autokeyEncode, autokeyDecode, lzssCompress, lzssDecompress } from '../docs/js/codec.js';
import { buildPatched, sha256hex } from '../docs/js/patch.js';
import { reconstructLogicalFromPhysical, splitLogicalToPhysical, inspectLogicalAnchors } from '../docs/js/eeprom_tools.js';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

// 1. RFC 1321 MD5 test vectors
assert.equal(md5hex(new Uint8Array(0)), 'd41d8cd98f00b204e9800998ecf8427e');
assert.equal(md5hex(new TextEncoder().encode('abc')), '900150983cd24fb0d6963f7d28e17f72');

// 2. Autokey & LZSS round-trip on synthetic pattern
const sample = new Uint8Array(4096);
for (let i = 0; i < sample.length; i++) sample[i] = (i * 17 + (i >> 4)) & 0xff;
const enc = autokeyEncode(sample, 0x64, 25, 160);
const [dec] = autokeyDecode(enc, 0x64, 25, 160);
assert.deepEqual(dec, sample, 'autokey round-trip failed');

const comp = lzssCompress(sample, 256, true);
const [decomp, used] = lzssDecompress(comp, sample.length);
assert.equal(used, comp.length);
assert.deepEqual(decomp, sample, 'LZSS round-trip failed');

// 3. EEPROM 64-byte stripe bijection & SUM8 verification on synthetic buffers
const synLogical = new Uint8Array(262144);
for (let i = 0; i < synLogical.length; i++) synLogical[i] = (i * 31 + 7) & 0xff;
let s1 = 0;
for (let i = 0x3fb47; i < 0x3fb50; i++) s1 = (s1 + synLogical[i]) & 0xff;
synLogical[0x3fb50] = s1;
let s2 = 0;
for (let i = 0x3fb51; i < 0x3fb5b; i++) s2 = (s2 + synLogical[i]) & 0xff;
synLogical[0x3fb5b] = s2;
synLogical.set(new TextEncoder().encode('SX4075 V0.10-000'), 0x3ff40);

const [p705, p706] = splitLogicalToPhysical(synLogical);
const rebuilt = reconstructLogicalFromPhysical(p705, p706);
assert.deepEqual(rebuilt, synLogical, 'EEPROM 64-byte stripe bijection failed');
const anchors = await inspectLogicalAnchors(rebuilt);
assert.equal(anchors.sum8_domain1.valid, true);
assert.equal(anchors.sum8_domain2.valid, true);
assert.equal(anchors.touch_marker_ascii, 'SX4075 V0.10-000');

// 4. Patch registry & patch JSON integrity
const index = JSON.parse(fs.readFileSync(path.join(ROOT, 'docs/patches/index.json'), 'utf8'));
assert.equal(index.releases.length >= 1, true);
const rel = index.releases[0];
const patch = JSON.parse(fs.readFileSync(path.join(ROOT, 'docs/patches', rel.release.patch_file), 'utf8'));
assert.equal(patch.input.md5, rel.input.md5);
assert.equal(patch.input.sha256, rel.input.sha256);
assert.equal(patch.output.md5, rel.release.output_md5);
assert.equal(patch.output.sha256, rel.release.output_sha256);
assert.equal(patch.bootloader_modified, false);

// 5. Optional end-to-end real .dat build verification (never committed to Git!)
const datPath = process.env.IC705_J140_DAT;
if (datPath && fs.existsSync(datPath)) {
  const raw = new Uint8Array(fs.readFileSync(datPath));
  const { out, md5, sha256 } = await buildPatched(raw, patch);
  assert.equal(out.length, patch.output.size_bytes);
  assert.equal(md5, patch.output.md5);
  assert.equal(sha256, patch.output.sha256);
  const shaCheck = await sha256hex(out);
  assert.equal(shaCheck, '17841ee947ad2a20912b289770776817963d73cfcf9d6b16b76cd8c32fc903bb');
  console.log(`PASS (with real J140 .dat E2E): built byte-identical 705RB3B5.dat SHA-256=${sha256}`);
} else {
  console.log('PASS (unit + synthetic + schema tests; set IC705_J140_DAT=/path/to/705J140.dat for E2E .dat test)');
}
