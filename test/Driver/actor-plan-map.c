// FR-62 (slice 2): the `--actor-map` override surface, the exact isomorph
// of FR-59's --partition-map. Pins four facts:
//  - LONGEST PREFIX WINS: prefix `ALPHA` names grp_alpha, but the longer
//    `ALPHA_TWO` line wins for that symbol, so ALPHA_ONE -> grp_alpha and
//    ALPHA_TWO -> special; unmatched BETA keeps its co-access default.
//  - OVERRIDES CANNOT CREATE UNSOUNDNESS: `mix` closure-writes both
//    ALPHA_ONE (pinned to grp_alpha) and BETA, so the writer rule condenses
//    the pinned actor into BETA with a note -- a pin can widen or name an
//    actor but never split what the writer rule requires whole. The
//    condensation survivor is the smallest participating name
//    (deterministic, FR-59's earliest-member analogue).
//  - a malformed map line is a clean error naming the line, in
//    --partition-map's wording style;
//  - `--actor-map` without `--emit=actor-plan` is a clean gate error.
//
// RUN: echo "ALPHA grp_alpha" > %t.map
// RUN: echo "ALPHA_TWO special" >> %t.map
// RUN: emitrust-cc --emit=actor-plan --actor-map %t.map %s -o - 2>%t.err | FileCheck %s --match-full-lines
// RUN: FileCheck %s --check-prefix=WARN < %t.err
//
// RUN: echo "ALPHA" > %t.bad
// RUN: not emitrust-cc --emit=actor-plan --actor-map %t.bad %s -o - 2>&1 | FileCheck %s --check-prefix=BAD
//
// RUN: not emitrust-cc --emit=item-graph --actor-map %t.map %s -o - 2>&1 | FileCheck %s --check-prefix=GATE

int alpha_one;
int alpha_two;
int beta;

void touch_one(void) { alpha_one = 1; }

void touch_two(void) { alpha_two = 2; }

void touch_beta(void) { beta = 3; }

void mix(void) {
  alpha_one += 1;
  beta += 1;
}

// ALPHA_ONE's pinned actor grp_alpha was condensed into BETA; ALPHA_TWO's
// longest-prefix pin stands, so its actor keeps the override name.
// CHECK:      actor BETA globals=ALPHA_ONE,BETA
// CHECK-NEXT: actor special globals=ALPHA_TWO
// CHECK-NEXT: fn mix role=arm actor=BETA
// CHECK-NEXT: fn touch_beta role=arm actor=BETA
// CHECK-NEXT: fn touch_one role=arm actor=BETA
// CHECK-NEXT: fn touch_two role=arm actor=special
// CHECK-NEXT: note function 'mix' closure-writes globals of actors {BETA,grp_alpha}; merged into 'BETA'
// CHECK-NOT:  {{.+}}

// WARN: warning: actor plan: function 'mix' closure-writes globals of actors {BETA,grp_alpha}; merged into 'BETA'

// BAD: error: malformed --actor-map line: 'ALPHA' (expected '<symbol-prefix> <actor-name>')

// GATE: error: --actor-map requires --emit=actor-plan
