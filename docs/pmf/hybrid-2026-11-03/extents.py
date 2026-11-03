import json,subprocess,sys,os
rows=json.load(open('rows.json'))
paths=sorted({r['path'] for r in rows})
inc={'woff2':['-I/tmp/fr264/fsrc/woff2/include','-I/tmp/fr264/fsrc/woff2/src','-I/tmp/fr264/fsrc/brotli/c/include'],
     'brunsli':['-I/tmp/fr264/fsrc/brunsli/c/include','-I/tmp/fr264/fsrc/brunsli/c','-I/tmp/fr264/fsrc/brotli/c/include','-I/tmp/fr264/front-head/generated/brunsli']}
ext={}
for p in paths:
  full='/tmp/fr264/fsrc/'+p
  mod=p.split('/')[0]
  out=subprocess.run(['clang++','-std=c++17','-fsyntax-only','-Xclang','-ast-dump=json']+inc[mod]+[full],capture_output=True,text=True)
  if out.returncode: print('ERR',p,out.stderr[:300],file=sys.stderr)
  try: ast=json.loads(out.stdout)
  except Exception as e: print('JSONERR',p,file=sys.stderr); continue
  cur={'file':None,'line':0}
  def upd(l):
    if not l: return
    if 'spellingLoc' in l or 'expansionLoc' in l:
      upd(l.get('spellingLoc')); upd(l.get('expansionLoc')); return
    if 'file' in l: cur['file']=l['file']
    if 'line' in l: cur['line']=l['line']
  def walk(n,ns):
    k=n.get('kind')
    upd(n.get('loc'))
    r=n.get('range',{})
    b=r.get('begin',{}); upd(b); bl=cur['line']; bf=cur['file']
    e=r.get('end',{}); upd(e); el=cur['line']; ef=cur['file']
    if k in('FunctionDecl','CXXMethodDecl','CXXConstructorDecl','CXXDestructorDecl','FunctionTemplateDecl') and any(c.get('kind')=='CompoundStmt' for c in n.get('inner',[])) and ef and ef.endswith('/'+p.split('/')[-1]) and not n.get('isImplicit'):
      ext.setdefault(p,[]).append(dict(name=n.get('name'),kind=k,begin=bl,end=el))
    for c in n.get('inner',[]): walk(c,ns)
  walk(ast,'')
json.dump(ext,open('ext.json','w'))
print(sum(len(v) for v in ext.values()))
