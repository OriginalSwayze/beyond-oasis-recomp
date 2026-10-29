// Translates decoded 68000 instructions into C (see runtime/src/recomp_rt.h).
// Each basic block becomes `uint32_t b_XXXXXX(void)` returning the next PC.
// Instructions the generator does not handle are left to the interpreter: the
// block is split there, or (for a rare runtime case such as division by zero)
// returns that instruction's PC with the BAIL bit set.
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { COND, format } from './m68k.mjs';
import { ROOT } from './rom.mjs';

// Instruction-level patches (config/patches.json): compares whose operands come from runtime variables.
const PATCHES = JSON.parse(readFileSync(join(ROOT, 'config', 'patches.json'), 'utf8'));
const CULL_PCS = new Set(PATCHES.spriteCull.pcs.map((p) => parseInt(p, 16)));
const CAM = PATCHES.cameraClamp;
// Whole-instruction replacements: pc -> [expected text, C lines].
const REPLACED = new Map([
  [parseInt(CAM.minTest, 16), ['tst.w	d1', ['cmpw(cam_margin(), D(1) & 0xFFFFu);']]],               // bpl: d1 >= margin
  [parseInt(CAM.minSet, 16), ['clr.w	d1', ['setw(&D(1), cam_margin()); logicw(D(1) & 0xFFFFu);']]],
  [parseInt(CAM.maxLoad, 16), ['move.w	($FF177A).l,d0',
    ['setw(&D(0), rd16(0xFF177A) - cam_margin()); logicw(D(0) & 0xFFFFu);']]],
  [parseInt(PATCHES.playerDamage.pc, 16), ['sub.w	d0,d1',
    ['setw(&D(1), subw(patch_no_damage ? 0u : (D(0) & 0xFFFFu), D(1) & 0xFFFFu));']]],
]);

const hex = (v) => '0x' + (v >>> 0).toString(16).toUpperCase();
const SFX = { 1: 'b', 2: 'w', 4: 'l' };
const MASK = { 1: '0xFFu', 2: '0xFFFFu', 4: '0xFFFFFFFFu' };
const SIGN = { 1: '0x80u', 2: '0x8000u', 4: '0x80000000u' };

const COND_C = [
  '1', '0', '(!(FLAG_C & 0x100) && FLAG_Z)', '((FLAG_C & 0x100) || !FLAG_Z)',
  '!(FLAG_C & 0x100)', '(FLAG_C & 0x100)', 'FLAG_Z', '!FLAG_Z',
  '!(FLAG_V & 0x80)', '(FLAG_V & 0x80)', '!(FLAG_N & 0x80)', '(FLAG_N & 0x80)',
  '!((FLAG_N ^ FLAG_V) & 0x80)', '((FLAG_N ^ FLAG_V) & 0x80)',
  '(!((FLAG_N ^ FLAG_V) & 0x80) && FLAG_Z)', '(((FLAG_N ^ FLAG_V) & 0x80) || !FLAG_Z)',
];

// Instructions left to the interpreter: exceptions, privileged flow, BCD, rare forms.
const UNSUPPORTED = new Set(['trap', 'trapv', 'chk', 'illegal', 'stop', 'reset', 'rte', 'rtr',
  'move_usp', 'movep', 'abcd', 'sbcd', 'nbcd']);

export function isSupported(i) {
  if (UNSUPPORTED.has(i.op)) return false;
  // Division by zero bails to the interpreter, which must not repeat an address-register update.
  if ((i.op === 'divu' || i.op === 'divs') && (i.src.mode === 'postinc' || i.src.mode === 'predec')) return false;
  return true;
}

class Emitter {
  constructor(rom) { this.rom = rom; this.lines = []; this.tmp = 0; }
  t() { return `t${this.tmp++}`; }
  emit(s) { this.lines.push('    ' + s); }

  indexExpr(o) {
    const r = o.idxA ? `A(${o.idxReg})` : `D(${o.idxReg})`;
    return o.idxLong ? r : `sx16(${r})`;
  }

  // Resolves an operand to a "location" once, so read-modify-write sees one address.
  loc(o, size) {
    const inc = (reg) => (size === 1 && reg === 7 ? 2 : size);
    switch (o.mode) {
      case 'dn': case 'an': case 'imm': return o;
      case 'ind': return { mem: `A(${o.reg})` };
      case 'postinc': {
        const t = this.t();
        this.emit(`uint32_t ${t} = A(${o.reg}); A(${o.reg}) += ${inc(o.reg)};`);
        return { mem: t };
      }
      case 'predec': {
        const t = this.t();
        this.emit(`A(${o.reg}) -= ${inc(o.reg)}; uint32_t ${t} = A(${o.reg});`);
        return { mem: t };
      }
      case 'disp': return { mem: `(A(${o.reg}) + ${o.disp})` };
      case 'idx': {
        const t = this.t();
        this.emit(`uint32_t ${t} = A(${o.reg}) + ${o.disp} + ${this.indexExpr(o)};`);
        return { mem: t };
      }
      case 'absw': case 'absl': case 'pcdisp': return { mem: hex(o.addr) };
      case 'pcidx': {
        const t = this.t();
        this.emit(`uint32_t ${t} = ${hex(o.base)} + ${this.indexExpr(o)};`);
        return { mem: t };
      }
    }
    throw new Error('bad operand ' + o.mode);
  }

  // Effective address only (lea/pea/jmp/jsr), no memory access.
  addr(o) {
    const l = this.loc(o, 4);
    if (!l.mem) throw new Error('not an address operand');
    return l.mem;
  }

  read(l, size) {
    if (l.mode === 'dn') return size === 4 ? `D(${l.reg})` : `(D(${l.reg}) & ${MASK[size]})`;
    if (l.mode === 'an') return size === 4 ? `A(${l.reg})` : `(A(${l.reg}) & ${MASK[size]})`;
    if (l.mode === 'imm') {
      const v = size === 1 ? l.value & 0xff : size === 2 ? l.value & 0xffff : l.value >>> 0;
      return hex(v) + 'u';
    }
    return `rd${size === 1 ? 8 : size === 2 ? 16 : 32}(${l.mem})`;
  }

  // Reads into a temporary so the value is fetched exactly once.
  readTmp(l, size) {
    const t = this.t();
    this.emit(`uint32_t ${t} = ${this.read(l, size)};`);
    return t;
  }

  write(l, size, v) {
    if (l.mode === 'dn') {
      if (size === 4) this.emit(`D(${l.reg}) = ${v};`);
      else this.emit(`set${SFX[size]}(&D(${l.reg}), ${v});`);
    } else if (l.mode === 'an') {
      this.emit(`A(${l.reg}) = ${v};`);
    } else {
      this.emit(`wr${size === 1 ? 8 : size === 2 ? 16 : 32}(${l.mem}, ${v});`);
    }
  }
}

// Emits one instruction. Returns 'cont' to fall through, or 'end' when the
// instruction already returned (branches, jumps, bails).
function genInst(e, i) {
  const rep = REPLACED.get(i.pc);
  if (rep) {
    if (!format(i).startsWith(rep[0])) throw new Error(`patch at ${hex(i.pc)} expects "${rep[0]}", found "${format(i)}"`);
    e.emit(`CYC(${hex(e.rom.readUInt16BE(i.pc))});`);
    for (const line of rep[1]) e.emit(line);
    return 'cont';
  }
  const next = hex(i.pc + i.len);
  const sz = i.size;
  const s = SFX[sz];
  const op = e.rom.readUInt16BE(i.pc);
  e.emit(`/* ${hex(i.pc).padEnd(8)} ${format(i).replace(/\t/g, ' ')} */`);
  e.emit(`CYC(${hex(op)});`);

  switch (i.op) {
    case 'nop': return 'cont';

    case 'move': case 'movea': {
      const src = e.loc(i.src, sz);
      const v = e.readTmp(src, sz);
      const dst = e.loc(i.dst, sz);
      if (i.op === 'movea') e.write(dst, 4, sz === 2 ? `sx16(${v})` : v);
      else { e.write(dst, sz, v); e.emit(`logic${s}(${v});`); }
      return 'cont';
    }
    case 'moveq':
      e.emit(`D(${i.dst.reg}) = ${hex(i.src.value)}u; logicl(D(${i.dst.reg}));`);
      return 'cont';

    case 'lea': e.emit(`A(${i.dst.reg}) = ${e.addr(i.src)};`); return 'cont';
    case 'pea': { const a = e.t(); e.emit(`uint32_t ${a} = ${e.addr(i.src)}; push32(${a});`); return 'cont'; }

    case 'clr': { const d = e.loc(i.dst, sz); e.write(d, sz, '0'); e.emit(`logic${s}(0);`); return 'cont'; }
    case 'tst': { const d = e.loc(i.dst, sz); e.emit(`logic${s}(${e.read(d, sz)});`); return 'cont'; }
    case 'not': {
      const d = e.loc(i.dst, sz); const v = e.readTmp(d, sz);
      e.emit(`${v} = ~${v} & ${MASK[sz]};`); e.write(d, sz, v); e.emit(`logic${s}(${v});`);
      return 'cont';
    }
    case 'neg': case 'negx': {
      const d = e.loc(i.dst, sz); const v = e.readTmp(d, sz);
      const r = e.t();
      e.emit(`uint32_t ${r} = ${i.op === 'neg' ? 'sub' : 'subx'}${s}(${v}, 0);`);
      e.write(d, sz, r);
      return 'cont';
    }
    case 'ext':
      if (sz === 2) e.emit(`setw(&D(${i.dst.reg}), sx8(D(${i.dst.reg}))); logicw(D(${i.dst.reg}) & 0xFFFF);`);
      else e.emit(`D(${i.dst.reg}) = sx16(D(${i.dst.reg})); logicl(D(${i.dst.reg}));`);
      return 'cont';
    case 'swap':
      e.emit(`D(${i.dst.reg}) = (D(${i.dst.reg}) << 16) | (D(${i.dst.reg}) >> 16); logicl(D(${i.dst.reg}));`);
      return 'cont';
    case 'exg': {
      const a = i.src.mode === 'dn' ? `D(${i.src.reg})` : `A(${i.src.reg})`;
      const b = i.dst.mode === 'dn' ? `D(${i.dst.reg})` : `A(${i.dst.reg})`;
      const t = e.t();
      e.emit(`uint32_t ${t} = ${a}; ${a} = ${b}; ${b} = ${t};`);
      return 'cont';
    }
    case 'tas': {
      const d = e.loc(i.dst, 1); const v = e.readTmp(d, 1);
      e.emit(`logicb(${v});`); e.write(d, 1, `${v} | 0x80`);
      return 'cont';
    }
    case 'scc': {
      const d = e.loc(i.dst, 1);
      e.write(d, 1, `(${COND_C[i.cond]}) ? 0xFF : 0x00`);
      // Musashi's dedicated st/sf handlers have no extra cost; other conditions add 2 when true.
      if (i.dst.mode === 'dn' && i.cond >= 2) e.emit(`if (${COND_C[i.cond]}) USE_CYCLES(2);`);
      return 'cont';
    }

    case 'add': case 'sub': case 'addi': case 'subi': case 'addq': case 'subq': {
      const fn = i.op.startsWith('add') ? 'add' : 'sub';
      const src = e.loc(i.src, sz);
      const sv = e.readTmp(src, sz);
      const dst = e.loc(i.dst, sz);
      if (dst.mode === 'an') { // addq/subq to An: full 32 bits, no flags
        e.emit(`A(${dst.reg}) ${fn === 'add' ? '+' : '-'}= ${sv};`);
        return 'cont';
      }
      const dv = e.readTmp(dst, sz);
      const r = e.t();
      e.emit(`uint32_t ${r} = ${fn}${s}(${sv}, ${dv});`);
      e.write(dst, sz, r);
      return 'cont';
    }
    case 'addx': case 'subx': {
      const src = e.loc(i.src, sz); const sv = e.readTmp(src, sz);
      const dst = e.loc(i.dst, sz); const dv = e.readTmp(dst, sz);
      const r = e.t();
      e.emit(`uint32_t ${r} = ${i.op}${s}(${sv}, ${dv});`);
      e.write(dst, sz, r);
      return 'cont';
    }
    case 'adda': case 'suba': {
      const src = e.loc(i.src, sz);
      const v = e.readTmp(src, sz);
      e.emit(`A(${i.dst.reg}) ${i.op === 'adda' ? '+' : '-'}= ${sz === 2 ? `sx16(${v})` : v};`);
      return 'cont';
    }
    case 'cmp': case 'cmpi': {
      if (CULL_PCS.has(i.pc)) {
        if (i.op !== 'cmpi' || sz !== 2 || i.dst.mode !== 'dn') throw new Error(`spriteCull patch at ${hex(i.pc)} is not cmpi.w #,Dn`);
        e.emit(`cmpw(${hex(i.src.value & 0xffff)}u + patch_cull_widen, (D(${i.dst.reg}) + patch_cull_left) & 0xFFFFu);`);
        return 'cont';
      }
      const src = e.loc(i.src, sz); const sv = e.readTmp(src, sz);
      const dst = e.loc(i.dst, sz);
      e.emit(`cmp${s}(${sv}, ${e.read(dst, sz)});`);
      return 'cont';
    }
    case 'cmpa': {
      const src = e.loc(i.src, sz); const v = e.readTmp(src, sz);
      e.emit(`cmpl(${sz === 2 ? `sx16(${v})` : v}, A(${i.dst.reg}));`);
      return 'cont';
    }
    case 'cmpm': {
      const src = e.loc(i.src, sz); const sv = e.readTmp(src, sz);
      const dst = e.loc(i.dst, sz); const dv = e.readTmp(dst, sz);
      e.emit(`cmp${s}(${sv}, ${dv});`);
      return 'cont';
    }

    case 'and': case 'or': case 'eor': case 'andi': case 'ori': case 'eori': {
      const c = { and: '&', or: '|', eor: '^' }[i.op.replace(/i$/, '')];
      const src = e.loc(i.src, sz); const sv = e.readTmp(src, sz);
      const dst = e.loc(i.dst, sz); const dv = e.readTmp(dst, sz);
      const r = e.t();
      e.emit(`uint32_t ${r} = (${dv} ${c} ${sv}) & ${MASK[sz]};`);
      e.write(dst, sz, r);
      e.emit(`logic${s}(${r});`);
      return 'cont';
    }

    case 'mulu': case 'muls': {
      const src = e.loc(i.src, 2); const v = e.readTmp(src, 2);
      const d = `D(${i.dst.reg})`;
      e.emit(`USE_CYCLES(${i.op}_cycles(${v}));`);
      if (i.op === 'mulu') e.emit(`${d} = (${d} & 0xFFFF) * ${v};`);
      else e.emit(`${d} = (uint32_t)((int32_t)(int16_t)${d} * (int32_t)(int16_t)${v});`);
      e.emit(`logicl(${d});`);
      return 'cont';
    }
    case 'divu': case 'divs': {
      const src = e.loc(i.src, 2); const v = e.readTmp(src, 2);
      // Division by zero raises an exception: let the interpreter do it. Nothing
      // has been written yet (only operand reads), except a post-increment, which
      // the interpreter would redo, so bail before resolving such operands.
      e.emit(`if (${v} == 0) return ${hex(i.pc)} | BAIL;`);
      const d = `D(${i.dst.reg})`;
      if (i.op === 'divu') {
        e.emit(`{ uint32_t q = ${d} / ${v}, r = ${d} % ${v};`);
        e.emit(`  if (q > 0xFFFF) { FLAG_V = 0x80; FLAG_C = 0; FLAG_N = 0x80; }`);
        e.emit(`  else { ${d} = (r << 16) | q; FLAG_N = (q & 0x8000) ? 0x80 : 0; FLAG_Z = q; FLAG_V = 0; FLAG_C = 0; } }`);
      } else {
        e.emit(`{ int32_t n = (int32_t)${d}, dv = (int16_t)${v};`);
        e.emit(`  if (n == (int32_t)0x80000000 && dv == -1) { ${d} = 0; FLAG_N = 0; FLAG_Z = 0; FLAG_V = 0; FLAG_C = 0; }`);
        e.emit(`  else { int32_t q = n / dv, r = n % dv;`);
        e.emit(`    if (q < -32768 || q > 32767) { FLAG_V = 0x80; FLAG_C = 0; FLAG_N = 0x80; }`);
        e.emit(`    else { ${d} = ((uint32_t)(r & 0xFFFF) << 16) | (q & 0xFFFF); FLAG_N = (q & 0x8000) ? 0x80 : 0; FLAG_Z = q & 0xFFFF; FLAG_V = 0; FLAG_C = 0; } } }`);
      }
      return 'cont';
    }

    case 'asl': case 'asr': case 'lsl': case 'lsr': case 'rol': case 'ror': case 'roxl': case 'roxr': {
      const dst = e.loc(i.dst, sz);
      const v = e.readTmp(dst, sz);
      const n = i.src.mode === 'imm' ? String(i.src.value) : `(D(${i.src.reg}) & 63)`;
      const r = e.t();
      e.emit(`uint32_t ${r} = ${i.op}${s}(${v}, ${n});`);
      if (i.src.mode === 'dn') e.emit(`USE_CYCLES(2 * (D(${i.src.reg}) & 63));`);
      else if (dst.mode === 'dn') e.emit(`USE_CYCLES(${2 * i.src.value});`); // memory forms shift by 1 at no extra cost
      e.write(dst, sz, r);
      return 'cont';
    }

    case 'btst': case 'bchg': case 'bclr': case 'bset': {
      const bitExpr = i.src.mode === 'imm' ? String(i.src.value & (sz === 4 ? 31 : 7)) : `(D(${i.src.reg}) & ${sz === 4 ? 31 : 7})`;
      const dst = e.loc(i.dst, sz);
      const v = e.readTmp(dst, sz);
      const b = e.t();
      e.emit(`uint32_t ${b} = 1u << ${bitExpr}; FLAG_Z = ${v} & ${b};`);
      if (i.op === 'bchg') e.write(dst, sz, `${v} ^ ${b}`);
      if (i.op === 'bclr') e.write(dst, sz, `${v} & ~${b}`);
      if (i.op === 'bset') e.write(dst, sz, `${v} | ${b}`);
      return 'cont';
    }

    case 'movem': {
      const regs = [];
      for (let k = 0; k < 16; k++) if (i.mask & (1 << k)) regs.push(k);
      const R = (k) => (k < 8 ? `D(${k})` : `A(${k - 8})`);
      const w = sz === 2 ? 16 : 32;
      if (i.mem.mode === 'predec') {
        // Mask is reversed: bit 0 = a7. The register being decremented stores its initial value.
        const a = `A(${i.mem.reg})`;
        const t = e.t();
        e.emit(`uint32_t ${t} = ${a};`);
        for (let k = 0; k < 16; k++) {
          if (!(i.mask & (1 << k))) continue;
          const reg = 15 - k;
          e.emit(`${t} -= ${sz}; wr${w}(${t}, ${reg === i.mem.reg + 8 ? a : R(reg)});`);
        }
        e.emit(`${a} = ${t};`);
      } else {
        const l = i.mem.mode === 'postinc' ? null : e.loc(i.mem, sz);
        const t = e.t();
        e.emit(`uint32_t ${t} = ${l ? l.mem : `A(${i.mem.reg})`};`);
        for (const k of regs) {
          if (i.toRegs) e.emit(`${R(k)} = ${sz === 2 ? `sx16(rd16(${t}))` : `rd32(${t})`}; ${t} += ${sz};`);
          else e.emit(`wr${w}(${t}, ${R(k)}); ${t} += ${sz};`);
        }
        if (i.mem.mode === 'postinc') e.emit(`A(${i.mem.reg}) = ${t};`);
      }
      e.emit(`USE_CYCLES(${regs.length * (sz === 4 ? 8 : 4)});`);
      return 'cont';
    }

    case 'link':
      e.emit(`push32(A(${i.dst.reg})); A(${i.dst.reg}) = REG_SP; REG_SP += ${i.src.value};`);
      return 'cont';
    case 'unlk':
      e.emit(`REG_SP = A(${i.dst.reg}); A(${i.dst.reg}) = pop32();`);
      return 'cont';

    case 'move_from_sr': { const d = e.loc(i.dst, 2); e.write(d, 2, 'm68ki_get_sr()'); return 'cont'; }
    case 'move_to_ccr': { const l = e.loc(i.src, 2); e.emit(`m68ki_set_ccr(${e.read(l, 2)});`); return 'cont'; }
    case 'andi_ccr': e.emit(`m68ki_set_ccr(m68ki_get_ccr() & ${hex(i.src.value)});`); return 'cont';
    case 'ori_ccr': e.emit(`m68ki_set_ccr(m68ki_get_ccr() | ${hex(i.src.value)});`); return 'cont';
    case 'eori_ccr': e.emit(`m68ki_set_ccr(m68ki_get_ccr() ^ ${hex(i.src.value)});`); return 'cont';
    case 'move_to_sr': case 'andi_sr': case 'ori_sr': case 'eori_sr': {
      // Changing SR may unmask a pending interrupt, which Musashi takes at once
      // using REG_PC, so the block ends here with REG_PC already correct.
      let v;
      if (i.op === 'move_to_sr') { const l = e.loc(i.src, 2); v = e.readTmp(l, 2); }
      else v = `(m68ki_get_sr() ${{ andi_sr: '&', ori_sr: '|', eori_sr: '^' }[i.op]} ${hex(i.src.value)})`;
      e.emit(`REG_PC = ${next}; m68ki_set_sr(${v}); return REG_PC;`);
      return 'end';
    }

    case 'bra': e.emit(`return ${hex(i.target)};`); return 'end';
    case 'bsr': e.emit(`push32(${next}); return ${hex(i.target)};`); return 'end';
    case 'bcc':
      // Cycle adjustments for an untaken branch match Musashi's 68000 timing.
      e.emit(`if (${COND_C[i.cond]}) return ${hex(i.target)};`);
      e.emit(`USE_CYCLES(${i.short ? -2 : 2}); return ${next};`);
      return 'end';
    case 'dbcc': {
      const d = `D(${i.dst.reg})`;
      e.emit(`if (!(${COND_C[i.cond]})) { uint32_t c = (${d} - 1) & 0xFFFF; setw(&${d}, c);`);
      e.emit(`  if (c != 0xFFFF) { USE_CYCLES(-2); return ${hex(i.target)}; } USE_CYCLES(2); }`);
      e.emit(`return ${next};`);
      return 'end';
    }
    case 'jmp': e.emit(`return ${e.addr(i.dst)};`); return 'end';
    case 'jsr': { const t = e.t(); e.emit(`uint32_t ${t} = ${e.addr(i.dst)}; push32(${next}); return ${t};`); return 'end'; }
    case 'rts': e.emit(`return pop32();`); return 'end';
  }
  throw new Error(`codegen: unhandled ${i.op} at ${hex(i.pc)}`);
}

const SETS_SR = new Set(['move_to_sr', 'andi_sr', 'ori_sr', 'eori_sr']);

// Splits a traced block at unsupported instructions (left to the interpreter)
// and after SR changes (which return to the dispatcher), and emits C for each part.
// Returns [{ start, entries, name, code }].
export function genBlock(rom, block) {
  const parts = [];
  let cur = [];
  for (const i of block.insts) {
    if (!isSupported(i)) {
      if (cur.length) parts.push(cur);
      cur = [];
      continue;
    }
    cur.push(i);
    if (SETS_SR.has(i.op)) { parts.push(cur); cur = []; }
  }
  if (cur.length) parts.push(cur);

  return parts.map((insts) => {
    const e = new Emitter(rom);
    let ended = false;
    // A part can be entered at any of its instructions (resuming after a time
    // slice ended inside it), so each instruction gets a label.
    const label = (pc) => `L_${pc.toString(16).toUpperCase()}`;
    for (let k = 0; k < insts.length; k++) {
      const i = insts[k];
      if (k > 0) e.lines.push(`${label(i.pc)}:;`);
      if (genInst(e, i) === 'end') { ended = true; break; }
      if (k < insts.length - 1) e.emit(`if (m68ki_remaining_cycles <= 0) return ${hex(i.pc + i.len)};`);
    }
    if (!ended) {
      const last = insts[insts.length - 1];
      e.emit(`return ${hex(last.pc + last.len)};`);
    }
    const dispatch = insts.length > 1
      ? ['    switch (pc) {', ...insts.slice(1).map((i) => `    case ${hex(i.pc)}: goto ${label(i.pc)};`), '    }']
      : [];
    const name = `b_${insts[0].pc.toString(16).toUpperCase().padStart(6, '0')}`;
    return { start: insts[0].pc, entries: insts.map((i) => i.pc), name,
             code: `static uint32_t ${name}(uint32_t pc) {\n${[...dispatch, ...e.lines].join('\n')}\n}\n` };
  });
}
