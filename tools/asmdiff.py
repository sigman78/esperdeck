"""Compile render_scan.c at two revisions with IDENTICAL flags and compare the
emitted scan_band_8x16: instruction mix, register pressure, stack traffic."""
import json, os, re, shlex, subprocess, sys, collections

BIN = 'C:/Espressif/tools/xtensa-esp-elf/esp-14.2.0_20251107/xtensa-esp-elf/bin'
GCC = os.path.join(BIN, 'xtensa-esp32s3-elf-gcc.exe')
OBJDUMP = os.path.join(BIN, 'xtensa-esp32s3-elf-objdump.exe')
REPO = 'D:/esp32/unbreezy/cyberdeck'
SCRATCH = os.path.dirname(os.path.abspath(__file__))
FILES = ['components/display/render_scan.c',
         'components/display/render_scan.inc',
         'components/display/render_internal.h']

cc = json.load(open(os.path.join(REPO, 'build/compile_commands.json')))
entry = next(e for e in cc
             if os.path.basename(e['file'].replace('\\', '/')) == 'render_scan.c')
argv = shlex.split(entry['command'].replace('\\', '/'))


def build(rev, tag):
    d = os.path.join(SCRATCH, 'asm_' + tag).replace('\\', '/')
    os.makedirs(d, exist_ok=True)
    for f in FILES:
        blob = subprocess.run(['git', '-C', REPO, 'show', '%s:%s' % (rev, f)],
                              capture_output=True)
        if blob.returncode:
            sys.exit('git show failed for %s @ %s' % (f, rev))
        open(os.path.join(d, os.path.basename(f)), 'wb').write(blob.stdout)

    obj = os.path.join(d, 'render_scan.o').replace('\\', '/')
    a = [GCC if t.endswith('gcc') or t.endswith('gcc.exe') else t for t in argv]
    for i, t in enumerate(a):
        if t == '-o':
            a[i + 1] = obj
        elif t.endswith('render_scan.c'):
            a[i] = d + '/render_scan.c'
    r = subprocess.run(a, cwd=entry['directory'], capture_output=True, text=True)
    if r.returncode:
        sys.exit('compile failed @ %s:\n%s' % (rev, r.stderr[:1500]))
    dis = subprocess.run([OBJDUMP, '-d', '--no-show-raw-insn', obj],
                         capture_output=True, text=True).stdout
    return dis


def analyse(dis, fn):
    body, on = [], False
    for line in dis.splitlines():
        if re.search(r'<%s>:\s*$' % fn, line):
            on = True
            continue
        if on:
            if line.strip() == '':
                break
            m = re.match(r'^\s*[0-9a-f]+:\s+(\S+)\s*(.*)$', line)
            if m:
                body.append((m.group(1), m.group(2)))
    ops = collections.Counter(op for op, _ in body)
    regs = set()
    for _, args in body:
        regs.update(re.findall(r'\ba(\d{1,2})\b', args))
    spills = sum(1 for op, args in body
                 if op.startswith(('s32i', 'l32i')) and re.search(r'\ba1\b', args))
    return body, ops, regs, spills


print('compiling render_scan.c at both revisions with identical flags...\n')
before = build('d57bb76', 'before')
after = build('HEAD', 'after')

rows = []
for tag, dis in (('before (scan_gpair)', before), ('after  (pair LUT)', after)):
    body, ops, regs, spills = analyse(dis, 'render_scan_band')
    rows.append((tag, body, ops, regs, spills))

print('%-22s %8s %8s %10s %8s' % ('render_scan_band', 'instrs', 'regs', 'stack a1', 'loops'))
for tag, body, ops, regs, spills in rows:
    nloop = sum(v for k, v in ops.items() if k.startswith('loop'))
    print('%-22s %8d %8d %10d %8d' % (tag, len(body), len(regs), spills, nloop))

print('\nper-opcode delta (after - before), biggest movers:')
b_ops, a_ops = rows[0][2], rows[1][2]
delta = {k: a_ops.get(k, 0) - b_ops.get(k, 0)
         for k in set(b_ops) | set(a_ops)}
for k in sorted(delta, key=lambda k: -abs(delta[k]))[:14]:
    if delta[k]:
        print('  %-12s %+5d   (%d -> %d)' % (k, delta[k], b_ops.get(k, 0),
                                             a_ops.get(k, 0)))
