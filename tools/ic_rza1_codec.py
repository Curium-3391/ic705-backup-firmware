#!/usr/bin/env python3
"""
ic_rza1_codec.py — Standalone Icom RZ/A1 firmware container codec (autokey + Okumura LZSS + repack).

Adapted from icom_rza1_decode.py and ic_rza1_repack.py in sq6emm/icom-custom-firmware
(Copyright (c) 2026 Dawid SQ6EMM <sq6emm@hamradio.pl>, MIT License).
Produces bit-identical output to docs/js/codec.js.
"""
from __future__ import annotations

import hashlib
import struct

ARM_VECTORS = bytes.fromhex("18f09fe518f09fe5")
BOOTSTRAP = 0x10000


def autokey_reverse(data: bytes, base: int, key: int, last: int = 0xFF) -> tuple[bytes, int]:
    out = bytearray(len(data))
    p = last
    for i in range(len(data) - 1, -1, -1):
        out[i] = p
        p = ((data[i] ^ p) - base - i + key) & 255
    return bytes(out), p


def autokey_decode(data: bytes, base: int, key: int, previous: int) -> tuple[bytes, int]:
    out = bytearray()
    for i, c in enumerate(data):
        previous = c ^ ((previous + base + i - key) & 255)
        out.append(previous)
    return bytes(out), previous


def autokey_encode(plain: bytes, base: int, key: int, previous: int) -> bytes:
    out = bytearray()
    for i, p in enumerate(plain):
        out.append(p ^ ((previous + base + i - key) & 255))
        previous = p
    return bytes(out)


def lzss_decompress(data: bytes, expected: int) -> tuple[bytes, int]:
    if not 0 < expected <= 64 * 1024 * 1024:
        raise ValueError("unreasonable output length")
    ring = bytearray(4096)
    cursor = 0xFEE
    pos = 0
    flags = 0
    out = bytearray()
    while len(out) < expected:
        if pos >= len(data):
            break
        flags >>= 1
        if not (flags & 0x100):
            flags = data[pos] | 0xFF00
            pos += 1
        if flags & 1:
            out.append(data[pos])
            ring[cursor] = data[pos]
            cursor = (cursor + 1) & 4095
            pos += 1
        else:
            lo, hi = data[pos], data[pos + 1]
            pos += 2
            index = lo | ((hi & 0xF0) << 4)
            length = (hi & 0x0F) + 3
            for j in range(length):
                if len(out) >= expected:
                    break
                v = ring[(index + j) & 4095]
                out.append(v)
                ring[cursor] = v
                cursor = (cursor + 1) & 4095
    return bytes(out), pos


def lzss_compress(data: bytes, max_chain: int = 256, lazy: bool = True) -> bytes:
    n = len(data)
    heads: dict[int, int] = {}
    chain_prev = [-1] * max(n, 1)

    def insert(p: int) -> None:
        if p + 3 <= n:
            k = (data[p] << 16) | (data[p + 1] << 8) | data[p + 2]
            chain_prev[p] = heads.get(k, -1)
            heads[k] = p

    def match(i: int) -> tuple[int, int]:
        if i + 3 > n:
            return 0, 0
        k = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2]
        p = heads.get(k, -1)
        chain = 0
        max_len = min(18, n - i)
        best = 0
        dist = 0
        while p >= 0 and (i - p) <= 4095 and chain < max_chain:
            if data[p + best] == data[i + best]:
                l = 0
                while l < max_len and data[p + l] == data[i + l]:
                    l += 1
                if l > best:
                    best = l
                    dist = i - p
                if best == max_len:
                    break
            p = chain_prev[p]
            chain += 1
        return best, dist

    tokens = []
    i = 0
    while i < n:
        best, dist = match(i)
        if best >= 3 and lazy and i + 1 < n and match(i + 1)[0] > best:
            tokens.append((1, data[i]))
            insert(i)
            i += 1
            continue
        if best >= 3:
            tokens.append((0, dist, best))
            for kk in range(best):
                insert(i + kk)
            i += best
        else:
            tokens.append((1, data[i]))
            insert(i)
            i += 1

    out = bytearray()
    cursor = 0xFEE
    j = 0
    while j < len(tokens):
        group = tokens[j : j + 8]
        flag = 0
        body = bytearray()
        for b, t in enumerate(group):
            if t[0] == 1:
                flag |= 1 << b
                body.append(t[1])
                cursor = (cursor + 1) & 4095
            else:
                _, d, l = t
                index = (cursor - d) & 4095
                body.append(index & 0xFF)
                body.append(((index >> 4) & 0xF0) | ((l - 3) & 0x0F))
                cursor = (cursor + l) & 4095
        out.append(flag)
        out += body
        j += 8
    return bytes(out)


def derive_key(raw: bytes, search: int = 0x8000) -> tuple[int, int]:
    for s in range(0x20, min(search, len(raw) - 16)):
        run = raw[s : s + 16]
        if all((run[i] - 1) & 255 == run[i + 1] for i in range(15)):
            return (run[0] + s) & 255, s
    raise ValueError("no 0xFF padding ramp found; cannot derive KEY")


def find_start(raw: bytes, key: int) -> tuple[int, int, bytes]:
    for s in range(0x10, 0x1000):
        for window in (0x1000, BOOTSTRAP):
            if s + window > len(raw):
                continue
            boot, iv = autokey_reverse(raw[s : s + window], s, key)
            if boot[:8] == ARM_VECTORS:
                full, _ = autokey_decode(raw[s : s + BOOTSTRAP], s, key, iv)
                return s, iv, full
    raise ValueError("ARM bootstrap not found")


def find_md5_region(raw: bytes, start: int) -> int | None:
    h = hashlib.md5()
    last = start
    for p in range(start, len(raw) - 16, 0x1000):
        if p > start:
            h.update(raw[last:p])
            last = p
            if h.copy().digest() == raw[p : p + 16]:
                return p
    return None


def open_container(raw: bytes) -> dict:
    key, _ = derive_key(raw)
    start, iv, _ = find_start(raw, key)
    digest_at = find_md5_region(raw, start)
    if digest_at is None:
        raise ValueError("no single prefix-MD5 region")
    body, _ = autokey_decode(raw[start:digest_at], start, key, iv)
    stream_off = None
    for off in range(BOOTSTRAP, BOOTSTRAP + 0x10):
        probe, _ = lzss_decompress(body[off : off + 64], 8)
        if probe[:8] == ARM_VECTORS:
            stream_off = off
            break
    if stream_off is None:
        raise ValueError("main LZSS stream not located")
    words = [struct.unpack_from("<I", body, BOOTSTRAP + o)[0] for o in range(0, stream_off - BOOTSTRAP, 4)]
    return {
        "key": key,
        "start": start,
        "iv": iv,
        "digest_at": digest_at,
        "body": body,
        "stream_off": stream_off,
        "words": words,
    }


def extract(raw: bytes) -> tuple[bytes, bytes, dict]:
    ctx = open_container(raw)
    image, _ = lzss_decompress(ctx["body"][ctx["stream_off"] :], max(ctx["words"]))
    return image, ctx["body"][:BOOTSTRAP], ctx


def repack(raw: bytes, new_image: bytes, new_bootstrap: bytes | None = None) -> bytes:
    ctx = open_container(raw)
    key = ctx["key"]
    start = ctx["start"]
    iv = ctx["iv"]
    digest_at = ctx["digest_at"]
    body = ctx["body"]
    stream_off = ctx["stream_off"]
    words = ctx["words"]

    out_len = len(new_image)
    _, consumed = lzss_decompress(body[stream_off:], max(words) if words else out_len)

    chain = b""
    two_word_hdr = (stream_off - BOOTSTRAP) == 8
    if two_word_hdr:
        old_csz = words[0]
        if old_csz < consumed or any(b != 0xFF for b in body[stream_off + consumed : stream_off + old_csz]):
            raise ValueError("main header csz does not match the stream")
        p = stream_off + old_csz
        while p + 8 <= len(body) and body[p : p + 8] != b"\xff" * 8:
            bcsz, bosz = struct.unpack_from("<II", body, p)
            out, used = lzss_decompress(body[p + 8 : p + 8 + bcsz], bosz)
            if len(out) != bosz or used > bcsz:
                raise ValueError(f"unrecognised data right after main stream at 0x{p:x}")
            p += 8 + bcsz
        chain = body[stream_off + old_csz : p]
        chain_end = p
    else:
        chain_end = stream_off + consumed

    next_comp = chain_end
    while next_comp < len(body) and body[next_comp] == 0xFF:
        next_comp += 1

    stream = lzss_compress(new_image)
    if chain:
        stream += b"\xff" * (-len(stream) % 4)
    header = struct.pack("<II", len(stream), out_len) if two_word_hdr else struct.pack("<I", out_len)
    main = header + stream + chain
    slot = next_comp - BOOTSTRAP
    if len(main) > slot:
        raise ValueError(f"re-compressed main ({len(main)}) exceeds slot ({slot})")
    main += b"\xff" * (slot - len(main))

    boot = body[:BOOTSTRAP]
    if new_bootstrap is not None:
        if len(new_bootstrap) != BOOTSTRAP:
            raise ValueError("bootstrap must be exactly 0x10000 bytes")
        if new_bootstrap[0x4F00:0x4F10] != boot[0x4F00:0x4F10]:
            raise ValueError("bootloader tag mismatch")
        if new_bootstrap[:8] != ARM_VECTORS:
            raise ValueError("bootstrap missing ARM vectors")
        boot = bytes(new_bootstrap)

    new_body = boot + main + body[next_comp:]
    assert len(new_body) == digest_at - start
    new_domain = autokey_encode(new_body, start, key, iv)
    digest = hashlib.md5(new_domain).digest()
    return raw[:start] + new_domain + digest + raw[digest_at + 16 :]
