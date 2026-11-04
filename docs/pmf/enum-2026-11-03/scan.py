# Usage census of enums in the frontier TUs (main file + project headers), per FUNCTION.
import json, subprocess, sys, glob, os, collections
SRC = '/tmp/fr264/fsrc'
mods = {'woff2': (['woff2/src/*.cc'], ['woff2/include', 'woff2/src', 'brotli/c/include'],
                  ['woff2/src/convert_woff2ttf_fuzzer.cc', 'woff2/src/convert_woff2ttf_fuzzer_new_entry.cc']),
        'brunsli': (['brunsli/c/common/*.cc', 'brunsli/c/dec/*.cc', 'brunsli/c/enc/*.cc', 'brunsli/c/tools/*.cc'],
                    ['brunsli/c/include', 'brotli/c/include'], ['brunsli/c/common/nothing.cc'])}
enum_decls = {}   # id -> (name, file, scoped, fixed, nvals, dupvals, dependent, parent_kind)
uses = collections.defaultdict(collections.Counter)  # func -> Counter(shape)
ENAMES = set()
def is_enum_t(t):
    if not isinstance(t, dict): return False
    for q in (t.get('desugaredQualType'), t.get('qualType')):
        q = (q or '').replace('const ', '').replace('&', '').replace('enum ', '').strip()
        if q.split('::')[-1] in ENAMES: return True
    return False
def walk(x, f, func, parent, enumtypes, cur_file):
    if isinstance(x, list):
        for y in x:
            walk(y, f, func, parent, enumtypes, cur_file)
        return
    if not isinstance(x, dict): return
    loc = x.get('loc') or {}
    if 'file' in loc: cur_file[0] = loc['file']
    k = x.get('kind')
    if k == 'EnumDecl' and x.get('name') and not cur_file[0].startswith('/nix'):
        ENAMES.add(x['name'])
    if k == 'TypedefDecl' and not cur_file[0].startswith('/nix') and ('EnumType' in json.dumps(x.get('inner', []))[:2000]):
        ENAMES.add(x['name'])
    if k == 'EnumDecl' and x.get('inner'):
        vals = []; dep = False
        for c in x['inner']:
            if c.get('kind') == 'EnumConstantDecl':
                s = json.dumps(c.get('inner', []))
                if 'EnumConstantDecl' in s: dep = True
        enum_decls[x['id'] + '|' + f] = (x.get('name') or '<anon>', cur_file[0].replace(SRC + '/', ''), bool(x.get('scopedEnumTag')),
                               x.get('fixedUnderlyingType', {}).get('qualType'), len(x['inner']), dep, parent)
    if k in ('FunctionDecl', 'CXXMethodDecl', 'CXXConstructorDecl') and any(c.get('kind') == 'CompoundStmt' for c in x.get('inner', [])):
        func = (cur_file[0].replace(SRC + '/', ''), x.get('name'))
    if func:
        u = uses[func]
        if k == 'DeclRefExpr' and (x.get('referencedDecl') or {}).get('kind') == 'EnumConstantDecl':
            u['enumerator'] += 1
        if k == 'SwitchStmt':
            c = x['inner'][0] if x.get('inner') else {}
            while c.get('kind') in ('ImplicitCastExpr', 'ParenExpr') and c.get('inner'):
                c = c['inner'][0]
            if is_enum_t(c.get('type')): u['switch over enum-typed value'] += 1
            elif 'EnumConstantDecl' in json.dumps(x)[:200000]: u['switch over int with enumerator case labels'] += 1
        if k in ('CStyleCastExpr', 'CXXStaticCastExpr', 'CXXFunctionalCastExpr') and is_enum_t(x.get('type')):
            u['explicit cast TO enum'] += 1
        if k in ('CStyleCastExpr', 'CXXStaticCastExpr') and x.get('inner') and is_enum_t(x['inner'][0].get('type')) and not is_enum_t(x.get('type')):
            u['explicit cast FROM enum'] += 1
        if k == 'BinaryOperator' and x.get('opcode') in ('&', '|', '+', '-', '*', '<<', '>>', '^', '<', '>', '<=', '>='):
            if 'EnumConstantDecl' in json.dumps(x.get('inner', []))[:4000]:
                u['arith/relational on enumerator op=' + x['opcode']] += 1
        if k == 'ImplicitCastExpr' and x.get('castKind') == 'IntegralCast' and x.get('inner') and is_enum_t(x['inner'][0].get('type')) and not is_enum_t(x.get('type')):
            u['implicit enum->integer conversion to ' + (x.get('type') or {}).get('qualType', '?')] += 1
        if k == 'CallExpr' and 'printf' in json.dumps(x.get('inner', [{}])[0])[:2000] and 'EnumConstantDecl' in json.dumps(x)[:20000]:
            u['enum in printf'] += 1
        if k == 'MemberExpr' and is_enum_t(x.get('type')):
            u['enum-typed data member access'] += 1
        if k in ('VarDecl',) and is_enum_t(x.get('type')):
            u['enum-typed local'] += 1
        if k == 'ParmVarDecl' and is_enum_t(x.get('type')):
            u['enum-typed parameter'] += 1
    if k == 'FieldDecl' and is_enum_t(x.get('type')):
        uses[('<fields>', cur_file[0].replace(SRC + '/', ''))]['enum-typed FIELD'] += 1
    for key in ('inner',):
        if key in x:
            walk(x[key], f, func, k or parent, enumtypes, cur_file)
for m, (tus, incs, excl) in mods.items():
    files = sorted(set(sum([glob.glob(os.path.join(SRC, m == 'woff2' and t or t)) for t in tus], [])) - {os.path.join(SRC, e) for e in excl})
    for f in files:
        out = subprocess.run(['clang++', '-std=c++17', '-fsyntax-only', '-Xclang', '-ast-dump=json'] + sum([['-I', os.path.join(SRC, i)] for i in incs], []) + [f],
                             capture_output=True, text=True)
        try:
            ast = json.loads(out.stdout)
        except Exception as e:
            print('FAIL', f, out.stderr[:300]); continue
        cf = [f]
        for n in ast.get('inner', []):
            walk(n, f, None, 'TranslationUnitDecl', None, cf)
json.dump({'enums': {k: v for k, v in enum_decls.items()}, 'uses': {'|'.join(k): v for k, v in uses.items() if v}}, open('/tmp/enumspk/scan/scan.json', 'w'), indent=1)
