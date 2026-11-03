// Usage: node tools/setup.mjs "<ROM>" ["<ROM>" ...]
// Checks each ROM is one of the versions in config/versions.json and remembers where it
// is in local.json, by version.
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { identifyRom, romError, saveRomPath, VERSIONS } from './lib/rom.mjs';

const args = process.argv.slice(2);
if (!args.length) {
  console.error(`Usage: node tools/setup.mjs "<ROM>" [...]  (any of: ${VERSIONS.map((v) => v.name).join(', ')})`);
  process.exit(1);
}
for (const arg of args) {
  const path = resolve(arg), rom = readFileSync(path), version = identifyRom(rom);
  if (!version) {
    console.error(`ROM rejected: ${path}: ${romError(rom)}`);
    process.exit(1);
  }
  saveRomPath(version.id, path);
  console.log(`OK: ${version.name} verified, path saved to local.json`);
}
