import io,os,re,subprocess,sys
sys.stdout.reconfigure(encoding='utf-8',errors='replace')
BS=chr(92)
ROOT=os.getcwd(); HERE=os.path.join(ROOT,'tools','host')
def extract(src,sig):
    i=src.index(sig); j=src.index('{',i); d=0
    for k in range(j,len(src)):
        if src[k]=='{': d+=1
        elif src[k]=='}':
            d-=1
            if d==0: return src[i:k+1]
inc=io.open('fw/lyrics.inc',encoding='utf-8').read()
pc=io.open('fw/player.c',encoding='utf-8',errors='replace').read()
TMAX=int(re.search(r'#define@s+LRC_TEXT_MAX@s+(@d+)'.replace('@',BS),pc).group(1))
parse=extract(inc,'static void lrc_parse')
lines=[]; n=0
while n < TMAX+200:
    t='[%02d:%02d.00] the quick brown fox jumps over the lazy dog good'%(len(lines)//60,len(lines)%60)
    lines.append(t); n+=len(t)+1
full=chr(10).join(lines)
trunc=full[:TMAX-1]
gen=os.path.join(HERE,'trunc_gen.c')
NL=BS+'n'; QT=BS+'"'; BB=BS+BS
with io.open(gen,'w',encoding='utf-8',newline=chr(10)) as f:
    f.write('#include "hostio.h"'+chr(10))
    f.write('#define LRC_TEXT_MAX %du'%TMAX+chr(10)+'#define LRC_LINES 80u'+chr(10)+'#define LRC_NOTIME 0xFFFFu'+chr(10))
    f.write('static char lrc_text[LRC_TEXT_MAX];'+chr(10)+'static uint16_t lrc_off[LRC_LINES];'+chr(10))
    f.write('static uint16_t lrc_sec[LRC_LINES];'+chr(10)+'static uint16_t lrc_n;'+chr(10)+'static uint8_t lrc_synced;'+chr(10))
    f.write(parse+chr(10))
    esc=trunc.replace(BS,BB).replace('"',QT).replace(chr(10),NL)
    f.write('static const char SRC[] = "%s";'%esc+chr(10))
    f.write('int main(void){ uint32_t n=0; while(SRC[n]){lrc_text[n]=SRC[n];n++;} lrc_text[n]=0;'+chr(10))
    f.write('  lrc_parse(n); hputu(lrc_n); hnl();'+chr(10))
    f.write('  const char*p=lrc_text+lrc_off[lrc_n-1]; while(*p){hputc((unsigned char)*p);p++;} hnl();'+chr(10))
    f.write('  return 0; }'+chr(10))
gcc=os.path.join(ROOT,'toolchain','xpack-riscv-none-elf-gcc-15.2.0-1','bin','riscv-none-elf-gcc.exe')
elf=os.path.join(HERE,'trunc.elf')
r=subprocess.run([gcc,'-march=rv32im','-mabi=ilp32','-mno-relax','-O2','-ffreestanding',
                  '-nostdlib','-nostartfiles','-T',os.path.join(HERE,'link.ld'),'-o',elf,
                  os.path.join(HERE,'start.S'),gen,'-lgcc'],capture_output=True,text=True)
if r.returncode: print(r.stderr); sys.exit(2)
o=subprocess.run([sys.executable,'tools/rv32sim.py',elf],capture_output=True,text=True)
rows=o.stdout.splitlines()
print('LRC_TEXT_MAX = %d; fed %d bytes (buffer FULL)'%(TMAX,len(trunc)))
print('lines parsed : %s'%rows[0])
last=rows[1] if len(rows)>1 else ''
print('last line    : %r'%last)
print()
print('TRUNCATION HANDLED -- no partial line shown' if last.endswith('good')
      else 'FAULT -- partial line still on screen')
