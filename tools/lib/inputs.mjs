// Extra trusted entry points for the tracer: hand-listed addresses in
// config/entries.json, code seen running in earlier play (config/coverage.txt, committed,
// so a fresh build has it), and code the runtime saw running here (coverage.bin). Each
// version has its own: coverage.txt and coverage.bin are the USA one's, the others'
// are coverage-<id>.txt and coverage-<id>.bin (see coverageName()).
import { readFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';
import { ROOT } from './rom.mjs';

export const coverageName = (id, ext) => (id === 'us' ? `coverage.${ext}` : `coverage-${id}.${ext}`);

// config/coverage.txt: one hex address, or a "start-end" range of even addresses, per line.
export function loadCoverageList(id = 'us') {
  const file = join(ROOT, 'config', coverageName(id, 'txt')), out = [];
  if (!existsSync(file)) return out;
  for (const line of readFileSync(file, 'utf8').split('\n')) {
    const m = /^([0-9A-Fa-f]+)(?:-([0-9A-Fa-f]+))?/.exec(line.trim());
    if (m) for (let a = parseInt(m[1], 16); a <= parseInt(m[2] || m[1], 16); a += 2) out.push(a);
  }
  return out;
}

export function loadExtraEntries(id = 'us') {
  const entries = [], covered = loadCoverageList(id);
  const entriesFile = join(ROOT, 'config', 'entries.json');
  if (id === 'us' && existsSync(entriesFile)) for (const s of JSON.parse(readFileSync(entriesFile, 'utf8'))) entries.push(parseInt(s, 16));
  const cov = join(ROOT, 'build', 'cmake', coverageName(id, 'bin'));
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
