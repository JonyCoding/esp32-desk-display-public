"""Export the shared C++ renderer to a pixel-exact PNG contact sheet."""
import struct
import zlib
from pathlib import Path
from preview import Engine


def chunk(kind, data):
    return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data)&0xffffffff)


def export(path):
    engine=Engine()
    frames=[]
    try:
        engine.request({'op':'pause','value':True})
        for preset,page in [('work','quota'),('nightclock','loft'),('afterhours','loft'),('status','status')]:
            engine.request({'op':'preset','value':preset})
            result=engine.request({'op':'skip','value':6500})
            frames.append(result['frames'][page])
    finally:engine.close()
    scale=7; padding=24; size=64*scale
    width=height=size*2+padding*3
    rows=[]
    for y in range(height):
        row=bytearray([0])
        for x in range(width):
            gx=(x-padding)//(size+padding);gy=(y-padding)//(size+padding)
            px=(x-padding)%(size+padding);py=(y-padding)%(size+padding)
            if 0<=gx<2 and 0<=gy<2 and px<size and py<size:
                pixels=frames[gy*2+gx];i=(py//scale*64+px//scale)*4;p=int(pixels[i:i+4],16)
                row.extend((((p>>11)&31)*255//31,((p>>5)&63)*255//63,(p&31)*255//31))
            else:row.extend((21,25,31))
        rows.append(row)
    data=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(b''.join(rows)))+chunk(b'IEND',b'')
    Path(path).write_bytes(data)


if __name__=='__main__':
    import sys
    export(sys.argv[1] if len(sys.argv)>1 else '/tmp/desk-display-pages.png')
