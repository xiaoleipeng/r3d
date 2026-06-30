/*
 * backend_opengl.c — OpenGL ES 2.0 后端（Linux 开发优先，架构文档 §7）
 * GLFW 建窗口/ES 上下文；GPU 投影 + z-buffer，无需 CPU 排序。
 */
#include "r3d/r3d_backend.h"
#include <GLFW/glfw3.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    GLFWwindow *win;
    int         win_w, win_h;
    GLuint      prog;
    GLint       u_mvp, u_light, u_tint, u_use_tex, u_flat, u_v3, u_matcap;
    GLint       a_pos, a_uv, a_nrm;
    GLuint      vbo, ebo;
    const void *last_vptr; uint32_t last_vcount;
    const void *last_iptr; uint32_t last_icount;
    r3d_mat4_t  view, proj;
    r3d_target_t target;
    double      scroll_accum;   /* 滚轮累积(回调写入) */
} gl_impl_t;

/* ---- shader 源（GLSL ES 100）---- */
static const char *VS_SRC =
    "uniform mat4 u_mvp;\n"
    "uniform mat3 u_v3;\n"
    "uniform vec3 u_light;\n"
    "uniform float u_flat;\n"
    "attribute vec3 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "attribute vec3 a_nrm;\n"
    "attribute float a_ao;\n"
    "attribute vec4 a_col;\n"
    "varying vec2 v_uv;\n"
    "varying vec3 v_nv;\n"
    "varying float v_ao;\n"
    "varying vec4 v_col;\n"
    "varying float v_flat;\n"
    "void main(){\n"
    "  gl_Position = u_mvp * vec4(a_pos,1.0);\n"
    "  v_uv = a_uv;\n"
    "  v_nv = u_v3 * a_nrm;\n"
    "  v_ao = a_ao;\n"
    "  v_col = a_col;\n"
    "  v_flat = u_flat;\n"
    "}\n";

static const char *FS_SRC =
    "precision mediump float;\n"
    "uniform sampler2D u_tex;\n"
    "uniform float u_use_tex;\n"
    "uniform float u_matcap;\n"
    "uniform vec4  u_tint;\n"
    "uniform vec3  u_light;\n"
    "varying vec2 v_uv;\n"
    "varying vec3 v_nv;\n"
    "varying float v_ao;\n"
    "varying vec4 v_col;\n"
    "varying float v_flat;\n"
    "void main(){\n"
    "  vec3 nv = normalize(v_nv);\n"
    "  if(u_matcap > 0.5){\n"                                  /* matcap 金属 */
    "    vec2 muv = nv.xy*0.5 + 0.5;\n"
    "    vec3 mc = texture2D(u_tex, muv).rgb;\n"
    "    gl_FragColor = vec4(mc * u_tint.rgb * v_ao, u_tint.a);\n"
    "    return;\n"
    "  }\n"
    /* 无纹理且有顶点色(baked-vertex 模式)时优先用顶点色，否则用 tint。
       顶点色按 ARGB u32 存储，小端逐字节读入为 (B,G,R,A)，故取 .bgra 还原 RGBA */
    "  vec4 vcol = v_col.bgra;\n"
    "  vec4 tint = mix(u_tint, vcol, (1.0 - u_use_tex) * step(0.001, vcol.a));\n"
    "  vec4 base = mix(tint, texture2D(u_tex, v_uv)*u_tint, u_use_tex);\n"
    "  vec3 L = normalize(u_light);\n"
    "  vec3 V = vec3(0.0,0.0,1.0);\n"
    "  vec3 H = normalize(L+V);\n"
    "  float d = max(dot(nv,L),0.0);\n"
    "  float hemi = 0.5+0.5*nv.y;\n"
    "  float spec = pow(max(dot(nv,H),0.0), 28.0);\n"
    "  float lit = (0.50 + 0.32*d + 0.18*hemi) * v_ao;\n"
    "  vec3 col = mix(base.rgb, base.rgb*lit + vec3(0.45*spec), v_flat);\n"
    "  gl_FragColor = vec4(col, base.a);\n"
    "}\n";

static GLuint compile(GLenum type, const char *src){
    GLuint s=glCreateShader(type); glShaderSource(s,1,&src,NULL); glCompileShader(s);
    GLint ok=0; glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(!ok){ char log[512]; glGetShaderInfoLog(s,512,NULL,log); fprintf(stderr,"shader: %s\n",log); }
    return s;
}

static void gl_scroll_cb(GLFWwindow *w, double dx, double dy){
    (void)dx;
    gl_impl_t *im=(gl_impl_t*)glfwGetWindowUserPointer(w);
    if(im) im->scroll_accum += dy;
}

static r3d_result_t gl_init(r3d_backend_t *self, const r3d_backend_cfg_t *cfg){
    (void)cfg;
    gl_impl_t *im=(gl_impl_t*)self->impl;
    if(!glfwInit()){ fprintf(stderr,"glfwInit 失败\n"); return R3D_ERR_UNSUPPORTED; }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    im->win_w = im->win_w?im->win_w:800; im->win_h = im->win_h?im->win_h:600;
    im->win = glfwCreateWindow(im->win_w, im->win_h, "r3d (OpenGL ES)", NULL, NULL);
    if(!im->win){ fprintf(stderr,"创建窗口失败\n"); glfwTerminate(); return R3D_ERR_UNSUPPORTED; }
    glfwMakeContextCurrent(im->win);
    glfwSetWindowUserPointer(im->win, im);
    glfwSetScrollCallback(im->win, gl_scroll_cb);

    GLuint vs=compile(GL_VERTEX_SHADER,VS_SRC), fs=compile(GL_FRAGMENT_SHADER,FS_SRC);
    im->prog=glCreateProgram();
    glAttachShader(im->prog,vs); glAttachShader(im->prog,fs);
    glBindAttribLocation(im->prog,0,"a_pos");
    glBindAttribLocation(im->prog,1,"a_uv");
    glBindAttribLocation(im->prog,2,"a_nrm");
    glBindAttribLocation(im->prog,3,"a_ao");
    glBindAttribLocation(im->prog,4,"a_col");
    glLinkProgram(im->prog);
    GLint ok=0; glGetProgramiv(im->prog,GL_LINK_STATUS,&ok);
    if(!ok){ char log[512]; glGetProgramInfoLog(im->prog,512,NULL,log); fprintf(stderr,"link: %s\n",log); return R3D_ERR_UNSUPPORTED; }
    glDeleteShader(vs); glDeleteShader(fs);

    im->a_pos=0; im->a_uv=1; im->a_nrm=2;
    im->u_mvp=glGetUniformLocation(im->prog,"u_mvp");
    im->u_light=glGetUniformLocation(im->prog,"u_light");
    im->u_tint=glGetUniformLocation(im->prog,"u_tint");
    im->u_use_tex=glGetUniformLocation(im->prog,"u_use_tex");
    im->u_flat=glGetUniformLocation(im->prog,"u_flat");
    im->u_v3=glGetUniformLocation(im->prog,"u_v3");
    im->u_matcap=glGetUniformLocation(im->prog,"u_matcap");

    glGenBuffers(1,&im->vbo); glGenBuffers(1,&im->ebo);
    glEnable(GL_DEPTH_TEST);
    return R3D_OK;
}

static void gl_destroy(r3d_backend_t *self){
    if(!self) return;
    gl_impl_t *im=(gl_impl_t*)self->impl;
    if(im){
        if(im->vbo)glDeleteBuffers(1,&im->vbo);
        if(im->ebo)glDeleteBuffers(1,&im->ebo);
        if(im->prog)glDeleteProgram(im->prog);
        if(im->win)glfwDestroyWindow(im->win);
        glfwTerminate();
        free(im);
    }
    free(self);
}

static r3d_texture_handle_t gl_create_texture(r3d_backend_t *self, const r3d_image_t *img){
    (void)self;
    GLuint tex; glGenTextures(1,&tex);
    glBindTexture(GL_TEXTURE_2D,tex);
    /* B3DM 存的是 BGRA 预乘；GLES2 无 GL_BGRA 上传，做 BGRA→RGBA 交换 */
    uint32_t n=img->w*img->h;
    uint8_t *rgba=(uint8_t*)malloc((size_t)n*4);
    const uint8_t *s=(const uint8_t*)img->data;
    for(uint32_t i=0;i<n;i++){ rgba[i*4+0]=s[i*4+2]; rgba[i*4+1]=s[i*4+1]; rgba[i*4+2]=s[i*4+0]; rgba[i*4+3]=s[i*4+3]; }
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,img->w,img->h,0,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
    free(rgba);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    return (r3d_texture_handle_t)tex;
}

static void gl_destroy_texture(r3d_backend_t *self, r3d_texture_handle_t h){
    (void)self; GLuint t=(GLuint)h; if(t)glDeleteTextures(1,&t);
}

static void gl_begin_frame(r3d_backend_t *self, const r3d_target_t *t){
    gl_impl_t *im=(gl_impl_t*)self->impl;
    if(t) im->target=*t;
    int w=im->win_w,h=im->win_h;
    glfwGetFramebufferSize(im->win,&w,&h);
    glViewport(0,0,w,h);
    glDepthMask(GL_TRUE);   /* 恢复深度写，否则清不掉深度缓冲 */
    glClearColor(0.12f,0.12f,0.15f,1.0f);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    glUseProgram(im->prog);
}

static void gl_set_camera(r3d_backend_t *self, const r3d_camera_t *c){
    gl_impl_t *im=(gl_impl_t*)self->impl; im->view=c->view; im->proj=c->proj;
}

static void gl_draw(r3d_backend_t *self, const r3d_mesh_t *mesh,
                    const r3d_mat4_t *model, const r3d_material_t *mat){
    gl_impl_t *im=(gl_impl_t*)self->impl;
    extern void r3d_mat4_mul(r3d_mat4_t*,const r3d_mat4_t*,const r3d_mat4_t*);
    r3d_mat4_t vp, mvp;
    r3d_mat4_mul(&vp,&im->proj,&im->view);
    r3d_mat4_mul(&mvp,&vp,model);
    glUniformMatrix4fv(im->u_mvp,1,GL_FALSE,mvp.m);

    /* 法线→view space 矩阵 = (view × model) 的 3x3 旋转部分 */
    r3d_mat4_t mv; r3d_mat4_mul(&mv,&im->view,model);
    float v3[9]={ mv.m[0],mv.m[1],mv.m[2], mv.m[4],mv.m[5],mv.m[6], mv.m[8],mv.m[9],mv.m[10] };
    glUniformMatrix3fv(im->u_v3,1,GL_FALSE,v3);

    /* view space 主光方向（从相机右上前方射入，随视角变化）*/
    float light[3]={0.3f,0.5f,0.8f};
    glUniform3fv(im->u_light,1,light);
    int flat = (mat->flags & R3D_MAT_FLAT_SHADING)?1:0;
    glUniform1f(im->u_flat, (float)flat);
    glUniform1f(im->u_matcap, (mat->flags & R3D_MAT_USE_MATCAP)?1.0f:0.0f);

    /* tint = base_color_factor(ARGB) 或白 */
    float tint[4]={1,1,1,1};
    if(mat->base_color_factor){
        uint32_t c=mat->base_color_factor;
        tint[3]=((c>>24)&0xFF)/255.0f; tint[0]=((c>>16)&0xFF)/255.0f;
        tint[1]=((c>>8)&0xFF)/255.0f;  tint[2]=(c&0xFF)/255.0f;
    }
    glUniform4fv(im->u_tint,1,tint);
    glUniform1f(im->u_use_tex, mat->base_color?1.0f:0.0f);
    if(mat->base_color){ glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,(GLuint)mat->base_color); }

    int translucent = (mat->flags & R3D_MAT_TRANSLUCENT) || tint[3] < 0.99f;
    if(translucent){
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);            /* 半透明不写深度 */
        glDisable(GL_CULL_FACE);          /* 玻璃双面可见 */
    } else {
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
        if(mat->flags & R3D_MAT_DOUBLE_SIDED) glDisable(GL_CULL_FACE);
        else { glEnable(GL_CULL_FACE); glCullFace(GL_BACK); }
    }

    /* 上传 VBO/EBO（按指针缓存，静态顶点只传一次）*/
    glBindBuffer(GL_ARRAY_BUFFER,im->vbo);
    if(mesh->dynamic || mesh->vertices!=im->last_vptr || mesh->vertex_count!=im->last_vcount){
        glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)mesh->vertex_count*sizeof(r3d_vertex_t),mesh->vertices,mesh->dynamic?GL_DYNAMIC_DRAW:GL_STATIC_DRAW);
        im->last_vptr=mesh->vertices; im->last_vcount=mesh->vertex_count;
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,im->ebo);
    if(mesh->indices!=im->last_iptr || mesh->index_count!=im->last_icount){
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,(GLsizeiptr)mesh->index_count*mesh->index_size,mesh->indices,GL_STATIC_DRAW);
        im->last_iptr=mesh->indices; im->last_icount=mesh->index_count;
    }

    GLsizei stride=sizeof(r3d_vertex_t);
    glEnableVertexAttribArray(im->a_pos);
    glVertexAttribPointer(im->a_pos,3,GL_FLOAT,GL_FALSE,stride,(void*)0);
    glEnableVertexAttribArray(im->a_uv);
    glVertexAttribPointer(im->a_uv,2,GL_FLOAT,GL_FALSE,stride,(void*)(sizeof(float)*3));
    glEnableVertexAttribArray(im->a_nrm);
    glVertexAttribPointer(im->a_nrm,3,GL_FLOAT,GL_FALSE,stride,(void*)(sizeof(float)*5));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3,1,GL_FLOAT,GL_FALSE,stride,(void*)(sizeof(float)*8));
    /* a_col: 顶点色 uint32(BGRA 字节) → 归一化 4×u8，偏移在 ao 之后 */
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4,4,GL_UNSIGNED_BYTE,GL_TRUE,stride,(void*)(sizeof(float)*9));

    glDrawElements(GL_TRIANGLES,(GLsizei)mesh->index_count,
                   mesh->index_size==4?GL_UNSIGNED_INT:GL_UNSIGNED_SHORT,0);
}

static void gl_end_frame(r3d_backend_t *self){ (void)self; glFlush(); }

static void gl_present(r3d_backend_t *self){
    gl_impl_t *im=(gl_impl_t*)self->impl;
    glfwSwapBuffers(im->win);
    glfwPollEvents();
}

static bool gl_query_feature(r3d_backend_t *self, r3d_feature_t f){
    (void)self;
    switch(f){
        case R3D_FEATURE_ZBUFFER:
        case R3D_FEATURE_PER_PIXEL_LIGHT:
        case R3D_FEATURE_PERSPECTIVE_TEXTURE:
        case R3D_FEATURE_BLEND_MULTIPLY: return true;
        default: return false;
    }
}

static const r3d_backend_vtable_t GL_VT={
    .init=gl_init,.destroy=gl_destroy,
    .create_texture=gl_create_texture,.destroy_texture=gl_destroy_texture,
    .begin_frame=gl_begin_frame,.set_camera=gl_set_camera,
    .draw=gl_draw,.end_frame=gl_end_frame,.present=gl_present,
    .query_feature=gl_query_feature,
};

r3d_backend_t *r3d_backend_opengl_create(void){
    r3d_backend_t *be=(r3d_backend_t*)calloc(1,sizeof(r3d_backend_t));
    if(!be) return NULL;
    be->impl=calloc(1,sizeof(gl_impl_t));
    if(!be->impl){ free(be); return NULL; }
    be->vt=&GL_VT;
    return be;
}

/* 暴露窗口关闭查询，供 demo 主循环 */
bool r3d_opengl_should_close(r3d_backend_t *be){
    gl_impl_t *im=(gl_impl_t*)be->impl;
    return im->win ? glfwWindowShouldClose(im->win) : true;
}

/* 截图：读当前帧缓冲存 PPM（P6），用于离屏验证渲染结果 */
int r3d_opengl_screenshot(r3d_backend_t *be, const char *path){
    gl_impl_t *im=(gl_impl_t*)be->impl;
    int w=800,h=600; glfwGetFramebufferSize(im->win,&w,&h);
    unsigned char *px=(unsigned char*)malloc((size_t)w*h*4);
    glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,px);
    FILE *fp=fopen(path,"wb");
    if(!fp){ free(px); return -1; }
    fprintf(fp,"P6\n%d %d\n255\n",w,h);
    /* GL 原点在左下，翻转到上 */
    for(int y=h-1;y>=0;y--)
        for(int x=0;x<w;x++){ unsigned char*p=px+((size_t)y*w+x)*4; fputc(p[0],fp);fputc(p[1],fp);fputc(p[2],fp); }
    fclose(fp); free(px);
    return 0;
}


/* 鼠标/输入查询：供 demo 驱动 orbit 相机 */
void r3d_opengl_poll_input(r3d_backend_t *be, r3d_opengl_input_t *in){
    gl_impl_t *im=(gl_impl_t*)be->impl;
    double mx,my; glfwGetCursorPos(im->win,&mx,&my);
    in->mouse_x=mx; in->mouse_y=my;
    in->left  = glfwGetMouseButton(im->win,GLFW_MOUSE_BUTTON_LEFT)==GLFW_PRESS;
    in->right = glfwGetMouseButton(im->win,GLFW_MOUSE_BUTTON_RIGHT)==GLFW_PRESS;
    in->middle= glfwGetMouseButton(im->win,GLFW_MOUSE_BUTTON_MIDDLE)==GLFW_PRESS;
    in->scroll= im->scroll_accum; im->scroll_accum=0.0;  /* 取出并清零 */
}
