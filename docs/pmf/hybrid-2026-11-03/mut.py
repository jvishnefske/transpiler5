import subprocess,os,sys
M=[
 ('f09','if (i == 0 && code == 0x80)','if (code == 0x80)','drop i==0 guard on leading-zero check'),
 ('f09','0xfe000000u','0xfc000000u','overflow mask off by one bit'),
 ('f05','arg_size += 8;','arg_size += 6;','2x2 transform size 8->6'),
 ('f05','} else if ((static_cast<int32_t>(flags) & kFLAG_WE_HAVE_AN_X_AND_Y_SCALE) != 0) {\n            arg_size += 4;\n        } else if ((static_cast<int32_t>(flags) & kFLAG_WE_HAVE_A_TWO_BY_TWO) != 0) {\n            arg_size += 8;','} else if ((static_cast<int32_t>(flags) & kFLAG_WE_HAVE_A_TWO_BY_TWO) != 0) {\n            arg_size += 8;\n        } else if ((static_cast<int32_t>(flags) & kFLAG_WE_HAVE_AN_X_AND_Y_SCALE) != 0) {\n            arg_size += 4;','swap else-if priority XY-scale vs 2x2'),
 ('f05','glyph->have_instructions = false;\n','','drop have_instructions reset'),
 ('f10','0x4c545348u','0x4c544853u','table typo LTSH byte swap'),
 ('f10','0x67766172u','0x67766171u','table typo gvar last byte (index ~47)'),
 ('f17','        items.pop_back();\n','','forget pop_back after manual erase'),
 ('f11','            n_bits -= kHuffmanTableBits;\n','','forget n_bits -= root bits'),
 ('f13','std::vector<uint8_t> bytes(bw->pos','std::vector<uint8_t> bytes(bw->pos - 1','copy pos-1 bytes'),
 ('f13','    bw->buf = std::vector<uint8_t>();\n','','forget to clear buffer after finish'),
 ('f02','const std::size_t at = g + 1;','const std::size_t at = g;','insert loca BEFORE glyf'),
 ('f02','if (output_order[i] == kGlyfTableTag && glyf_loc == output_order.size()) {','if (output_order[i] == kGlyfTableTag) {','find LAST glyf instead of first'),
 ('f19','if (x >= half)','if (x > half)','>= vs >'),
 ('f20','if (left <= 0)','if (left < 0)','<= vs <'),
 ('f01','static_cast<int>(this->bbox_bitmap_.size() + this->bbox_stream_.size())','static_cast<int>(this->bbox_stream_.size())','drop bbox bitmap size from length'),
 ('f01','WriteVecBytes(result, this->bbox_bitmap_);\n        WriteVecBytes(result, this->bbox_stream_);','WriteVecBytes(result, this->bbox_stream_);\n        WriteVecBytes(result, this->bbox_bitmap_);','swap stream order'),
 ('f14','original_jpg_size > *len','original_jpg_size >= *len','> vs >= capacity'),
 ('f03','        font->num_tables = static_cast<uint16_t>(font->tables.size());\n','','forget num_tables update'),
 ('f18','return data[pos + offset];','return data[pos + offset + 0];','EQUIVALENT mutant (control)'),
]
for i,(d,old,new,desc) in enumerate(M):
  src=open(f'/tmp/hyb/{d}/rw.cpp').read()
  if src.count(old)!=1: print(i,d,'PATTERN NOT UNIQUE',src.count(old)); continue
  m=src.replace(old,new); fn=f'm{i}.cpp'; open(fn,'w').write(m)
  r=subprocess.run(['clang++','-std=c++17','-fsanitize=address,undefined','-fno-sanitize-recover=all','-O1',fn,'-o',f'm{i}.bin'],capture_output=True,text=True)
  if r.returncode: print(i,d,'COMPILE FAIL',r.stderr[:200]); continue
  p=subprocess.run([f'./m{i}.bin'],capture_output=True,text=True,timeout=120)
  ref=open(f'/tmp/hyb/{d}/o_san.out').read()
  if p.returncode: verdict=f'CAUGHT (sanitizer/crash rc={p.returncode})'
  elif p.stdout!=ref:
    nd=sum(a!=b for a,b in zip(p.stdout.split('\n'),ref.split('\n')))
    verdict=f'CAUGHT ({nd} lines differ of {len(ref.splitlines())})'
  else: verdict='SURVIVED'
  print(i,d,desc,'->',verdict)
