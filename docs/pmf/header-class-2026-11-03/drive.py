import importlib.util, json, os, re, subprocess, sys, multiprocessing
spec = importlib.util.spec_from_file_location("census", "/tmp/wt-hc-spike/scripts/bx-census.py")
census = importlib.util.module_from_spec(spec); spec.loader.exec_module(census)
S = "/tmp/fr264/fsrc"
INC = {"woff2": [S+"/woff2/include", S+"/woff2/src", S+"/brotli/c/include"],
       "brunsli": [S+"/brunsli/c/include", "/tmp/fr264/front-head/generated/brunsli", S+"/brotli/c/include"]}
NOTE_RX = re.compile(r"^(?P<path>.*?):(?P<line>\d+):(?P<col>\d+): note: (?P<msg>.*)$")
BX = os.environ.get("BX", "/tmp/wt-hc-spike/tools/emitrust-bx/emitrust-bx.py")

FK = ("FunctionDecl", "CXXMethodDecl", "CXXConstructorDecl", "CXXDestructorDecl")

def extract(ast, begins):
    refs, self_ids, rec_fields, decl2def = {}, {}, {}, {}
    enums = set()
    def collect_refs(n, acc):
        st = [n]
        while st:
            x = st.pop()
            if isinstance(x, list):
                st.extend(x); continue
            if not isinstance(x, dict):
                continue
            k = x.get("kind")
            if k == "MemberExpr":
                d = x.get("referencedMemberDecl")
                if d: acc.add(d)
            if k == "DeclRefExpr":
                d = (x.get("referencedDecl") or {})
                if d.get("kind") in FK: acc.add(d.get("id"))
            if k == "CXXConstructExpr" or k == "CXXTemporaryObjectExpr":
                acc.add("ctor:" + ((x.get("type") or {}).get("qualType") or ""))
            for kk, v in x.items():
                if kk in ("loc", "range", "type", "referencedDecl", "referencedMemberDecl"):
                    continue
                if isinstance(v, (list, dict)):
                    st.append(v)
    def walk(n):
        if not isinstance(n, dict):
            return
        k = n.get("kind")
        if k == "EnumDecl" and n.get("name"):
            enums.add(n["name"])
        if k == "CXXRecordDecl" and n.get("completeDefinition") and n.get("name"):
            rec_fields[n["id"]] = [n["name"]] + [ (c.get("type") or {}).get("qualType", "")
                                   for c in n.get("inner", []) if c.get("kind") == "FieldDecl"]
        if k in FK:
            b = ((n.get("range") or {}).get("begin") or {})
            b = b.get("offset", (b.get("expansionLoc") or {}).get("offset"))
            body = [c for c in n.get("inner", []) if isinstance(c, dict) and c.get("kind") == "CompoundStmt"]
            if body and b in begins and not n.get("isImplicit"):
                acc = set()
                collect_refs(n.get("inner", []), acc)
                refs[b] = sorted(a for a in acc if a)
                ids = [n["id"]] + ([n["previousDecl"]] if n.get("previousDecl") else [])
                self_ids[b] = ids
                for i in ids:
                    decl2def[i] = b
        for c in n.get("inner", []) or []:
            walk(c)
    for n in ast.get("inner", []):
        walk(n)
    return {"refs": refs, "self_ids": self_ids, "rec_fields": rec_fields, "decl2def": decl2def, "enums": sorted(enums)}

def one(job):
    mod, rel = job
    path = os.path.join(S, rel)
    env = dict(os.environ, CPLUS_INCLUDE_PATH=":".join(INC[mod]), BX_HC_SPIKE=os.environ.get("HCMODE", "1"))
    p = subprocess.run(["clang++", "-std=c++17", "-fsyntax-only", "-Xclang", "-ast-dump=json", path],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
    ast = json.loads(p.stdout)
    funcs = census.main_file_functions(ast, path)
    deps = extract(ast, {f["begin"] for f in funcs})
    b = subprocess.run([sys.executable, BX, "--emit=kernel", "--normalize", "--project-root=" + S, path],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
    with open(path, "rb") as fh:
        starts = census.line_starts(fh.read())
    diags, notes = [], []
    for line in b.stderr.decode("utf-8", "replace").splitlines():
        m = census.DIAG_RX.match(line)
        if m:
            diags.append((os.path.realpath(m["path"]), int(m["line"]), int(m["col"]), m["msg"]))
            continue
        m = NOTE_RX.match(line)
        if m:
            notes.append((os.path.realpath(m["path"]), int(m["line"]), int(m["col"]), m["msg"]))
    rp = os.path.realpath(path)
    classes = {}
    for f, l, c, msg in notes:
        mm = re.match(r"HC class (\d+|0x[0-9a-f]+) '(\w+)' range (\d+)-(\d+)", msg)
        if mm:
            classes[mm[1]] = (mm[2], f, int(mm[3]), int(mm[4]))
    out = []
    for fn in funcs:
        own = []
        for f, l, c, msg in diags:
            if f == rp:
                off = starts[l-1] + c - 1
                if fn["begin"] <= off <= fn["end"]:
                    own.append((l, msg))
        cls = None
        for f, l, c, msg in notes:
            mm = re.match(r"HC def of class (\S+)", msg)
            if mm and f == rp and fn["begin"] <= starts[l-1] + c - 1 <= fn["end"]:
                cls = mm[1]
        crow = []
        if cls in classes:
            nm, hf, a, z = classes[cls]
            crow = [(l, msg) for f, l, c, msg in diags if f == hf and a <= l <= z]
        out.append({"name": fn["name"], "kind": fn["kind"], "begin": fn["begin"], "class": classes.get(cls, [None])[0],
                    "own": own, "class_rows": crow})
    for f in out:
        f["refs"] = deps["refs"].get(f["begin"], [])
        f["self_ids"] = deps["self_ids"].get(f["begin"], [])
    return {"mod": mod, "rec_fields": deps["rec_fields"], "decl2def": deps["decl2def"], "enums": deps["enums"], "path": rel, "rc": b.returncode, "funcs": out,
            "other_diags": [d for d in diags if d[0] != rp], "notes": notes}

if __name__ == "__main__":
    jobs = [tuple(a.split(":", 1)) for a in sys.argv[2:]]
    with multiprocessing.get_context("fork").Pool(4) as pool:
        res = pool.map(one, jobs)
    json.dump(res, open(sys.argv[1], "w"), indent=1)
