// Usage: node tools/save-coverage.mjs
// Adds the code the runtime saw running (build/cmake/coverage.bin) to config/coverage.txt,
// so a fresh build finds it too. The file lists address ranges only, no bytes of the game:
// one address, or a "start-end" range (hex, end inclusive) of even addresses, per line.
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { ROOT } from './lib/rom.mjs';
import { loadCoverageList } from './lib/inputs.mjs';

const bin = join(ROOT, 'build', 'cmake', 'coverage.bin');
if (!existsSync(bin)) { console.error(`${bin} not found: play the game first`); process.exit(1); }
const set = new Set(loadCoverageList());
const before = set.size;
const bits = readFileSync(bin);
for (let k = 0; k < bits.length; k++) {
  if (!bits[k]) continue;
  for (let b = 0; b < 8; b++) if (bits[k] & (1 << b)) set.add(k * 16 + b * 2);
}
const all = [...set].sort((a, b) => a - b), lines = [];
for (let k = 0; k < all.length; ) {
  let j = k;
  while (j + 1 < all.length && all[j + 1] === all[j] + 2) j++;
  const hex = (a) => a.toString(16).toUpperCase().padStart(6, '0');
  lines.push(j > k ? `${hex(all[k])}-${hex(all[j])}` : hex(all[k]));
  k = j + 1;
}
writeFileSync(join(ROOT, 'config', 'coverage.txt'), lines.join('\n') + '\n');
console.log(`config/coverage.txt: ${all.length} addresses (${all.length - before} new), ${lines.length} ranges`);
