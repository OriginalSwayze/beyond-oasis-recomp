// Where the USA code addresses the runtime hooks (config/addresses.json, config/patches.json)
// are in the other cartridge versions (config/versions.json). tools/map-versions.mjs saves
// the result in config/version-addresses.json, so a build needs only the ROMs it builds.
//
// The translations moved the code but kept it: each address is found by the bytes around
// it in the USA ROM. Code that the game repeats (the same check in several routines) is
// placed from a nearby address that was found, at the same distance it has in the USA ROM.
// Every result is checked: the instruction there must be the USA one (its bytes; for a
// branch or call, whose distance can change, its opcode).
import { readFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';
import { ROOT } from './rom.mjs';
import { decode } from './m68k.mjs';

export const ADDRESSES = JSON.parse(readFileSync(join(ROOT, 'config', 'addresses.json'), 'utf8'));
export const PATCHES = JSON.parse(readFileSync(join(ROOT, 'config', 'patches.json'), 'utf8'));

const isHex = (s) => typeof s === 'string' && /^[0-9A-F]{6}$/.test(s);

// Every hex address in a config object (keys and values; "comment" skipped).
function addressesIn(obj, out = new Set()) {
  for (const [k, v] of Object.entries(obj)) {
    if (k === 'comment') continue;
    if (isHex(k)) out.add(parseInt(k, 16));
    if (isHex(v)) out.add(parseInt(v, 16));
    else if (Array.isArray(v)) v.forEach((x) => (isHex(x) ? out.add(parseInt(x, 16)) : typeof x === 'object' && addressesIn(x, out)));
    else if (v && typeof v === 'object') addressesIn(v, out);
  }
  return out;
}

export const USA_ADDRESSES = [...addressesIn(ADDRESSES, addressesIn(PATCHES))].sort((a, b) => a - b);


function uniqueHit(target, pat, at) {
  const first = target.indexOf(pat);
  if (first < 0 || target.indexOf(pat, first + 1) >= 0) return -1;
  return first + at;
}

// Map of USA address -> address in `target`, or throws naming the ones not found.
export function mapAddresses(usa, target, list = USA_ADDRESSES) {
  if (usa.equals(target)) return new Map(list.map((a) => [a, a]));
  const found = new Map();
  for (const a of list) {
    // Windows from the address forward (the code before it may be another routine that
    // changed), then centred on it, growing until the bytes occur once.
    for (const len of [12, 16, 24, 32, 48, 64]) {
      let hit = uniqueHit(target, usa.subarray(a, a + len), 0);
      if (hit < 0) hit = uniqueHit(target, usa.subarray(a - len / 2, a + len / 2), len / 2);
      if (hit >= 0) { found.set(a, hit); break; }
    }
  }
  const ok = (a, b) => {
    if (b < 0 || b + 10 > target.length) return false;
    const i = decode(usa, a), len = i.len;
    if (usa.subarray(a, a + len).equals(target.subarray(b, b + len))) return true;
    return i.target !== undefined && usa.readUInt16BE(a) === target.readUInt16BE(b);
  };
  for (const a of list) {
    if (found.has(a)) continue;
    // Repeated code: from the nearest address found, at its USA distance.
    const near = [...found.keys()].filter((b) => Math.abs(b - a) < 0x1000).sort((x, y) => Math.abs(x - a) - Math.abs(y - a));
    for (const b of near) {
      const c = found.get(b) + (a - b);
      if (ok(a, c)) { found.set(a, c); break; }
    }
  }
  const missing = list.filter((a) => !found.has(a) || !ok(a, found.get(a)));
  if (missing.length) throw new Error(`not found: ${missing.map((a) => a.toString(16).toUpperCase().padStart(6, '0')).join(', ')}`);
  return found;
}

// A config object with every USA address in it replaced by the target's.
export function remapConfig(obj, map) {
  const conv = (s) => (isHex(s) && map.has(parseInt(s, 16)) ? map.get(parseInt(s, 16)).toString(16).toUpperCase().padStart(6, '0') : s);
  const walk = (v) => {
    if (Array.isArray(v)) return v.map(walk);
    if (v && typeof v === 'object') return Object.fromEntries(Object.entries(v).map(([k, x]) => [k === 'comment' ? k : conv(k), k === 'comment' ? x : walk(x)]));
    return conv(v);
  };
  return walk(obj);
}

const MAPPED = join(ROOT, 'config', 'version-addresses.json');

// { addresses, patches } for a version: the USA ones, or the saved mapping.
export function configFor(id) {
  if (id === 'us') return { addresses: ADDRESSES, patches: PATCHES };
  const all = existsSync(MAPPED) ? JSON.parse(readFileSync(MAPPED, 'utf8')) : {};
  if (!all[id]) throw new Error(`no addresses for version "${id}" in config/version-addresses.json: run node tools/map-versions.mjs`);
  return all[id];
}
