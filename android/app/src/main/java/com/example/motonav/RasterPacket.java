package com.example.motonav;

/** 离线包只接收完整的 1200×1200、2 米普通地图，不把损坏缓存发往圆屏。 */
public final class RasterPacket {
    public static boolean isBuilding(byte[] b) {
        if(b.length<16)return false;
        int v=u16(b,14);
        // 每像素毫米数 = 2000，带楼块标志（0x4000）；高度位（0x2000）可有可无
        return (v&0x1fff)==2000 && (v&0xc000)==AmapBuildings.FLAG;
    }
    /** 连接旧固件时保留完整范围，将屋顶退回地面，绝不发送它不认识的标志。 */
    public static byte[] forLegacy(byte[] b) {
        if(!isBuilding(b))return b;
        byte[] out=b.clone();out[14]=(byte)2000;out[15]=(byte)(2000>>>8);
        for(int p=16;p<out.length;) {
            int v=out[p]&255;
            if((v&15)>3)out[p]=(byte)((v&240)|(v&2));
            p+=((v>>>4)==15)?2:1;
        }
        return out;
    }
    private static int u16(byte[] b,int p) { return (b[p]&255)|((b[p+1]&255)<<8); }
    private static int i32(byte[] b,int p) { return u16(b,p)|(u16(b,p+2)<<16); }

    /** Crop a centered, legacy-safe raster window while retaining its native pixel scale. */
    public static byte[] compactForLegacy(byte[] src, int maxBytes, int side, double centerX, double centerY) {
        if (src == null || src.length < 17 || maxBytes < 128) return null;
        int width=u16(src,10), height=u16(src,12), rawMm=u16(src,14), mm=rawMm&0x7fff;
        boolean palette=(rawMm&0x8000)!=0;
        int header=palette?48:16;
        if(width<=0 || height<=0 || mm<=0 || src.length<header) return null;
        int outW=Math.min(side,width), outH=Math.min(side,height);
        int tiles=((outW+15)>>4)*((outH+15)>>4);
        if(outW<=0 || outH<=0 || tiles*128>8*1024) return null;
        double mpp=mm/1000.0;
        int startX=(int)Math.round((centerX-i32(src,2))/mpp-outW*0.5);
        int startY=(int)Math.round((i32(src,6)-centerY)/mpp-outH*0.5);
        startX=Math.max(0,Math.min(width-outW,startX));
        startY=Math.max(0,Math.min(height-outH,startY));
        byte[] pixels=new byte[outW*outH];
        int pos=header;
        long consumed=0, pixelCount=(long)width*height;
        while(consumed<pixelCount && pos<src.length) {
            int token=src[pos++]&255, color=token&15, run=(token>>>4)+1;
            if((token>>>4)==15) {
                if(pos>=src.length) return null;
                run=16+(src[pos++]&255);
            }
            if(run>pixelCount-consumed) return null;
            if(color!=0) for(int k=0;k<run;k++) {
                long at=consumed+k;
                int y=(int)(at/width), x=(int)(at-(long)y*width);
                if(y>=startY && y<startY+outH && x>=startX && x<startX+outW)
                    pixels[(y-startY)*outW+x-startX]=(byte)color;
            }
            consumed+=run;
        }
        if(consumed!=pixelCount) return null;
        java.io.ByteArrayOutputStream out=new java.io.ByteArrayOutputStream(Math.min(maxBytes,outW*outH+header));
        writeU16(out,u16(src,0));
        writeI32(out,i32(src,2)+(int)Math.round(startX*mpp));
        writeI32(out,i32(src,6)-(int)Math.round(startY*mpp));
        writeU16(out,outW); writeU16(out,outH); writeU16(out,rawMm);
        if(palette) out.write(src,16,32);
        int i=0, count=pixels.length;
        while(i<count) {
            int color=pixels[i]&15, run=1;
            while(i+run<count && run<271 && (pixels[i+run]&15)==color) run++;
            if(run<=15) out.write(((run-1)<<4)|color);
            else { out.write(0xf0|color); out.write(run-16); }
            i+=run;
            if(out.size()>maxBytes) return null;
        }
        return out.toByteArray();
    }

    /** Create a small legacy raster directly for memory constrained displays. */
    public static byte[] compactForSmallDisplay(byte[] src, int maxBytes, int side, double centerX, double centerY) {
        byte[] compact=compactForLegacy(src,maxBytes,side,centerX,centerY);
        if(compact==null || compact.length<17) return null;
        int rawMm=u16(compact,14);
        boolean palette=(rawMm&0x8000)!=0;
        int header=palette?48:16;
        if(compact.length<header) return null;
        if(!palette) { compact[14]=(byte)(rawMm&0xff); compact[15]=(byte)((rawMm>>>8)&0x7f); }
        byte[] encoded=new byte[compact.length-header+compact.length/8+32];
        int read=header, write=16;
        while(read<compact.length) {
            int token=compact[read++]&255, run=(token>>>4)+1;
            if((token>>>4)==15) {
                if(read>=compact.length) return null;
                run=16+(compact[read++]&255);
            }
            int color=token&15;
            if(!palette && color>3) color=color==4 || color==6 ? 2 : 0;
            while(run>0) {
                int part=Math.min(run,271);
                if(write+2>encoded.length) return null;
                if(part<=15) encoded[write++]=(byte)(((part-1)<<4)|color);
                else { encoded[write++]=(byte)(0xf0|color); encoded[write++]=(byte)(part-16); }
                run-=part;
            }
        }
        System.arraycopy(compact,0,encoded,0,16);
        compact=java.util.Arrays.copyOf(encoded,write);
        return compact.length<=maxBytes ? compact : null;
    }

    private static void writeU16(java.io.ByteArrayOutputStream out,int v) {
        out.write(v&255); out.write((v>>>8)&255);
    }
    private static void writeI32(java.io.ByteArrayOutputStream out,int v) {
        out.write(v&255); out.write((v>>>8)&255); out.write((v>>>16)&255); out.write((v>>>24)&255);
    }

    public static boolean valid(byte[] b,int frame,int x,int y) {
        if (b.length<17 || b.length>480000 || u16(b,0)!=frame || i32(b,2)!=x || i32(b,6)!=y ||
            u16(b,10)!=1200 || u16(b,12)!=1200 || (u16(b,14)!=2000 && u16(b,14)!=(2000|AmapBuildings.FLAG))) return false;
        int pos=16, pixels=0;
        while (pos<b.length && pixels<1440000) {
            int v=b[pos++]&255;
            if ((v&15)>(isBuilding(b)?7:3)) return false;
            int run=(v>>4)+1;
            if ((v>>4)==15) { if (pos==b.length) return false; run=16+(b[pos++]&255); }
            if (run>1440000-pixels) return false;
            pixels+=run;
        }
        return pixels==1440000 && pos==b.length;
    }
}
