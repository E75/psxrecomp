/* Real GL + real format/decoder + renderer facade. Source-owned fixtures. */
#define PSX_TEST_HD_TEXTURE_PACK 1
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "../third_party/stb_image.h"
#define main scale_fixture_main
#include "test_gl_scale_invariance.c"
#undef main
#include "gpu_render.c"
#include "duckstation_texture_pack.h"
#include "hd_texture_pack.h"
#include "png_write.h"

const GpuRenderBackend* vk_backend_get(void) { return NULL; }
void present_shot_done(int ok) { (void)ok; }
int host_osd_needs_present(void) { return 0; }
int psx_present_vsync_owns_cadence(void) { return 0; }
void latency_ring_mark(LatencyStage stage) { (void)stage; }
void psx_host_sleep_ms(unsigned ms) { SDL_Delay(ms); }
int present_shot_take(char* out,int n) { (void)out; (void)n; return 0; }
int host_osd_image(const uint32_t** p,int* w,int* h) { (void)p; (void)w; (void)h; return 0; }
int host_osd_volume_image(const uint32_t** p,int* w,int* h) { return host_osd_image(p,w,h); }
void host_osd_present_done(void) {}
int psx_rewind_overlay_image(const uint32_t** p,int* w,int* h) { return host_osd_image(p,w,h); }
float psx_rewind_slide(void) { return 0; }
int psx_savestate_menu_overlay_image(const uint32_t** p,int* w,int* h) { return host_osd_image(p,w,h); }

static uint16_t source_words[4*4];
static uint16_t reference[1024*512];
static uint8_t pixels[64*64*4];
static const uint16_t texture_page=8u|(2u<<7);

static void pack_png(const char* root,int st) {
    DuckTextureKey key={0};
    key.source_hash=duck_texture_hash_words_le(source_words,16);
    key.source_width_words=4; key.source_height=4;
    key.width=4; key.height=4; key.kind=DUCK_TEXTURE_UPLOAD;
    key.depth=HD_TEXTURE_DEPTH_16BPP; key.semitransparent=st;
    char stem[256],path[2048];
    check(duck_texture_format_name(&key,stem,sizeof(stem)),"fixture filename");
    snprintf(path,sizeof(path),"%s/replacements/%s.png",root,stem);
    uint8_t rgba[16*16*4];
    for(int y=0;y<16;++y) for(int x=0;x<16;++x) {
        uint8_t* at=rgba+(y*16+x)*4;
        at[0]=(x&1)?0:255; at[1]=0; at[2]=(x&1)?255:0; at[3]=255;
        if(st) {
            const uint8_t alpha[6]={0,127,128,242,243,255};
            at[3]=alpha[x%6];
            if(y>=8) at[0]=at[1]=at[2]=0;
        } else if(y>=8) {
            at[0]=at[1]=at[2]=0;
            at[3]=(x&1)?128:127;
        }
    }
    FILE* file=fopen(path,"wb");
    check(file!=NULL,"open fixture PNG");
    if(file) { check(png_write_rgba(file,rgba,16,16),"write fixture PNG"); fclose(file); }
}
static void state(void) {
    gr_set_draw_area(0,0,1023,511); gr_set_draw_offset(0,0);
    gr_set_mask_bits(0,0); gr_set_semi_transparency(0,0);
    gr_set_texture_window(0); gr_set_color_modulation(128,128,128,1);
}
static void native_scene(void) {
    state();
    gr_vram_transfer_in(512,0,4,4,source_words);
    gr_fill_rect(0,0,64,64,0x4210);
    gr_draw_textured_rect(16,16,4,4,0,0,0,0,texture_page);
    for(int mode=0;mode<4;++mode) {
        gr_set_semi_transparency(1,mode);
        gr_draw_textured_rect(20+mode*5,20,4,4,0,0,0,0,texture_page);
    }
    gr_set_semi_transparency(0,0);
    gr_set_mask_bits(1,0); gr_draw_flat_rect(40,40,8,8,0x001f);
    gr_set_mask_bits(0,1); gr_draw_textured_rect(38,38,12,12,0,0,0,0,texture_page);
    gr_set_mask_bits(0,0);
    gr_set_texture_filter(1);
    gr_set_perspective_triangle(1,1.0f,0.5f,0.25f);
    gr_set_precise_triangle(1,(4<<16)+32768,30<<16,14<<16,30<<16,4<<16,38<<16);
    gr_draw_textured_triangle(4,30,0,0,14,30,3,0,4,38,0,3,0,0,texture_page);
    gr_set_texture_filter(0);
    gr_draw_line(3,50,53,50,0x7fff);
    gr_copy_rect(16,16,2,2,4,4);
    uint16_t masked[4]={0x7c00,0x7c00,0x7c00,0x7c00};
    gr_set_mask_bits(0,1); gr_vram_transfer_in(40,40,4,1,masked);
    gr_set_mask_bits(0,0);
    gr_fill_rect(1022,510,4,4,0x1234);
}
static void wait_ready(int st) {
    const int bounds[4]={0,0,3,3};
    int ready=0;
    for(int i=0;i<2000 && !ready;++i) {
        GpuHdTextureImage image={0};
        ready=gpu_hd_textures_acquire_draw(texture_page,0,0,bounds,0,st,&image);
        gpu_hd_textures_release_image(&image);
        if(!ready) SDL_Delay(1);
    }
    check(ready,"async replacement decode completed");
}
static void capture(void) {
    uint32_t argb[64*64];
    check(gl_renderer_capture_display_hires(argb,64*4,0,0,16,16)==64*64,"high-resolution capture");
    for(int i=0;i<64*64;++i) {
        pixels[i*4]=(uint8_t)(argb[i]>>16); pixels[i*4+1]=(uint8_t)(argb[i]>>8);
        pixels[i*4+2]=(uint8_t)argb[i]; pixels[i*4+3]=(uint8_t)(argb[i]>>24);
    }
}
static const uint8_t* sample(int x,int y) { return pixels+(y*64+x)*4; }

int main(int argc,char** argv) {
    if(argc==2 && !strcmp(argv[1],"--native-baseline")) {
        char* native_args[]={"hd-baseline","4","twin","1"};
        return scale_fixture_main(4,native_args);
    }
    if(argc!=2) return 2;
    for(int i=0;i<16;++i) source_words[i]=i%4==3?0:i%4==2?0x8000:i%4==1?0x83e0:0x03e0;
    pack_png(argv[1],0); pack_png(argv[1],1);
    gr_set_backend(GR_BACKEND_SOFTWARE); gr_init(reference);
    sw_set_faithful_authority(1); native_scene(); sw_set_faithful_authority(0);
    if(SDL_Init(SDL_INIT_VIDEO)!=0) return 2;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window* win=SDL_CreateWindow("HD pack regression",0,0,128,128,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
    if(!win) return 2;
    gr_set_backend(GR_BACKEND_OPENGL); gr_init(vram); gr_set_scale(4);
    char error[512]={0};
    check(gpu_hd_textures_configure(argv[1],1,1,error,sizeof(error)),error);
    gl_renderer_set_swap_interval(0);
    if(!gl_renderer_init_context(win)) return 2;
    if(s_hiw) check(hiw_ensure(0,64)!=NULL,"high-resolution window allocated before draws");
    state(); gr_vram_transfer_in(512,0,4,4,source_words);
    wait_ready(0); wait_ready(1);
    check(gpu_hd_textures_reload(error,sizeof(error)),"live pack reload succeeds");
    /* No new upload: a reload must retain identities of resident textures. */
    wait_ready(0); wait_ready(1);
    native_scene(); gl_renderer_sync_cpu();
    check(memcmp(vram,reference,sizeof(vram))==0,"HD native VRAM matches faithful software across all operations");
    uint16_t readback[64*64];
    gr_vram_transfer_out(0,0,64,64,readback);
    int unchanged=1;
    for(int y=0;y<64;++y) for(int x=0;x<64;++x) unchanged&=readback[y*64+x]==reference[y*1024+x];
    check(unchanged,"guest GPUREAD stays native with replacements");

    /* Source rectangle is four native texels, replacement is sixteen. The
     * first native texel contains alternating red and blue HD columns. */
    state(); gr_fill_rect(0,0,16,16,0x03e0);
    gr_draw_textured_rect(4,4,4,4,0,0,0,0,texture_page); capture();
    const uint8_t* a=sample(16,16); const uint8_t* b=sample(17,16);
    check(a[0]>200 && a[2]<20 && b[2]>200 && b[0]<20,
          "replacement detail survives inside one native texel at 4x");
    check(vram[4*1024+4]==0x03e0,"presentation red/blue never enters guest VRAM");
    check(sample(16,24)[1]>200,"non-ST alpha127 cuts out black");
    check(sample(17,24)[0]==0 && sample(17,24)[1]==0 && sample(17,24)[2]==0,
          "non-ST alpha128 retains opaque black");

    /* ST alpha bytes encode PSX classes, including alpha0 with colored RGB.
     * Mode 1 is additive, so the green destination survives STP texels. */
    state(); gr_fill_rect(0,0,16,16,0x03e0); gr_set_semi_transparency(1,1);
    gr_draw_textured_rect(4,4,4,4,0,0,0,0,texture_page); capture();
    check(sample(16,16)[0]>200 && sample(16,16)[1]>200,"ST colored alpha0 stays semitransparent");
    check(sample(19,16)[1]>200,"ST alpha242 stays semitransparent");
    check(sample(17,16)[1]>200 && sample(18,16)[1]>200,"ST alpha127 and alpha128 stay semitransparent");
    check(sample(20,16)[0]>200 && sample(20,16)[1]<20,"ST alpha243 is opaque");
    check(sample(21,16)[2]>200 && sample(21,16)[1]<20,"ST alpha255 is opaque");
    check(sample(16,24)[1]>200,"ST black RGBA zero cuts out");
    check(sample(20,24)[1]>200,"ST opaque black follows native-zero cutout");
    check(sample(17,24)[0]==0 && sample(17,24)[1]>200 && sample(17,24)[2]==0,
          "ST black alpha127 remains occupied semitransparent black");
    gr_set_semi_transparency(0,0);

    /* A queued draw must consume its source before a later source overwrite.
     * The next draw falls back because upload identity has been invalidated. */
    gr_draw_textured_rect(0,0,4,4,0,0,0,0,texture_page);
    uint16_t overwrite[16]; for(int i=0;i<16;++i) overwrite[i]=0x7c00;
    gr_vram_transfer_in(512,0,4,4,overwrite);
    gr_draw_textured_rect(8,0,4,4,0,0,0,0,texture_page); capture();
    check(sample(0,0)[0]>200,"queued replacement retained before source overwrite");
    check(sample(32,0)[2]>200 && sample(32,0)[0]<20,"next draw sees new native source");
    check(vram[8]==0x7c00,"new source word retained natively");
    state(); gr_vram_transfer_in(512,0,4,4,source_words);
    gr_draw_textured_rect(513,0,3,1,0,0,0,0,texture_page);
    gr_vram_transfer_out(512,0,4,1,readback);
    check(readback[0]==0x03e0 && readback[1]==0x03e0 && readback[2]==0x03e0 && readback[3]==0x03e0,
          "self-overlap keeps sequential native texture reads");
    const int bounds[4]={0,0,3,3}; GpuHdTextureImage lease={0};
    check(!gpu_hd_textures_acquire_draw(texture_page,0,0,bounds,0,0,&lease),
          "self-overlap invalidates upload only after its draw");
    gpu_hd_textures_release_image(&lease);
    gr_vram_transfer_in(512,0,4,4,source_words);
    check(gpu_hd_textures_acquire_draw(texture_page,0,0,bounds,0,0,&lease),
          "upload is resident before an interrupted A0");
    gpu_hd_textures_release_image(&lease);
    gr_vram_upload_begin(512,0,4,4);
    check(!gpu_hd_textures_acquire_draw(texture_page,0,0,bounds,0,0,&lease),
          "A0 header invalidates identity even if payload is later aborted");
    gpu_hd_textures_release_image(&lease);
    gr_vram_transfer_in(512,0,4,4,source_words);
    gl_renderer_restage_vram_after_savestate();
    check(!gpu_hd_textures_acquire_draw(texture_page,0,0,bounds,0,0,&lease),
          "savestate restage cannot fabricate original upload identity");
    gpu_hd_textures_release_image(&lease);
    gr_vram_transfer_in(512,0,4,4,source_words);
    check(gpu_hd_textures_acquire_draw(texture_page,0,0,bounds,0,0,&lease),
          "fresh upload restores identity after state load");
    gpu_hd_textures_release_image(&lease);
    check(!gl_renderer_texture_banks_supported(),"texture banks explicitly refuse HD authority");
    check(gl_renderer_pass_unavailable()!=PSX_MOD_RENDER_PASS_READY,"passes refuse HD authority");
    check(gl_renderer_stereo_unavailable()!=PSX_MOD_RENDER_PASS_READY,"stereo refuses HD authority");
    GpuHdTextureDiag diag; gpu_hd_textures_get_diag(&diag);
    check(diag.applied_draws>0,"replacement diagnostics record actual GL submissions");
    /* Preserve the established Beetle numeric-key path with source origin
     * offset, native cutout and native STP authority. */
    char beetle_root[2048],beetle_png[2304],hashes[2304];
    snprintf(beetle_root,sizeof(beetle_root),"%s/beetle",argv[1]);
    snprintf(hashes,sizeof(hashes),"%s/Hashes.ini",beetle_root);
    FILE* hf=fopen(hashes,"wb"); if(hf) fclose(hf);
    snprintf(beetle_png,sizeof(beetle_png),"%s/demo-texture-replacements/%x-0.png",beetle_root,
             hd_texture_crc32_words_le(source_words,16));
    uint8_t beetle_rgba[16*16*4];
    for(int y=0;y<16;++y) for(int x=0;x<16;++x) {
        uint8_t* at=beetle_rgba+(y*16+x)*4;
        at[0]=x<4?255:0; at[1]=x>=8?255:0; at[2]=x>=4&&x<8?255:0; at[3]=255;
    }
    FILE* bf=fopen(beetle_png,"wb"); check(bf!=NULL,"Beetle PNG open");
    if(bf){check(png_write_rgba(bf,beetle_rgba,16,16),"Beetle PNG fixture");fclose(bf);}
    check(gpu_hd_textures_configure(beetle_root,1,0,error,sizeof(error)),"Beetle session opens");
    state(); gr_vram_transfer_in(512,0,4,4,source_words);
    wait_ready(0);
    gr_vram_transfer_in(516,0,4,4,source_words);
    gr_fill_rect(0,0,16,16,0x03e0);
    gr_draw_textured_rect(4,4,2,4,5,0,0,0,texture_page); capture();
    check(sample(16,16)[2]>200 && sample(16,16)[0]<20,"Beetle partial draw keeps upload-relative UV origin");
    check(vram[4*1024+4]==0x03e0 && vram[517]==0x83e0,
          "Beetle image preserves native draw RGB and source STP");
    gr_draw_textured_rect(8,4,4,4,4,0,0,0,texture_page); capture();
    check(sample(44,16)[1]>200,"Beetle native transparent zero still cuts out replacement");
    memcpy(reference,vram,sizeof(vram));
    gl_renderer_set_cpu_auth_dual(1);
    gpu_hd_textures_shutdown();
    check(!s_hd_native_authority,"session teardown clears independent authority");
    check(gl_renderer_cpu_auth_dual(),"HD teardown preserves netplay authority");
    check(memcmp(reference,vram,sizeof(vram))==0,"teardown cannot read presentation pixels into native VRAM");
    gl_renderer_set_cpu_auth_dual(0);
    check(gl_renderer_texture_banks_supported(),"session teardown restores backend capabilities");
    check(glGetError()==GL_NO_ERROR,"HD GL errors");
    printf("scale=%d checks=%d failures=%d applied=%llu dumps=%llu\n",gr_scale(),checks,failures,
           (unsigned long long)diag.applied_draws,(unsigned long long)diag.dumped_textures);
    gl_renderer_shutdown(); SDL_DestroyWindow(win); SDL_Quit();
    return failures?1:0;
}
