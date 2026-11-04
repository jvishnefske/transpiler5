import json, re, sys, collections, itertools
R = json.load(open(sys.argv[1]))
B = json.load(open('/tmp/fr264/front-head/frontier-2026-11-03-4f2a2f5-normalize.json'))['frontier']['records']
TARGET_TAGS = {'member function', 'CXXConstructorDecl', 'CXXDestructorDecl'}
targets = set()
for mod, tus in B.items():
    for tu in tus:
        D = tu['diags']; occ = collections.Counter()
        for f in tu['functions']:
            key = (f['name'], f['kind']); i = occ[key]; occ[key] += 1
            tags = {D[j]['tag'] for j in f['diags']} | {D[j]['tag'] for j in f.get('container_diags', [])}
            if tags and tags <= TARGET_TAGS:
                targets.add((tu['path'].split('fsrc/')[1], f['name'], f['kind'], i))

def crow(msg):
    """class-level row (header)"""
    m = msg.replace('unsupported: ', '')
    if m.startswith('struct with members outside'): return None
    if m == 'private or protected member': return 'C:private'
    if m == 'default member initializer': return 'C:NSDMI'
    if m == 'const data member': return 'C:const-field'
    if m == 'static data member': return 'C:static-data-member'
    if m == 'nested class': return 'C:nested-class'
    if m == 'enum inside a class': return 'C:enum-in-class'
    if m == 'user-declared destructor': return 'C:destructor'
    if m.startswith('class with a base class'): return 'C:inheritance'
    if m in ('overloaded constructor', 'user-declared copy or move constructor'): return 'C:ctor-overload/copy'
    if m == 'member function declared but not defined': return 'C:method-defined-in-another-TU'
    mm = re.match(r"data member of type '(.*)'", m)
    if mm:
        t = re.sub(r'\s*const$', '', mm.group(1))
        if t.split('::')[-1] in ENUMS: return 'C:enum-field'
        if t in ('std::string', 'std::string *'): pass
        if re.search(r'\[\d+\]$', t): return 'C:array-field'
        if t.endswith('*'): return 'C:pointer-field'
        if 'unique_ptr' in t: return 'C:unique_ptr-field'
        if 'std::map' in t: return 'C:map-field'
        if 'std::vector' in t: return 'C:vector<non-kernel>-field'
        return 'C:field-of-unimported-class'
    if 'whose body is not a member-initializer' in m or 'constructor whose body' in m: return 'C:ctor-body(inline)'
    if m.startswith('enumerator'): return 'C:enum-in-class'
    if m.startswith('use of a rejected data member'): return None
    if 'expression statement that is not a call' in m or 'may reach its end' in m: return None  # instrument artifact
    return 'C:inline-body:' + census_tag(m)

sys.path.insert(0, '/tmp/hc')
import importlib.util
spec = importlib.util.spec_from_file_location("census", "/tmp/wt-hc-spike/scripts/bx-census.py")
census = importlib.util.module_from_spec(spec); spec.loader.exec_module(census)
def census_tag(m):
    return census.classify(m)[0]

def orow(msg, kind):
    m = msg.replace('unsupported: ', '')
    if kind in ('CXXConstructorDecl',) and ('expression statement that is not a call' in m or 'may reach its end' in m):
        return None  # instrument artifact
    if kind == 'CXXDestructorDecl' and (m.startswith("identifier '~") or m == 'exception specification'):
        return None  # instrument artifact (dtor imported as a method)
    if m.startswith('use of a rejected data member'): return None  # cascade of a C: field row
    if m == 'member access': return 'B:member access (base of an unimported/rejected class)'
    if 'whose body is not a member-initializer' in m: return 'B:ctor-body'
    if m == 'delegating or base-class constructor initializer': return 'C:inheritance'
    if re.match(r"member function '.*' defined twice", m) or re.match(r"'.*' takes \d+ arguments, \d+ given", m):
        return 'B:method-overloading'
    if m == 'return with a value in a void function': return None  # cascade of a rejected return type
    if m == 'array subscript': return 'B:array-subscript'
    if m.startswith('call to library function'):
        mm = re.search(r"'(\w+)'", m); return 'B:libcall:' + (mm.group(1) if mm else '?')
    if m.startswith('call of a rejected header function'):
        return 'B:hdr-fn-rejected'
    t = census_tag(m)
    return 'B:' + t

def fn_key(t, f, occ):
    k = (f['name'], f['kind']); i = occ[k]; occ[k] += 1
    return (t['path'], f['name'], f['kind'], i)

ENUMS = set()
for t in R: ENUMS |= set(t['enums'])
rows = {}      # begin-key -> set
info = {}
for t in R:
    occ = collections.Counter()
    # class rows by class name
    for f in t['funcs']:
        key = fn_key(t, f, occ)
        own = {orow(m, f['kind']) for _, m in f['own']} - {None}
        cls = {crow(m) for _, m in f['class_rows']} - {None}
        info[(t['path'], f['begin'])] = dict(key=key, own=own, cls=cls, clsname=f['class'], refs=f['refs'])
# class field dependency closure (by class name within TU) + callee closure
def class_rows_closure(t):
    rf = t['rec_fields']
    by_name = {}
    for f in t['funcs']:
        if f['class']:
            by_name.setdefault(f['class'], set()).update({crow(m) for _, m in f['class_rows']} - {None})
    deps = {}
    for rid, v in rf.items():
        nm, ftys = v[0], v[1:]
        if nm in by_name:
            deps[nm] = {o for o in by_name if o != nm and any(re.search(r'\b%s\b' % o, ft) for ft in ftys)}
    def clos(nm, seen=None):
        seen = seen or set(); out = set(by_name.get(nm, set()))
        for d in deps.get(nm, ()):
            if d not in seen:
                seen.add(d); out |= {r + ' [via field %s]' % d if False else r for r in clos(d, seen)}
        return out
    return {nm: clos(nm) for nm in by_name}, deps
full = {}
for t in R:
    cc, deps = class_rows_closure(t)
    d2d = t['decl2def']
    loc = {f['begin']: f for f in t['funcs']}
    def base(b):
        f = loc[b]; i = info[(t['path'], b)]
        return i['own'] | (cc.get(f['class'], set()) if f['class'] else set())
    for f in t['funcs']:
        i = info[(t['path'], f['begin'])]
        if i['key'] not in targets: continue
        # callee closure: main-file defs in this TU reached through refs
        seen = {f['begin']}; st = [f['begin']]; callees = []
        while st:
            b = st.pop()
            for r in loc[b]['refs']:
                cb = d2d.get(r)
                if cb is not None and cb not in seen:
                    seen.add(cb); st.append(cb); callees.append(cb)
        trans = set(base(f['begin']))
        for cb in callees:
            trans |= {('T:' + x[2:]) if False else x for x in base(cb)}
        full[i['key']] = dict(own=sorted(i['own']), cls=sorted(cc.get(f['class'], set()) if f['class'] else []),
                              strict=sorted(base(f['begin'])), trans=sorted(trans),
                              callees=[loc[c]['name'] for c in callees], clsname=f['class'],
                              deps=sorted(deps.get(f['class'], [])) if f['class'] else [])
missing = targets - set(full)
print('targets', len(targets), 'measured', len(full), 'missing', sorted(missing))
json.dump({'|'.join(map(str, k)): v for k, v in full.items()}, open(sys.argv[2], 'w'), indent=1)
