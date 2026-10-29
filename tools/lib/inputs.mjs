// Extra trusted entry points for the tracer: hand-listed addresses in
// config/entries.json, and code the runtime saw executing (coverage.bin).
import { readFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';
import { ROOT } from './rom.mjs';

export function loadExtraEntries() {
  const entries = [], covered = [];
  const entriesFile = join(ROOT, 'config', 'entries.json');
  if (existsSync(entriesFile)) for (const s of JSON.parse(readFileSync(entriesFile, 'utf8'))) entries.push(parseInt(s, 16));
  const cov = join(ROOT, 'build', 'cmake', 'coverage.bin');
  if (existsSync(cov)) {
    // One bit per even address: byte k bit b covers address k*16 + b*2.
    const bits = readFileSync(cov);
    for (let k = 0; k < bits.length; k++) {
      if (!bits[k]) continue;
      for (let b = 0; b < 8; b++) if (bits[k] & (1 << b)) covered.push(k * 16 + b * 2);
    }
  }
  return { entries, covered };
}
