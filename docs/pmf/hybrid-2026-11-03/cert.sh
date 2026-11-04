#!/usr/bin/env bash
# usage: cert.sh DIR   (DIR has orig.cpp, rw.cpp) -- runs outside nix for compilers; bx inside nix
d=$1; cd $d || exit 2
rm -f *.out *.bin rw.rs back.cpp bx*.err
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -g"
clang++ -std=c++17 $SAN orig.cpp -o o_san.bin 2> cc_o1.err || { echo "ORIG clang FAIL"; cat cc_o1.err | head; exit 1; }
g++ -std=c++17 -O1 orig.cpp -o o_gcc.bin 2> cc_o2.err || { echo "ORIG g++ FAIL"; head cc_o2.err; exit 1; }
./o_san.bin > o_san.out 2> o_san.err || { echo "ORIG san run FAIL rc=$?"; head o_san.err; exit 1; }
./o_gcc.bin > o_gcc.out || { echo "ORIG gcc run FAIL"; exit 1; }
clang++ -std=c++17 $SAN rw.cpp -o r_san.bin 2> cc_r1.err || { echo "RW clang FAIL"; head cc_r1.err; exit 1; }
./r_san.bin > r_san.out 2> r_san.err || { echo "RW san run FAIL"; head r_san.err; exit 1; }
cd /home/j/transpiler5 && nix develop -c bash -c "TMPDIR=/tmp python3 tools/emitrust-bx/emitrust-bx.py --normalize --emit=rust $d/rw.cpp > $d/rw.rs 2> $d/bx1.err; echo \$? > $d/bx1.rc; TMPDIR=/tmp python3 tools/emitrust-bx/emitrust-bx.py --from rust --emit=cpp $d/rw.rs > $d/back.cpp 2> $d/bx2.err; echo \$? > $d/bx2.rc" >/dev/null 2>&1
cd $d
if [ "$(cat bx1.rc)" != 0 ]; then echo "BX REFUSED:"; grep -E 'error|unsupported' bx1.err | head -20; exit 3; fi
grep -c unsafe rw.rs | sed 's/^/unsafe_count=/'
rustc -O --edition 2021 rw.rs -o r_rs.bin 2> rustc.err || { echo "RUSTC FAIL"; head -30 rustc.err; exit 4; }
./r_rs.bin > r_rs.out || { echo "RUST run FAIL rc=$?"; }
if [ "$(cat bx2.rc)" != 0 ]; then echo "BX back FAIL"; head bx2.err; exit 5; fi
g++ -std=c++17 -O1 back.cpp -o r_back.bin 2> cc_b.err && ./r_back.bin > r_back.out || { echo "BACK build/run FAIL"; head cc_b.err; }
for f in o_gcc r_san r_rs r_back; do cmp -s o_san.out $f.out && echo "$f: identical" || { echo "$f: DIFF"; diff o_san.out $f.out | head -5; }; done
wc -l < o_san.out | sed 's/^/out_lines=/'
