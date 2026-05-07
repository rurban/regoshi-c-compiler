#!/usr/bin/env python3
"""
Convert x86_64 Intel-syntax assembly printf strings in codegen.c to AT&T syntax.

Usage:  python3 tools/intel2att.py src/codegen.c src/codegen.c.att
Then:   mv src/codegen.c.att src/codegen.c

Strategy
--------
1. Track #ifdef ARCH_ARM64 depth to identify x86-only regions.
2. Collect each complete printf/fprintf call (possibly multi-line).
3. Split off the format-string literal(s) from the C argument list.
4. For each assembly instruction inside the format string:
     - Convert memory:   [base±off] → ±off(%base), [%s] → (%s), [rip+s] → s(%rip)
     - Add % to hardcoded register names: rax → %%rax  (double-% inside printf fmt)
     - Add $ to immediate operands: 15 → $15
     - Reverse two-operand mnemonics (dst,src → src,dst) AND reorder C args
     - Remove size keywords (byte ptr / dword ptr) and add mnemonic suffix
5. Emit the transformed call with correct indentation.

A few edge cases are marked /* TODO Intel2AT&T */ for manual inspection.
"""

import re, sys

# ── register tables ───────────────────────────────────────────────────────────

X86_REGS = {
    'rax','rbx','rcx','rdx','rsi','rdi','rbp','rsp',
    'r8','r9','r10','r11','r12','r13','r14','r15',
    'eax','ebx','ecx','edx','esi','edi','ebp','esp',
    'r8d','r9d','r10d','r11d','r12d','r13d','r14d','r15d',
    'ax','bx','cx','dx','si','di','bp','sp',
    'r8w','r9w','r10w','r11w','r12w','r13w','r14w','r15w',
    'al','bl','cl','dl','sil','dil','bpl','spl',
    'r8b','r9b','r10b','r11b','r12b','r13b','r14b','r15b',
    'ah','bh','ch','dh',
    'xmm0','xmm1','xmm2','xmm3','xmm4','xmm5','xmm6','xmm7',
    'xmm8','xmm9','xmm10','xmm11','xmm12','xmm13','xmm14','xmm15',
}

TWO_OP = {
    'mov','movss','movsd','movq','movaps','movapd','movups','movupd',
    'movdqa','movdqu','movhlps','movlhps',
    'lea',
    'add','sub','and','or','xor','cmp','test',
    'shl','shr','sar','rol','ror',
    'movzx','movsx','movzxd','movslq','movsxd',
    'imul',
    'addss','addsd','subss','subsd','mulss','mulsd','divss','divsd',
    'ucomiss','ucomisd','comisd','comiss',
    'cvtsi2ss','cvtsi2sd','cvtss2sd','cvtsd2ss',
    'cvttss2si','cvttsd2si','cvtss2si','cvtsd2si',
    'xchg','xadd',
    'lzcnt','tzcnt','popcnt',
    'bt','bts','btr','btc','bsf','bsr',
    'movabs',
    'lock xadd','lock cmpxchg',
    'lock add','lock sub','lock and','lock or','lock xor',
}
for _cc in 'e ne l g le ge b a be ae s ns p np z nz c nc o no nb nbe na nae'.split():
    TWO_OP.add('cmov' + _cc)

ONE_OP = {
    'push','pop','inc','dec','not','neg','bswap',
    'mul','div','idiv',
    'fld','fst','fstp','fldcw','fnstcw','fstcw','fild','fistp','fists',
    'call',
    'jmp','je','jne','jl','jg','jle','jge',
    'jb','ja','jbe','jae','js','jns','jp','jnp',
    'jc','jnc','jo','jno','jz','jnz','jnb','jnbe','jna','jnae',
    'sete','setne','setl','setg','setle','setge',
    'setb','seta','setbe','setae','sets','setns',
    'setp','setnp','setc','setnc','setz','setnz',
    'int',
}

ZERO_OP = {'ret','cqo','cdq','nop','hlt','syscall','cld','std'}
REP_OPS  = {'rep','repe','repne','repz','repnz'}

SIZE_KW  = {'byte':'b','word':'w','dword':'l','qword':'q','tbyte':'t'}
# x87 float mnemonics that use different suffix convention
X87_MN   = {'fld','fst','fstp','fild','fistp','fists','fldcw','fnstcw','fstcw'}
X87_SIZE = {'b':'s','w':'s','l':'s','q':'l','t':'t','':''}  # dword→flds, qword→fldl

FLOAT_MN = {
    'movss','movsd','addss','addsd','subss','subsd',
    'mulss','mulsd','divss','divsd',
    'ucomiss','ucomisd','comisd','comiss',
    'cvtss2sd','cvtsd2ss','cvttss2si','cvttsd2si','cvtss2si','cvtsd2si',
    'cvtsi2ss','cvtsi2sd',
}

# Regex matching a C-style format specifier
_FMT_SPEC_RE = re.compile(r'%(?:llu|lld|lu|ld|ll[duxXo]|[duxXosp])')


def reg_suffix(name):
    name = name.lstrip('%')
    if name in {'rax','rbx','rcx','rdx','rsi','rdi','rbp','rsp',
                'r8','r9','r10','r11','r12','r13','r14','r15'}:
        return 'q'
    if name in {'eax','ebx','ecx','edx','esi','edi','ebp','esp',
                'r8d','r9d','r10d','r11d','r12d','r13d','r14d','r15d'}:
        return 'l'
    if name in {'ax','bx','cx','dx','si','di','bp','sp',
                'r8w','r9w','r10w','r11w','r12w','r13w','r14w','r15w'}:
        return 'w'
    if name in {'al','bl','cl','dl','sil','dil','bpl','spl',
                'r8b','r9b','r10b','r11b','r12b','r13b','r14b','r15b',
                'ah','bh','ch','dh'}:
        return 'b'
    return ''   # xmm, unknown – let GAS infer


# ── memory address conversion ─────────────────────────────────────────────────

def intel_mem_to_att(inner):
    """
    Convert the inside of Intel [...] to AT&T addressing form.
    Returns the AT&T string. %%reg means literal %reg in printf output.
    """
    s = inner.strip()

    # [%s]  →  (%s)
    if s == '%s':
        return '(%s)'

    # [rip + sym]  or  [rip + .LFN]  →  sym(%%rip)
    m = re.fullmatch(r'rip\s*\+\s*(.+)', s)
    if m:
        return f'{m.group(1).strip()}(%%rip)'

    # [rbp - %d]
    m = re.fullmatch(r'rbp\s*-\s*(%(?:llu|lld|lu|ld|d))', s)
    if m: return f'-{m.group(1)}(%%rbp)'

    # [rbp + %d]
    m = re.fullmatch(r'rbp\s*\+\s*(%(?:llu|lld|lu|ld|d))', s)
    if m: return f'{m.group(1)}(%%rbp)'

    # [rbp - N]
    m = re.fullmatch(r'rbp\s*-\s*(\d+)', s)
    if m: return f'-{m.group(1)}(%%rbp)'

    # [rbp + N]
    m = re.fullmatch(r'rbp\s*\+\s*(\d+)', s)
    if m: return f'{m.group(1)}(%%rbp)'

    # [rsp + N]
    m = re.fullmatch(r'rsp\s*\+\s*(\d+)', s)
    if m: return f'{m.group(1)}(%%rsp)'

    # [rsp - N]
    m = re.fullmatch(r'rsp\s*-\s*(\d+)', s)
    if m: return f'-{m.group(1)}(%%rsp)'

    # [rsp - %d]
    m = re.fullmatch(r'rsp\s*-\s*(%[dlu]+)', s)
    if m: return f'-{m.group(1)}(%%rsp)'

    # [rsp + %d]
    m = re.fullmatch(r'rsp\s*\+\s*(%[dlu]+)', s)
    if m: return f'{m.group(1)}(%%rsp)'

    # [%s + N]
    m = re.fullmatch(r'(%s)\s*\+\s*(\d+)', s)
    if m: return f'{m.group(2)}({m.group(1)})'

    # [%s - N]
    m = re.fullmatch(r'(%s)\s*-\s*(\d+)', s)
    if m: return f'-{m.group(2)}({m.group(1)})'

    # [%s + %d]
    m = re.fullmatch(r'(%s)\s*\+\s*(%(?:llu|lld|lu|ld|d))', s)
    if m: return f'{m.group(2)}({m.group(1)})'

    # [reg + N]  literal register name
    m = re.fullmatch(r'([a-z][a-z0-9]*)\s*\+\s*(\d+)', s)
    if m and m.group(1) in X86_REGS:
        return f'{m.group(2)}(%%{m.group(1)})'

    # [reg - N]
    m = re.fullmatch(r'([a-z][a-z0-9]*)\s*-\s*(\d+)', s)
    if m and m.group(1) in X86_REGS:
        return f'-{m.group(2)}(%%{m.group(1)})'

    # [reg]
    if s in X86_REGS:
        return f'(%%{s})'

    # [reg + reg]
    m = re.fullmatch(r'([a-z][a-z0-9]*)\s*\+\s*([a-z][a-z0-9]*)', s)
    if m and m.group(1) in X86_REGS and m.group(2) in X86_REGS:
        return f'(%%{m.group(1)},%%{m.group(2)})'

    # [reg1 + reg2 - 1]
    m = re.fullmatch(r'([a-z][a-z0-9]*)\s*\+\s*([a-z][a-z0-9]*)\s*-\s*1', s)
    if m and m.group(1) in X86_REGS and m.group(2) in X86_REGS:
        return f'-1(%%{m.group(1)},%%{m.group(2)})'

    # [%s + reg - 1]  e.g. [%s + rcx - 1]  ->  -1(%s,%%rcx)
    m = re.fullmatch(r'(%s)\s*\+\s*([a-z][a-z0-9]*)\s*-\s*1', s)
    if m and m.group(2) in X86_REGS:
        return f'-1({m.group(1)},%%{m.group(2)})'

    # [rbp - %d + rcx - 1]  ->  -%d-1(%%rbp,%%rcx)   (GAS evaluates -%d-1 as constant)
    m = re.fullmatch(r'rbp\s*-\s*(%[dlu]+)\s*\+\s*([a-z][a-z0-9]*)\s*-\s*1', s)
    if m and m.group(2) in X86_REGS:
        return f'-{m.group(1)}-1(%%rbp,%%{m.group(2)})'

    # [rbp + %d + reg - 1]  ->  %d-1(%%rbp,%%reg)
    m = re.fullmatch(r'rbp\s*\+\s*(%[dlu]+)\s*\+\s*([a-z][a-z0-9]*)\s*-\s*1', s)
    if m and m.group(2) in X86_REGS:
        return f'{m.group(1)}-1(%%rbp,%%{m.group(2)})'

    # [%d + %s + rcx - 1]  ->  %d-1(%s,%%rcx)   (copy_len case)
    m = re.fullmatch(r'(%[dlu]+)\s*\+\s*(%s)\s*\+\s*([a-z][a-z0-9]*)\s*-\s*1', s)
    if m and m.group(3) in X86_REGS:
        return f'{m.group(1)}-1({m.group(2)},%%{m.group(3)})'

    # [%s + %d + rcx - 1]  ->  %d-1(%s,%%rcx)   (note: spec order change = arg swap needed)
    m = re.fullmatch(r'(%s)\s*\+\s*(%[dlu]+)\s*\+\s*([a-z][a-z0-9]*)\s*-\s*1', s)
    if m and m.group(3) in X86_REGS:
        return f'{m.group(2)}-1({m.group(1)},%%{m.group(3)})'

    # [reg + reg * scale]
    m = re.fullmatch(r'([a-z][a-z0-9]*)\s*\+\s*([a-z][a-z0-9]*)\s*\*\s*(\d+)', s)
    if m and m.group(1) in X86_REGS and m.group(2) in X86_REGS:
        return f'(%%{m.group(1)},%%{m.group(2)},{m.group(3)})'

    # complex / unrecognised – preserve but mark
    return f'/* TODO: [{s}] */'


# ── operand conversion ────────────────────────────────────────────────────────

def is_immediate(tok):
    tok = tok.strip()
    return bool(re.fullmatch(r'-?(?:0[xX][0-9a-fA-F]+|\d+)', tok))


def convert_operand(op):
    """
    Convert a single Intel operand to AT&T form.
    Returns (att_str, size_char) where size_char is one of 'b','w','l','q',''.
    att_str uses %% for literal % (as needed inside a C printf format string).
    """
    op = op.strip()
    size = ''

    # Strip size keyword:  byte ptr X  /  dword ptr [X]
    m = re.match(r'^(byte|word|dword|qword|tbyte)\s+ptr\s+(.*)', op, re.I)
    if m:
        size = SIZE_KW[m.group(1).lower()]
        op = m.group(2).strip()

    # Memory
    m = re.fullmatch(r'\[([^\]]+)\]', op)
    if m:
        return intel_mem_to_att(m.group(1)), size

    # Hardcoded register
    if op in X86_REGS:
        return f'%%{op}', size or reg_suffix(op)

    # Format specifier for a register string (%s) – pass through unchanged;
    # the register name already carries its own '%' prefix (from the reg arrays).
    if re.fullmatch(r'%s', op):
        return op, size

    # Format specifier for an integer/immediate (%d, %lld, %u, %lu, etc.)
    # In AT&T, immediate operands need '$' prefix.
    if re.fullmatch(r'%(?:llu|lld|lu|ld|ll[duxXo]|[duxXo])', op):
        return f'${op}', size

    # Bare immediate
    if is_immediate(op):
        return f'${op}', size

    # Anything else (label, symbol, string like %s, complex expr)
    return op, size


# ── instruction line conversion ───────────────────────────────────────────────

def split_operands_balanced(s):
    """Split on commas at bracket depth 0."""
    ops, depth, cur = [], 0, []
    for ch in s:
        if ch == '[': depth += 1
        elif ch == ']': depth -= 1
        if ch == ',' and depth == 0:
            ops.append(''.join(cur).strip()); cur = []
        else:
            cur.append(ch)
    if cur: ops.append(''.join(cur).strip())
    return ops


def _fmt_specs_in(s):
    return _FMT_SPEC_RE.findall(s)


def _perm(orig, reordered, force_swap=False):
    """
    Build permutation list: reordered[j] = orig[perm[j]].
    Returns None if trivial or can't build.
    force_swap=True: for 2-element case, always return [1,0] even if types are identical.
    """
    if len(orig) != len(reordered): return None
    # 2-element swap (most common case)
    if len(orig) == 2:
        if force_swap or orig != reordered:
            return [1, 0]
        return None
    if orig == reordered: return None
    used = [False] * len(orig)
    perm = []
    for spec in reordered:
        for i, s in enumerate(orig):
            if s == spec and not used[i]:
                perm.append(i); used[i] = True; break
        else:
            return None
    return None if perm == list(range(len(perm))) else perm


def convert_asm_line(raw):
    """
    Convert one assembly instruction line (raw string, may have leading spaces).
    Returns (new_line, perm_or_None).
    perm maps output-spec-index -> input-spec-index for C arg reordering.
    """
    indent = raw[:len(raw) - len(raw.lstrip())]
    stripped = raw.strip()

    if not stripped:               return raw, None
    if stripped.endswith(':'):     return raw, None   # label
    if stripped.startswith('.'):   return raw, None   # directive
    if stripped.startswith('//'):  return raw, None   # comment
    if stripped.startswith('/*'):  return raw, None   # comment

    # Tokenise mnemonic
    parts = stripped.split(None, 1)
    mn = parts[0].lower()
    rest = parts[1].strip() if len(parts) > 1 else ''

    # 'lock' prefix: consume next word as part of mnemonic
    if mn == 'lock':
        sub = rest.split(None, 1)
        mn = 'lock ' + sub[0].lower()
        rest = sub[1].strip() if len(sub) > 1 else ''

    # rep*/repe/repne: leave as-is
    if mn in REP_OPS:
        return raw, None

    # Collect input format specs (for permutation tracking)
    in_specs = _fmt_specs_in(stripped)

    # Zero-operand
    if mn in ZERO_OP or not rest:
        return raw, None

    # One-operand (no reversal)
    if mn in ONE_OP:
        size = ''
        m = re.match(r'^(byte|word|dword|qword|tbyte)\s+ptr\s+(.*)', rest, re.I)
        if m:
            size = SIZE_KW[m.group(1).lower()]; rest = m.group(2).strip()
        att_op, sh = convert_operand(rest)
        size = size or sh
        # x87 instructions use special suffixes
        if mn in X87_MN:
            size = X87_SIZE.get(size, size)
        att_mn = mn + size if size else mn
        return f'{indent}{att_mn} {att_op}', None

    # movsxd is Intel 64-bit name for movsx r64,r/m32; AT&T uses movslq
    if mn == 'movsxd':
        ops = split_operands_balanced(rest)
        if len(ops) == 2:
            dst_att, _ = convert_operand(ops[0])
            src_att, _ = convert_operand(ops[1])
            perm = _perm(in_specs, _fmt_specs_in(ops[1]) + _fmt_specs_in(ops[0]),
                         force_swap=True)
            return f'{indent}movslq {src_att}, {dst_att}', perm

    # movzx / movsx: compound suffix
    if mn in ('movzx', 'movsx'):
        ops = split_operands_balanced(rest)
        if len(ops) != 2:
            return f'{indent}/* TODO Intel2AT&T: {stripped} */', None
        dst_att, dst_sz = convert_operand(ops[0])
        src_att, src_sz = convert_operand(ops[1])
        # src size from the 'byte ptr' / 'word ptr' already parsed in convert_operand
        # or from source register name
        if not src_sz:
            bare = ops[1].strip()
            m2 = re.match(r'^(byte|word|dword|qword)\s+ptr', bare, re.I)
            src_sz = SIZE_KW[m2.group(1).lower()] if m2 else (reg_suffix(bare) if bare in X86_REGS else 'b')
        if not dst_sz:
            bare = ops[0].strip()
            dst_sz = reg_suffix(bare) if bare in X86_REGS else 'l'
        prefix = 'movz' if mn == 'movzx' else 'movs'
        att_mn = prefix + src_sz + dst_sz
        # perm: reverse dst/src specs
        perm = _perm(in_specs, _fmt_specs_in(ops[1]) + _fmt_specs_in(ops[0]),
                     force_swap=True)
        return f'{indent}{att_mn} {src_att}, {dst_att}', perm

    # Two-operand
    if mn in TWO_OP:
        ops = split_operands_balanced(rest)

        # Special: dynamic-size memory pattern  %s [%s]  (ptr_size() + register)
        # e.g.  mov %s, %s [%s]  with args (dst_reg, size_kw, addr_reg)
        # -> AT&T:  mov (%s), %s  with args (addr_reg, dst_reg)  [middle arg dropped]
        # Similarly  add %s [%s], %d  -> addX $%d, (%s)  [drop size, reorder]
        if len(ops) == 2 and re.fullmatch(r'%s\s+\[(%s)\]', ops[1]):
            addr_att = f'({ops[1].split("[")[1].rstrip("]").strip()})'
            dst_att, dst_sz = convert_operand(ops[0])
            att_mn = mn + dst_sz if dst_sz else mn
            # Args: original [dst, size_kw, addr] -> new [addr, dst]  (drop index 1)
            spec_map = [_fmt_specs_in(ops[1].split('[')[1].rstrip(']')),
                        _fmt_specs_in(ops[0])]
            new_specs_flat = spec_map[0] + spec_map[1]  # addr specs first, dst specs second
            # Compute which of the in_specs these correspond to
            # original order: dst_specs + [size_spec] + addr_specs
            # We want to map to: addr_specs + dst_specs (dropping size_spec at index 1)
            dst_s = _fmt_specs_in(ops[0])
            # size arg is in_specs[len(dst_s)] - we drop it
            # addr spec is in_specs[len(dst_s)+1] if it exists
            n_dst = len(dst_s)
            if len(in_specs) == n_dst + 2:  # dst + size + addr
                # arg map: [n_dst+1, 0..n_dst-1]  (addr arg then dst args, drop size arg at n_dst)
                arg_map = [n_dst + 1] + list(range(n_dst))
                return f'{indent}{att_mn} {addr_att}, {dst_att}', arg_map
            return f'{indent}{att_mn} {addr_att}, {dst_att}', None

        if len(ops) == 2 and re.fullmatch(r'%s\s+\[(%s)\]', ops[0]):
            # Store: add %s [%s], %d  or  sub %s [%s], %d
            # ops[0] = '%s [%s]' (size_kw, addr_reg), ops[1] = '%d' (immediate/value)
            addr_att = f'({ops[0].split("[")[1].rstrip("]").strip()})'
            val_att, _ = convert_operand(ops[1])
            # No size info available; leave unsuffixed for GAS to infer
            att_mn = mn
            # Args: original [size_kw, addr, value] -> new [value, addr]  (drop size)
            size_s = _fmt_specs_in(ops[0].split('[')[0])   # the %s for size_kw
            addr_s = _fmt_specs_in(ops[0].split('[')[1].rstrip(']'))
            val_s  = _fmt_specs_in(ops[1])
            n_size = len(size_s); n_addr = len(addr_s); n_val = len(val_s)
            if len(in_specs) == n_size + n_addr + n_val and n_size == 1 and n_addr == 1:
                # arg map: [value, addr]  = indices [n_size+n_addr, n_size]
                arg_map = [n_size + n_addr] + [n_size]
                return f'{indent}{att_mn} {val_att}, {addr_att}', arg_map
            return f'{indent}{att_mn} {val_att}, {addr_att}', None

        # Three-operand imul (imul dst, src, imm)
        if mn == 'imul' and len(ops) == 3:
            dst_att, dst_sz = convert_operand(ops[0])
            src_att, _      = convert_operand(ops[1])
            imm_att, _      = convert_operand(ops[2])
            sz = dst_sz or reg_suffix(ops[0].strip())
            att_mn = 'imul' + sz
            # AT&T: imulX imm, src, dst
            perm = _perm(in_specs,
                         _fmt_specs_in(ops[2]) + _fmt_specs_in(ops[1]) + _fmt_specs_in(ops[0]))
            return f'{indent}{att_mn} {imm_att}, {src_att}, {dst_att}', perm

        if len(ops) < 2:
            att_op, sh = convert_operand(ops[0] if ops else rest)
            return f'{indent}{mn} {att_op}', None

        dst_intel, src_intel = ops[0], ops[1]
        dst_att, dst_sz = convert_operand(dst_intel)
        src_att, src_sz = convert_operand(src_intel)

        # Determine size suffix
        sz = dst_sz or src_sz
        if not sz:
            # Try to infer from hardcoded register names in operands
            for op_str in [dst_intel, src_intel]:
                bare = op_str.strip()
                if bare in X86_REGS:
                    sz = reg_suffix(bare); break

        if mn in FLOAT_MN or mn == 'movq':
            att_mn = mn     # already carries size info
        else:
            att_mn = mn + sz if sz else mn

        perm = _perm(in_specs,
                     _fmt_specs_in(src_intel) + _fmt_specs_in(dst_intel),
                     force_swap=True)
        return f'{indent}{att_mn} {src_att}, {dst_att}', perm

    # Shift/rotate with explicit count
    if mn in ('shl','shr','sar','rol','ror','rcl','rcr'):
        ops = split_operands_balanced(rest)
        if len(ops) == 1:
            att_op, sh = convert_operand(ops[0])
            return f'{indent}{mn}{sh} {att_op}', None
        dst_att, dst_sz = convert_operand(ops[0])
        cnt_att, _      = convert_operand(ops[1])
        sz = dst_sz or (reg_suffix(ops[0].strip()) if ops[0].strip() in X86_REGS else '')
        att_mn = mn + sz if sz else mn
        perm = _perm(in_specs,
                     _fmt_specs_in(ops[1]) + _fmt_specs_in(ops[0]),
                     force_swap=True)
        return f'{indent}{att_mn} {cnt_att}, {dst_att}', perm

    # Unknown – leave with TODO marker
    return f'{indent}/* TODO Intel2AT&T: {stripped} */', None


# ── format-string + args transformer ─────────────────────────────────────────

# fmt_str uses C escape chars: backslash-n = instruction separator, etc.
# In Python we represent them as two-character sequences (the file bytes).
_NL = '\\n'   # the two chars  \  n  as they appear in C string literals

_ASM_MN_RE = re.compile(
    r'(?:^|' + re.escape(_NL) + r')\s{2,}'
    r'(?:mov|lea|add|sub|push|pop|call|ret|jmp|je|jne|jl|jg|jle|jge|jb|ja|'
    r'jbe|jae|js|jns|jp|jnp|jz|jnz|cmp|xor|and|or|inc|dec|imul|idiv|not|neg|'
    r'shl|shr|sar|lzcnt|tzcnt|popcnt|bswap|movzx|movsx|test|set|cvt|fld|fst|'
    r'movss|movsd|addss|addsd|mulss|mulsd|subss|subsd|divss|divsd|ucomisd|'
    r'ucomiss|comisd|xchg|cqo|cdq|rep|lock|bt|bsf|bsr|movabs|movq|movsxd|'
    r'movslq|cld|repe|rol|ror)\b'
)


def transform_fmt_and_args(fmt_str, c_args):
    """
    Params
    ------
    fmt_str : Python str with raw C escape sequences (e.g. backslash-n = \\n)
    c_args  : list of C argument strings (stripped, no leading comma)

    Returns
    -------
    (new_fmt_str, new_c_args)
    """
    lines = fmt_str.split(_NL)
    out_lines = []
    c_args_work = list(c_args)
    spec_offset = 0   # index of first format-spec arg for the current line

    for raw in lines:
        specs_here = _FMT_SPEC_RE.findall(raw)
        n = len(specs_here)

        new_line, perm = convert_asm_line(raw)

        if perm is not None and n >= 1:
            base = spec_offset
            old_slice = c_args_work[base:base + n]
            if len(old_slice) == n:
                new_slice = [old_slice[p] for p in perm]
                # Replace n old args with len(new_slice) new args (may drop some)
                c_args_work[base:base + n] = new_slice
                # Also reorder format specs inside new_line if types differ
                new_specs = _FMT_SPEC_RE.findall(new_line)
                want_specs = [specs_here[p] for p in perm if p < len(specs_here)]
                if new_specs != want_specs and len(new_specs) == len(want_specs):
                    # Replace format specs in new_line one by one
                    result = []; si = 0; i = 0
                    while i < len(new_line):
                        m = _FMT_SPEC_RE.match(new_line, i)
                        if m and si < len(want_specs):
                            result.append(want_specs[si]); si += 1; i = m.end()
                        else:
                            result.append(new_line[i]); i += 1
                    new_line = ''.join(result)

        out_lines.append(new_line)
        # spec_offset advances by the number of specs REMAINING after any drops
        if perm is not None and n >= 1:
            spec_offset += len(perm)
        else:
            spec_offset += n

    return _NL.join(out_lines), c_args_work


# ── C source parser helpers ───────────────────────────────────────────────────

def collect_call(lines, start):
    """Collect a complete printf/fprintf call from lines[start].
    Returns (end_idx_exclusive, joined_text)."""
    depth, started, text, i = 0, False, [], start
    while i < len(lines):
        text.append(lines[i])
        for ch in lines[i]:
            if ch == '(':
                depth += 1; started = True
            elif ch == ')':
                depth -= 1
                if started and depth == 0:
                    return i + 1, '\n'.join(text)
        i += 1
    return i, '\n'.join(text)


def parse_call(call_text):
    """
    Parse printf/fprintf call into (prefix, fmt_str, arg_list).

    prefix   – 'printf(' or 'fprintf(stderr, ' etc.
    fmt_str  – concatenated C string literal content (with \\n as two chars)
    arg_list – list of C expression strings (may be empty)

    Returns None if unparseable.
    """
    m = re.match(r'^(\s*)((?:fprintf|printf)\s*\()', call_text, re.DOTALL)
    if not m: return None
    # prefix is the function+open-paren WITHOUT leading whitespace
    # (indent is handled separately at reconstruction time)
    prefix = m.group(2)
    pos = m.end()

    # For fprintf: skip first arg (file pointer) up to the first comma at depth 0
    if 'fprintf' in prefix:
        d = 0
        while pos < len(call_text):
            c = call_text[pos]
            if c == '(': d += 1
            elif c == ')': d -= 1
            elif c == ',' and d == 0:
                prefix += call_text[m.end():pos + 1]
                pos += 1
                break
            pos += 1
        while pos < len(call_text) and call_text[pos] in ' \t\n':
            pos += 1

    # Macro expansions used in format string concatenation (x86-64 side)
    _MACRO_VALS = {
        'FRAME_PTR': 'rbp',
        'STACK_REG': 'rsp',
    }

    # Collect consecutive string literals (and macro substitutions between them)
    fmt_parts = []
    while pos < len(call_text):
        # Skip whitespace
        while pos < len(call_text) and call_text[pos] in ' \t\n':
            pos += 1
        if pos >= len(call_text):
            break
        # Check for known macros used as string-concat between literals
        macro_match = None
        for macro, val in _MACRO_VALS.items():
            if call_text[pos:pos+len(macro)] == macro and \
               (pos+len(macro) >= len(call_text) or not call_text[pos+len(macro)].isalnum()):
                macro_match = (macro, val)
                break
        if macro_match:
            fmt_parts.append(macro_match[1])
            pos += len(macro_match[0])
            continue
        if call_text[pos] != '"':
            break
        # Scan to end of string literal (respecting \\)
        start = pos; pos += 1
        while pos < len(call_text):
            c = call_text[pos]
            if c == '\\': pos += 2; continue
            if c == '"': pos += 1; break
            pos += 1
        raw = call_text[start:pos]   # includes surrounding quotes
        # Extract content (everything between outer quotes)
        fmt_parts.append(raw[1:-1])

    if not fmt_parts: return None
    fmt_str = ''.join(fmt_parts)

    # Remaining text after the string literal(s)
    rest = call_text[pos:].rstrip()
    # Strip trailing );
    rest = re.sub(r'\s*\)\s*;\s*$', '', rest, re.DOTALL)
    # Strip leading comma (separator between fmt string and first arg)
    rest = rest.lstrip()
    if rest.startswith(','):
        rest = rest[1:].lstrip()

    # Split args
    args = split_c_args(rest) if rest else []
    return prefix, fmt_str, args


def split_c_args(s):
    """Split comma-separated C expressions (depth-aware)."""
    args, depth, cur = [], 0, []
    for ch in s:
        if ch in '([{': depth += 1
        elif ch in ')]}': depth -= 1
        if ch == ',' and depth == 0:
            t = ''.join(cur).strip()
            if t: args.append(t)
            cur = []
        else:
            cur.append(ch)
    t = ''.join(cur).strip()
    if t: args.append(t)
    return args


# ── top-level file transformation ─────────────────────────────────────────────

def has_x86_asm(fmt_str):
    """True if the format string contains x86 assembly instructions."""
    return bool(_ASM_MN_RE.search(fmt_str))


def transform_file(src):
    lines = src.split('\n')
    out = []
    i = 0
    # ifdef stack: each entry is 'arm64', 'x86', or None
    #   'arm64' = inside ARCH_ARM64 branch  -> do NOT transform
    #   'x86'   = inside !ARCH_ARM64 branch -> OK to transform
    #   None    = inside some other ifdef (e.g. __APPLE__) -> neutral
    ifdef_stack = []

    def push_arm64(arm64_branch):
        ifdef_stack.append('arm64' if arm64_branch else 'x86')

    def push_other():
        ifdef_stack.append(None)

    def flip_top():
        if ifdef_stack:
            top = ifdef_stack[-1]
            if top == 'arm64':   ifdef_stack[-1] = 'x86'
            elif top == 'x86':   ifdef_stack[-1] = 'arm64'
            # None: flip is a no-op (we don't track other ifdefs precisely)

    def pop_top():
        if ifdef_stack: ifdef_stack.pop()

    def in_arm64():
        return any(v == 'arm64' for v in ifdef_stack)

    while i < len(lines):
        line = lines[i]
        s = line.strip()

        # ── Track ifdef depth ──────────────────────────────────────────────
        if re.match(r'#\s*ifdef\s+ARCH_ARM64\b', s) or \
           re.match(r'#\s*if\s+defined\s*\(\s*ARCH_ARM64\s*\)', s):
            push_arm64(True)
            out.append(line); i += 1; continue

        if re.match(r'#\s*ifndef\s+ARCH_ARM64\b', s):
            push_arm64(False)          # currently in x86 branch
            out.append(line); i += 1; continue

        if re.match(r'#\s*ifdef\b', s) or re.match(r'#\s*if\b', s):
            push_other()
            out.append(line); i += 1; continue

        if re.match(r'#\s*else\b', s):
            flip_top()
            out.append(line); i += 1; continue

        if re.match(r'#\s*endif\b', s):
            pop_top()
            out.append(line); i += 1; continue

        # Only transform when NOT inside an ARM64-only block
        in_x86 = not in_arm64()

        # ── Register array definitions ─────────────────────────────────────
        if in_x86 and re.search(r'\b(?:static\s+)?char\s*\*\s*(reg64|reg32|reg16|reg8)\s*\[\]', s):
            new = re.sub(r'"([a-z][a-z0-9]*)"',
                         lambda m: f'"%{m.group(1)}"' if m.group(1) in X86_REGS else m.group(0),
                         line)
            out.append(new); i += 1; continue

        if in_x86 and re.search(r'\bchar\s*\*\s*(argreg32|argreg64|argxmm|param_regs64|param_regs32)\s*\[\]', s):
            new = re.sub(r'"([a-z][a-z0-9]*)"',
                         lambda m: f'"%{m.group(1)}"' if m.group(1) in X86_REGS else m.group(0),
                         line)
            out.append(new); i += 1; continue

        # ── Inline register name strings used as printf args ──────────────
        # e.g. char *retreg = "rdi"; or  sz == 8 ? "rax" : "eax"
        # Add % to string literals that are pure register names.
        # BUT skip lines with strcmp/strncmp/strstr/reserved_regs (not format args).
        if in_x86 and '"' in line and not re.search(r'strcmp|strncmp|strstr|reserved|base_reg|reg_name', line):
            def _add_pct(m):
                name = m.group(1)
                if name in X86_REGS:
                    return f'"%{name}"'
                return m.group(0)
            # Apply to lines that look like assignment/arg contexts (not includes/macros)
            # Also catch standalone string literals on their own line (like in #ifdef retreg)
            if re.search(r'=\s*"[a-z]|"[a-z][a-z0-9]+"\s*[;,)]|"\s*:\s*"[a-z]|\?\s*"[a-z]', line) or \
               re.fullmatch(r'\s*"[a-z][a-z0-9]*"\s*', line):
                new = re.sub(r'"([a-z][a-z0-9]*)"', _add_pct, line)
                if new != line:
                    out.append(new); i += 1; continue

        # ── Syntax-directive removals ──────────────────────────────────────
        if in_x86 and 'printf(".intel_syntax noprefix\\n")' in line:
            indent = re.match(r'^(\s*)', line).group(1)
            out.append(f'{indent}/* AT&T syntax is now the default */'); i += 1; continue

        if in_x86 and 'printf(".att_syntax prefix\\n")' in line:
            indent = re.match(r'^(\s*)', line).group(1)
            out.append(f'{indent}/* inline asm already in AT&T */'); i += 1; continue

        # ── format() calls that create Intel memory strings ────────────────
        if in_x86 and 'format(' in line:
            new = line
            new = re.sub(r'format\("\[%s\]"',       'format("(%s)"',          new)
            new = re.sub(r'format\("\[rbp-%d\]"',   'format("-%d(%%rbp)"',    new)
            new = re.sub(r'format\("\[rbp\+%d\]"',  'format("%d(%%rbp)"',     new)
            new = re.sub(r'format\("\[rip \+ %s\]"','format("%s(%%rip)"',     new)
            new = re.sub(r'format\("\[rip \+ ([^"]+)\]"', r'format("\1(%%rip)"', new)
            if new != line:
                out.append(new); i += 1; continue

        # ── printf / fprintf calls ─────────────────────────────────────────
        if in_x86 and re.match(r'\s*(?:printf|fprintf)\s*\(', line):
            end_i, call_text = collect_call(lines, i)
            parsed = parse_call(call_text)

            if parsed is None or not has_x86_asm(parsed[1]):
                # Not a parseable assembly printf – emit as-is but still add % to reg literals
                def _pct2(m):
                    if m.group(1) in X86_REGS: return f'"%{m.group(1)}"'
                    return m.group(0)
                for l in call_text.split('\n'):
                    if '"' in l and not re.search(r'strcmp|strncmp|reserved|reg_name', l):
                        l = re.sub(r'"([a-z][a-z0-9]*)"', _pct2, l)
                    out.append(l)
                i = end_i
                continue

            prefix_fn, fmt_str, c_args = parsed

            new_fmt, new_args = transform_fmt_and_args(fmt_str, c_args)

            # Add % to register name string literals inside C args
            # e.g. sz==8 ? "rax" : "eax"  ->  sz==8 ? "%rax" : "%eax"
            def _pct_reg(m):
                if m.group(1) in X86_REGS: return f'"%{m.group(1)}"'
                return m.group(0)
            new_args = [re.sub(r'"([a-z][a-z0-9]*)"', _pct_reg, a) for a in new_args]

            # Reconstruct the call as a single-line printf
            orig_indent = re.match(r'^(\s*)', call_text).group(1)
            args_str = (', ' + ', '.join(new_args)) if new_args else ''
            new_call = f'{orig_indent}{prefix_fn}"{new_fmt}"{args_str});'
            out.append(new_call)
            i = end_i
            continue

        out.append(line)
        i += 1

    return '\n'.join(out)


def main():
    if len(sys.argv) < 2:
        print('Usage: intel2att.py <input.c> [output.c]', file=sys.stderr)
        sys.exit(1)
    src = open(sys.argv[1]).read()
    result = transform_file(src)
    if len(sys.argv) >= 3:
        open(sys.argv[2], 'w').write(result)
    else:
        sys.stdout.write(result)

if __name__ == '__main__':
    main()
