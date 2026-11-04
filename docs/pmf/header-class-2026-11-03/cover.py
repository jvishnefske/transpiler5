import json, itertools, collections, sys
F = json.load(open(sys.argv[1]))
mode = sys.argv[2]
sets = {k.split('|')[1].split('::')[-1] + '@' + k.split('|')[0].split('/')[-1] + '#' + k.split('|')[3]: set(v[mode]) for k, v in F.items()}
hist = collections.Counter(len(s) for s in sets.values())
print(mode, 'functions', len(sets), 'blocker-count histogram', sorted(hist.items()))
freq = collections.Counter(r for s in sets.values() for r in s)
print('row frequency (FUNCTIONS carrying it):')
for r, c in freq.most_common(): print('  %3d %s' % (c, r))
rows = sorted(freq)
print('zero-blocker:', [k for k, s in sets.items() if not s])
best = {}
for k in range(1, 5):
    bk = (0, None)
    for combo in itertools.combinations(rows, k):
        c = set(combo)
        n = sum(1 for s in sets.values() if s and s <= c)
        if n > bk[0]: bk = (n, combo)
    print('best %d-row combo: +%d %s' % (k, bk[0], bk[1]))
    if bk[1]:
        print('     unlocks', [f for f, s in sets.items() if s and s <= set(bk[1])])
# greedy: pick row that clears most; if 0, pick the row with most single-remaining-gate co-occurrence
chosen = set(); cum = 0
print('greedy:')
for step in range(12):
    gain = {}
    for r in rows:
        if r in chosen: continue
        gain[r] = sum(1 for s in sets.values() if s and not s <= chosen and s <= chosen | {r})
    r, g = max(gain.items(), key=lambda x: (x[1], freq[x[0]]))
    if g == 0:
        # pick the row in the most functions with <= 2 remaining rows
        r = max((x for x in rows if x not in chosen), key=lambda x: (sum(1 for s in sets.values() if x in s and len(s - chosen) <= 2), freq[x]))
    chosen.add(r); cum = sum(1 for s in sets.values() if s and s <= chosen)
    print('  %2d +%d cum %d  %s' % (step + 1, g, cum, r))
