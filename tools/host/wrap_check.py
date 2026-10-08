import io,os,re,subprocess,sys
sys.stdout.reconfigure(encoding='utf-8',errors='replace')
"""Runs the SHIPPING lrc_wrap() over every lyric line on the card, at both
box widths, and checks that every row it produces actually FITS.

The old lrc_split() guaranteed only that the FIRST row fitted. At full width
the tails fitted by luck; with the art panel shown they did not, and that is
what this exists to stop coming back."""
BS=chr(92); ROOT=os.getcwd(); HERE=os.path.join(ROOT,'tools','host')
def extract(s,sig):
    i=s.index(sig); j=s.index('{',i); d=0
    for k in range(j,len(s)):
        if s[k]=='{': d+=1
        elif s[k]=='}':
            d-=1
            if d==0: return s[i:k+1]
pc=io.open('fw/player.c',encoding='utf-8',errors='replace').read()
fm=io.open('fw/font_metrics.h',encoding='utf-8',errors='replace').read()
adv=[int(x) for x in re.search(r'font_adv@[@d*@]@s*=@s*@{(.*?)@};'.replace('@',BS),fm,re.S).group(1).replace(chr(10),'').split(',') if x.strip()]
WMAX=int(re.search(r'#define@s+LRC_WRAP_MAX@s+(@d+)'.replace('@',BS),pc).group(1))
u8h=io.open('fw/utf8.h',encoding='utf-8').read()
nextfn=extract(u8h,'static uint32_t u8_next')
maxw=extract(pc,'static uint32_t lrc_maxw'); wrap=extract(pc,'static uint32_t lrc_wrap')
MP='E:/Assets/mp3player/common/MP3s'
lines=[]
for fn in sorted(os.listdir(MP)):
    if fn.lower().endswith('.lrc'):
        for l in io.open(os.path.join(MP,fn),encoding='utf-8',errors='replace'):
            s=re.sub(r'^@s*(@[@d+:@d+(?:[.:]@d+)?@]@s*)+'.replace('@',BS),'',l.strip())
            if s: lines.append(s)
gen=os.path.join(HERE,'wrap_gen.c')
with io.open(gen,'w',encoding='utf-8',newline=chr(10)) as f:
    f.write('#include "hostio.h"'+chr(10))
    f.write('#define TS_1X 0'+chr(10)+'#define FONT_CELL_W 16u'+chr(10))
    f.write('#define LRC_WRAP_MAX %du'%WMAX+chr(10))
    f.write('static const unsigned char ts_half[6]={2,3,4,6,3,4};'+chr(10))
    f.write('static const unsigned char font_adv[%d]={%s};'%(len(adv),','.join(map(str,adv)))+chr(10))
    f.write('static uint32_t fb_adv(uint32_t cp,uint32_t sx){(void)sx;'
            'return (cp>=32u&&cp<32u+%du)?font_adv[cp-32u]:font_adv[31];}'%len(adv)+chr(10))
    f.write(nextfn+chr(10)); f.write(maxw+chr(10)); f.write(wrap+chr(10))
    f.write('static const char *L[]={'+chr(10))
    for s in lines:
        e=s.replace(BS,BS+BS).replace('"',BS+'"')
        f.write('"%s",'%e+chr(10))
    f.write('};'+chr(10))
    f.write('#define NL %du'%len(lines)+chr(10))
    f.write('int main(void){ uint32_t wws[2]={252u,360u};'+chr(10))
    f.write(' for(int q=0;q<2;q++){ uint32_t ww=wws[q]; uint32_t bad=0;'+chr(10))
    f.write('  for(uint32_t i=0;i<NL;i++){ const char*s=L[i]; uint16_t st[LRC_WRAP_MAX];'+chr(10))
    f.write('   uint32_t n=lrc_wrap(s,ww,st); uint32_t len=0; while(s[len])len++;'+chr(10))
    f.write('   for(uint32_t k=0;k<n;k++){ uint32_t a=st[k], b=(k+1<n)?st[k+1]:len;'+chr(10))
    f.write('    while(b>a&&s[b-1]==32)b--; if(b<=a)continue;'+chr(10))
    f.write('    uint32_t w=0,last=0; const char*q=s+a;'+chr(10))
    f.write('    while((uint32_t)(q-s)<b){last=fb_adv(u8_next(&q),0);w+=last;}'+chr(10))
    f.write('    uint32_t pw=w+((16u>last)?(16u-last):0u); if(pw>ww){bad++;}'+chr(10))
    f.write('    if(a>0 && ((unsigned char)s[a]&0xC0u)==0x80u){bad++;}'+chr(10))
    f.write('   } }'+chr(10))
    f.write('  hputu(ww);hputc(32);hputu(bad);hnl(); }'+chr(10))
    f.write(' return 0;}'+chr(10))
gcc=os.path.join(ROOT,'toolchain','xpack-riscv-none-elf-gcc-15.2.0-1','bin','riscv-none-elf-gcc.exe')
elf=os.path.join(HERE,'wrap.elf')
r=subprocess.run([gcc,'-march=rv32im','-mabi=ilp32','-mno-relax','-O2','-ffreestanding',
                  '-nostdlib','-nostartfiles','-T',os.path.join(HERE,'link.ld'),'-o',elf,
                  os.path.join(HERE,'start.S'),gen,'-lgcc'],capture_output=True,text=True)
if r.returncode: print(r.stderr[:2000]); sys.exit(2)
o=subprocess.run([sys.executable,'tools/rv32sim.py',elf],capture_output=True,text=True)
print('%d lyric lines, LRC_WRAP_MAX=%d'%(len(lines),WMAX))
ok=True
for row in o.stdout.split(chr(10)):
    if not row.strip(): continue
    ww,bad=row.split()
    lbl='art shown ' if ww=='252' else 'full width'
    print('  %s ww=%s : %s rows do not fit'%(lbl,ww,bad))
    if bad!='0': ok=False
print()
print('WRAP OK -- every row fits' if ok else 'WRAP FAULT')
