// Loads and verifies the user's own ROM. The ROM is never copied into the repo:
// local.json (gitignored) only stores where it lives on disk.
import { createHash } from 'node:crypto';
import { readFileSync, existsSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

export const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
export const ROM_INFO = JSON.parse(readFileSync(join(ROOT, 'config', 'rom.json'), 'utf8'));

export function verifyRom(buf) {
  if (buf.length !== ROM_INFO.size) {
    return `wrong size: ${buf.length} bytes, expected ${ROM_INFO.size}`;
  }
  const sha1 = createHash('sha1').update(buf).digest('hex');
  if (sha1 !== ROM_INFO.sha1) {
    return `wrong SHA-1: ${sha1}, expected ${ROM_INFO.sha1} (${ROM_INFO.name})`;
  }
  return null;
}

export function romPathFromConfig() {
  const cfg = join(ROOT, 'local.json');
  if (!existsSync(cfg)) {
    throw new Error('No ROM configured. Run: node tools/setup.mjs "<path to Beyond Oasis (USA) ROM>"');
  }
  return JSON.parse(readFileSync(cfg, 'utf8')).romPath;
}

export function loadRom() {
  const path = romPathFromConfig();
  const buf = readFileSync(path);
  const err = verifyRom(buf);
  if (err) throw new Error(`${path}: ${err}`);
  return buf;
}
