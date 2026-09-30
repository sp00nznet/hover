import maz,struct,os
def r_texture(a,o):
    maz.r_object(a,o); o['flag']=a.u16(); nl=a.u16(); o['levels']=[]
    for L in range(nl):
        w,xm,h,ym,sh=(a.i16() for _ in range(5)); n=struct.unpack_from('<I',a.b,a.p)[0]; a.p+=4
        a.p+=n; m=struct.unpack_from('<I',a.b,a.p)[0]; a.p+=4
        tot=0; spans=0
        for c in range(w):
            k=a.u16()
            for _ in range(k):
                s,e=a.u16(),a.u16(); tot+=(e>>L)-(s>>L)+1; spans+=1
        a.skip()
        o['levels'].append((w,xm,h,ym,sh,n,m,spans,tot))
maz.READERS['CMerlinTexture']=r_texture
for f in ['SMALL.TEX','TEXT1.TEX','TEXT2.TEX','TEXT3.TEX']:
    a=maz.Ar(open(os.path.join(maz.MAZES,f),'rb').read()); a.p=1024
    T=[a.obj() for _ in range(a.count())]; tail=a.skip()
    print(f,len(T),'consumed',a.p==len(a.b),len(a.b)-a.p,'tail',len(tail))
    bad=[(t['name'],l) for t in T for l in t['levels'] if l[5]!=l[8] or l[6]!=l[7]]
    print('  size==sum(spans):',not bad, bad[:3])
    for t in T[:4]+T[-2:]: print('  ',t['name'],t['flag'],t['levels'][:2])
    print('  names',[t['name'] for t in T])
import collections
for mz,tx in [('SMALL','SMALL'),('MAZE1','TEXT1'),('MAZE2','TEXT2'),('MAZE3','TEXT3')]:
    a=maz.Ar(open(os.path.join(maz.MAZES,tx+'.TEX'),'rb').read()); a.p=1024
    F={t['name']:t['flag'] for t in (a.obj() for _ in range(a.count()))}
    w=maz.parse(os.path.join(maz.MAZES,mz+'.MAZ')); S=w['statics']
    c=collections.Counter((s['b'][0]==F.get(s['tex'][2],0), s['b'][1]==F.get(s['tex'][3],0)) for s in S)
    missing=set(t for s in S for t in s['tex'] if t and t not in F)
    print(mz,'b0/b4 == texflag(tex2/tex3):',dict(c),'missing tex:',missing,
          'w2w3 nonzero:',sum(1 for s in S if s['w'][2] or s['w'][3]),'c0 nonzero:',sum(1 for s in S if s['ext'] and s['ext'][0]),
          'w0>w1:',sum(1 for s in S if s['w'][0]>s['w'][1]))
