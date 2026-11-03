import json,hashlib,collections
rows=json.load(open('rows.json')); ext=json.load(open('ext.json'))
def size(r):
  cands=[e for e in ext.get(r['path'],[]) if e['name']==r['name'].split('::')[-1] and e['begin']<=r['line']<=e['end']]
  if not cands:
    cands=[e for e in ext.get(r['path'],[]) if e['name']==r['name'].split('::')[-1]]
  if len(cands)>=1:
    e=min(cands,key=lambda e:e['end']-e['begin']); return e['begin'],e['end']
  return None
pop=[]
for r in rows:
  if not r['tags'] or r['name'].split('::')[-1]=='main': continue
  s=size(r)
  if not s: continue
  r['begin'],r['end']=s; r['lines']=s[1]-s[0]+1
  pop.append(r)
print('blocked w/ extent',len(pop))
SEED='hyb-2026-11-03'
small=[r for r in pop if r['lines']<=40]
print('small(<=40 lines)',len(small), collections.Counter(r['module'] for r in small))
small.sort(key=lambda r: hashlib.sha1((SEED+r['module']+r['name']+str(r['begin'])).encode()).hexdigest())
picks=[]
for mod in ('woff2','brunsli'):
  quotas=[('method',2,lambda r:r['kind']!='FunctionDecl'),('multi3',3,lambda r:r['kind']=='FunctionDecl' and len(r['tags'])>=3),('free12',5,lambda r:r['kind']=='FunctionDecl' and len(r['tags'])<=2)]
  for qn,n,pred in quotas:
    seen=set();got=0
    for r in small:
      if r['module']!=mod or not pred(r) or r in picks: continue
      key=tuple(r['tags'])
      if key in seen: continue
      seen.add(key); r['stratum']=qn; picks.append(r); got+=1
      if got==n: break
    print(mod,qn,got)
json.dump(picks,open('picks.json','w'),indent=1)
with open('sample.md','w') as f:
  f.write("""# Hybrid-wedge spike sample (chosen before any rewrite was attempted)

Instrument: /tmp/fr264/front-head/frontier-2026-11-03-4f2a2f5-normalize.json (bx 4f2a2f5, --normalize --project-root).
Population: blocked functions (any diag or container_diag), excluding `main`, whose body extent
(clang++ AST JSON range, main file) is <= 40 lines.  %d of %d blocked functions with a resolvable extent.
Order: sha1("%s" + module + qualified name + begin line), ascending.
Quotas per module (woff2, brunsli), each taking the first functions in hash order that fit and whose
exact tag set has not already been taken within that quota:
  method (CXXMethodDecl/ctor/dtor, any tag count): 2
  multi3 (free function, >=3 distinct tags): 3
  free12 (free function, 1-2 tags): 5
Total 20.

| # | module | function | stratum | lines | file:begin-end | tags |
|---|---|---|---|---|---|---|
"""%(len(small),len(pop),SEED))
  for i,r in enumerate(picks,1):
    f.write(f"| {i} | {r['module']} | {r['name']} | {r['stratum']} | {r['lines']} | {r['path']}:{r['begin']}-{r['end']} | {'; '.join(r['tags'])} |\n")
print(open('sample.md').read())
