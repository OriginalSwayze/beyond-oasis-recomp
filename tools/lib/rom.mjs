// Loads and verifies the user's own ROMs. No ROM is ever copied into the repo:
// local.json (gitignored) only stores where each one lives on disk, by version
// ({ "roms": { "us": "<path>", "fr": "<path>" } }; an older { "romPath" } is the USA one).
import { createHash } from 'node:crypto';
import { readFileSync, writeFileSync, existsSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

export const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
export const ROM_SIZE = 0x300000;
export const VERSIONS = JSON.parse(readFileSync(join(ROOT, 'config', 'versions.json'), 'utf8')).versions;

// The version a ROM image is, or null.
export function identifyRom(buf) {
  if (buf.length !== ROM_SIZE) return null;
  const sha1 = createHash('sha1').update(buf).digest('hex');
  return VERSIONS.find((v) => v.sha1 === sha1) || null;
}

export function romError(buf) {
  if (buf.length !== ROM_SIZE) return `wrong size: ${buf.length} bytes, expected ${ROM_SIZE}`;
  if (identifyRom(buf)) return null;
  return `not one of: ${VERSIONS.map((v) => v.name).join(', ')} (unchanged, no header)`;
}

const LOCAL = join(ROOT, 'local.json');

// { id: path } of the ROMs set up so far.
export function romPaths() {
  if (!existsSync(LOCAL)) return {};
  const cfg = JSON.parse(readFileSync(LOCAL, 'utf8'));
  return cfg.roms || (cfg.romPath ? { us: cfg.romPath } : {});
}

export function saveRomPath(id, path) {
  writeFileSync(LOCAL, JSON.stringify({ roms: { ...romPaths(), [id]: path } }, null, 2) + '\n');
}

// [{ version, path, rom }] for every ROM set up, in config/versions.json order.
export function loadRoms() {
  const paths = romPaths(), out = [];
  for (const version of VERSIONS) {
    const path = paths[version.id];
    if (!path) continue;
    const rom = readFileSync(path);
    if (identifyRom(rom)?.id !== version.id) throw new Error(`${path}: ${romError(rom) || `this is ${identifyRom(rom).name}`}`);
    out.push({ version, path, rom });
  }
  if (!out.length) throw new Error('No ROM set up. Run: node tools/setup.mjs "<path to a Beyond Oasis / Story of Thor ROM>"');
  return out;
}

