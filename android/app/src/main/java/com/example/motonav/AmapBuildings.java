package com.example.motonav;

/** 只提取高德建筑层开关差分得到的轮廓，不从普通地面颜色猜测或虚构房屋。 */
public final class AmapBuildings {
    public static final int FLAG=0x4000;
    /** v2 高度格式（固件 CAP,BLD,2）：类 8-15，低 2 位 = 高度档 0..3，bit2 = 绿地/水基底。 */
    public static final int HFLAG=0x2000;
    public static final int ROOF=4, OUTLINE=5;
    /** 连通域面积 → 伪高度档（2 米/像素，面积为 tail*4 平方米）。 */
    private static int heightBucket(int areaPx) {
        if(areaPx<200)return 0;      // <800m²：平房，不挤出
        if(areaPx<800)return 1;      // <3200m²
        if(areaPx<2400)return 2;     // <9600m²
        return 3;
    }
    private AmapBuildings() {}
    public static boolean marked(int color) {
        int r=(color>>>16)&255,g=(color>>>8)&255,b=color&255;
        return r>=140 && b>=140 && g*2<Math.min(r,b);
    }
    public static final class Result {
        public final byte[] pixels;
        public final int buildings, roofPixels;
        Result(byte[] pixels,int buildings,int roofPixels){this.pixels=pixels;this.buildings=buildings;this.roofPixels=roofPixels;}
    }
    /** mask 里 3..6 = 有效屋顶 + 高度档 0..3，7 = 无效碎片。 */
    private static boolean roofQ(int m){return m>=3&&m<=6;}
    public static Result apply(int[] raw,byte[] base,int w,int h) { return apply(raw,base,w,h,false); }
    public static Result apply(int[] raw,byte[] base,int w,int h,boolean withHeights) {
        if(w<=0 || h<=0 || (long)w*h!=raw.length || raw.length!=base.length)throw new IllegalArgumentException("楼块图尺寸不一致");
        byte[] mask=new byte[raw.length];int marked=0;
        for(int i=0;i<raw.length;i++)if(marked(raw[i])){mask[i]=1;marked++;}
        if(marked>raw.length/2)throw new IllegalArgumentException("建筑图层覆盖异常，保留当前地图");
        int[] component=new int[raw.length];int buildings=0;
        for(int i=0;i<mask.length;i++){
            if(mask[i]!=1)continue;
            int head=0,tail=1,minX=i%w,maxX=minX,minY=i/w,maxY=minY;component[0]=i;mask[i]=2;
            while(head<tail){
                int p=component[head++],x=p%w,y=p/w;
                minX=Math.min(minX,x);maxX=Math.max(maxX,x);minY=Math.min(minY,y);maxY=Math.max(maxY,y);
                if(x>0 && mask[p-1]==1){mask[p-1]=2;component[tail++]=p-1;}
                if(x+1<w && mask[p+1]==1){mask[p+1]=2;component[tail++]=p+1;}
                if(y>0 && mask[p-w]==1){mask[p-w]=2;component[tail++]=p-w;}
                if(y+1<h && mask[p+w]==1){mask[p+w]=2;component[tail++]=p+w;}
            }
            int bw=maxX-minX+1,bh=maxY-minY+1;
            boolean valid=tail>=6 && tail<=raw.length/8 && bw>=2 && bh>=2 && (long)tail*8>=(long)bw*bh;
            int height=withHeights&&valid?heightBucket(tail):0;
            if(valid)buildings++;
            for(int j=0;j<tail;j++)mask[component[j]]=(byte)(valid?(3+height):7);
        }
        byte[] out=base.clone();int pixels=0;
        for(int y=0;y<h;y++)for(int x=0;x<w;x++){
            int i=y*w+x;
            // 道路和水面优先；保留屋顶下原本是地面还是绿地，供旧固件无损回退。
            if(!roofQ(mask[i]) || (base[i]!=0 && base[i]!=2))continue;
            if(withHeights){
                // v2 高度格式：8 | 高度档 | (绿地/水基底<<2)；mask 3..6 = 屋顶 + 高度档 0..3
                out[i]=(byte)(8|(mask[i]-3)|((base[i]&2)<<1));pixels++;
            }else{
                boolean edge=x==0 || x+1==w || y==0 || y+1==h || !roofQ(mask[i-1]) || !roofQ(mask[i+1]) || !roofQ(mask[i-w]) || !roofQ(mask[i+w]);
                out[i]=(byte)((edge?OUTLINE:ROOF)|(base[i]&2));pixels++;
            }
        }
        return new Result(out,buildings,pixels);
    }
}
