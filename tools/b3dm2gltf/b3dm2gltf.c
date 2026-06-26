/*
 * b3dm2gltf — 反向转换 B3DM → glTF（验证 B3DM 数据正确性）
 * 输出 .gltf + .bin + 纹理 PNG。注意：B3DM 是有损成品(减面/定点/烘焙)，
 * 反映的是引擎实际使用的数据，非原始模型。
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

int main(int argc,char**argv){
    if(argc<3){ fprintf(stderr,"用法: %s in.b3dm out.gltf\n",argv[0]); return 2; }
    const char *inpath=argv[1], *outgltf=argv[2];

    /* 输出基名(去 .gltf)用于 .bin 和纹理 */
    char base[1024]; snprintf(base,sizeof base,"%s",outgltf);
    char *dot=strrchr(base,'.'); if(dot)*dot=0;
    char *slash=strrchr(base,'/'); const char*bname=slash?slash+1:base;

    /* 读文件 */
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
    if(!sv||!si){fprintf(stderr,"缺顶点/索引段\n");return 1;}

    uint32_t vcnt=sv->count, icnt=si->count;
    const r3d_b3dm_vertex_t*qv=(const r3d_b3dm_vertex_t*)(d+sv->offset);
    const uint16_t*idx=(const uint16_t*)(d+si->offset);

    /* 还原顶点：POSITION(定点*scale+bias), NORMAL(oct解码), TEXCOORD */
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
        /* oct 解码 */
        float ex=qv[i].normal_oct[0]/32767.0f, ey=qv[i].normal_oct[1]/32767.0f;
        float nz=1.0f-(ex<0?-ex:ex)-(ey<0?-ey:ey), nx=ex, ny=ey;
        if(nz<0){float ax=nx,ay=ny;nx=(1-(ay<0?-ay:ay))*(ax>=0?1:-1);ny=(1-(ax<0?-ax:ax))*(ay>=0?1:-1);}
        float il=1.0f/sqrtf(nx*nx+ny*ny+nz*nz); nrm[i*3]=nx*il;nrm[i*3+1]=ny*il;nrm[i*3+2]=nz*il;
        uv[i*2]=qv[i].uv[0]/65535.0f; uv[i*2+1]=qv[i].uv[1]/65535.0f;
    }

    /* 导出纹理为 PNG（BGRA→RGBA）*/
    uint32_t texcnt = st?st->count:0;
    const r3d_b3dm_texture_t*qt = st?(const r3d_b3dm_texture_t*)(d+st->offset):NULL;
    char texname[64][128];
    for(uint32_t i=0;i<texcnt;i++){
        int tw=qt[i].width,th=qt[i].height;
        const uint8_t*px=d+st->offset+qt[i].data_offset;
        uint8_t*rgba=(uint8_t*)malloc((size_t)tw*th*4);
        for(int k=0;k<tw*th;k++){ rgba[k*4]=px[k*4+2];rgba[k*4+1]=px[k*4+1];rgba[k*4+2]=px[k*4];rgba[k*4+3]=px[k*4+3]; }
        snprintf(texname[i],128,"%s_tex%u.png",bname,i);
        char tpath[1200]; snprintf(tpath,sizeof tpath,"%s_tex%u.png",base,i);
        stbi_write_png(tpath,tw,th,4,rgba,tw*4);
        free(rgba);
    }

    /* 写 .bin: pos + nrm + uv + indices */
    char binpath[1100]; snprintf(binpath,sizeof binpath,"%s.bin",base);
    char binname[600]; snprintf(binname,sizeof binname,"%s.bin",bname);
    FILE*bf=fopen(binpath,"wb");
    uint32_t off_pos=0; fwrite(pos,4,vcnt*3,bf);
    uint32_t off_nrm=vcnt*3*4; fwrite(nrm,4,vcnt*3,bf);
    uint32_t off_uv=off_nrm+vcnt*3*4; fwrite(uv,4,vcnt*2,bf);
    uint32_t off_idx=off_uv+vcnt*2*4; fwrite(idx,2,icnt,bf);
    uint32_t binlen=off_idx+icnt*2;
    if(icnt*2 & 3){ uint8_t z[2]={0,0}; fwrite(z,1,2,bf); binlen+=2; } /* 4对齐 */
    fclose(bf);

    /* submesh → primitives */
    uint32_t smcnt = ss?ss->count:1;
    const r3d_b3dm_submesh_t*qs = ss?(const r3d_b3dm_submesh_t*)(d+ss->offset):NULL;

    /* 写 glTF JSON */
    FILE*g=fopen(outgltf,"wb");
    fprintf(g,"{\n\"asset\":{\"version\":\"2.0\",\"generator\":\"b3dm2gltf\"},\n");
    fprintf(g,"\"buffers\":[{\"uri\":\"%s\",\"byteLength\":%u}],\n",binname,binlen);
    /* bufferViews: 0=pos 1=nrm 2=uv 3=idx */
    fprintf(g,"\"bufferViews\":[\n");
    fprintf(g,"{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u,\"target\":34962},\n",off_pos,vcnt*3*4);
    fprintf(g,"{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u,\"target\":34962},\n",off_nrm,vcnt*3*4);
    fprintf(g,"{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u,\"target\":34962},\n",off_uv,vcnt*2*4);
    fprintf(g,"{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u,\"target\":34963}\n],\n",off_idx,icnt*2);
    /* accessors: 0=pos 1=nrm 2=uv 3..=每submesh索引 */
    fprintf(g,"\"accessors\":[\n");
    fprintf(g,"{\"bufferView\":0,\"componentType\":5126,\"count\":%u,\"type\":\"VEC3\",\"min\":[%g,%g,%g],\"max\":[%g,%g,%g]},\n",
        vcnt,pmin[0],pmin[1],pmin[2],pmax[0],pmax[1],pmax[2]);
    fprintf(g,"{\"bufferView\":1,\"componentType\":5126,\"count\":%u,\"type\":\"VEC3\"},\n",vcnt);
    fprintf(g,"{\"bufferView\":2,\"componentType\":5126,\"count\":%u,\"type\":\"VEC2\"}",vcnt);
    for(uint32_t s=0;s<smcnt;s++){
        uint32_t io = qs?qs[s].index_offset:0, ic = qs?qs[s].index_count:icnt;
        fprintf(g,",\n{\"bufferView\":3,\"byteOffset\":%u,\"componentType\":5123,\"count\":%u,\"type\":\"SCALAR\"}",io*2,ic);
    }
    fprintf(g,"\n],\n");
    /* images + textures */
    if(texcnt){
        fprintf(g,"\"images\":[");
        for(uint32_t i=0;i<texcnt;i++) fprintf(g,"%s{\"uri\":\"%s\"}",i?",":"",texname[i]);
        fprintf(g,"],\n\"textures\":[");
        for(uint32_t i=0;i<texcnt;i++) fprintf(g,"%s{\"source\":%u}",i?",":"",i);
        fprintf(g,"],\n");
    }
    /* materials: 每 submesh 一个 */
    fprintf(g,"\"materials\":[\n");
    for(uint32_t s=0;s<smcnt;s++){
        uint32_t bcf = qs?qs[s].base_color_factor:0xFFFFFFFFu;
        float a=((bcf>>24)&0xFF)/255.0f,r=((bcf>>16)&0xFF)/255.0f,gg=((bcf>>8)&0xFF)/255.0f,b=(bcf&0xFF)/255.0f;
        int tid = qs?qs[s].tex_id:0xFFFF;
        int ds = qs?(qs[s].mat_flags&1):0;
        fprintf(g,"%s{\"pbrMetallicRoughness\":{\"baseColorFactor\":[%g,%g,%g,%g],\"metallicFactor\":0,\"roughnessFactor\":0.8",
            s?",\n":"",r,gg,b,a);
        if(tid!=0xFFFF && (uint32_t)tid<texcnt)
            fprintf(g,",\"baseColorTexture\":{\"index\":%d}",tid);
        fprintf(g,"}%s}", ds?",\"doubleSided\":true":"");
    }
    fprintf(g,"\n],\n");
    /* mesh: 一个 mesh 多 primitive */
    fprintf(g,"\"meshes\":[{\"primitives\":[\n");
    for(uint32_t s=0;s<smcnt;s++){
        fprintf(g,"%s{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":%u,\"material\":%u}",
            s?",\n":"", 3+s, s);
    }
    fprintf(g,"\n]}],\n");
    fprintf(g,"\"nodes\":[{\"mesh\":0}],\n\"scenes\":[{\"nodes\":[0]}],\n\"scene\":0\n}\n");
    fclose(g);

    printf("OK: %s → %s (+%s +%u PNG)\n  顶点%u 索引%u submesh%u 纹理%u\n",
        inpath,outgltf,binname,texcnt,vcnt,icnt,smcnt,texcnt);
    free(d);free(pos);free(nrm);free(uv);
    return 0;
}
