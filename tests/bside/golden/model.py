# An independent model of the patch program engine written from docs/architecture.md 8.6 (and the readings in
# docs/decisions.md "S2c"): it parses the table TEXT itself and simulates it literally, tick by tick. Used to derive
# golden write traces. Authentic resolution, 985248 Hz clock.
import math, re

CLOCK = 985248
NOTE = {'c': 0, 'd': 2, 'e': 4, 'f': 5, 'g': 7, 'a': 9, 'b': 11}


def note_number(tok):
    t = tok.lower()
    if t.isdigit():
        return int(t)
    n = NOTE[t[0]]
    i = 1
    if t[i] == '#':
        n += 1; i += 1
    elif t[i] == 'b':
        n -= 1; i += 1
    return (int(t[i:]) + 1) * 12 + n


def freq_reg(m):
    hz = 440.0 * 2 ** ((m - 69) / 12.0)
    r = hz * 2 ** 24 / CLOCK
    return min(max(int(math.floor(r + 0.5)), 1), 65535)


def rnd(v):
    return int(math.floor(v + 0.5))


BITS = {'tri': 0x10, 'saw': 0x20, 'pulse': 0x40, 'noise': 0x80, 'gate': 1, 'sync': 2, 'ring': 4, 'test': 8}
MODES = {'off': 0, 'lp': 1, 'bp': 2, 'hp': 4, 'lp+bp': 3, 'bp+hp': 6, 'lp+hp': 5, 'lp+bp+hp': 7}


def bits_of(tok):
    if tok == 'none':
        return 0
    v = 0
    for p in tok.split('+'):
        v |= BITS[p]
    return v


def parse_pitch(tok):
    if tok == '~':
        return None
    if tok[0] == '=':
        return ('abs', note_number(tok[1:]))
    return ('rel', float(tok))


def parse_table(text):
    lanes = {}
    restart = dict(mode='off', lead=2, ad=0, sr=0, gateoff=None, firstwave=None)
    section = None
    pending_labels = []
    lines = text.split('\n')
    assert lines[0].strip() == 'bside-table 1'
    for raw in lines[1:]:
        line = re.sub(r'(?<!=[a-gA-G])#.*$', '', raw).strip()
        if not line:
            continue
        m = re.fullmatch(r'\[(\w+)\]', line)
        if m:
            section = m.group(1)
            if section != 'restart':
                lanes[section] = dict(rows=[], labels={})
            continue
        if section == 'restart':
            k, v = line.split(None, 1)
            if k == 'mode': restart['mode'] = v
            elif k == 'lead': restart['lead'] = int(v)
            elif k == 'ad': restart['ad'] = int(v, 16)
            elif k == 'sr': restart['sr'] = int(v, 16)
            elif k == 'gateoff': restart['gateoff'] = int(v)
            elif k == 'firstwave': restart['firstwave'] = bits_of(v)
            continue
        lane = lanes[section]
        if re.fullmatch(r'[a-z][a-z0-9_]*:', line):
            lane['labels'][line[:-1]] = len(lane['rows'])
            continue
        delay = None
        m = re.search(r'\s+ticks\s+(\d+)$', line)
        if m:
            delay = int(m.group(1)); line = line[:m.start()]
        cmds = []
        for c in line.split(','):
            w = c.split()
            cmds.append(w)
        last = cmds[-1][0]
        if delay is None:
            delay = 0 if last in ('jump', 'repeat') else 1
        lane['rows'].append(dict(cmds=cmds, delay=delay))
    return lanes, restart


class Voice:
    def __init__(self, table_text, host, period, out):
        self.lanes, self.restart = parse_table(table_text)
        self.host = host
        self.period = period
        self.out = out          # list of (cycle, kind, a, b)
        self.sh = dict(freq=[0] * 3, pw=[0] * 3, ctrl=[0] * 3, ad=[0] * 3, sr=[0] * 3, cutoff=0, rr=0, mv=0)
        self.running = False
        self.in_restart = False
        self.restart_state = None
        self.restart_cycle = 0

    # -- low level emission, in register terms
    def w(self, cycle, reg, val):
        self.out.append((cycle, 'w', reg, val))

    def e_freq(self, c, n, f):
        self.w(c, 7 * n, f & 255); self.w(c, 7 * n + 1, f >> 8); self.sh['freq'][n] = f

    def e_pw(self, c, n, p):
        self.w(c, 7 * n + 2, p & 255); self.w(c, 7 * n + 3, p >> 8); self.sh['pw'][n] = p

    def e_ctrl(self, c, n, v):
        self.w(c, 7 * n + 4, v); self.sh['ctrl'][n] = v

    def e_env(self, c, n, ad, sr):
        self.w(c, 7 * n + 5, ad); self.w(c, 7 * n + 6, sr); self.sh['ad'][n] = ad; self.sh['sr'][n] = sr

    def e_cut(self, c, v):
        self.w(c, 0x15, v & 7); self.w(c, 0x16, v >> 3); self.sh['cutoff'] = v

    def e_rr(self, c, v):
        self.w(c, 0x17, v); self.sh['rr'] = v

    def e_mv(self, c, v):
        self.w(c, 0x18, v); self.sh['mv'] = v

    # -- effective values
    def host_osc(self, n): return self.host['osc'][n]

    def active(self, n):
        return self.host_osc(n)['enabled'] or ('wave%d' % (n + 1)) in self.lanes and len(self.lanes['wave%d' % (n + 1)]['rows']) > 0

    def eff_freq(self, n):
        o = self.own['pitch'][n]
        h = self.host_osc(n)['sem']
        if o is None:
            return freq_reg(h)
        kind, v = o
        return freq_reg(v if kind == 'abs' else h + v)

    def eff_pw(self, n):
        base = self.own['pw'][n] if self.own['pw'][n] is not None else min(max(self.host_osc(n)['pw'], 0), 4095)
        return rnd(min(max(base + self.host['macro_pw'], 0), 4095))

    def eff_ctrl(self, n):
        h = self.host_osc(n)
        if self.firstwave_active and self.active(n):
            return (self.restart['firstwave'] & ~1) | (1 if self.held else 0)
        gate = self.own['gate'][n] if self.own['gate'][n] is not None else (self.held and h['enabled'])
        if self.own['ctrl'][n] is not None:
            base = self.own['ctrl'][n]
        elif h['enabled']:
            base = (h['bits'] & 0xf0) | (2 if h['sync'] else 0) | (4 if h['ring'] else 0)
        else:
            base = 0
        return base | (1 if gate else 0)

    def eff_env(self, n):
        if self.own['env'][n] is not None:
            return self.own['env'][n]
        h = self.host_osc(n)
        return (h['ad'], h['sr'])

    def eff_cut(self):
        base = self.own['cutoff'] if self.own['cutoff'] is not None else min(max(self.host['filter']['cutoff'], 0), 2047)
        return rnd(min(max(base + self.host['macro_cut'], 0), 2047))

    def eff_rr(self):
        f = self.host['filter']
        res = self.own['res'] if self.own['res'] is not None else f['res']
        route = self.own['route'] if self.own['route'] is not None else f['route']
        return (res << 4) | route

    def eff_mv(self):
        f = self.host['filter']
        mode = self.own['mode'] if self.own['mode'] is not None else f['mode']
        vol = self.own['vol'] if self.own['vol'] is not None else f['vol']
        return (0x80 if f['osc3off'] else 0) | (mode << 4) | vol

    # -- sync of host-driven registers
    def sync_wave(self, c, n):
        if self.active(n):
            f = self.eff_freq(n)
            if f != self.sh['freq'][n]:
                self.e_freq(c, n, f)
            ad, sr = self.eff_env(n)
            if (ad, sr) != (self.sh['ad'][n], self.sh['sr'][n]):
                self.e_env(c, n, ad, sr)
        v = self.eff_ctrl(n)
        if v != self.sh['ctrl'][n]:
            self.e_ctrl(c, n, v)

    def sync_pulse(self, c, n):
        if self.active(n) or self.own['pw'][n] is not None:
            p = self.eff_pw(n)
            if p != self.sh['pw'][n]:
                self.e_pw(c, n, p)

    def sync_filter(self, c):
        v = self.eff_cut()
        if v != self.sh['cutoff']: self.e_cut(c, v)
        v = self.eff_rr()
        if v != self.sh['rr']: self.e_rr(c, v)
        v = self.eff_mv()
        if v != self.sh['mv']: self.e_mv(c, v)

    def sync(self, c):
        for n in range(3): self.sync_wave(c, n)
        for n in range(3): self.sync_pulse(c, n)
        self.sync_filter(c)

    # -- restart
    def do_restart(self, c, r):
        for n in range(3):
            self.e_env(c, n, r['ad'], r['sr'])
            self.e_ctrl(c, n, (self.sh['ctrl'][n] & ~1) | (8 if r['mode'] == 'phase' else 0))

    def enter_idle(self, c, policy):
        self.running = False
        self.held = False
        if policy == 'authentic' or self.restart['mode'] == 'off':
            self.in_restart = False
            return
        self.do_restart(c, self.restart)
        self.in_restart = True
        self.restart_cycle = c
        self.restart_state = dict(self.restart)

    # -- notes
    def note_on(self, c, policy, stolen):
        self.own = dict(pitch=[None] * 3, ctrl=[None] * 3, gate=[None] * 3, env=[None] * 3, pw=[None] * 3,
                        cutoff=None, res=None, route=None, mode=None, vol=None)
        self.state = {}
        for name, lane in self.lanes.items():
            self.state[name] = dict(pc=0, wait=0, active=len(lane['rows']) > 0, mode=None, left=0, rep={}, lo=0, hi=0, step=0, dir=1)
        self.held = True
        self.running = True
        self.tick = 0
        self.next_tick = c
        self.firstwave_active = False
        mode_on = self.restart['mode'] != 'off'
        if stolen and policy != 'authentic':
            self.out.append((c, 'reset', 0, 0))
            self.sh = dict(freq=[0] * 3, pw=[0] * 3, ctrl=[0] * 3, ad=[0] * 3, sr=[0] * 3, cutoff=0, rr=0, mv=0)
            self.in_restart = False
            if mode_on:
                self.do_restart(c, self.restart)
                self.in_restart = True
                self.restart_cycle = c
                self.restart_state = dict(self.restart)
        if self.in_restart and self.restart_state != self.restart:
            self.in_restart = False
        if policy == 'clean':
            full = self.in_restart and c - self.restart_cycle >= self.restart['lead'] * self.period
            if not full:
                self.out.append((c, 'clear', 0, 0))
        if policy != 'authentic' and not mode_on:
            for n in range(3):
                if self.sh['ctrl'][n] & 1:
                    self.e_ctrl(c, n, self.sh['ctrl'][n] & ~1)
        self.run_tick()

    def run_tick(self):
        c = self.next_tick
        mode_on = self.restart['mode'] != 'off'
        if self.tick == 0:
            if mode_on and not self.in_restart:
                self.do_restart(c, self.restart)
            self.in_restart = False
            self.firstwave_active = mode_on and self.restart['firstwave'] is not None
            if self.firstwave_active:
                for n in range(3):
                    if ('wave%d' % (n + 1)) in self.state:
                        self.state['wave%d' % (n + 1)]['wait'] = 1
        elif self.tick == 1:
            self.firstwave_active = False
        names = ['wave1', 'wave2', 'wave3', 'pulse1', 'pulse2', 'pulse3', 'filter']
        for i, name in enumerate(names):
            self.tick_lane(c, name)
            if i < 3: self.sync_wave(c, i)
            elif i < 6: self.sync_pulse(c, i - 3)
            else: self.sync_filter(c)
        self.tick += 1
        self.next_tick = c + self.period

    def note_off(self, c):
        if not self.running:
            return
        self.held = False
        names = ['wave1', 'wave2', 'wave3', 'pulse1', 'pulse2', 'pulse3', 'filter']
        for i, name in enumerate(names):
            lane = self.lanes.get(name)
            if lane and lane['rows']:
                if 'release' in lane['labels']:
                    st = self.state[name]
                    st.update(pc=lane['labels']['release'], wait=0, mode=None, active=True)
                    self.burst(c, name)
                elif i < 3:
                    self.own['gate'][i] = False
                    self.e_ctrl(c, i, self.eff_ctrl(i))
            if i < 3: self.sync_wave(c, i)
            elif i < 6: self.sync_pulse(c, i - 3)
            else: self.sync_filter(c)

    # -- lanes
    def tick_lane(self, c, name):
        if name not in self.state: return
        st = self.state[name]
        lane = self.lanes[name]
        if not st['active']: return
        if st['wait'] > 0:
            st['wait'] -= 1
            return
        if st['mode'] == 'sweep':
            self.sweep_step(c, name, lane['rows'][st['pc']]['cmds'][-1])
            st['left'] -= 1
            if st['left'] == 0:
                st['mode'] = None
                st['pc'] += 1
            return
        if st['mode'] == 'pp':
            self.pp_step(c, name)
            return
        self.burst(c, name)

    def burst(self, c, name):
        st = self.state[name]
        lane = self.lanes[name]
        for _ in range(128):
            if st['pc'] >= len(lane['rows']):
                st['active'] = False
                return
            r = st['pc']
            row = lane['rows'][r]
            nxt = r + 1
            for cmd in row['cmds']:
                op = cmd[0]
                if op == 'jump':
                    nxt = lane['labels'][cmd[1]]
                elif op == 'repeat':
                    cnt = st['rep'].get(r, 0)
                    if cnt < int(cmd[2]):
                        st['rep'][r] = cnt + 1
                        nxt = lane['labels'][cmd[1]]
                    else:
                        st['rep'][r] = 0
                elif op == 'end':
                    st['active'] = False
                    return
                elif op == 'sweep':
                    st['mode'] = 'sweep'
                    st['left'] = int(cmd[2][1:])
                    self.sweep_step(c, name, cmd)
                    st['left'] -= 1
                    if st['left'] == 0:
                        st['mode'] = None
                        st['pc'] = nxt
                    else:
                        st['pc'] = r
                    return
                elif op == 'pingpong':
                    st['mode'] = 'pp'
                    st['lo'], st['hi'], st['step'] = int(cmd[1]), int(cmd[2]), int(cmd[3])
                    st['dir'] = 1
                    st['pc'] = r
                    self.pp_step(c, name)
                    return
                else:
                    self.command(c, name, cmd)
            st['pc'] = nxt
            if row['delay'] >= 1:
                st['wait'] = row['delay'] - 1
                return

    def idx(self, name):
        return int(name[-1]) - 1 if name[-1].isdigit() else None

    def command(self, c, name, cmd):
        op = cmd[0]
        n = self.idx(name)
        if op in ('wave', 'pitch'):
            if op == 'wave':
                b = bits_of(cmd[1]); ptok = cmd[2] if len(cmd) > 2 else '~'
            else:
                ptok = cmd[1]
            p = parse_pitch(ptok)
            if p is not None:
                self.own['pitch'][n] = p
                self.e_freq(c, n, self.eff_freq(n))
            if op == 'wave':
                self.own['ctrl'][n] = b & ~1
                self.own['gate'][n] = bool(b & 1)
                self.e_ctrl(c, n, self.eff_ctrl(n))
        elif op == 'gate':
            self.own['gate'][n] = cmd[1] == 'on'
            self.e_ctrl(c, n, self.eff_ctrl(n))
        elif op == 'adsr':
            a, d, s, r = [int(x, 16) for x in cmd[1:5]]
            self.own['env'][n] = (a << 4 | d, s << 4 | r)
            self.e_env(c, n, *self.own['env'][n])
        elif op == 'set':
            t = cmd[1]
            if t.endswith('%'):
                h = round(float(t[:-1]) * 100)
                v = min((h * 4096 + 5000) // 10000, 4095)
            else:
                v = int(t)
            self.own['pw'][n] = v
            self.e_pw(c, n, self.eff_pw(n))
        elif op == 'cutoff':
            self.own['cutoff'] = int(cmd[1]); self.e_cut(c, self.eff_cut())
        elif op == 'res':
            self.own['res'] = int(cmd[1]); self.e_rr(c, self.eff_rr())
        elif op == 'route':
            r = 0
            if cmd[1] != 'none':
                for ch in cmd[1]: r |= 1 << (int(ch) - 1)
            self.own['route'] = r; self.e_rr(c, self.eff_rr())
        elif op == 'volume':
            self.own['vol'] = int(cmd[1]); self.e_mv(c, self.eff_mv())
        elif op == 'mode':
            self.own['mode'] = MODES[cmd[1]]; self.e_mv(c, self.eff_mv())
        else:
            raise Exception(op)

    def sweep_step(self, c, name, cmd):
        d = int(cmd[1]) * self.host['depth']
        if name == 'filter':
            if self.own['cutoff'] is None:
                self.own['cutoff'] = min(max(self.host['filter']['cutoff'], 0), 2047)
            self.own['cutoff'] = min(max(self.own['cutoff'] + d, 0), 2047)
            self.e_cut(c, self.eff_cut())
        else:
            n = self.idx(name)
            if self.own['pw'][n] is None:
                self.own['pw'][n] = min(max(self.host_osc(n)['pw'], 0), 4095)
            self.own['pw'][n] = min(max(self.own['pw'][n] + d, 0), 4095)
            self.e_pw(c, n, self.eff_pw(n))

    def pp_step(self, c, name):
        st = self.state[name]
        n = self.idx(name)
        if self.own['pw'][n] is None:
            self.own['pw'][n] = min(max(self.host_osc(n)['pw'], 0), 4095)
        v = min(max(self.own['pw'][n], st['lo']), st['hi']) + st['dir'] * st['step'] * self.host['depth']
        if v >= st['hi']:
            v = st['hi']; st['dir'] = -1
        elif v <= st['lo']:
            v = st['lo']; st['dir'] = 1
        self.own['pw'][n] = v
        self.e_pw(c, n, self.eff_pw(n))


def default_host():
    def osc(**kw):
        d = dict(enabled=False, bits=0, sem=60.0, pw=2048, ad=0, sr=0xf6, sync=False, ring=False)
        d.update(kw)
        return d
    return dict(osc=[osc(enabled=True, bits=0x40), osc(), osc()],
                filter=dict(cutoff=1024, res=0, route=7, mode=1, osc3off=False, vol=15),
                macro_pw=0, macro_cut=0, depth=1.0)


def run(table_text, host, period, events, end):
    out = []
    v = Voice(table_text, host, period, out)
    pending = sorted(range(len(events)), key=lambda i: (events[i][0], i))
    for i in pending:
        cyc, kind, args = events[i]
        while v.running and v.next_tick <= cyc:
            v.run_tick()
        if kind == 'on': v.note_on(cyc, args['policy'], args.get('stolen', False))
        elif kind == 'off': v.note_off(cyc)
        elif kind == 'idle': v.enter_idle(cyc, args['policy'])
    while v.running and v.next_tick <= end:
        v.run_tick()
    return out


def to_text(out, end, header_comment):
    lines = ['# ' + header_comment, 'bside-trace 1', 'clock %d' % CLOCK, 'model B']
    for (c, kind, a, b) in out:
        if kind == 'w': lines.append('%d %02x %02x' % (c, a, b))
        elif kind == 'clear': lines.append('# %d direct: rate counters and dividers cleared' % c)
        elif kind == 'reset': lines.append('# %d direct: chip reset to power-on' % c)
    lines.append('end %d' % end)
    return '\n'.join(lines) + '\n'
