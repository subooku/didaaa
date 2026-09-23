import re, struct, os, glob, collections

ELF = '/Users/zgf/WorkBuddy/2026-09-18-10-07-27/didaaa/build/FoloToy-AI-Passport.elf'
SRC = '/Users/zgf/WorkBuddy/2026-09-18-10-07-27/didaaa/main'

# ---- 1. 解析 ELF 段头，得到每个段的 [文件偏移, 大小] ----
data = open(ELF, 'rb').read()
assert data[:4] == b'\x7fELF'
e_shoff, = struct.unpack_from('<I', data, 0x20)
e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', data, 0x2E)
secs = []
def sh(i):
    o = e_shoff + i*e_shentsize
    return struct.unpack_from('<10I', data, o)   # name type flags addr offset size link info addralign entsize
_, _, _, _, shstroff, _, _, _, _, _ = sh(e_shstrndx)
shstr = data[shstroff:]
strtab = {}
for i in range(e_shnum):
    name, typ, flags, addr, off, size = struct.unpack_from('<6I', data, e_shoff+i*e_shentsize)
    nm = shstr[name:shstr.index(b'\x00', name)].decode('utf-8', 'replace')
    strtab[nm] = (off, size, addr, typ)
keep = [n for n in strtab if n.startswith(('.flash.rodata','.dram0.data','.dram0.bss','.flash.text','.iram0'))]
def locate(off):
    for n,(o,s,a,t) in strtab.items():
        if s and o <= off < o+s: return n
    return '?'

# ---- 2. 从源码抽出所有 ESP_LOG 的格式串 ----
pat = re.compile(r'ESP_LOG[A-Z]\s*\(\s*(?:[A-Za-z_0-9]+\s*,\s*)?((?:"(?:[^"\\]|\\.)*"\s*)+)\)', re.S)
tot_cn = tot_all = 0
per_file = collections.OrderedDict()
found = collections.Counter()
samples = []
for f in sorted(glob.glob(os.path.join(SRC, '*.c'))):
    src = open(f, encoding='utf-8').read()
    fcn = fall = 0; ncn = 0
    for m in pat.finditer(src):
        raw = m.group(1)
        parts = re.findall(r'"((?:[^"\\]|\\.)*)"', raw, re.S)
        s = ''.join(parts)
        has_cn = bool(re.search(r'[\u4e00-\u9fff]', s))
        b = len(s.encode('utf-8')) + 1          # +1 = 结尾 \0
        fall += b; tot_all += b
        if has_cn: fcn += b; tot_cn += b; ncn += 1
        if has_cn and len(samples) < 3: samples.append((os.path.basename(f), s[:52]))
    if fall: per_file[os.path.basename(f)] = (fall, fcn, ncn)

print('=== 各源文件的 ESP_LOG 格式串（含所有相邻字符串拼接）===')
print(f'{"文件":<22}{"全部字节":>10}{"其中含中文":>12}{"中文条数":>10}')
for f,(a,c,n) in per_file.items():
    print(f'{f:<22}{a:>10}{c:>12}{n:>10}')
print(f'{"合计":<22}{tot_all:>10}{tot_cn:>12}')

# ---- 3. 这些中文串实际出现在 ELF 的哪个段？ ----
print('\n=== 抽查：中文日志串落在哪个段 ===')
offs_seen = []
for f, s in samples:
    b = s.encode('utf-8')
    off = data.find(b)
    if off < 0:
        print(f'  {f}: "{s}" → 未在 ELF 中找到（可能被编译器优化/去重）'); continue
    sec = locate(off)
    t = strtab[sec][3]
    kind = 'FLASH(只读，不占RAM)' if 'flash' in sec else ('DRAM(占RAM!)' if 'dram' in sec else sec)
    print(f'  {f}: "{s}"\n      段={sec:<20} type={t}  → {kind}')

print('\n=== 全项目含中文的 ESP_LOG 串抽样 20 条，看段分布 ===')
allcn = []
for f in sorted(glob.glob(os.path.join(SRC, '*.c'))):
    src = open(f, encoding='utf-8').read()
    for m in pat.finditer(src):
        s = ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1), re.S))
        if re.search(r'[\u4e00-\u9fff]', s): allcn.append(s.encode('utf-8'))
dist = collections.Counter()
for b in allcn:
    off = data.find(b)
    dist[locate(off) if off >= 0 else '未找到'] += 1
for k, v in dist.most_common():
    print(f'  {k:<24} {v} 条')

print('\n=== 关键段大小 ===')
for k in ['.flash.rodata', '.flash.text', '.dram0.data', '.dram0.bss', '.iram0.text']:
    off, size, addr, t = strtab[k]
    where = 'flash' if k.startswith('.flash') else ('RAM' if k.startswith('.dram') else 'RAM')
    print(f'  {k:<20} {size:>9} B   {where}')
