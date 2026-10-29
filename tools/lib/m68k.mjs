// MC68000 instruction decoder. Produces structured instructions shared by the
// disassembler (text output, code tracing) and the recompiler (C output).
// decode() returns null for anything that is not a valid 68000 instruction,
// which the tracer uses to stop following paths that run into data.

export const COND = ['t', 'f', 'hi', 'ls', 'cc', 'cs', 'ne', 'eq', 'vc', 'vs', 'pl', 'mi', 'ge', 'lt', 'gt', 'le'];
export const SZ = { 1: 'b', 2: 'w', 4: 'l' };

const s8 = (v) => (v << 24) >> 24;
const s16 = (v) => (v << 16) >> 16;

// Effective address categories (68000 Programmer's Reference, table 2-4).
export const isData = (o) => o.mode !== 'an';
export const isMem = (o) => o.mode !== 'dn' && o.mode !== 'an';
export const isAlt = (o) => o.mode !== 'pcdisp' && o.mode !== 'pcidx' && o.mode !== 'imm';
export const isControl = (o) => ['ind', 'disp', 'idx', 'absw', 'absl', 'pcdisp', 'pcidx'].includes(o.mode);
const dataAlt = (o) => isData(o) && isAlt(o);
const memAlt = (o) => isMem(o) && isAlt(o);
const ctrlAlt = (o) => isControl(o) && isAlt(o);

class Cursor {
  constructor(rom, pc) {
    this.rom = rom;
    this.start = pc;
    this.pc = pc;
  }
  word() {
    if (this.pc + 1 >= this.rom.length) throw new RangeError('decode past end of ROM');
    const v = this.rom.readUInt16BE(this.pc);
    this.pc += 2;
    return v;
  }
  long() {
    const hi = this.word();
    return ((hi << 16) | this.word()) >>> 0;
  }
}

const dn = (reg) => ({ mode: 'dn', reg });
const an = (reg) => ({ mode: 'an', reg });

function ea(c, mode, reg, size) {
  switch (mode) {
    case 0: return dn(reg);
    case 1: return an(reg);
    case 2: return { mode: 'ind', reg };
    case 3: return { mode: 'postinc', reg };
    case 4: return { mode: 'predec', reg };
    case 5: return { mode: 'disp', reg, disp: s16(c.word()) };
    case 6: return { mode: 'idx', reg, ...brief(c.word()) };
    case 7:
      switch (reg) {
        case 0: return { mode: 'absw', addr: s16(c.word()) >>> 0 };
        case 1: return { mode: 'absl', addr: c.long() };
        case 2: {
          const base = c.pc;
          const disp = s16(c.word());
          return { mode: 'pcdisp', disp, addr: (base + disp) >>> 0 };
        }
        case 3: {
          const base = c.pc;
          const b = brief(c.word());
          return { mode: 'pcidx', ...b, base: (base + b.disp) >>> 0 };
        }
        case 4: {
          if (!size) return null;
          const v = size === 4 ? c.long() : size === 2 ? c.word() : c.word() & 0xff;
          return { mode: 'imm', value: v };
        }
      }
  }
  return null;
}

// Brief extension word for d8(An,Xn) / d8(PC,Xn).
function brief(w) {
  return { idxA: !!(w & 0x8000), idxReg: (w >> 12) & 7, idxLong: !!(w & 0x800), disp: s8(w & 0xff) };
}

const SIZE_BITS = [1, 2, 4, 0];

export function decode(rom, pc) {
  const c = new Cursor(rom, pc);
  let inst;
  try {
    inst = decodeInner(c, c.word());
  } catch (e) {
    if (e instanceof RangeError) return null;
    throw e;
  }
  if (!inst) return null;
  inst.pc = pc;
  inst.len = c.pc - pc;
  return inst;
}

function decodeInner(c, op) {
  const hi = op >> 12;
  const r9 = (op >> 9) & 7;
  const m3 = (op >> 3) & 7;
  const r0 = op & 7;
  const sizeBits = (op >> 6) & 3;
  const I = (o) => o; // readability: each branch returns an instruction object

  switch (hi) {
    case 0x0: {
      if ((op & 0x0138) === 0x0108) {
        const opmode = (op >> 6) & 7;
        const disp = s16(c.word());
        const mem = { mode: 'disp', reg: r0, disp };
        const size = opmode & 1 ? 4 : 2;
        return opmode < 6
          ? I({ op: 'movep', size, src: mem, dst: dn(r9) })
          : I({ op: 'movep', size, src: dn(r9), dst: mem });
      }
      if (op & 0x100) {
        const kind = ['btst', 'bchg', 'bclr', 'bset'][sizeBits];
        const dst = ea(c, m3, r0, 1);
        if (!dst || dst.mode === 'an') return null;
        if (kind !== 'btst' && !dataAlt(dst)) return null;
        return I({ op: kind, size: dst.mode === 'dn' ? 4 : 1, src: dn(r9), dst });
      }
      if (r9 === 4) {
        const kind = ['btst', 'bchg', 'bclr', 'bset'][sizeBits];
        const bit = c.word();
        if (bit & 0xff00) return null;
        const dst = ea(c, m3, r0, 1);
        if (!dst || dst.mode === 'an' || dst.mode === 'imm') return null;
        if (kind !== 'btst' && !dataAlt(dst)) return null;
        return I({ op: kind, size: dst.mode === 'dn' ? 4 : 1, src: { mode: 'imm', value: bit }, dst });
      }
      const kind = ['ori', 'andi', 'subi', 'addi', null, 'eori', 'cmpi', null][r9];
      if (!kind) return null;
      if ((op & 0xff) === 0x3c || (op & 0xff) === 0x7c) {
        if (!['ori', 'andi', 'eori'].includes(kind)) return null;
        const toSR = (op & 0xff) === 0x7c;
        const v = c.word();
        if (!toSR && v & 0xff00) return null;
        return I({ op: kind + (toSR ? '_sr' : '_ccr'), size: toSR ? 2 : 1, src: { mode: 'imm', value: toSR ? v : v & 0xff } });
      }
      const size = SIZE_BITS[sizeBits];
      if (!size) return null;
      const src = ea(c, 7, 4, size);
      const dst = ea(c, m3, r0, size);
      if (!dst || !dataAlt(dst)) return null;
      return I({ op: kind, size, src, dst });
    }

    case 0x1: case 0x2: case 0x3: {
      const size = hi === 1 ? 1 : hi === 3 ? 2 : 4;
      const src = ea(c, m3, r0, size);
      if (!src || (size === 1 && src.mode === 'an')) return null;
      const dmode = (op >> 6) & 7;
      const dst = ea(c, dmode, r9, size);
      if (!dst) return null;
      if (dst.mode === 'an') return size === 1 ? null : I({ op: 'movea', size, src, dst });
      if (!dataAlt(dst)) return null;
      return I({ op: 'move', size, src, dst });
    }

    case 0x4: return decodeMisc(c, op, r9, m3, r0, sizeBits);

    case 0x5: {
      const cond = (op >> 8) & 0xf;
      if (sizeBits === 3) {
        if (m3 === 1) {
          const base = c.pc;
          const disp = s16(c.word());
          return I({ op: 'dbcc', cond, size: 2, dst: dn(r0), target: (base + disp) >>> 0 });
        }
        const dst = ea(c, m3, r0, 1);
        if (!dst || !dataAlt(dst)) return null;
        return I({ op: 'scc', cond, size: 1, dst });
      }
      const size = SIZE_BITS[sizeBits];
      const dst = ea(c, m3, r0, size);
      if (!dst || !isAlt(dst) || (dst.mode === 'an' && size === 1)) return null;
      return I({ op: op & 0x100 ? 'subq' : 'addq', size, src: { mode: 'imm', value: r9 || 8 }, dst });
    }

    case 0x6: {
      const cond = (op >> 8) & 0xf;
      let disp = op & 0xff;
      const base = c.pc;
      let short = true;
      if (disp === 0) { disp = s16(c.word()); short = false; }
      else if (disp === 0xff) return null; // 68020+ 32-bit displacement
      else disp = s8(disp);
      const target = (base + disp) >>> 0;
      if (cond === 0) return I({ op: 'bra', short, target });
      if (cond === 1) return I({ op: 'bsr', short, target });
      return I({ op: 'bcc', cond, short, target });
    }

    case 0x7:
      if (op & 0x100) return null;
      return I({ op: 'moveq', size: 4, src: { mode: 'imm', value: s8(op & 0xff) >>> 0 }, dst: dn(r9) });

    case 0x8: case 0xC: {
      const isAnd = hi === 0xC;
      const opmode = (op >> 6) & 7;
      if (opmode === 3 || opmode === 7) {
        const src = ea(c, m3, r0, 2);
        if (!src || !isData(src)) return null;
        const signed = opmode === 7;
        return I({ op: isAnd ? (signed ? 'muls' : 'mulu') : (signed ? 'divs' : 'divu'), size: 2, src, dst: dn(r9) });
      }
      if ((op & 0x1f0) === 0x100) {
        const k = isAnd ? 'abcd' : 'sbcd';
        return op & 8
          ? I({ op: k, size: 1, src: { mode: 'predec', reg: r0 }, dst: { mode: 'predec', reg: r9 } })
          : I({ op: k, size: 1, src: dn(r0), dst: dn(r9) });
      }
      if (isAnd && (op & 0x130) === 0x100) {
        const k = (op >> 3) & 0x1f;
        if (k === 0x08) return I({ op: 'exg', size: 4, src: dn(r9), dst: dn(r0) });
        if (k === 0x09) return I({ op: 'exg', size: 4, src: an(r9), dst: an(r0) });
        if (k === 0x11) return I({ op: 'exg', size: 4, src: dn(r9), dst: an(r0) });
        return null;
      }
      const size = SIZE_BITS[opmode & 3];
      const other = ea(c, m3, r0, size);
      if (!other) return null;
      const kind = isAnd ? 'and' : 'or';
      if (opmode < 4) {
        if (!isData(other)) return null;
        return I({ op: kind, size, src: other, dst: dn(r9) });
      }
      if (!memAlt(other)) return null;
      return I({ op: kind, size, src: dn(r9), dst: other });
    }

    case 0x9: case 0xD: {
      const kind = hi === 0xD ? 'add' : 'sub';
      const opmode = (op >> 6) & 7;
      if (opmode === 3 || opmode === 7) {
        const size = opmode === 7 ? 4 : 2;
        const src = ea(c, m3, r0, size);
        if (!src) return null;
        return I({ op: kind + 'a', size, src, dst: an(r9) });
      }
      const size = SIZE_BITS[opmode & 3];
      if (opmode >= 4 && (op & 0x30) === 0) {
        return op & 8
          ? I({ op: kind + 'x', size, src: { mode: 'predec', reg: r0 }, dst: { mode: 'predec', reg: r9 } })
          : I({ op: kind + 'x', size, src: dn(r0), dst: dn(r9) });
      }
      const other = ea(c, m3, r0, size);
      if (!other) return null;
      if (opmode < 4) {
        if (other.mode === 'an' && size === 1) return null;
        return I({ op: kind, size, src: other, dst: dn(r9) });
      }
      if (!memAlt(other)) return null;
      return I({ op: kind, size, src: dn(r9), dst: other });
    }

    case 0xB: {
      const opmode = (op >> 6) & 7;
      if (opmode === 3 || opmode === 7) {
        const size = opmode === 7 ? 4 : 2;
        const src = ea(c, m3, r0, size);
        if (!src) return null;
        return I({ op: 'cmpa', size, src, dst: an(r9) });
      }
      const size = SIZE_BITS[opmode & 3];
      if (opmode < 4) {
        const src = ea(c, m3, r0, size);
        if (!src || (src.mode === 'an' && size === 1)) return null;
        return I({ op: 'cmp', size, src, dst: dn(r9) });
      }
      if (m3 === 1) return I({ op: 'cmpm', size, src: { mode: 'postinc', reg: r0 }, dst: { mode: 'postinc', reg: r9 } });
      const dst = ea(c, m3, r0, size);
      if (!dst || !dataAlt(dst)) return null;
      return I({ op: 'eor', size, src: dn(r9), dst });
    }

    case 0xE: {
      const left = !!(op & 0x100);
      if (sizeBits === 3) {
        if (op & 0x800) return null;
        const kind = ['as', 'ls', 'rox', 'ro'][r9 & 3] + (left ? 'l' : 'r');
        const dst = ea(c, m3, r0, 2);
        if (!dst || !memAlt(dst)) return null;
        return I({ op: kind, size: 2, src: { mode: 'imm', value: 1 }, dst });
      }
      const kind = ['as', 'ls', 'rox', 'ro'][m3 & 3] + (left ? 'l' : 'r');
      const src = op & 0x20 ? dn(r9) : { mode: 'imm', value: r9 || 8 };
      return I({ op: kind, size: SIZE_BITS[sizeBits], src, dst: dn(r0) });
    }

    default:
      return null; // line A / line F emulator traps: not used as code here
  }
}

function decodeMisc(c, op, r9, m3, r0, sizeBits) {
  switch (op) {
    case 0x4e70: return { op: 'reset' };
    case 0x4e71: return { op: 'nop' };
    case 0x4e72: return { op: 'stop', src: { mode: 'imm', value: c.word() } };
    case 0x4e73: return { op: 'rte' };
    case 0x4e75: return { op: 'rts' };
    case 0x4e76: return { op: 'trapv' };
    case 0x4e77: return { op: 'rtr' };
    case 0x4afc: return { op: 'illegal' };
  }
  if ((op & 0xfff0) === 0x4e40) return { op: 'trap', src: { mode: 'imm', value: op & 0xf } };
  if ((op & 0xfff8) === 0x4e50) return { op: 'link', size: 2, dst: an(r0), src: { mode: 'imm', value: s16(c.word()) } };
  if ((op & 0xfff8) === 0x4e58) return { op: 'unlk', dst: an(r0) };
  if ((op & 0xfff8) === 0x4e60) return { op: 'move_usp', toUsp: true, src: an(r0) };
  if ((op & 0xfff8) === 0x4e68) return { op: 'move_usp', toUsp: false, dst: an(r0) };
  if ((op & 0xff80) === 0x4e80) {
    const dst = ea(c, m3, r0, 4);
    if (!dst || !isControl(dst)) return null;
    return { op: op & 0x40 ? 'jmp' : 'jsr', dst };
  }
  if ((op & 0xf1c0) === 0x41c0) {
    const src = ea(c, m3, r0, 4);
    if (!src || !isControl(src)) return null;
    return { op: 'lea', size: 4, src, dst: an(r9) };
  }
  if ((op & 0xf1c0) === 0x4180) {
    const src = ea(c, m3, r0, 2);
    if (!src || !isData(src)) return null;
    return { op: 'chk', size: 2, src, dst: dn(r9) };
  }
  if ((op & 0xfff8) === 0x4840) return { op: 'swap', size: 4, dst: dn(r0) };
  if ((op & 0xffc0) === 0x4840) {
    const src = ea(c, m3, r0, 4);
    if (!src || !isControl(src)) return null;
    return { op: 'pea', size: 4, src };
  }
  if ((op & 0xfff8) === 0x4880) return { op: 'ext', size: 2, dst: dn(r0) };
  if ((op & 0xfff8) === 0x48c0) return { op: 'ext', size: 4, dst: dn(r0) };
  if ((op & 0xfb80) === 0x4880) {
    const toRegs = !!(op & 0x400);
    const size = op & 0x40 ? 4 : 2;
    const mask = c.word();
    const mem = ea(c, m3, r0, size);
    if (!mem) return null;
    if (toRegs ? !(isControl(mem) || mem.mode === 'postinc') : !(ctrlAlt(mem) || mem.mode === 'predec')) return null;
    return { op: 'movem', size, toRegs, mask, mem };
  }
  if ((op & 0xffc0) === 0x40c0) {
    const dst = ea(c, m3, r0, 2);
    if (!dst || !dataAlt(dst)) return null;
    return { op: 'move_from_sr', size: 2, dst };
  }
  if ((op & 0xffc0) === 0x44c0 || (op & 0xffc0) === 0x46c0) {
    const src = ea(c, m3, r0, 2);
    if (!src || !isData(src)) return null;
    return { op: (op & 0x200) ? 'move_to_sr' : 'move_to_ccr', size: 2, src };
  }
  if ((op & 0xffc0) === 0x4800 || (op & 0xffc0) === 0x4ac0) {
    const dst = ea(c, m3, r0, 1);
    if (!dst || !dataAlt(dst)) return null;
    return { op: (op & 0x200) ? 'tas' : 'nbcd', size: 1, dst };
  }
  const single = { 0x40: 'negx', 0x42: 'clr', 0x44: 'neg', 0x46: 'not', 0x4a: 'tst' }[op >> 8];
  if (single && sizeBits !== 3) {
    const size = SIZE_BITS[sizeBits];
    const dst = ea(c, m3, r0, size);
    if (!dst || !dataAlt(dst)) return null;
    return { op: single, size, dst };
  }
  return null;
}

// ---- Text formatting -------------------------------------------------------

const hex = (v, w = 0) => '$' + (v >>> 0).toString(16).toUpperCase().padStart(w, '0');
const shex = (v) => (v < 0 ? '-' + hex(-v) : hex(v));

export function fmtOperand(o, size, label = (a) => hex(a)) {
  switch (o.mode) {
    case 'dn': return `d${o.reg}`;
    case 'an': return o.reg === 7 ? 'sp' : `a${o.reg}`;
    case 'ind': return `(a${o.reg})`;
    case 'postinc': return `(a${o.reg})+`;
    case 'predec': return `-(a${o.reg})`;
    case 'disp': return `${shex(o.disp)}(a${o.reg})`;
    case 'idx': return `${shex(o.disp)}(a${o.reg},${o.idxA ? 'a' : 'd'}${o.idxReg}.${o.idxLong ? 'l' : 'w'})`;
    case 'absw': return `(${hex(o.addr, 8)}).w`;
    case 'absl': return `(${label(o.addr)}).l`;
    case 'pcdisp': return `${label(o.addr)}(pc)`;
    case 'pcidx': return `${label(o.base)}(pc,${o.idxA ? 'a' : 'd'}${o.idxReg}.${o.idxLong ? 'l' : 'w'})`;
    case 'imm': {
      const v = size === 1 ? o.value & 0xff : size === 2 ? o.value & 0xffff : o.value >>> 0;
      return '#' + hex(v);
    }
  }
  return '?';
}

function regList(mask, predec) {
  const names = [];
  for (let i = 0; i < 16; i++) {
    const bit = predec ? 15 - i : i;
    if (mask & (1 << bit)) names.push(i < 8 ? `d${i}` : `a${i - 8}`);
  }
  // Collapse runs like d0-d3.
  const out = [];
  for (let i = 0; i < names.length; i++) {
    let j = i;
    while (j + 1 < names.length && names[j + 1][0] === names[i][0] && +names[j + 1][1] === +names[j][1] + 1) j++;
    out.push(j > i ? `${names[i]}-${names[j]}` : names[i]);
    i = j;
  }
  return out.join('/');
}

export function format(inst, label = (a) => hex(a)) {
  const f = (o) => fmtOperand(o, inst.size, label);
  const sfx = inst.size ? '.' + SZ[inst.size] : '';
  switch (inst.op) {
    case 'bra': case 'bsr': return `${inst.op}${inst.short ? '.s' : '.w'}\t${label(inst.target)}`;
    case 'bcc': return `b${COND[inst.cond]}${inst.short ? '.s' : '.w'}\t${label(inst.target)}`;
    case 'dbcc': return `db${inst.cond === 1 ? 'ra' : COND[inst.cond]}\t${f(inst.dst)},${label(inst.target)}`;
    case 'scc': return `s${COND[inst.cond]}\t${f(inst.dst)}`;
    case 'movem': {
      const regs = regList(inst.mask, inst.mem.mode === 'predec');
      return inst.toRegs ? `movem${sfx}\t${f(inst.mem)},${regs}` : `movem${sfx}\t${regs},${f(inst.mem)}`;
    }
    case 'ori_ccr': case 'andi_ccr': case 'eori_ccr': return `${inst.op.slice(0, -4)}\t${f(inst.src)},ccr`;
    case 'ori_sr': case 'andi_sr': case 'eori_sr': return `${inst.op.slice(0, -3)}\t${f(inst.src)},sr`;
    case 'move_from_sr': return `move.w\tsr,${f(inst.dst)}`;
    case 'move_to_sr': return `move.w\t${f(inst.src)},sr`;
    case 'move_to_ccr': return `move.w\t${f(inst.src)},ccr`;
    case 'move_usp': return inst.toUsp ? `move.l\t${f(inst.src)},usp` : `move.l\tusp,${f(inst.dst)}`;
    case 'link': return `link\t${f(inst.dst)},#${shex(inst.src.value)}`;
    case 'stop': return `stop\t#${hex(inst.src.value, 4)}`;
    case 'trap': return `trap\t#${inst.src.value}`;
    case 'swap': case 'unlk': return `${inst.op}\t${f(inst.dst)}`;
    case 'exg': return `exg\t${f(inst.src)},${f(inst.dst)}`;
    case 'moveq': return `moveq\t#${shex(inst.src.value | 0)},${f(inst.dst)}`;
    case 'addq': case 'subq': return `${inst.op}${sfx}\t#${inst.src.value},${f(inst.dst)}`;
  }
  if (inst.src && inst.dst) return `${inst.op}${sfx}\t${f(inst.src)},${f(inst.dst)}`;
  if (inst.dst) return `${inst.op}${sfx}\t${f(inst.dst)}`;
  if (inst.src) return `${inst.op}${sfx}\t${f(inst.src)}`;
  return inst.op;
}

// Instructions after which execution does not continue to the next address.
export const isTerminator = (i) => ['rts', 'rte', 'rtr', 'bra', 'jmp', 'illegal', 'stop'].includes(i.op);
