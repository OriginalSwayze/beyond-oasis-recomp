// Recursive-descent code discovery. Starting from the vector table (plus any
// extra entry points in config/entries.json), follows every branch and call
// and splits the discovered code into basic blocks.
import { decode, isTerminator } from './m68k.mjs';

const ROM_END = 0x300000;

// Resolves the target of a jmp/jsr when the address is static.
function staticTarget(i) {
  const d = i.dst;
  if (d.mode === 'absl' || d.mode === 'absw' || d.mode === 'pcdisp') return d.addr >>> 0;
  return null;
}

const s16 = (v) => (v << 16) >> 16;
const sameReg = (o, mode, reg) => o && o.mode === mode && o.reg === reg;

// Largest byte offset the index register can hold, from an earlier `andi #mask`
// (doubled when followed by `add dN,dN`). Returns null when unknown.
function indexLimit(hist, reg) {
  let scale = 1;
  for (let k = hist.length - 1; k >= 0; k--) {
    const h = hist[k];
    if (h.op === 'add' && sameReg(h.src, 'dn', reg) && sameReg(h.dst, 'dn', reg)) scale *= 2;
    else if (h.op === 'andi' && sameReg(h.dst, 'dn', reg)) return (h.src.value & 0xffff) * scale;
    else if (h.op === 'subi' && sameReg(h.dst, 'dn', reg) && h.size === 1 && h.src.value === 0xf0) return 15 * scale;
  }
  return null;
}

// Recognizes the jump-table idioms this game uses and returns the targets.
//   A: move.w T(pc,dN.w),dN ; jmp/jsr T(pc,dN.w)        word offsets from T
//   B: lea T(pc,dN.w),aX ; adda.w (aX),aX ; jmp/jsr (aX) self-relative word offsets
//   C: lea T(pc),aX ; movea.l 0(aX,dN.w),aX ; jmp/jsr (aX)  absolute long pointers
function jumpTable(rom, i, hist, isCode) {
  const [p2, p1] = [hist[hist.length - 2], hist[hist.length - 1]];
  let base, kind, idx;
  if (i.dst.mode === 'pcidx' && p1 && p1.op === 'move' && p1.size === 2 &&
      p1.src.mode === 'pcidx' && p1.src.base === i.dst.base && sameReg(p1.dst, 'dn', i.dst.idxReg)) {
    base = i.dst.base; kind = 'A'; idx = p1.src.idxReg;
  } else if (i.dst.mode === 'ind' && (() => {
    // B may have unrelated instructions (e.g. movem saves) between the adda and the jump.
    for (let k = hist.length - 1; k >= 1; k--) {
      const a = hist[k], l = hist[k - 1];
      if (a.op === 'adda' && a.size === 2 && sameReg(a.src, 'ind', i.dst.reg) && sameReg(a.dst, 'an', i.dst.reg) &&
          l.op === 'lea' && l.src.mode === 'pcidx' && sameReg(l.dst, 'an', i.dst.reg)) {
        base = l.src.base; idx = l.src.idxReg;
        return true;
      }
      if (a.dst && sameReg(a.dst, 'an', i.dst.reg)) return false; // register overwritten
    }
    return false;
  })()) {
    kind = 'B';
  } else if (i.dst.mode === 'ind' && p1 && p2 && p1.op === 'movea' && p1.size === 4 &&
      p1.src.mode === 'idx' && p1.src.reg === i.dst.reg && sameReg(p1.dst, 'an', i.dst.reg) &&
      p2.op === 'lea' && (p2.src.mode === 'pcdisp' || p2.src.mode === 'absl') && sameReg(p2.dst, 'an', i.dst.reg)) {
    base = (p2.src.addr + p1.src.disp) >>> 0; kind = 'C'; idx = p1.src.idxReg;
  } else {
    return null;
  }

  const limit = indexLimit(hist, idx);
  const step = kind === 'C' ? 4 : 2;
  const maxEntries = limit !== null ? Math.floor(limit / step) + 1 : 256;
  const targets = [];
  let lowest = Infinity;
  for (let k = 0; k < maxEntries; k++) {
    const at = base + k * step;
    if (at >= lowest || isCode(at)) break;
    let t;
    if (kind === 'A') t = base + s16(rom.readUInt16BE(at));
    else if (kind === 'B') t = at + s16(rom.readUInt16BE(at));
    else t = rom.readUInt32BE(at);
    t >>>= 0;
    if (t & 1 || t >= ROM_END || !decode(rom, t)) {
      if (limit !== null) continue; // bounded table: tolerate a junk slot
      break;
    }
    targets.push(t);
    if (t > base) lowest = Math.min(lowest, t);
  }
  return targets;
}

// Code addresses held in immediates (`move.l #addr,...`, `pea addr`, `lea addr,aN`),
// typically object update routines stored into RAM and called through a pointer.
function pointerCandidates(insts) {
  const out = new Set();
  for (const i of insts.values()) {
    let a = null;
    if (i.op === 'move' && i.size === 4 && i.src.mode === 'imm') a = i.src.value;
    else if ((i.op === 'pea' || i.op === 'lea') && (i.src.mode === 'absl' || i.src.mode === 'pcdisp')) a = i.src.addr;
    if (a !== null && a >= 0x200 && a < ROM_END && !(a & 1)) out.add(a >>> 0);
  }
  return out;
}

export function trace(rom, { entries = [], covered = [] } = {}) {
  const insts = new Map();       // pc -> instruction
  const owner = new Map();       // every byte address covered -> instruction pc (overlap detection)
  const funcs = new Set();       // call targets and vectors
  const labels = new Set();      // anything that must start a block
  const unresolved = [];         // indirect jumps we could not follow
  const tables = [];             // resolved jump tables
  const problems = [];           // conflicts / invalid paths from trusted entry points
  const maybeCode = [];          // jump table slots: may run past the table's real end

  // Follows code from the seeds. When tentative, any invalid opcode or overlap
  // means the seed was really data: everything it added is rolled back.
  function explore(seeds, tentative) {
    const journal = { insts: [], labels: [], funcs: [], unresolved: unresolved.length, tables: tables.length };
    const work = [];
    let failed = null;
    const add = (addr, isFunc, from) => {
      addr >>>= 0;
      if (addr & 1 || addr >= ROM_END) { failed = { addr, from, why: 'odd or out-of-ROM target' }; return; }
      if (!labels.has(addr)) { labels.add(addr); journal.labels.push(addr); }
      if (isFunc && !funcs.has(addr)) { funcs.add(addr); journal.funcs.push(addr); }
      work.push(addr);
    };
    const label = (addr) => { if (!labels.has(addr)) { labels.add(addr); journal.labels.push(addr); } };
    for (const [a, f, from] of seeds) add(a, f, from);

    while (work.length && !(tentative && failed)) {
      let pc = work.pop();
      const hist = [];
      while (true) {
        if (insts.has(pc)) break;
        if (owner.has(pc)) { failed = { addr: pc, why: 'overlaps instruction at $' + owner.get(pc).toString(16) }; break; }
        const i = decode(rom, pc);
        if (!i) { failed = { addr: pc, why: 'invalid opcode' }; break; }
        insts.set(pc, i);
        journal.insts.push(i);
        for (let b = 0; b < i.len; b++) owner.set(pc + b, pc);
        const next = pc + i.len;

        switch (i.op) {
          case 'bra': case 'bcc': case 'dbcc':
            add(i.target, false, pc);
            break;
          case 'bsr':
            add(i.target, true, pc);
            label(next);
            break;
          case 'jsr': case 'jmp': {
            const t = staticTarget(i);
            if (t !== null) add(t, i.op === 'jsr', pc);
            else {
              const targets = jumpTable(rom, i, hist, (a) => owner.has(a));
              if (targets && targets.length) {
                tables.push({ pc, targets });
                label(pc + i.len);
                for (const x of targets) maybeCode.push(x);
              } else unresolved.push({ pc, op: i.op });
            }
            if (i.op === 'jsr') label(next);
            break;
          }
          case 'trap':
            add(rom.readUInt32BE(0x80 + i.src.value * 4), true, pc);
            label(next);
            break;
          case 'stop':
            label(next); // resumes here after the interrupt returns
            break;
        }
        if (isTerminator(i)) break;
        hist.push(i);
        if (hist.length > 8) hist.shift();
        pc = next;
      }
      if (failed && !tentative) { problems.push(failed); failed = null; }
    }

    if (tentative && failed) {
      for (const i of journal.insts) {
        insts.delete(i.pc);
        for (let b = 0; b < i.len; b++) owner.delete(i.pc + b);
      }
      for (const a of journal.labels) labels.delete(a);
      for (const a of journal.funcs) funcs.delete(a);
      unresolved.length = journal.unresolved;
      tables.length = journal.tables;
      return false;
    }
    return true;
  }

  // Trusted entry points: reset PC at $4, exception/interrupt vectors, entries.json.
  const seeds = [[rom.readUInt32BE(4), true, 'reset']];
  for (let v = 8; v < 0x100; v += 4) {
    const a = rom.readUInt32BE(v);
    if (a < ROM_END) seeds.push([a, true, 'vector $' + v.toString(16)]);
  }
  for (const e of entries) seeds.push([e, true, 'entries.json']);
  explore(seeds, false);

  // Code the runtime saw running. Only addresses not already decoded become
  // new entry points, so a run of executed instructions adds one label.
  for (const a of covered) {
    if (!owner.has(a)) explore([[a, false, 'coverage']], false);
  }

  // Jump table slots and pointer candidates, repeated until no new code turns up.
  const tried = new Set();
  let accepted = 0, rejected = 0;
  for (let changed = true; changed;) {
    changed = false;
    const candidates = [...maybeCode.splice(0), ...pointerCandidates(insts)];
    for (const a of candidates) {
      if (tried.has(a)) continue;
      tried.add(a);
      if (insts.has(a)) { if (!funcs.has(a)) { funcs.add(a); labels.add(a); } continue; }
      if (explore([[a, true, 'pointer']], true)) { accepted++; changed = true; } else rejected++;
    }
  }

  return { insts, funcs, labels, unresolved, tables, problems, pointerStats: { accepted, rejected },
           blocks: buildBlocks(insts, labels) };
}

// A block is a straight run of instructions entered only at its first address.
function buildBlocks(insts, labels) {
  const pcs = [...insts.keys()].sort((a, b) => a - b);
  const blocks = [];
  let cur = null;
  for (const pc of pcs) {
    const i = insts.get(pc);
    const contiguous = cur && cur.end === pc;
    if (!contiguous || labels.has(pc)) {
      cur = { start: pc, end: pc, insts: [] };
      blocks.push(cur);
    }
    cur.insts.push(i);
    cur.end = pc + i.len;
    const exits = isTerminator(i) || ['bcc', 'dbcc', 'bsr', 'jsr', 'trap', 'stop'].includes(i.op);
    if (exits) cur = null;
  }
  return blocks;
}
