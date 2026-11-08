"""Emit C for analysed functions.

Each MIPS function becomes `void <name>(void)` operating on the global CPU
state (runtime/recomp.h). Intra-function control flow uses gotos; direct
calls within the same image are direct C calls, everything else goes through
rt_call(). Delay slots are executed before the branch takes effect, with the
branch condition / jump target captured first.
"""
from mips import REG


# Widescreen culling patch points (analysis/widescreen_cull.txt, loaded by recomp.py):
# main-exe `slti` address -> widened immediate. The instruction is emitted as
# `rs < WS_IMM(orig, wide)`, which is the original comparison unless --widescreen is on.
WS_PATCHES = {}
WS_EMITTED = set()


def fname(addr, ovl_tag=''):
    return f'f_{addr:08x}{ovl_tag}'


def R(n):
    return '0' if n == 0 else f'R{n}'


class FuncEmitter:
    def __init__(self, func, image_funcs, name_of, ovl_tag):
        self.f = func
        self.image_funcs = image_funcs  # set of entry addrs callable directly
        self.name_of = name_of          # addr -> C name for direct calls
        self.ovl_tag = ovl_tag
        self.out = []
        self.pending = 0  # instructions since last cycle flush

    def w(self, s):
        self.out.append(s)

    def flush_cycles(self):
        if self.pending:
            self.w(f'    CYC({self.pending});')
            self.pending = 0

    # -- single non-control instruction ----------------------------------
    def simple(self, i):
        o, rs, rt, rd = i.op, R(i.rs), R(i.rt), R(i.rd)
        sa, imm = i.sa, i.imm

        def setr(dst, expr):
            if dst == 0:
                return f'/* {i} */' if not expr.startswith('mem_') else f'(void){expr};'
            return f'R{dst} = {expr};'

        if o == 'sll':
            if i.word == 0:
                return ''
            return setr(i.rd, f'{rt} << {sa}')
        if o == 'srl':
            return setr(i.rd, f'{rt} >> {sa}')
        if o == 'sra':
            return setr(i.rd, f'(u32)((s32){rt} >> {sa})')
        if o == 'sllv':
            return setr(i.rd, f'{rt} << ({rs} & 31)')
        if o == 'srlv':
            return setr(i.rd, f'{rt} >> ({rs} & 31)')
        if o == 'srav':
            return setr(i.rd, f'(u32)((s32){rt} >> ({rs} & 31))')
        if o == 'mfhi':
            return setr(i.rd, 'HI')
        if o == 'mflo':
            return setr(i.rd, 'LO')
        if o == 'mthi':
            return f'HI = {rs};'
        if o == 'mtlo':
            return f'LO = {rs};'
        if o == 'mult':
            return f'{{ s64 p = (s64)(s32){rs} * (s32){rt}; LO = (u32)p; HI = (u32)(p >> 32); }}'
        if o == 'multu':
            return f'{{ u64 p = (u64){rs} * {rt}; LO = (u32)p; HI = (u32)(p >> 32); }}'
        if o == 'div':
            return f'op_div({rs}, {rt});'
        if o == 'divu':
            return f'op_divu({rs}, {rt});'
        if o in ('add', 'addu', 'or') and i.rd and (i.rs == 0 or i.rt == 0):
            # register move: carry the PGXP precision tag (runtime/pgxp.h)
            src = i.rt if i.rs == 0 else i.rs
            return setr(i.rd, f'{rs} + {rt}' if o != 'or' else f'{rs} | {rt}') + \
                f' pgxp_mov({i.rd}, {src});'
        if o in ('add', 'addu'):
            return setr(i.rd, f'{rs} + {rt}')
        if o in ('sub', 'subu'):
            return setr(i.rd, f'{rs} - {rt}')
        if o == 'and':
            return setr(i.rd, f'{rs} & {rt}')
        if o == 'or':
            return setr(i.rd, f'{rs} | {rt}')
        if o == 'xor':
            return setr(i.rd, f'{rs} ^ {rt}')
        if o == 'nor':
            return setr(i.rd, f'~({rs} | {rt})')
        if o == 'slt':
            return setr(i.rd, f'(s32){rs} < (s32){rt}')
        if o == 'sltu':
            return setr(i.rd, f'{rs} < {rt}')
        if o in ('addi', 'addiu') and imm == 0 and i.rt:
            return setr(i.rt, f'{rs} + {imm}') + f' pgxp_mov({i.rt}, {i.rs});'
        if o in ('addi', 'addiu'):
            return setr(i.rt, f'{rs} + {imm}')
        if o == 'slti':
            if self.ovl_tag == '' and i.addr in WS_PATCHES:
                WS_EMITTED.add(i.addr)
                return setr(i.rt, f'(s32){rs} < WS_IMM({imm}, {WS_PATCHES[i.addr]})')
            return setr(i.rt, f'(s32){rs} < {imm}')
        if o == 'sltiu':
            return setr(i.rt, f'{rs} < (u32){imm}')
        if o == 'andi':
            return setr(i.rt, f'{rs} & 0x{imm:x}')
        if o == 'ori':
            return setr(i.rt, f'{rs} | 0x{imm:x}')
        if o == 'xori':
            return setr(i.rt, f'{rs} ^ 0x{imm:x}')
        if o == 'lui':
            return setr(i.rt, f'0x{(imm << 16) & 0xffffffff:08x}')
        ea = f'{rs} + {imm}' if imm else rs
        if o == 'lb':
            return setr(i.rt, f'(u32)(s8)mem_r8({ea})')
        if o == 'lbu':
            return setr(i.rt, f'mem_r8({ea})')
        if o == 'lh':
            return setr(i.rt, f'(u32)(s16)mem_r16({ea})')
        if o == 'lhu':
            return setr(i.rt, f'mem_r16({ea})')
        if o == 'lw':
            if i.rt == 0:
                return setr(i.rt, f'mem_r32({ea})')
            return f'{{ u32 a_ = {ea}; R{i.rt} = mem_r32(a_); pgxp_lw({i.rt}, a_); }}'
        if o == 'lwl':
            return setr(i.rt, f'op_lwl({rt}, {ea})') if i.rt else ''
        if o == 'lwr':
            return setr(i.rt, f'op_lwr({rt}, {ea})') if i.rt else ''
        if o == 'sb':
            return f'mem_w8({ea}, {rt});'
        if o == 'sh':
            return f'mem_w16({ea}, {rt});'
        if o == 'sw':
            return f'{{ u32 a_ = {ea}; pgxp_sw(a_, {i.rt}); mem_w32(a_, {rt}); }}'
        if o == 'swl':
            return f'op_swl({rt}, {ea});'
        if o == 'swr':
            return f'op_swr({rt}, {ea});'
        if o == 'lwc2':
            return f'{{ u32 a_ = {ea}; gte_write_data({i.rt}, mem_r32(a_)); pgxp_lwc2({i.rt}, a_); }}'
        if o == 'swc2':
            return f'{{ u32 a_ = {ea}; pgxp_swc2(a_, {i.rt}); mem_w32(a_, gte_read_data({i.rt})); }}'
        if o == 'mfc2':
            if i.rt == 0:
                return setr(i.rt, f'gte_read_data({i.rd})')
            return setr(i.rt, f'gte_read_data({i.rd})') + f' pgxp_mfc2({i.rt}, {i.rd});'
        if o == 'cfc2':
            return setr(i.rt, f'gte_read_ctrl({i.rd})')
        if o == 'mtc2':
            return f'gte_write_data({i.rd}, {rt}); pgxp_mtc2({i.rd}, {i.rt});'
        if o == 'ctc2':
            return f'gte_write_ctrl({i.rd}, {rt});'
        if o == 'cop2':
            return f'gte_command(0x{i.word:08x});'
        if o == 'mfc0':
            return setr(i.rt, f'rt_mfc0({i.rd})')
        if o == 'mtc0':
            return f'rt_mtc0({i.rd}, {rt});'
        if o == 'rfe':
            return 'rt_rfe();'
        if o == 'syscall':
            return f'CYC_FLUSH(); rt_syscall(0x{i.addr:08x});'
        if o == 'break':
            return f'rt_break(0x{i.addr:08x}, 0x{(i.word >> 6) & 0xfffff:x});'
        if o == 'invalid':
            return f'rt_invalid(0x{i.addr:08x}, 0x{i.word:08x}); FN_RET();'
        raise ValueError(f'unhandled {o}')

    def call_expr(self, target):
        if target in self.image_funcs:
            return f'{self.name_of(target)}();'
        return f'rt_call(0x{target:08x});'

    def cond(self, i):
        rs, rt = R(i.rs), R(i.rt)
        return {
            'beq': f'{rs} == {rt}', 'bne': f'{rs} != {rt}',
            'blez': f'(s32){rs} <= 0', 'bgtz': f'(s32){rs} > 0',
            'bltz': f'(s32){rs} < 0', 'bgez': f'(s32){rs} >= 0',
            'bltzal': f'(s32){rs} < 0', 'bgezal': f'(s32){rs} >= 0',
        }[i.op]

    def goto(self, src, t):
        # backward edges are where long-running loops live: poll events there
        if t <= src:
            return f'{{ EVT_CHECK(); goto L_{t:08x}; }}'
        return f'goto L_{t:08x};'

    def emit(self):
        f = self.f
        addrs = sorted(f.insns)
        name = self.name_of(f.entry)
        self.w(f'void {name}(void) {{')
        self.w(f'    FN_ENTER(0x{f.entry:08x});')
        self.w('    EVT_CHECK();')
        skip = set()
        prev_end = None
        for idx, a in enumerate(addrs):
            if a in skip:
                continue
            i = f.insns[a]
            # Labels are only needed for goto targets; every target address is
            # in f.labels before emission reaches it (explicit fallthrough
            # gotos always point forward).
            if a in f.labels:
                self.flush_cycles()
                self.w(f'L_{a:08x}:;')
            self.pending += 1
            if not i.has_delay:
                s = self.simple(i)
                if s:
                    self.w(f'    {s}')
                if i.op == 'invalid':
                    prev_end = None
                    continue
                prev_end = a + 4
                # if the next emitted address isn't a+4, jump explicitly
                nxt = addrs[idx + 1] if idx + 1 < len(addrs) else None
                if nxt != a + 4:
                    if (a + 4) in f.insns:
                        self.flush_cycles()
                        self.w(f'    goto L_{a + 4:08x};')
                        f.labels.add(a + 4)
                    else:
                        self.flush_cycles()
                        self.w(f'    rt_unreached(0x{a + 4:08x}); FN_RET();')
                    prev_end = None
                continue
            # control transfer with delay slot
            d = f.insns.get(a + 4)
            ds = self.simple(d) if d is not None else ''
            if d is not None and d.has_delay:
                ds = f'/* branch in delay slot: {d} */'
            self.pending += 1
            o = i.op
            self.w(f'    /* {a:08x}: {i} */')
            if o == 'jal':
                self.flush_cycles()
                self.w(f'    R31 = 0x{a + 8:08x};')
                if ds:
                    self.w(f'    {ds}')
                self.w(f'    {self.call_expr(i.target)}')
                after = a + 8
            elif o == 'jalr':
                self.flush_cycles()
                self.w(f'    {{ u32 t = {R(i.rs)};')
                if i.rd:
                    self.w(f'      R{i.rd} = 0x{a + 8:08x};')
                if ds:
                    self.w(f'      {ds}')
                self.w('      rt_call(t); }')
                after = a + 8
            elif o in ('bltzal', 'bgezal'):
                self.flush_cycles()
                self.w(f'    {{ int c = {self.cond(i)}; R31 = 0x{a + 8:08x};')
                if ds:
                    self.w(f'      {ds}')
                self.w(f'      if (c) {self.call_expr(i.target)} }}')
                after = a + 8
            elif o == 'j':
                self.flush_cycles()
                if ds:
                    self.w(f'    {ds}')
                if i.target in f.labels:
                    self.w(f'    {self.goto(a, i.target)}')
                else:
                    self.w(f'    {self.call_expr(i.target)} FN_RET();')
                after = None
            elif o == 'jr':
                self.flush_cycles()
                if i.rs == 31:
                    if ds:
                        self.w(f'    {ds}')
                    self.w('    FN_RET();')
                elif a in f.jtables:
                    self.w(f'    {{ u32 t = {R(i.rs)};')
                    if ds:
                        self.w(f'      {ds}')
                    self.w('      switch (t) {')
                    for t in sorted(set(f.jtables[a])):
                        self.w(f'      case 0x{t:08x}: {self.goto(a, t)}')
                    self.w('      }')
                    self.w('      rt_jump(t); FN_RET(); }')
                else:
                    self.w(f'    {{ u32 t = {R(i.rs)};')
                    if ds:
                        self.w(f'      {ds}')
                    self.w('      rt_jump(t); FN_RET(); }')
                after = None
            else:
                self.flush_cycles()
                uncond = (o == 'beq' and i.rs == 0 and i.rt == 0) or (o == 'bgez' and i.rs == 0)
                if uncond:
                    if ds:
                        self.w(f'    {ds}')
                    self.w(f'    {self.goto(a, i.target)}')
                    after = None
                else:
                    self.w(f'    {{ int c = {self.cond(i)};')
                    if ds:
                        self.w(f'      {ds}')
                    self.w(f'      if (c) {self.goto(a, i.target)} }}')
                    after = a + 8
            # delay slot consumed; skip it unless something jumps into it
            if (a + 4) not in f.labels:
                skip.add(a + 4)
                nxt_list = [x for x in addrs[idx + 1:] if x not in skip]
                nxt = nxt_list[0] if nxt_list else None
            else:
                nxt = a + 4
            if after is not None:
                if nxt != after:
                    if after in f.insns:
                        self.w(f'    goto L_{after:08x};')
                        f.labels.add(after)
                    else:
                        self.w(f'    rt_unreached(0x{after:08x}); FN_RET();')
                prev_end = after if nxt == after else None
            else:
                prev_end = None
                if nxt == a + 4:
                    # delay-slot label reached only by jumps
                    pass
        self.flush_cycles()
        self.w('    rt_unreached(0); FN_RET();' if prev_end is not None else '')
        self.w('}')
        return self.out
