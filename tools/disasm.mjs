// Traces the ROM and writes:
//   build/disasm/beyond_oasis.asm  - readable listing of discovered code
//   build/analysis.json            - blocks, functions and unresolved jumps (input to the recompiler)
import { mkdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { ROOT, loadRom } from './lib/rom.mjs';
import { format } from './lib/m68k.mjs';
import { trace } from './lib/trace.mjs';
import { loadExtraEntries } from './lib/inputs.mjs';

const rom = loadRom();
const extra = loadExtraEntries();

const t = trace(rom, extra);

const hex6 = (a) => (a >>> 0).toString(16).toUpperCase().padStart(6, '0');
const label = (a) => (t.funcs.has(a) ? `sub_${hex6(a)}` : t.labels.has(a) ? `loc_${hex6(a)}` : `$${hex6(a)}`);

const out = [];
for (const b of t.blocks) {
  if (t.funcs.has(b.start)) out.push('', `; ${'-'.repeat(60)}`, `sub_${hex6(b.start)}:`);
  else out.push(`loc_${hex6(b.start)}:`);
  for (const i of b.insts) {
    const bytes = [];
    for (let k = 0; k < i.len; k += 2) bytes.push(rom.readUInt16BE(i.pc + k).toString(16).padStart(4, '0'));
    out.push(`\t${format(i, label).padEnd(44)}; ${hex6(i.pc)}  ${bytes.join(' ')}`);
  }
}

const dir = join(ROOT, 'build', 'disasm');
mkdirSync(dir, { recursive: true });
writeFileSync(join(dir, 'beyond_oasis.asm'), out.join('\n') + '\n');

const codeBytes = t.blocks.reduce((n, b) => n + (b.end - b.start), 0);
writeFileSync(join(ROOT, 'build', 'analysis.json'), JSON.stringify({
  funcs: [...t.funcs].sort((a, b) => a - b),
  blocks: t.blocks.map((b) => ({ start: b.start, end: b.end })),
  unresolved: t.unresolved,
  tables: t.tables,
  problems: t.problems,
}, null, 1));

console.log(`functions:  ${t.funcs.size}`);
console.log(`blocks:     ${t.blocks.length}`);
console.log(`code bytes: ${codeBytes} (${(codeBytes / rom.length * 100).toFixed(1)}% of ROM)`);
console.log(`unresolved indirect jumps: ${t.unresolved.length}`);
console.log(`jump tables: ${t.tables.length}`);
console.log(`pointer candidates: ${t.pointerStats.accepted} accepted, ${t.pointerStats.rejected} rejected as data`);
console.log(`problems on trusted paths: ${t.problems.length}`);
