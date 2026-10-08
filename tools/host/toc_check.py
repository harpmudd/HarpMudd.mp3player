import io,os,re,struct,subprocess,sys
sys.stdout.reconfigure(encoding='utf-8',errors='replace')
BS=chr(92); ROOT=os.getcwd(); HERE=os.path.join(ROOT,'tools','host')
def extract(s,sig):
    i=s.index(sig); j=s.index('{',i); d=0
    for k in range(j,len(s)):
        if s[k]=='{': d+=1
        elif s[k]=='}':
            d-=1
            if d==0: return s[i:k+1]
MP3=sys.argv[1]
head=open(MP3,'rb').read(300000)
i=head.find(b'Xing')
flags=struct.unpack('>I',head[i+4:i+8])[0]
o=i+8
frames=struct.unpack('>I',head[o:o+4])[0]; o+=4
byts=struct.unpack('>I',head[o:o+4])[0]; o+=4
toc=list(head[o:o+100])
dur=int(frames*1152/44100.0)
AUDIO_START=0
def py_byte_at(secs):
    if secs>=dur: return 0
    num=secs*100; idx=num//dur; rem=num-idx*dur
    if idx>99: return 0
    a=toc[idx]; b=toc[idx+1] if idx<99 else 256
    v=a+((b-a)*rem)//dur if b>a else a
    return AUDIO_START+((byts*v)>>8)
pc=io.open('fw/player.c',encoding='utf-8',errors='replace').read()
fn=extract(pc,'static uint32_t xing_byte_at')
gen=os.path.join(HERE,'toc_gen.c')
with io.open(gen,'w',encoding='utf-8',newline=chr(10)) as f:
    f.write('#include "hostio.h"'+chr(10))
    f.write('static unsigned char xing_toc[100]={%s};'%','.join(str(c) for c in toc)+chr(10))
    f.write('static unsigned char xing_toc_ok=1;'+chr(10))
    f.write('static uint32_t track_secs=%du;'%dur+chr(10))
    f.write('static uint32_t track_bytes=%du;'%byts+chr(10))
    f.write('static uint32_t audio_start=%du;'%AUDIO_START+chr(10))
    f.write(fn+chr(10))
    f.write('int main(void){'+chr(10))
    f.write(' for(uint32_t s=0;s<%d;s+=5u){hputu(xing_byte_at(s));hnl();}'%dur+chr(10))
    f.write(' return 0;}'+chr(10))
gcc=os.path.join(ROOT,'toolchain','xpack-riscv-none-elf-gcc-15.2.0-1','bin','riscv-none-elf-gcc.exe')
elf=os.path.join(HERE,'toc.elf')
r=subprocess.run([gcc,'-march=rv32im','-mabi=ilp32','-mno-relax','-O2','-ffreestanding',
                  '-nostdlib','-nostartfiles','-T',os.path.join(HERE,'link.ld'),'-o',elf,
                  os.path.join(HERE,'start.S'),gen,'-lgcc'],capture_output=True,text=True)
if r.returncode: print(r.stderr); sys.exit(2)
o2=subprocess.run([sys.executable,'tools/rv32sim.py',elf],capture_output=True,text=True)
got=[int(x) for x in o2.stdout.split() if x.strip()]
bad=0
print('duration %d s, audio %d bytes, TOC %d entries'%(dur,byts,len(toc)))
print(' secs   firmware     python   linear-estimate   error-of-linear')
for k,s in enumerate(range(0,dur,5)):
    w=py_byte_at(s)
    lin=int(s/dur*byts)
    if k>=len(got): break
    g=got[k]
    if g!=w: bad+=1
    if s%30==0:
        # how many seconds wrong would the linear guess have been?
        err=(lin-w)/byts*dur
        print('  %4d %10d %10d %10d        %+.1f s'%(s,g,w,lin,err))
print()
print('%d of %d lookups differ from the independent calculation'%(bad,len(got)))
print('TOC LOOKUP OK' if bad==0 else 'TOC LOOKUP FAULT')
