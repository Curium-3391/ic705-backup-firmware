// 100% local in-browser IC-705 EEPROM logical image reconstructor & checksum verifier.
// Implements the verified J140 64-byte stripe bijection (0x2005C41C):
//   chip        = (L >> 6) & 1       (0 = IC705, 1 = IC706)
//   phys_offset = ((L >> 7) << 6) | (L & 0x3F)
// Zero uploads: private EEPROM dumps stay strictly inside local browser memory.

import { sha256hex } from './patch.js';
import { toHex } from './md5.js';

export const CHIP_SIZE = 131072;
export const LOGICAL_SIZE = 262144;

export function reconstructLogicalFromPhysical(ic705, ic706) {
  if (ic705.length !== CHIP_SIZE || ic706.length !== CHIP_SIZE) {
    throw new Error(`Expected 131,072 bytes per chip (got IC705=${ic705.length}, IC706=${ic706.length})`);
  }
  const out = new Uint8Array(LOGICAL_SIZE);
  for (let stripe = 0; stripe < LOGICAL_SIZE / 64; stripe++) {
    const lOff = stripe * 64;
    const chip = stripe & 1;
    const pOff = (stripe >> 1) * 64;
    const src = chip === 0 ? ic705 : ic706;
    out.set(src.subarray(pOff, pOff + 64), lOff);
  }
  return out;
}

export function splitLogicalToPhysical(logical) {
  if (logical.length !== LOGICAL_SIZE) {
    throw new Error(`Expected 262,144 bytes logical EEPROM image (got ${logical.length})`);
  }
  const ic705 = new Uint8Array(CHIP_SIZE);
  const ic706 = new Uint8Array(CHIP_SIZE);
  for (let stripe = 0; stripe < LOGICAL_SIZE / 64; stripe++) {
    const lOff = stripe * 64;
    const chip = stripe & 1;
    const pOff = (stripe >> 1) * 64;
    const dst = chip === 0 ? ic705 : ic706;
    dst.set(logical.subarray(lOff, lOff + 64), pOff);
  }
  return [ic705, ic706];
}

export async function inspectLogicalAnchors(logical) {
  let s1 = 0;
  for (let i = 0x3fb47; i < 0x3fb50; i++) s1 = (s1 + logical[i]) & 0xff;
  const s1Stored = logical[0x3fb50];

  let s2 = 0;
  for (let i = 0x3fb51; i < 0x3fb5b; i++) s2 = (s2 + logical[i]) & 0xff;
  const s2Stored = logical[0x3fb5b];

  const touchCalHex = toHex(logical.subarray(0x3ff30, 0x3ff40));
  const markerBytes = logical.subarray(0x3ff40, 0x3ff50);
  const markerAscii = Array.from(markerBytes, (b) => (b >= 32 && b < 127 ? String.fromCharCode(b) : '.')).join('');

  return {
    logical_size_bytes: logical.length,
    logical_sha256: await sha256hex(logical),
    sum8_domain1: { calculated: s1, stored: s1Stored, valid: s1 === s1Stored },
    sum8_domain2: { calculated: s2, stored: s2Stored, valid: s2 === s2Stored },
    touch_cal_hex: touchCalHex,
    touch_marker_ascii: markerAscii,
  };
}
