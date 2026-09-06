// REQUIRES: cargo
// FR-198 SCALE test: a fall-through chain of 256 cases.
//
// This is the size the old lowering could not reach at all. `lift-cf-to-scf`
// structurized a chain by duplicating the tail into every arm, so case k was
// emitted about (N - k + 1) times: FR-197 measured 0.86s at N=32, 7.93s at
// N=48 and, at N=100, more than 360 seconds with NO output and NO diagnostic
// -- the one failure mode this repo forbids outright. The guarded form emits
// each section exactly once, so this file's emitted Rust is linear in N
// (measured: 8N + 13 lines, 0.07s at N=256) and the compile finishes in the
// same time a 16-case switch takes.
//
// It is a real differential test, not just a compile-time probe: `chain` is
// sampled at every entry point, above and below the label range, with the
// scrutinee derived from `argc` so nothing can be folded at compile time.
// Byte-identical stdout and exit codes against the clang-built native binary
// are required.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/switch_fallthrough_ladder_scale > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

int chain(int x) {
  int acc = 0;
  switch (x) {
  case 0: acc += 1;
  case 1: acc += 8;
  case 2: acc += 2;
  case 3: acc += 9;
  case 4: acc += 3;
  case 5: acc += 10;
  case 6: acc += 4;
  case 7: acc += 11;
  case 8: acc += 5;
  case 9: acc += 12;
  case 10: acc += 6;
  case 11: acc += 13;
  case 12: acc += 7;
  case 13: acc += 1;
  case 14: acc += 8;
  case 15: acc += 2;
  case 16: acc += 9;
  case 17: acc += 3;
  case 18: acc += 10;
  case 19: acc += 4;
  case 20: acc += 11;
  case 21: acc += 5;
  case 22: acc += 12;
  case 23: acc += 6;
  case 24: acc += 13;
  case 25: acc += 7;
  case 26: acc += 1;
  case 27: acc += 8;
  case 28: acc += 2;
  case 29: acc += 9;
  case 30: acc += 3;
  case 31: acc += 10;
  case 32: acc += 4;
  case 33: acc += 11;
  case 34: acc += 5;
  case 35: acc += 12;
  case 36: acc += 6;
  case 37: acc += 13;
  case 38: acc += 7;
  case 39: acc += 1;
  case 40: acc += 8;
  case 41: acc += 2;
  case 42: acc += 9;
  case 43: acc += 3;
  case 44: acc += 10;
  case 45: acc += 4;
  case 46: acc += 11;
  case 47: acc += 5;
  case 48: acc += 12;
  case 49: acc += 6;
  case 50: acc += 13;
  case 51: acc += 7;
  case 52: acc += 1;
  case 53: acc += 8;
  case 54: acc += 2;
  case 55: acc += 9;
  case 56: acc += 3;
  case 57: acc += 10;
  case 58: acc += 4;
  case 59: acc += 11;
  case 60: acc += 5;
  case 61: acc += 12;
  case 62: acc += 6;
  case 63: acc += 13;
  case 64: acc += 7;
  case 65: acc += 1;
  case 66: acc += 8;
  case 67: acc += 2;
  case 68: acc += 9;
  case 69: acc += 3;
  case 70: acc += 10;
  case 71: acc += 4;
  case 72: acc += 11;
  case 73: acc += 5;
  case 74: acc += 12;
  case 75: acc += 6;
  case 76: acc += 13;
  case 77: acc += 7;
  case 78: acc += 1;
  case 79: acc += 8;
  case 80: acc += 2;
  case 81: acc += 9;
  case 82: acc += 3;
  case 83: acc += 10;
  case 84: acc += 4;
  case 85: acc += 11;
  case 86: acc += 5;
  case 87: acc += 12;
  case 88: acc += 6;
  case 89: acc += 13;
  case 90: acc += 7;
  case 91: acc += 1;
  case 92: acc += 8;
  case 93: acc += 2;
  case 94: acc += 9;
  case 95: acc += 3;
  case 96: acc += 10;
  case 97: acc += 4;
  case 98: acc += 11;
  case 99: acc += 5;
  case 100: acc += 12;
  case 101: acc += 6;
  case 102: acc += 13;
  case 103: acc += 7;
  case 104: acc += 1;
  case 105: acc += 8;
  case 106: acc += 2;
  case 107: acc += 9;
  case 108: acc += 3;
  case 109: acc += 10;
  case 110: acc += 4;
  case 111: acc += 11;
  case 112: acc += 5;
  case 113: acc += 12;
  case 114: acc += 6;
  case 115: acc += 13;
  case 116: acc += 7;
  case 117: acc += 1;
  case 118: acc += 8;
  case 119: acc += 2;
  case 120: acc += 9;
  case 121: acc += 3;
  case 122: acc += 10;
  case 123: acc += 4;
  case 124: acc += 11;
  case 125: acc += 5;
  case 126: acc += 12;
  case 127: acc += 6;
  case 128: acc += 13;
  case 129: acc += 7;
  case 130: acc += 1;
  case 131: acc += 8;
  case 132: acc += 2;
  case 133: acc += 9;
  case 134: acc += 3;
  case 135: acc += 10;
  case 136: acc += 4;
  case 137: acc += 11;
  case 138: acc += 5;
  case 139: acc += 12;
  case 140: acc += 6;
  case 141: acc += 13;
  case 142: acc += 7;
  case 143: acc += 1;
  case 144: acc += 8;
  case 145: acc += 2;
  case 146: acc += 9;
  case 147: acc += 3;
  case 148: acc += 10;
  case 149: acc += 4;
  case 150: acc += 11;
  case 151: acc += 5;
  case 152: acc += 12;
  case 153: acc += 6;
  case 154: acc += 13;
  case 155: acc += 7;
  case 156: acc += 1;
  case 157: acc += 8;
  case 158: acc += 2;
  case 159: acc += 9;
  case 160: acc += 3;
  case 161: acc += 10;
  case 162: acc += 4;
  case 163: acc += 11;
  case 164: acc += 5;
  case 165: acc += 12;
  case 166: acc += 6;
  case 167: acc += 13;
  case 168: acc += 7;
  case 169: acc += 1;
  case 170: acc += 8;
  case 171: acc += 2;
  case 172: acc += 9;
  case 173: acc += 3;
  case 174: acc += 10;
  case 175: acc += 4;
  case 176: acc += 11;
  case 177: acc += 5;
  case 178: acc += 12;
  case 179: acc += 6;
  case 180: acc += 13;
  case 181: acc += 7;
  case 182: acc += 1;
  case 183: acc += 8;
  case 184: acc += 2;
  case 185: acc += 9;
  case 186: acc += 3;
  case 187: acc += 10;
  case 188: acc += 4;
  case 189: acc += 11;
  case 190: acc += 5;
  case 191: acc += 12;
  case 192: acc += 6;
  case 193: acc += 13;
  case 194: acc += 7;
  case 195: acc += 1;
  case 196: acc += 8;
  case 197: acc += 2;
  case 198: acc += 9;
  case 199: acc += 3;
  case 200: acc += 10;
  case 201: acc += 4;
  case 202: acc += 11;
  case 203: acc += 5;
  case 204: acc += 12;
  case 205: acc += 6;
  case 206: acc += 13;
  case 207: acc += 7;
  case 208: acc += 1;
  case 209: acc += 8;
  case 210: acc += 2;
  case 211: acc += 9;
  case 212: acc += 3;
  case 213: acc += 10;
  case 214: acc += 4;
  case 215: acc += 11;
  case 216: acc += 5;
  case 217: acc += 12;
  case 218: acc += 6;
  case 219: acc += 13;
  case 220: acc += 7;
  case 221: acc += 1;
  case 222: acc += 8;
  case 223: acc += 2;
  case 224: acc += 9;
  case 225: acc += 3;
  case 226: acc += 10;
  case 227: acc += 4;
  case 228: acc += 11;
  case 229: acc += 5;
  case 230: acc += 12;
  case 231: acc += 6;
  case 232: acc += 13;
  case 233: acc += 7;
  case 234: acc += 1;
  case 235: acc += 8;
  case 236: acc += 2;
  case 237: acc += 9;
  case 238: acc += 3;
  case 239: acc += 10;
  case 240: acc += 4;
  case 241: acc += 11;
  case 242: acc += 5;
  case 243: acc += 12;
  case 244: acc += 6;
  case 245: acc += 13;
  case 246: acc += 7;
  case 247: acc += 1;
  case 248: acc += 8;
  case 249: acc += 2;
  case 250: acc += 9;
  case 251: acc += 3;
  case 252: acc += 10;
  case 253: acc += 4;
  case 254: acc += 11;
  case 255: acc += 5;
  default: break;
  }
  return acc;
}

int main(int argc, char **argv) {
  int s = argc;
  int total = 0;
  for (int i = -3; i < 260; ++i) {
    int v = i * s;
    total += chain(v);
    printf("%d %d\n", v, chain(v));
  }
  printf("total %d\n", total);
  return 0;
}
