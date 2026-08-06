import struct, sys, os
from PIL import Image

BPP = {0:'8P',1:'8',2:'16',3:'32',4:'DXT1',5:'DXT3',6:'DXT5',7:'32P',8:'24'}

def c565(v):
    r=(v>>11)&0x1F; g=(v>>5)&0x3F; b=v&0x1F
    return (r*255+15)//31, (g*255+31)//63, (b*255+15)//31

def dxt_block_colors(c0,c1,dxt1):
    r0,g0,b0=c565(c0); r1,g1,b1=c565(c1)
    cols=[(r0,g0,b0,255),(r1,g1,b1,255)]
    if c0>c1 or not dxt1:
        cols.append(((2*r0+r1)//3,(2*g0+g1)//3,(2*b0+b1)//3,255))
        cols.append(((r0+2*r1)//3,(g0+2*g1)//3,(b0+2*b1)//3,255))
    else:
        cols.append(((r0+r1)//2,(g0+g1)//2,(b0+b1)//2,255))
        cols.append((0,0,0,0))
    return cols

def decode_dxt(data, w, h, fmt):
    img = Image.new('RGBA',(w,h)); px = img.load()
    bw,bh = (w+3)//4, (h+3)//4
    bs = 8 if fmt=='DXT1' else 16
    o=0
    for by in range(bh):
        for bx in range(bw):
            if o+bs>len(data): return img
            blk=data[o:o+bs]; o+=bs
            alpha=[255]*16
            if fmt=='DXT3':
                a=blk[:8]
                for i in range(16):
                    nib=(a[i//2]>>(4*(i%2)))&0xF
                    alpha[i]=nib*17
                cb=blk[8:]
            elif fmt=='DXT5':
                a0,a1=blk[0],blk[1]
                bits=int.from_bytes(blk[2:8],'little')
                tbl=[a0,a1]
                if a0>a1: tbl+= [((6-i)*a0+(1+i)*a1)//7 for i in range(6)]
                else:     tbl+= [((4-i)*a0+(1+i)*a1)//5 for i in range(4)]+[0,255]
                for i in range(16): alpha[i]=tbl[(bits>>(3*i))&7]
                cb=blk[8:]
            else:
                cb=blk
            c0,c1=struct.unpack_from('<HH',cb,0)
            idx=struct.unpack_from('<I',cb,4)[0]
            cols=dxt_block_colors(c0,c1,fmt=='DXT1')
            for i in range(16):
                x=bx*4+(i%4); y=by*4+(i//4)
                if x<w and y<h:
                    r,g,b,a=cols[(idx>>(2*i))&3]
                    if fmt!='DXT1': a=alpha[i]
                    px[x,y]=(r,g,b,a)
    return img

def convert(path, out):
    d=open(path,'rb').read()
    rt,ver,w,h,nmip,nsec,ifl,ufl = struct.unpack_from('<iihhHHii',d,0)
    extra=d[24:36]
    fmt=BPP.get(extra[2] if extra[2]!=0 else 3,'?')
    cmd=d[36:164].split(b'\0')[0].decode('latin-1',errors='replace')
    print('%-34s %4dx%-4d mips=%-2d fmt=%-5s cmd=%r' % (os.path.basename(path),w,h,nmip,fmt,cmd))
    data=d[164:]
    if fmt in ('DXT1','DXT3','DXT5'):
        img=decode_dxt(data,w,h,fmt)
    elif fmt=='32':
        img=Image.frombytes('RGBA',(w,h),data[:w*h*4]).convert('RGBA')
        b,g,r,a=img.split(); img=Image.merge('RGBA',(r,g,b,a))
    else:
        print('   (unsupported format, skipped)'); return None
    img.save(out); return img

if __name__=='__main__':
    for p in sys.argv[1:]:
        base=os.path.basename(p).rsplit('.',1)[0]
        convert(p, base+'.png')
