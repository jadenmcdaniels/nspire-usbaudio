#!/bin/bash
# No Ndless syscall reachable from the OS USB task (brief 2.3, 4.1 item 2).
# Calculator objects of usbaudio.tns (nspire-gcc):
#  1. no instruction newer than ARMv5TE (the CX's ARM926EJ-S; the CX II's
#     CPU is ARMv5 too): uxt*, rev, ldrex, movw/movt, ...;
#  2. no floating point: no VFP instruction and no soft-float helper call
#     (the CPU has no FPU; the audio path is integer);
#  3. no Ndless syscall reachable from the OS's USB callbacks (match,
#     attach, detach, the isochronous completion): the walk below is
#     nspire-usbpad's tests/check_no_svc.sh, copied, with this program's roots.
#   tests/check_calc.sh obj.o ...   (make check passes the objects it links)
set -eu
cd "$(dirname "$0")/.."
command -v arm-none-eabi-objdump >/dev/null || . "${NSPIRE_ENV:?put the Ndless toolchain on PATH, or set NSPIRE_ENV to a script that does}"
OBJDUMP=${OBJDUMP:-arm-none-eabi-objdump}
NM=${NM:-arm-none-eabi-nm}
V6='(uxt|sxt|rev|ldrex|strex|pkh|usat|ssat|smlad|smlald|smlsd|smlsld|smuad|smusd|movw|movt|clrex|wfi|wfe|sev|yield|dmb|dsb|isb|sdiv|udiv)[a-z0-9.]*'
FP='^(v[a-z]+(\.f(32|64)|\.[us]32\.f|\.f[0-9]+\.[us]32)?|cdp2?|ldc2?l?|stc2?l?|mcrr2?|mrrc2?)$'
fail=0
for o in "$@"; do
    case "$o" in *usbshim.o) ;; *)
    v=$($OBJDUMP -d --no-show-raw-insn "$o" | awk -F'\t' 'NF>=2 {print $2}' | grep -E "^$V6\b" | sort -u | tr '\n' ' ' || true)
    [ -z "$v" ] || { echo "check_calc: $o has ARMv6+ instructions: $v"; fail=1; }
    f=$($OBJDUMP -d --no-show-raw-insn "$o" | awk -F'\t' 'NF>=2 {print $2}' | awk '{print $1}' | grep -E "$FP" | sort -u | tr '\n' ' ' || true)
    [ -z "$f" ] || { echo "check_calc: $o has floating point / coprocessor instructions: $f"; fail=1; }
    s=$($NM -u "$o" | awk '{print $2}' | grep -E '^__aeabi_(f|d|[ul]?[il]2[fd]|[fd]2)|^__(add|sub|mul|div|neg|cmp|eq|ne|lt|le|gt|ge|un)(s|d)f[23]|^__(float|fix|extend|trunc)' | tr '\n' ' ' || true)
    [ -z "$s" ] || { echo "check_calc: $o calls soft-float helpers: $s"; fail=1; }
    ;; esac
done
[ $fail -eq 0 ] && echo "check_calc: ARMv5 only, no floating point in $(($#-1)) objects"
ROOTS="ua_os.o:os_cb_match ua_os.o:os_cb_attach ua_os.o:os_cb_detach ua_os.o:cb_isoc"
check() {
    # shellcheck disable=SC2086
    python3 -I - "$OBJDUMP" "$NM" "$1" $ROOTS ${EXTRA_ROOTS:-} <<'EOF'
import glob, os, re, subprocess, sys
objdump, nm, objdir, roots = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
ALLOW = re.compile(r'^(memcpy|memset|memmove|memcmp)$|^__aeabi_|^__gnu_')
CALLS = ('R_ARM_CALL', 'R_ARM_JUMP24', 'R_ARM_PC24', 'R_ARM_PLT32')
COND = r'(eq|ne|cs|cc|hs|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)?'
if os.environ.get('CHECK_OBJS'):
    objs = sorted(os.environ['CHECK_OBJS'].split('\n'))
else:
    objs = sorted(glob.glob(os.path.join(objdir, '*.o')))
if not objs:
    sys.exit('check_no_svc: no objects in %s (run make first)' % objdir)

def run(*a):
    return subprocess.run(a, check=True, capture_output=True, text=True).stdout

funcs = {}      # (obj, name) -> {'svc': [...], 'calls': set, 'refs': set, 'indirect': n}
data = {}       # obj -> data symbols it defines
glob_fn, glob_data, class_roots = {}, set(), []
for o in objs:
    data[o] = set()
    for line in run(nm, o).splitlines():
        p = line.split()
        if len(p) != 3:
            continue
        t, s = p[1], p[2]
        if t in 'Tt':
            funcs[(o, s)] = {'svc': [], 'calls': set(), 'refs': set(), 'indirect': 0}
            if t == 'T':
                glob_fn[s] = o
        elif t in 'BbDdRrGgSsC':
            data[o].add(s)
            if t.isupper():
                glob_data.add(s)
    cur = None
    for line in run(objdump, '-dr', '--no-show-raw-insn', o).splitlines():
        m = re.match(r'^[0-9a-f]+ <(.+)>:$', line)
        if m:
            cur = funcs.get((o, m.group(1)))
            continue
        if cur is None:
            continue
        m = re.match(r'^\s+[0-9a-f]+: (R_ARM_\w+)\s+(\S+)', line)
        if m:
            sym = re.sub(r'[+-]0x[0-9a-f]+$', '', m.group(2))
            if sym.startswith('.text.'):
                sym = sym[6:]
            elif sym.startswith('.'):
                continue                    # a data section (strings, tables)
            (cur['calls'] if m.group(1) in CALLS else cur['refs']).add(sym)
            continue
        m = re.match(r'^\s+[0-9a-f]+:\s+(\S+)\s*(.*)$', line)
        if not m:
            continue
        op, args = m.group(1), m.group(2)
        if re.match(r'^(svc|swi)', op):
            cur['svc'].append(line.strip())
        elif (re.match(r'^(blx|bx)' + COND + '$', op) and re.match(r'^(r\d|ip)', args)) or \
                (re.match(r'^(ldr|mov)' + COND + '$', op) and args.startswith('pc,')):
            cur['indirect'] += 1            # (bx lr / pop {.., pc} are returns)
        elif re.match(r'^(bl|b)' + COND + '$', op):
            t = re.search(r'<([^>+]+)', args)
            if t:
                cur['calls'].add(t.group(1))
    if os.path.basename(o).startswith('class_'):
        sec = None
        for line in run(objdump, '-r', o).splitlines():
            m = re.match(r'^RELOCATION RECORDS FOR \[(.+)\]:', line)
            if m:
                sec = m.group(1)
                continue
            m = re.match(r'^[0-9a-f]+ R_ARM_ABS32\s+(\S+)', line)
            if m and sec and 'usbpad_class_' in sec and (o, m.group(1)) in funcs:
                class_roots.append((o, m.group(1)))

start = []
for r in roots:
    ob, _, fn = r.partition(':')
    k = (os.path.join(objdir, ob), fn)
    if k not in funcs:
        sys.exit('check_no_svc: root %s not found (renamed? inlined? not built?)' % r)
    start.append(k)
start += class_roots

def resolve(o, s):
    if (o, s) in funcs:
        return (o, s)
    if s in glob_fn:
        return (glob_fn[s], s)
    return None

parent, todo, bad, ext_ok, indirect = {}, [], [], set(), 0
for k in start:
    if k not in parent:
        parent[k] = None
        todo.append(k)

def chain(k):
    out = []
    while k is not None:
        out.append('%s:%s' % (os.path.basename(k[0]), k[1]))
        k = parent[k]
    return ' <- '.join(out)

while todo:
    k = todo.pop()
    f = funcs[k]
    indirect += f['indirect']
    for insn in f['svc']:
        bad.append('svc in %s: %s' % (chain(k), insn))
    for s in sorted(f['calls'] | f['refs']):
        t = resolve(k[0], s)
        if t:
            if t not in parent:
                parent[t] = k
                todo.append(t)
        elif s in data[k[0]] or s in glob_data:
            continue                        # one of our data objects
        elif ALLOW.match(s):
            ext_ok.add(s)
        else:
            what = 'calls' if s in f['calls'] else 'takes the address of'
            bad.append('%s %s %s (no object defines it; not allowlisted)' % (chain(k), what, s))

if os.environ.get('VERBOSE'):
    for k in sorted(parent, key=lambda k: (os.path.basename(k[0]), k[1])):
        print('  %-18s %-28s %d indirect' % (os.path.basename(k[0]), k[1], funcs[k]['indirect']))
if bad:
    print('check_calc: FAILED')
    for b in bad:
        print('  ' + b)
    sys.exit(1)
print('check_calc: OK, %d functions reachable from %d USB callbacks (+%d): '
      'no svc; outside calls only to %s; %d indirect call sites not followed'
      % (len(parent), len(roots), len(class_roots), ', '.join(sorted(ext_ok)) or 'nothing', indirect))
EOF
}
d=$(dirname "$1")
CHECK_OBJS=$(printf '%s\n' "$@" | sed '/^$/d') check "$d" || fail=1
exit $fail
