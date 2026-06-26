/*
 * demo_viewer.c — 加载 B3DM，开窗用 OpenGL 后端渲染，轨道相机自转。
 * 用法: demo_viewer model.b3dm
 */
#include "r3d/r3d_backend.h"
#include "r3d/r3d_model.h"
#include "r3d/r3d_math.h"
#include "r3d/r3d_anim.h"
#include "r3d/r3d_deform.h"
#include "r3d/r3d_skin.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

int main(int argc, char **argv){
    if(argc<2){ fprintf(stderr,"用法: %s model.b3dm [max_frames]\n",argv[0]); return 2; }
    setvbuf(stdout,NULL,_IONBF,0);
    int max_frames = (argc>=3)? atoi(argv[2]) : 0;  /* 0=无限 */
    extern int r3d_opengl_screenshot(r3d_backend_t*,const char*);

    r3d_backend_t *be = r3d_backend_create();   /* 编译期 = opengl */
    if(!be || be->vt->init(be,NULL)!=R3D_OK){ fprintf(stderr,"后端 init 失败\n"); return 1; }

    r3d_model_t *m = r3d_model_load(be, argv[1]);
    if(!m){ fprintf(stderr,"加载失败: %s\n",argv[1]); return 1; }
    printf("模型: v=%u i=%u submesh=%u tex=%u\n",
           m->vertex_count,m->index_count,m->submesh_count,m->texture_count);

    /* 动画：解析 ANIM 段并播放第一个 clip */
    r3d_anim_set_t aset; r3d_anim_set_parse(&aset, m->raw);
    r3d_anim_state_t ast; r3d_anim_state_init(&ast);
    int animated = 0;
    if(aset.clip_count>0){
        r3d_anim_play(&ast,&aset,NULL,true);
        animated=1;
        printf("动画: %u clip, 播放 '%s'\n", aset.clip_count, aset.clips[0].name);
    }

    /* morph 变形 */
    r3d_deform_t deform={0}; int has_morph=0;
    if(m->morph_target_count>0){
        r3d_deform_init(&deform,m); has_morph=1;
        printf("morph: %u targets\n", m->morph_target_count);
    }

    /* skin 蒙皮 */
    r3d_skin_t skin={0}; int has_skin=0;
    if(m->joint_count>0 && m->skinvtx && m->nodes){
        if(r3d_skin_init(&skin,m)==0){ has_skin=1;
            printf("skin: %u nodes, %u joints\n", m->node_count, m->joint_count); }
    }

    /* 包围球半径，用于相机距离 */
    float r = m->bounds.max.x - m->bounds.min.x;
    float ry= m->bounds.max.y - m->bounds.min.y;
    if(ry>r)r=ry; if(r<1e-3f)r=1.0f;
    r3d_vec3_t center={ (m->bounds.min.x+m->bounds.max.x)*0.5f,
                        (m->bounds.min.y+m->bounds.max.y)*0.5f,
                        (m->bounds.min.z+m->bounds.max.z)*0.5f };

    /* orbit 相机初始状态 */
    float dist0 = r*1.6f + 0.5f;
    float yaw=0.0f, pitch=0.4f, dist=dist0;
    if(getenv("R3D_YAW")) yaw=(float)atof(getenv("R3D_YAW"));
    if(getenv("R3D_PITCH")) pitch=(float)atof(getenv("R3D_PITCH"));
    if(getenv("R3D_DIST")) dist=dist0*(float)atof(getenv("R3D_DIST"));
    r3d_vec3_t target=center;
    int autospin = getenv("R3D_AUTOSPIN") ? atoi(getenv("R3D_AUTOSPIN")) : 1;
    r3d_opengl_input_t prev={0}; int prev_valid=0;
    printf("鼠标: 左键拖拽=旋转  右键拖拽=平移  滚轮=缩放\n");

    int frame=0;
    while(!r3d_opengl_should_close(be)){
        /* --- 鼠标 orbit 控制（行业通用规则）--- */
        r3d_opengl_input_t in; r3d_opengl_poll_input(be,&in);
        if(prev_valid){
            double ddx=in.mouse_x-prev.mouse_x, ddy=in.mouse_y-prev.mouse_y;
            if(in.left){               /* 左键拖拽：旋转 */
                yaw   -= (float)ddx*0.01f;
                pitch -= (float)ddy*0.01f;
                if(pitch> 1.5f)pitch= 1.5f; if(pitch<-1.5f)pitch=-1.5f;
            }
            if(in.right){              /* 右键拖拽：平移 */
                float rs=dist*0.0015f;
                /* 相机右/上方向 */
                r3d_vec3_t fwd={sinf(yaw)*cosf(pitch),sinf(pitch),cosf(yaw)*cosf(pitch)};
                r3d_vec3_t rgt=r3d_vec3_normalize(r3d_vec3_cross((r3d_vec3_t){0,1,0},fwd));
                r3d_vec3_t up =r3d_vec3_cross(fwd,rgt);
                target.x += (-rgt.x*(float)ddx + up.x*(float)ddy)*rs;
                target.y += (-rgt.y*(float)ddx + up.y*(float)ddy)*rs;
                target.z += (-rgt.z*(float)ddx + up.z*(float)ddy)*rs;
            }
        }
        if(in.scroll!=0.0){            /* 滚轮：缩放 */
            dist *= (in.scroll>0)? 0.9f : 1.1f;
            if(dist<0.05f)dist=0.05f; if(dist>r*20+5)dist=r*20+5;
        }
        prev=in; prev_valid=1;
        if(autospin && !in.left && !in.right) yaw += 0.006f;  /* 无拖拽时自动缓慢旋转 */

        r3d_vec3_t eye={ target.x+dist*cosf(pitch)*sinf(yaw),
                         target.y+dist*sinf(pitch),
                         target.z+dist*cosf(pitch)*cosf(yaw) };
        r3d_camera_t cam;
        r3d_mat4_look_at(&cam.view, eye, target, (r3d_vec3_t){0,1,0});
        r3d_mat4_perspective(&cam.proj, 1.0f, 800.0f/600.0f, 0.05f, 100.0f);

        r3d_target_t tgt={0}; tgt.w=800; tgt.h=600;
        be->vt->begin_frame(be,&tgt);
        be->vt->set_camera(be,&cam);

        /* model 矩阵：动画驱动 root_matrix，否则单位 */
        r3d_mat4_t model; r3d_mat4_identity(&model);
        if(animated) r3d_anim_update(&ast, 1.0f/60.0f);
        /* 单节点模型(spin等)：动画驱动根矩阵；多节点模型：变换已烘焙进顶点，用单位 */
        if(animated && m->node_count<=1) model=ast.root_matrix;

        /* 顶点变形：skin 优先于 morph */
        const r3d_vertex_t *verts = m->vertices;
        if(has_skin){
            r3d_skin_update(&skin, m, &ast);
            verts = skin.out;
            r3d_mat4_identity(&model);   /* 蒙皮已在模型空间，model 用单位 */
        } else if(has_morph){
            r3d_deform_apply(&deform, m, ast.morph_weights, ast.morph_weight_count);
            verts = deform.out;
        }
        /* 两趟绘制：先不透明，后半透明（表镜等透出下方）*/
        for(int pass=0; pass<2; pass++){
        for(uint32_t s=0;s<m->submesh_count;s++){
            r3d_submesh_t *sm=&m->submeshes[s];
            int tl = (sm->mat_flags & R3D_MAT_TRANSLUCENT)?1:0;
            if(tl!=pass) continue;
            r3d_mesh_t mesh={0};
            mesh.vertices=verts; mesh.vertex_count=m->vertex_count;
            mesh.indices=m->indices+sm->index_offset; mesh.index_count=sm->index_count;
            mesh.dynamic = (has_skin||has_morph)?1:0;  /* 变形顶点每帧重传 */
            r3d_material_t mat={0};
            mat.base_color=sm->base_color; mat.matcap=sm->matcap;
            mat.blend=sm->blend; mat.flags=sm->mat_flags|R3D_MAT_FLAT_SHADING;
            mat.base_color_factor=sm->base_color_factor;
            /* 动态 node submesh(如秒针)：用 node 动画世界矩阵 */
            r3d_mat4_t smodel=model;
            if((sm->mat_flags & R3D_MAT_DYNAMIC_NODE) && animated)
                r3d_anim_node_matrix(m,&ast,sm->node_id,&smodel);
            be->vt->draw(be,&mesh,&smodel,&mat);
        }
        }
        be->vt->end_frame(be);
        if(be->vt->present) be->vt->present(be);

        if(max_frames && ++frame>=max_frames){
            r3d_opengl_screenshot(be,"/tmp/r3d_demo.ppm");
            printf("已渲染 %d 帧，截图 → /tmp/r3d_demo.ppm\n",frame);
            break;
        }
    }

    if(has_skin) r3d_skin_free(&skin);
    if(has_morph) r3d_deform_free(&deform);
    r3d_anim_set_free(&aset);
    r3d_model_free(m);
    r3d_backend_destroy(be);
    return 0;
}
