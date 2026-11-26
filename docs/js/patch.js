// Client-side patch applier for Icom IC-705 Full-Storage Read-Only Backup Firmware.
// Applies a clean-room patch JSON to a user-supplied official Icom .dat file and verifies
// BOTH MD5 and SHA-256 of the input, patched main image, and final .dat output.
// Container codec (codec.js, md5.js) adapted from Dawid SQ6EMM's icom-custom-firmware (MIT).

import { md5hex, toHex } from './md5.js';
import { extract, repack } from './codec.js';

export class PatchError extends Error {
  constructor(code, message, details = {}) {
    super(message);
    this.code = code;
    this.details = details;
  }
}

export async function sha256hex(bytes) {
  const view = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  const digest = await crypto.subtle.digest('SHA-256', view);
  return toHex(new Uint8Array(digest));
}

function parseHexInt(s) {
  if (typeof s === 'number') return s;
  const str = String(s);
  if (!/^(0x)?[0-9a-f]+$/i.test(str)) {
    throw new PatchError('bad_patch', 'Invalid hex integer in patch: ' + str);
  }
  const v = parseInt(str.replace(/^0x/i, ''), 16);
  if (!Number.isFinite(v)) throw new PatchError('bad_patch', 'Invalid hex integer in patch: ' + str);
  return v;
}

function hexToBytes(h) {
  if (typeof h !== 'string' || h.length % 2 || /[^0-9a-f]/i.test(h)) {
    throw new PatchError('bad_patch', 'Invalid hex payload in patch');
  }
  const out = new Uint8Array(h.length / 2);
  for (let i = 0; i < out.length; i++) {
    out[i] = parseInt(h.slice(2 * i, 2 * i + 2), 16);
  }
  return out;
}

function bytesEqual(a, b) {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}

/** Apply clean-room patch ops to the decoded stock ARM application image. */
export function applyOps(stock, patch) {
  const base = parseHexInt(patch.load_address ?? 0);
  const size = patch.image_size ?? stock.length;
  const img = new Uint8Array(size);
  img.set(stock.subarray(0, Math.min(size, stock.length)));

  for (const op of patch.ops) {
    const at = parseHexInt(op.at) - base;
    const data = hexToBytes(op.hex);
    if (at < 0 || at + data.length > size) {
      throw new PatchError('bad_patch', 'Patch operation falls outside target image bounds');
    }
    if (op.expected_orig_hex) {
      const expected = hexToBytes(op.expected_orig_hex);
      const actual = stock.subarray(at, at + expected.length);
      if (!bytesEqual(actual, expected)) {
        throw new PatchError(
          'decode_mismatch',
          `Original bytes at ${op.at} did not match expected instruction signature`
        );
      }
    }
    img.set(data, at);
  }
  return img;
}

/**
 * Build a verified backup firmware .dat from an unmodified official Icom .dat.
 * Runs 100% locally in memory; refuses output unless MD5 AND SHA-256 match the tested build.
 */
export async function buildPatched(raw, patch, progress = () => {}) {
  progress('hash', 0);
  const inMd5 = md5hex(raw);
  const inSha256 = await sha256hex(raw);
  if (inMd5 !== patch.input.md5 || (patch.input.sha256 && inSha256 !== patch.input.sha256)) {
    throw new PatchError(
      'wrong_input',
      `Input file does not match ${patch.input.file} (MD5 ${inMd5}, SHA-256 ${inSha256})`,
      { got_md5: inMd5, got_sha256: inSha256, expected_md5: patch.input.md5, expected_sha256: patch.input.sha256 }
    );
  }

  progress('decode', 0);
  const { image: stock, ctx } = extract(raw);
  const stockMd5 = md5hex(stock);
  const stockSha256 = await sha256hex(stock);
  if (stockMd5 !== patch.input.main_md5 || (patch.input.main_sha256 && stockSha256 !== patch.input.main_sha256)) {
    throw new PatchError(
      'decode_mismatch',
      `Decoded main ARM image mismatch (MD5 ${stockMd5}, SHA-256 ${stockSha256})`
    );
  }

  progress('patch', 0);
  const image = applyOps(stock, patch);
  const imgMd5 = md5hex(image);
  const imgSha256 = await sha256hex(image);
  if (imgMd5 !== patch.output.main_md5 || (patch.output.main_sha256 && imgSha256 !== patch.output.main_sha256)) {
    throw new PatchError(
      'patch_mismatch',
      `Patched main ARM image mismatch (MD5 ${imgMd5}, SHA-256 ${imgSha256})`
    );
  }

  // Note: newBootstrap is strictly null (stock 64 KiB bootstrap is untouched bit-for-bit).
  progress('compress', 0);
  const out = repack(raw, image, null, (d, t) => progress('compress', d / t), ctx);

  progress('seal', 1);
  const outMd5 = md5hex(out);
  const outSha256 = await sha256hex(out);
  if (outMd5 !== patch.output.md5 || (patch.output.sha256 && outSha256 !== patch.output.sha256)) {
    throw new PatchError(
      'output_mismatch',
      `Output digest mismatch (got MD5 ${outMd5} / SHA-256 ${outSha256}; expected ${patch.output.md5} / ${patch.output.sha256})`,
      { got_md5: outMd5, got_sha256: outSha256, expected_md5: patch.output.md5, expected_sha256: patch.output.sha256 }
    );
  }

  progress('done', 1);
  return { out, md5: outMd5, sha256: outSha256 };
}
