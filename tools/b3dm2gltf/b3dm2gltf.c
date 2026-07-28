/*
 * b3dm2gltf — 反向转换 B3DM → glTF（验证 B3DM 数据正确性 / 在线预览）
 * 输出 .gltf + .bin + 纹理 PNG。注意：B3DM 是有损成品(减面/定点/烘焙)，
 * 反映的是引擎实际使用的数据，非原始模型。
 *
 * 还原范围：
 *   - 几何：VERTEX(定点→float pos/uv/oct法线) + INDEX + SUBMESH(多 primitive)
 *   - 纹理：TEXTURE → PNG + material
 *   - 骨骼蒙皮：NODE(node 树 TRS) + SKELETON(joints+IBM) + SKINVTX(JOINTS_0/WEIGHTS_0)
 *   - 动画：ANIM(每 clip 多 channel，重采样关键帧 → glTF animation samplers)
 */
#include "r3d/r3d_b3dm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static const r3d_b3dm_section_t* find_sec(const r3d_b3dm_header_t*h,
        const r3d_b3dm_section_t*tbl, uint32_t type){
    for(uint32_t i=0;i<h->section_count;i++) if(tbl[i].type==type) return &tbl[i];
    return NULL;
}

/* 动态二进制缓冲 */
typedef struct { uint8_t*p; uint32_t len, cap; } buf_t;
static void buf_ensure(buf_t*b,uint32_t add){ if(b->len+add>b->cap){ b->cap=(b->len+add)*2+256; b->p=(uint8_t*)realloc(b->p,b->cap);} }
static uint32_t buf_put(buf_t*b,const void*d,uint32_t n){ buf_ensure(b,n); uint32_t off=b->len; memcpy(b->p+b->len,d,n); b->len+=n; return off; }
static void buf_align4(buf_t*b){ while(b->len&3){ uint8_t z=0; buf_put(b,&z,1);} }

/* 反解出的每个动画通道 */
typedef struct {
    uint16_t node; uint8_t path; uint8_t comp; uint32_t frames;
    uint32_t off_time, off_val;   /* bin 内字节偏移 */
    float    tmin, tmax;
    int      bvTime, bvVal, accTime, accVal;
} chan_out_t;
typedef struct { char name[32]; float dur,fps; uint32_t nch; chan_out_t*ch; } clip_out_t;

int main(int argc,char**argv){
    if(argc<3){ fprintf(stderr,"用法: %s in.b3dm out.gltf\n",argv[0]); return 2; }
    const char *inpath=argv[1], *outgltf=argv[2];

    char base[1024]; snprintf(base,sizeof base,"%s",outgltf);
    char *dot=strrchr(base,'.'); if(dot)*dot=0;
    char *slash=strrchr(base,'/'); const char*bname=slash?slash+1:base;

    FILE*fp=fopen(inpath,"rb"); if(!fp){fprintf(stderr,"打不开 %s\n",inpath);return 1;}
    fseek(fp,0,SEEK_END); long sz=ftell(fp); fseek(fp,0,SEEK_SET);
    uint8_t*d=(uint8_t*)malloc(sz); fread(d,1,sz,fp); fclose(fp);

    const r3d_b3dm_header_t*h=(const r3d_b3dm_header_t*)d;
    if(h->magic!=R3D_B3DM_MAGIC){fprintf(stderr,"非 B3DM\n");return 1;}
    const r3d_b3dm_section_t*tbl=(const r3d_b3dm_section_t*)(d+sizeof(*h));

    const r3d_b3dm_section_t*sv=find_sec(h,tbl,R3D_SEC_VERTEX);
    const r3d_b3dm_section_t*si=find_sec(h,tbl,R3D_SEC_INDEX);
    const r3d_b3dm_section_t*ss=find_sec(h,tbl,R3D_SEC_SUBMESH);
    const r3d_b3dm_section_t*st=find_sec(h,tbl,R3D_SEC_TEXTURE);
    const r3d_b3dm_section_t*sn=find_sec(h,tbl,R3D_SEC_NODE);
    const r3d_b3dm_section_t*sk=find_sec(h,tbl,R3D_SEC_SKELETON);
    const r3d_b3dm_section_t*sx=find_sec(h,tbl,R3D_SEC_SKINVTX);
    const r3d_b3dm_section_t*sa=find_sec(h,tbl,R3D_SEC_ANIM);
    const r3d_b3dm_section_t*sm=find_sec(h,tbl,R3D_SEC_MORPH);
    if(!sv||!si){fprintf(stderr,"缺顶点/索引段\n");return 1;}

    uint32_t vcnt=sv->count, icnt=si->count;
    const r3d_b3dm_vertex_t*qv=(const r3d_b3dm_vertex_t*)(d+sv->offset);
    /* 索引统一成 32 位数组(文件可能是 16 或 32 位，由 IDX32 标志决定) */
    int idx_is32 = (h->flags & R3D_B3DM_FLAG_IDX32) ? 1 : 0;
    uint32_t *idx = (uint32_t*)malloc((size_t)(icnt?icnt:1)*sizeof(uint32_t));
    if(idx_is32){
        memcpy(idx, d+si->offset, (size_t)icnt*sizeof(uint32_t));
    } else {
        const uint16_t*i16=(const uint16_t*)(d+si->offset);
        for(uint32_t i=0;i<icnt;i++) idx[i]=i16[i];
    }

    /* 还原顶点 */
    float*pos=(float*)malloc(vcnt*3*4);
    float*nrm=(float*)malloc(vcnt*3*4);
    float*uv =(float*)malloc(vcnt*2*4);
    const float*sc=h->vertex_scale, *bi=h->vertex_bias;
    float pmin[3]={1e30f,1e30f,1e30f},pmax[3]={-1e30f,-1e30f,-1e30f};
    for(uint32_t i=0;i<vcnt;i++){
        float x=qv[i].pos[0]*sc[0]+bi[0], y=qv[i].pos[1]*sc[1]+bi[1], z=qv[i].pos[2]*sc[2]+bi[2];
        pos[i*3]=x;pos[i*3+1]=y;pos[i*3+2]=z;
        if(x<pmin[0])pmin[0]=x;if(x>pmax[0])pmax[0]=x;
        if(y<pmin[1])pmin[1]=y;if(y>pmax[1])pmax[1]=y;
        if(z<pmin[2])pmin[2]=z;if(z>pmax[2])pmax[2]=z;
        float ex=qv[i].normal_oct[0]/32767.0f, ey=qv[i].normal_oct[1]/32767.0f;
        float nz=1.0f-(ex<0?-ex:ex)-(ey<0?-ey:ey), nx=ex, ny=ey;
        if(nz<0){float ax=nx,ay=ny;nx=(1-(ay<0?-ay:ay))*(ax>=0?1:-1);ny=(1-(ax<0?-ax:ax))*(ay>=0?1:-1);}
        float il=1.0f/sqrtf(nx*nx+ny*ny+nz*nz); nrm[i*3]=nx*il;nrm[i*3+1]=ny*il;nrm[i*3+2]=nz*il;
        uv[i*2]=qv[i].uv[0]/65535.0f; uv[i*2+1]=qv[i].uv[1]/65535.0f;
    }

    /* 纹理 → PNG */
    uint32_t texcnt = st?st->count:0;
    const r3d_b3dm_texture_t*qt = st?(const r3d_b3dm_texture_t*)(d+st->offset):NULL;
    char texname[64][128];
    for(uint32_t i=0;i<texcnt && i<64;i++){
        int tw=qt[i].width,th=qt[i].height;
        const uint8_t*px=d+st->offset+qt[i].data_offset;
        uint8_t*rgba=(uint8_t*)malloc((size_t)tw*th*4);
        for(int k=0;k<tw*th;k++){ rgba[k*4]=px[k*4+2];rgba[k*4+1]=px[k*4+1];rgba[k*4+2]=px[k*4];rgba[k*4+3]=px[k*4+3]; }
        snprintf(texname[i],128,"%s_tex%u.png",bname,i);
        char tpath[1200]; snprintf(tpath,sizeof tpath,"%s_tex%u.png",base,i);
        stbi_write_png(tpath,tw,th,4,rgba,tw*4);
        free(rgba);
    }

    /* ---- 骨骼/蒙皮/动画 ---- */
    uint32_t nodecnt = sn?sn->count:0;
    const r3d_b3dm_node_t*qn = sn?(const r3d_b3dm_node_t*)(d+sn->offset):NULL;
    uint32_t jointcnt=0; const uint16_t*joint_nodes=NULL; const float*ibm=NULL;
    if(sk){
        const r3d_b3dm_skeleton_t*skh=(const r3d_b3dm_skeleton_t*)(d+sk->offset);
        jointcnt=skh->joint_count;
        ibm=(const float*)(d+sk->offset+skh->inverse_bind_offset);
        joint_nodes=(const uint16_t*)(d+sk->offset+skh->joint_node_offset);
    }
    const r3d_b3dm_skinvtx_t*skv = sx?(const r3d_b3dm_skinvtx_t*)(d+sx->offset):NULL;
    int has_skin = (nodecnt>0 && jointcnt>0 && skv!=NULL);

    /* ---- 组装 .bin ---- */
    buf_t bin={0};
    uint32_t off_pos=buf_put(&bin,pos,vcnt*3*4);
    uint32_t off_nrm=buf_put(&bin,nrm,vcnt*3*4);
    uint32_t off_uv =buf_put(&bin,uv, vcnt*2*4);

    uint32_t off_joints=0, off_weights=0;
    if(has_skin){
        uint16_t*ji=(uint16_t*)malloc(vcnt*4*2);
        float   *wf=(float*)malloc(vcnt*4*4);
        for(uint32_t i=0;i<vcnt;i++){
            float wsum=0,w4[4];
            for(int k=0;k<4;k++){ w4[k]=skv[i].weights[k]/255.0f; wsum+=w4[k]; }
            if(wsum<1e-6f){ w4[0]=1; wsum=1; }
            for(int k=0;k<4;k++){ ji[i*4+k]=skv[i].joints[k]; wf[i*4+k]=w4[k]/wsum; }
        }
        off_joints=buf_put(&bin,ji,vcnt*4*2);
        off_weights=buf_put(&bin,wf,vcnt*4*4);
        free(ji);free(wf);
    }

    buf_align4(&bin);
    uint32_t off_idx=buf_put(&bin,idx,icnt*4);   /* 32 位索引 */
    buf_align4(&bin);

    uint32_t off_ibm=0;
    if(has_skin){ off_ibm=buf_put(&bin,ibm,jointcnt*16*4); }

    /* 动画通道写进 bin */
    uint32_t anim_clipcnt=0; clip_out_t*clips=NULL;
    if(sa){
        const r3d_b3dm_anim_header_t*ah=(const r3d_b3dm_anim_header_t*)(d+sa->offset);
        anim_clipcnt=ah->clip_count;
        const r3d_b3dm_clip_t*qc=(const r3d_b3dm_clip_t*)(d+sa->offset+ah->clips_offset);
        clips=(clip_out_t*)calloc(anim_clipcnt,sizeof(clip_out_t));
        for(uint32_t c=0;c<anim_clipcnt;c++){
            memcpy(clips[c].name,qc[c].name,32); clips[c].name[31]=0;
            clips[c].dur=qc[c].duration; clips[c].fps=qc[c].fps; clips[c].nch=qc[c].channel_count;
            clips[c].ch=(chan_out_t*)calloc(qc[c].channel_count?qc[c].channel_count:1,sizeof(chan_out_t));
            const r3d_b3dm_channel_t*qch=(const r3d_b3dm_channel_t*)(d+sa->offset+qc[c].channels_offset);
            for(uint32_t k=0;k<qc[c].channel_count;k++){
                uint32_t fcnt=qch[k].frame_count, comp=qch[k].comp;
                const float*vals=(const float*)(d+sa->offset+qch[k].data_offset);
                float step=(qc[c].fps>0)?1.0f/qc[c].fps:(fcnt>1?qc[c].duration/(fcnt-1):0);
                float*times=(float*)malloc(fcnt*4);
                for(uint32_t f=0;f<fcnt;f++){ float t=f*step; if(t>qc[c].duration)t=qc[c].duration; times[f]=t; }
                chan_out_t*co=&clips[c].ch[k];
                co->node=qch[k].target_node; co->path=qch[k].path; co->comp=(uint8_t)comp; co->frames=fcnt;
                co->tmin=fcnt?times[0]:0; co->tmax=fcnt?times[fcnt-1]:0;
                co->off_time=buf_put(&bin,times,fcnt*4);
                co->off_val =buf_put(&bin,vals,fcnt*comp*4);
                free(times);
            }
        }
    }

    char binpath[1100]; snprintf(binpath,sizeof binpath,"%s.bin",base);
    char binname[600]; snprintf(binname,sizeof binname,"%s.bin",bname);

    uint32_t smcnt = ss?ss->count:1;
    const r3d_b3dm_submesh_t*qs = ss?(const r3d_b3dm_submesh_t*)(d+ss->offset):NULL;

    /* ---- MORPH：定位作用的 submesh + 写出每 target 的 VEC3 delta(全顶点对齐) ----
     * MORPH 段只覆盖某个 submesh 的顶点(连续区间)。glTF morph target 的 accessor
     * 需与 primitive 的 POSITION accessor 同 count(=vcnt)，故为每个 target 分配
     * vcnt 长的 VEC3，仅在该 submesh 顶点区间填 delta、其余为 0。 */
    uint32_t morph_tc=0, morph_vc=0; const float*morph_deltas=NULL;
    int morph_sm=-1; uint32_t morph_vbase=0;
    int *morph_acc=NULL;     /* [morph_tc] 每 target 的 accessor 号 */
    int *morph_bv=NULL;      /* [morph_tc] bufferView 号 */
    uint32_t *morph_off=NULL;/* [morph_tc] bin 偏移 */
    if(sm){
        const r3d_b3dm_morph_t*mh=(const r3d_b3dm_morph_t*)(d+sm->offset);
        morph_tc=mh->target_count; morph_vc=mh->vertex_count;
        morph_deltas=(const float*)(d+sm->offset+mh->deltas_offset);
        /* 找顶点跨度==morph_vc 的 submesh，确定 vbase(该 submesh 索引引用的最小顶点) */
        for(uint32_t s=0;s<smcnt && morph_sm<0;s++){
            uint32_t io=qs?qs[s].index_offset:0, ic=qs?qs[s].index_count:icnt;
            uint32_t mn=0xffffffffu,mx=0;
            for(uint32_t e=0;e<ic;e++){ uint32_t v=idx[io+e]; if(v<mn)mn=v; if(v>mx)mx=v; }
            if(mx>=mn && (mx-mn+1)==morph_vc){ morph_sm=(int)s; morph_vbase=mn; }
        }
        if(morph_sm<0){ morph_tc=0; } /* 没匹配到则放弃 morph */
    }

    /* ---- 写动画通道 + morph delta 到 bin ---- */
    if(morph_tc){
        morph_acc=(int*)malloc(sizeof(int)*morph_tc);
        morph_bv =(int*)malloc(sizeof(int)*morph_tc);
        morph_off=(uint32_t*)malloc(sizeof(uint32_t)*morph_tc);
        float *tmp=(float*)calloc((size_t)vcnt*3,sizeof(float));
        for(uint32_t t=0;t<morph_tc;t++){
            memset(tmp,0,(size_t)vcnt*3*sizeof(float));
            const float*src=&morph_deltas[(size_t)t*morph_vc*3];
            for(uint32_t i=0;i<morph_vc;i++){
                uint32_t dst=morph_vbase+i; if(dst>=vcnt) break;
                tmp[dst*3]=src[i*3]; tmp[dst*3+1]=src[i*3+1]; tmp[dst*3+2]=src[i*3+2];
            }
            morph_off[t]=buf_put(&bin,tmp,vcnt*3*4);
        }
        free(tmp);
    }

    FILE*bf=fopen(binpath,"wb"); if(bf){ fwrite(bin.p,1,bin.len,bf); fclose(bf); }

    /* ---- 分配 bufferView / accessor 编号 ---- */
    int bvPos=0,bvNrm=1,bvUv=2,bvIdx=3, nbv=4;
    int bvJoints=-1,bvWeights=-1,bvIbm=-1;
    if(has_skin){ bvJoints=nbv++; bvWeights=nbv++; bvIbm=nbv++; }
    for(uint32_t c=0;c<anim_clipcnt;c++)
        for(uint32_t k=0;k<clips[c].nch;k++){ clips[c].ch[k].bvTime=nbv++; clips[c].ch[k].bvVal=nbv++; }
    for(uint32_t t=0;t<morph_tc;t++) morph_bv[t]=nbv++;

    int accPos=0,accNrm=1,accUv=2, acc=3;
    int accIdx0=acc; acc+=smcnt;
    int accJoints=-1,accWeights=-1,accIbm=-1;
    if(has_skin){ accJoints=acc++; accWeights=acc++; accIbm=acc++; }
    for(uint32_t c=0;c<anim_clipcnt;c++)
        for(uint32_t k=0;k<clips[c].nch;k++){ clips[c].ch[k].accTime=acc++; clips[c].ch[k].accVal=acc++; }
    for(uint32_t t=0;t<morph_tc;t++) morph_acc[t]=acc++;

    /* ---- 写 glTF JSON ---- */
    FILE*g=fopen(outgltf,"wb");
    fprintf(g,"{\n\"asset\":{\"version\":\"2.0\",\"generator\":\"b3dm2gltf\"},\n");
    fprintf(g,"\"buffers\":[{\"uri\":\"%s\",\"byteLength\":%u}],\n",binname,bin.len);

    /* bufferViews */
    fprintf(g,"\"bufferViews\":[\n");
    fprintf(g,"{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u,\"target\":34962},\n",off_pos,vcnt*3*4);
    fprintf(g,"{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u,\"target\":34962},\n",off_nrm,vcnt*3*4);
    fprintf(g,"{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u,\"target\":34962},\n",off_uv,vcnt*2*4);
    fprintf(g,"{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u,\"target\":34963}",off_idx,icnt*4);
    if(has_skin){
        fprintf(g,",\n{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u}",off_joints,vcnt*4*2);
        fprintf(g,",\n{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u}",off_weights,vcnt*4*4);
        fprintf(g,",\n{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u}",off_ibm,jointcnt*16*4);
    }
    for(uint32_t c=0;c<anim_clipcnt;c++)
        for(uint32_t k=0;k<clips[c].nch;k++){
            chan_out_t*ch=&clips[c].ch[k];
            fprintf(g,",\n{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u}",ch->off_time,ch->frames*4);
            fprintf(g,",\n{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u}",ch->off_val,ch->frames*ch->comp*4);
        }
    for(uint32_t t=0;t<morph_tc;t++)
        fprintf(g,",\n{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u,\"target\":34962}",morph_off[t],vcnt*3*4);
    fprintf(g,"\n],\n");

    /* accessors */
    fprintf(g,"\"accessors\":[\n");
    fprintf(g,"{\"bufferView\":%d,\"componentType\":5126,\"count\":%u,\"type\":\"VEC3\",\"min\":[%g,%g,%g],\"max\":[%g,%g,%g]},\n",
        bvPos,vcnt,pmin[0],pmin[1],pmin[2],pmax[0],pmax[1],pmax[2]);
    fprintf(g,"{\"bufferView\":%d,\"componentType\":5126,\"count\":%u,\"type\":\"VEC3\"},\n",bvNrm,vcnt);
    fprintf(g,"{\"bufferView\":%d,\"componentType\":5126,\"count\":%u,\"type\":\"VEC2\"}",bvUv,vcnt);
    for(uint32_t s=0;s<smcnt;s++){
        uint32_t io=qs?qs[s].index_offset:0, ic=qs?qs[s].index_count:icnt;
        fprintf(g,",\n{\"bufferView\":%d,\"byteOffset\":%u,\"componentType\":5125,\"count\":%u,\"type\":\"SCALAR\"}",bvIdx,io*4,ic);
    }
    if(has_skin){
        fprintf(g,",\n{\"bufferView\":%d,\"componentType\":5123,\"count\":%u,\"type\":\"VEC4\"}",bvJoints,vcnt);    /* JOINTS u16 */
        fprintf(g,",\n{\"bufferView\":%d,\"componentType\":5126,\"count\":%u,\"type\":\"VEC4\"}",bvWeights,vcnt);   /* WEIGHTS f32 */
        fprintf(g,",\n{\"bufferView\":%d,\"componentType\":5126,\"count\":%u,\"type\":\"MAT4\"}",bvIbm,jointcnt);    /* IBM */
    }
    for(uint32_t c=0;c<anim_clipcnt;c++)
        for(uint32_t k=0;k<clips[c].nch;k++){
            chan_out_t*ch=&clips[c].ch[k];
            int isw=(ch->path==R3D_ANIM_PATH_W);
            const char*vt = isw?"SCALAR":((ch->comp==4)?"VEC4":(ch->comp==3?"VEC3":(ch->comp==2?"VEC2":"SCALAR")));
            /* weights: output 为 SCALAR，count=frames*comp(每帧 comp 个权重)；
             * T/R/S: VECn，count=frames */
            uint32_t outcount = isw ? ch->frames*ch->comp : ch->frames;
            fprintf(g,",\n{\"bufferView\":%d,\"componentType\":5126,\"count\":%u,\"type\":\"SCALAR\",\"min\":[%g],\"max\":[%g]}",
                ch->bvTime,ch->frames,ch->tmin,ch->tmax);
            fprintf(g,",\n{\"bufferView\":%d,\"componentType\":5126,\"count\":%u,\"type\":\"%s\"}",ch->bvVal,outcount,vt);
        }
    for(uint32_t t=0;t<morph_tc;t++)
        fprintf(g,",\n{\"bufferView\":%d,\"componentType\":5126,\"count\":%u,\"type\":\"VEC3\"}",morph_bv[t],vcnt);
    fprintf(g,"\n],\n");

    /* images + textures */
    if(texcnt){
        fprintf(g,"\"images\":[");
        for(uint32_t i=0;i<texcnt;i++) fprintf(g,"%s{\"uri\":\"%s\"}",i?",":"",texname[i]);
        fprintf(g,"],\n\"textures\":[");
        for(uint32_t i=0;i<texcnt;i++) fprintf(g,"%s{\"source\":%u}",i?",":"",i);
        fprintf(g,"],\n");
    }

    /* materials */
    fprintf(g,"\"materials\":[\n");
    for(uint32_t s=0;s<smcnt;s++){
        uint32_t bcf=qs?qs[s].base_color_factor:0xFFFFFFFFu;
        float a=((bcf>>24)&0xFF)/255.0f,r=((bcf>>16)&0xFF)/255.0f,gg=((bcf>>8)&0xFF)/255.0f,b=(bcf&0xFF)/255.0f;
        int tid=qs?qs[s].tex_id:0xFFFF;
        int ds=qs?(qs[s].mat_flags&1):0;
        /* 半透明(mat_flags bit3=R3D_MAT_TRANSLUCENT，或 base color alpha<255)需显式
         * 输出 alphaMode=BLEND，否则 glTF 默认 OPAQUE，three.js 预览会忽略 alpha 把
         * 玻璃/透明面渲染成不透明、遮住其后的表盘(设备侧靠 TRANSLUCENT 标志本已正确)。 */
        int tl=qs?((qs[s].mat_flags&8) || (((bcf>>24)&0xFF)<255)):0;
        fprintf(g,"%s{\"pbrMetallicRoughness\":{\"baseColorFactor\":[%g,%g,%g,%g],\"metallicFactor\":0,\"roughnessFactor\":0.8",
            s?",\n":"",r,gg,b,a);
        if(tid!=0xFFFF && (uint32_t)tid<texcnt) fprintf(g,",\"baseColorTexture\":{\"index\":%d}",tid);
        fprintf(g,"}%s%s}", tl?",\"alphaMode\":\"BLEND\"":"", ds?",\"doubleSided\":true":"");
    }
    fprintf(g,"\n],\n");

    /* meshes：每个 submesh 一个独立 mesh（便于按 node_id 挂到 NODE 树对应节点，
     * 使各 node 的 TRS(尤其量化模型的 scale)正确作用；否则动态/量化顶点会错位)。
     * morph 作用的那个 submesh 额外带 targets + 默认 weights。 */
    fprintf(g,"\"meshes\":[\n");
    for(uint32_t s=0;s<smcnt;s++){
        fprintf(g,"%s{\"primitives\":[{\"attributes\":{\"POSITION\":%d,\"NORMAL\":%d,\"TEXCOORD_0\":%d",
            s?",\n":"", accPos,accNrm,accUv);
        if(has_skin) fprintf(g,",\"JOINTS_0\":%d,\"WEIGHTS_0\":%d",accJoints,accWeights);
        fprintf(g,"},\"indices\":%d,\"material\":%u", accIdx0+(int)s, s);
        if(morph_tc && (int)s==morph_sm){
            fprintf(g,",\"targets\":[");
            for(uint32_t t=0;t<morph_tc;t++) fprintf(g,"%s{\"POSITION\":%d}",t?",":"",morph_acc[t]);
            fprintf(g,"]");
        }
        fprintf(g,"}]");
        if(morph_tc && (int)s==morph_sm){
            fprintf(g,",\"weights\":[");
            for(uint32_t t=0;t<morph_tc;t++) fprintf(g,"%s0",t?",":"");
            fprintf(g,"]");
        }
        fprintf(g,"}");
    }
    fprintf(g,"\n],\n");

    /* nodes / skins / scene
     *  (a) 有 NODE 段：输出完整 node 树(children 由 parent 反推)，并把每个
     *      submesh-mesh 挂到其 node_id 对应的 node(mesh 属性)。这样 node 链的
     *      TRS(量化模型的 scale/translation)会正确作用，动态顶点不再错位。
     *      同一 node 被多 submesh 引用时，第一个直接挂，其余追加为子节点。
     *  (b) 无 NODE 段，或纯静态模型：每个 submesh 一个引用 mesh 的根 node。
     *
     * 关键：静态(非 skin/morph)模型的顶点已在离线烘焙到世界空间(乘过各 node 的
     * 世界矩阵)，若再用 node 树的 TRS 就会二次变换 → 零件错位(如 ferrari 轮子飞出)。
     * 故仅当存在 skin 或 morph(顶点未烘焙、依赖运行时节点/蒙皮变换)时才输出 node 树；
     * 纯静态模型一律走扁平单位 node 路径。 */
    /* 节点动画(无 skin/morph)也需输出 node 树：动态顶点(带 DYNAMIC_NODE 标志的
     * submesh，如表针)未烘焙，依赖 node 树父链的 rest TRS(例如表针父节点 Hands 的
     * -90°X 旋转)把局部旋转重定向到表盘平面；走扁平路径会丢掉父链、动画在世界系里
     * 绕错轴 → 秒针方向不对。且扁平路径下动画通道 target 的是 node 索引，与按 submesh
     * 排布的扁平节点不对齐，本就无法正确驱动。 */
    int has_node_anim = (nodecnt>0) && (anim_clipcnt>0);
    int tree_all = (has_skin || morph_tc>0);   /* 顶点未烘焙：所有 submesh 都挂 node 树 */
    int need_node_tree = (nodecnt>0) && (tree_all || has_node_anim);
    if(need_node_tree){
        int *node_mesh=(int*)malloc(sizeof(int)*nodecnt);
        for(uint32_t i=0;i<nodecnt;i++) node_mesh[i]=-1;
        int *extra_sm=(int*)malloc(sizeof(int)*(smcnt?smcnt:1));
        int *extra_parent=(int*)malloc(sizeof(int)*(smcnt?smcnt:1));
        int extra_n=0;
        for(uint32_t s=0;s<smcnt;s++){
            int nid = qs ? (int)qs[s].node_id : -1;
            /* 纯节点动画模式：仅动态 submesh(顶点未烘焙)挂到 node 树；静态 submesh 顶点
             * 已烘焙到世界空间，必须走扁平单位 node，否则会被 node TRS 二次变换而错位。
             * skin/morph 模式(tree_all)下顶点均未烘焙，仍全部挂树，保持原行为。 */
            int dynamic = qs ? (qs[s].mat_flags & 16) : 0;  /* R3D_MAT_DYNAMIC_NODE */
            int attach_tree = tree_all || dynamic;
            if(attach_tree && nid>=0 && nid<(int)nodecnt){
                if(node_mesh[nid]<0) node_mesh[nid]=(int)s;
                else { extra_sm[extra_n]=(int)s; extra_parent[extra_n]=nid; extra_n++; }
            } else { extra_sm[extra_n]=(int)s; extra_parent[extra_n]=-1; extra_n++; }
        }

        fprintf(g,"\"nodes\":[\n");
        for(uint32_t i=0;i<nodecnt;i++){
            const r3d_b3dm_node_t*n=&qn[i];
            fprintf(g,"%s{",i?",\n":"");
            int first=1;
            for(uint32_t j=0;j<nodecnt;j++) if(qn[j].parent==(int16_t)i){
                if(first){ fprintf(g,"\"children\":["); first=0; } else fprintf(g,",");
                fprintf(g,"%u",j);
            }
            for(int e=0;e<extra_n;e++) if(extra_parent[e]==(int)i){
                if(first){ fprintf(g,"\"children\":["); first=0; } else fprintf(g,",");
                fprintf(g,"%u",nodecnt+(uint32_t)e);
            }
            if(!first) fprintf(g,"],");
            if(node_mesh[i]>=0){
                if(has_skin) fprintf(g,"\"mesh\":%d,\"skin\":0,",node_mesh[i]);
                else         fprintf(g,"\"mesh\":%d,",node_mesh[i]);
            }
            fprintf(g,"\"translation\":[%g,%g,%g],\"rotation\":[%g,%g,%g,%g],\"scale\":[%g,%g,%g]}",
                n->translation[0],n->translation[1],n->translation[2],
                n->rotation[0],n->rotation[1],n->rotation[2],n->rotation[3],
                n->scale[0],n->scale[1],n->scale[2]);
        }
        for(int e=0;e<extra_n;e++){
            fprintf(g,",\n{");
            if(has_skin) fprintf(g,"\"mesh\":%d,\"skin\":0,",extra_sm[e]);
            else         fprintf(g,"\"mesh\":%d,",extra_sm[e]);
            fprintf(g,"\"translation\":[0,0,0],\"rotation\":[0,0,0,1],\"scale\":[1,1,1]}");
        }
        fprintf(g,"\n],\n");

        if(has_skin){
            fprintf(g,"\"skins\":[{\"inverseBindMatrices\":%d,\"joints\":[",accIbm);
            for(uint32_t j=0;j<jointcnt;j++) fprintf(g,"%s%u",j?",":"",joint_nodes[j]);
            fprintf(g,"]}],\n");
        }

        fprintf(g,"\"scenes\":[{\"nodes\":[");
        int first=1;
        for(uint32_t i=0;i<nodecnt;i++) if(qn[i].parent<0){ fprintf(g,"%s%u",first?"":",",i); first=0; }
        for(int e=0;e<extra_n;e++) if(extra_parent[e]<0){ fprintf(g,"%s%u",first?"":",",nodecnt+(uint32_t)e); first=0; }
        fprintf(g,"]}],\n\"scene\":0");

        free(node_mesh); free(extra_sm); free(extra_parent);
    } else {
        fprintf(g,"\"nodes\":[");
        for(uint32_t s=0;s<smcnt;s++) fprintf(g,"%s{\"mesh\":%u}",s?",":"",s);
        fprintf(g,"],\n\"scenes\":[{\"nodes\":[");
        for(uint32_t s=0;s<smcnt;s++) fprintf(g,"%s%u",s?",":"",s);
        fprintf(g,"]}],\n\"scene\":0");
    }

    /* animations：还原 T/R/S 通道；morph 还原后(morph_tc>0)也还原 weights 通道。
     * weights 通道 output 为 frame_count×morph_tc 标量，作用到承载 morph mesh 的 node。 */
    if(anim_clipcnt){
        const char*pathname[4]={"translation","rotation","scale","weights"};
        /* morph mesh 所属 node(用于 weights 通道 target) */
        int morph_node = (morph_tc && morph_sm>=0 && qs) ? (int)qs[morph_sm].node_id : -1;
        /* 某 clip 有任意可还原通道(T/R/S，或 morph 有效时的 W)才输出 */
        int any_clip=0;
        for(uint32_t c=0;c<anim_clipcnt;c++)
            for(uint32_t k=0;k<clips[c].nch;k++){
                int isw=(clips[c].ch[k].path==R3D_ANIM_PATH_W);
                if(!isw || (morph_tc && morph_node>=0)){ any_clip=1; break; }
            }
        if(any_clip){
        fprintf(g,",\n\"animations\":[\n");
        int wrote_clip=0;
        for(uint32_t c=0;c<anim_clipcnt;c++){
            int valid=0;
            for(uint32_t k=0;k<clips[c].nch;k++){
                int isw=(clips[c].ch[k].path==R3D_ANIM_PATH_W);
                if(!isw || (morph_tc && morph_node>=0)) valid++;
            }
            if(!valid) continue;
            fprintf(g,"%s{\"name\":\"%s\",\"samplers\":[",wrote_clip?",\n":"",clips[c].name);
            int si2=0;
            for(uint32_t k=0;k<clips[c].nch;k++){
                chan_out_t*ch=&clips[c].ch[k];
                int isw=(ch->path==R3D_ANIM_PATH_W);
                if(isw && !(morph_tc && morph_node>=0)) continue;
                fprintf(g,"%s{\"input\":%d,\"output\":%d,\"interpolation\":\"LINEAR\"}",si2?",":"",ch->accTime,ch->accVal);
                si2++;
            }
            fprintf(g,"],\"channels\":[");
            int ci2=0; si2=0;
            for(uint32_t k=0;k<clips[c].nch;k++){
                chan_out_t*ch=&clips[c].ch[k];
                int isw=(ch->path==R3D_ANIM_PATH_W);
                if(isw && !(morph_tc && morph_node>=0)){ continue; }
                const char*pn = ch->path<4?pathname[ch->path]:"translation";
                uint32_t tnode = isw ? (uint32_t)morph_node : ch->node;
                fprintf(g,"%s{\"sampler\":%d,\"target\":{\"node\":%u,\"path\":\"%s\"}}",ci2?",":"",si2,tnode,pn);
                ci2++; si2++;
            }
            fprintf(g,"]}");
            wrote_clip++;
        }
        fprintf(g,"\n]");
        }
    }

    fprintf(g,"\n}\n");
    fclose(g);

    printf("OK: %s → %s (+%s +%u PNG)\n  顶点%u 索引%u submesh%u 纹理%u 骨骼%u 动画%u\n",
        inpath,outgltf,binname,texcnt,vcnt,icnt,smcnt,texcnt,jointcnt,anim_clipcnt);

    if(clips){ for(uint32_t c=0;c<anim_clipcnt;c++) free(clips[c].ch); free(clips); }
    if(morph_acc) free(morph_acc);
    if(morph_bv) free(morph_bv);
    if(morph_off) free(morph_off);
    free(d);free(pos);free(nrm);free(uv);free(bin.p);
    return 0;
}
