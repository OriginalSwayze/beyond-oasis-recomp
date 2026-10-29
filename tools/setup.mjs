// Usage: node tools/setup.mjs "<path to Beyond Oasis (USA) ROM>"
// Verifies the ROM and remembers its location in local.json.
import { readFileSync, writeFileSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { ROOT, verifyRom, ROM_INFO } from './lib/rom.mjs';

const arg = process.argv[2];
if (!arg) {
  console.error('Usage: node tools/setup.mjs "<path to Beyond Oasis (USA) ROM>"');
  process.exit(1);
}
const romPath = resolve(arg);
const err = verifyRom(readFileSync(romPath));
if (err) {
  console.error(`ROM rejected: ${err}`);
  process.exit(1);
}
writeFileSync(join(ROOT, 'local.json'), JSON.stringify({ romPath }, null, 2) + '\n');
console.log(`OK: ${ROM_INFO.name} verified, path saved to local.json`);
