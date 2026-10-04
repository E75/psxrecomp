/* Original source-owned GL readback-coherence regression. No retail payload. */
#include "gpu_gl_renderer.c"
#include "mod_texture_banks.c"
uint32_t psx_mod_gpu_dma_memory_alloc(uint32_t n,uint32_t a){(void)n;(void)a;return 0;}
uint32_t psx_mod_read_word(uint32_t a){(void)a;return 0;}
static uint16_t image[1024*512], oracle[1024*512];
int g_psx_vram_dirty_tracking=0;
uint64_t s_frame_count=0;
void gpu_vram_dirty_mark_row_impl(uint32_t y){}
void gpu_vram_dirty_mark_rect(int x,int y,int w,int h){}
void gpu_vram_dirty_mark_all(void){}
int psx_netplay_active(void){return 0;}
static int test_depth24;
int gpu_display_is_depth24(void){return test_depth24;}
void gpu_get_display_info(GpuDisplayInfo *out){memset(out,0,sizeof(*out));out->display_x=32;out->display_y=32;out->width=320;out->height=16;}
int psx_ws_prim_in_backdrop(void){return 0;}
int gpu_ws_nw_flat_backdrop_enabled(void){return 0;}
int g_ws_tex_edge_pct=0;
int psx_ws_prim_is_tagged(void){return 0;}
void gpu_depth24_upload_span_reset(void){}
void frame_interpolation_schedule_reset(FrameInterpolationSchedule *p){memset(p,0,sizeof(*p));}
void frame_flip_tracker_reset(FrameFlipTracker *p){memset(p,0,sizeof(*p));p->period=1;}
static int checks,failures;
static void check(int ok,const char *label){checks++;if(!ok){fprintf(stderr,"FAIL %s\n",label);failures++;}}
static void verify(const char *label){
 gl_renderer_sync_cpu();
 check(gl_renderer_fbo_peek(0,0,1024,512,oracle),"oracle read");
 int n=0;for(int i=0;i<1024*512;i++)n+=image[i]!=oracle[i];
 if(n)fprintf(stderr,"%s: %d native words differ\n",label,n);
 check(n==0,label);check(glGetError()==GL_NO_ERROR,"GL error");
}
static void verify_bank_batching(void) {
 static uint16_t bank[256*128], baseline[96*96], result[96*96];
 for(int i=256;i<256*128;++i)bank[i]=0x3210;
 bank[0]=0;bank[1]=0x001f;bank[2]=0x83e0;bank[3]=0xfc00;
 check(psx_mod_define_texture_bank(8,256,128,bank),"batch fixture bank");
 for(int filter=0;filter<2;++filter) for(int mask=0;mask<2;++mask) {
  int counts[2];
  for(int enabled=0;enabled<2;++enabled) {
   gl_renderer_select_texture_bank(0);
   glb_set_mask_bits(0,0);glb_set_semi_transparency(0,0);
   glb_draw_flat_rect(400,300,96,96,0x1234);flush_flat_batch();
   psx_mod_set_texture_bank_batching(enabled);
   s_tex_filter=filter;glb_set_mask_bits(0,mask);
   gl_renderer_select_texture_bank(8);
   const int before=s_cw_batches;
   for(int i=0;i<36;++i) {
    const int x=404+(i%6)*5,y=304+(i%4)*7;
    glb_set_semi_transparency(1,(i/6)%4);
    glb_draw_shaded_textured_triangle(x,y,0,2,0x808080,
        x+44,y+2,63,2,0x507090,x+3,y+48,0,65,0x907050,0,0,0,0);
   }
   flush_tex_batch();counts[enabled]=s_cw_batches-before;
   gl_renderer_select_texture_bank(0);
   gl_renderer_sync_cpu();
   check(gl_renderer_fbo_peek(400,300,96,96,enabled?result:baseline),"batch pixel read");
  }
  check(memcmp(baseline,result,sizeof baseline)==0,"ordered semi batching pixel equivalence");
  check(mask?counts[1]==counts[0]:counts[1]<counts[0],"batch reduction only on supported path");
 }
 psx_mod_set_texture_bank_batching(0);s_tex_filter=0;
 glb_set_mask_bits(0,0);glb_set_semi_transparency(0,0);
}
static void verify_oversize_wide_margins(int scale) {
 /* Captured hallway triangles exceed the PS1 height limit. They may fill
  * the added view, but must never write the canonical framebuffer/VRAM. */
 glb_set_draw_area(0,0,511,239);glb_set_precise_triangle(0,0,0,0,0,0,0);
 glb_wide_configure(848,168);glb_wide_set_target(0);
 glb_wide_set_view(0,0,0,0);s_wide_fast=1;
 glb_wide_clear(0,0,240,0);
 glb_draw_flat_rect(-168,0,848,240,0x7c00);
 gl_renderer_set_triangle_wide_only(1);
 glb_draw_flat_triangle(-300,-200,300,100,-100,500,0x001f);
 check(glb_vram_read(100,100)==0x7c00,"oversize flat leaves canonical pixels unchanged");
 /* A line queued immediately after a margin-only flat triangle must use
  * the ordinary batch at both native and supersampled resolutions. */
 gl_renderer_set_triangle_wide_only(1);
 glb_draw_flat_triangle(-300,-200,300,100,-100,500,0x001f);
 glb_draw_line(203,40,219,40,0x03e0);
 check(glb_vram_read(210,40)==0x03e0,"ordinary line after oversize flat");
 glb_vram_write(512,0,0x03e0);
 gl_renderer_set_triangle_wide_only(1);
 glb_draw_shaded_textured_triangle(561,184,0,0,0x808080,
   1023,-724,0,0,0x808080,1023,334,0,0,0x808080,0,0,0x108,1);
 gl_renderer_set_triangle_wide_only(1);
 glb_draw_shaded_textured_triangle(-300,-200,0,0,0x808080,
   300,100,0,0,0x808080,-100,500,0,0,0x808080,0,0,0x108,1);
 check(glb_vram_read(100,100)==0x7c00,"oversize textured leaves canonical pixels unchanged");
 gl_renderer_set_triangle_wide_only(1);
 glb_draw_shaded_textured_triangle(561,184,0,0,0x808080,
   1023,-724,0,0,0x808080,1023,334,0,0,0x808080,0,0,0x108,1);
 glb_draw_shaded_textured_triangle(40,100,0,0,0x808080,
   70,100,0,0,0x808080,40,130,0,0,0x808080,0,0,0x108,1);
 check(glb_vram_read(45,105)==0x03e0,"ordinary textured after oversize textured");
 verify("oversize wide triangles preserve canonical authority");
 uint32_t *pixels=calloc((size_t)848*240*scale*scale,sizeof(uint32_t));
 check(pixels!=NULL,"oversize wide pixels allocation");
 if(pixels) {
  check(glb_render_wide_display(pixels,848*scale*4,0,0,240)>0,"oversize wide readback");
  check(pixels[(100*scale)*(848*scale)+68*scale]==0xff00ff00u,"left oversize margin drawn in painter order");
  check(pixels[(100*scale)*(848*scale)+818*scale]==0xff00ff00u,"captured right hallway wall drawn");
  check(pixels[(100*scale)*(848*scale)+268*scale]==0xff0000f8u,"canonical center excludes oversize faces");
  free(pixels);
 }
 glb_wide_disable_target();
 gl_renderer_set_triangle_wide_only(1);
 glb_draw_flat_triangle(-300,-200,300,100,-100,500,0x001f);
 check(glb_vram_read(100,100)==0x7c00,"oversize rejected without a wide target");
 /* Mod-generated triangles keep their existing behavior; only the frontend
  * explicitly arms host-only clipping after rejecting canonical geometry. */
 glb_draw_flat_triangle(-300,-200,300,100,-100,500,0x001f);
 check(glb_vram_read(100,100)==0x001f,"unarmed backend triangles retain ordinary behavior");
 glb_wide_set_target(0);
}
int main(int argc,char **argv){
 int scale=argc>1?atoi(argv[1]):1;
 if(SDL_Init(SDL_INIT_VIDEO)!=0)return 2;
 SDL_Window *win=SDL_CreateWindow("Readback coherence hidden test",0,0,128,128,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
 if(!win)return 2;
 for(int i=0;i<1024*512;i++)image[i]=(uint16_t)((i*17)&0x7fff);
 glb_init(image);glb_set_scale(scale);gl_renderer_set_swap_interval(0);
 if(!gl_renderer_init_context(win))return 2;
 printf("driver=%s renderer=%s scale=%d\n",glGetString(GL_VERSION),glGetString(GL_RENDERER),scale);
 glb_set_draw_area(0,0,1023,511);glb_set_mask_bits(0,0);glb_set_semi_transparency(0,0);glb_set_color_modulation(128,128,128,1);
 verify("initial upload");
 /* Adaptive backdrop reflection must retain every edge texel at both native
  * and supersampled scales; the ordinary rect stays forward-facing. */
 for(int x=0;x<16;++x) glb_vram_write(512+x,0,(uint16_t)(0x400+x+1));
 glb_draw_textured_rect(700,200,16,1,0,0,0,0,0x108);
 glb_draw_textured_rect_scaled(720,200,16,1,15,0,-1,1,0,0,0x108);
 for(int x=0;x<16;++x) {
  check(glb_vram_read(700+x,200)==0x401+x,"forward panorama texel");
  check(glb_vram_read(720+x,200)==0x410-x,"reflected panorama texel");
 }
 glb_draw_flat_rect(1020,511,1,1,0x7fff);
 check(glb_vram_read(1020,511)==0x7fff,"test pixel value");
 GlCohEvent event;int found=0;
 for(uint64_t i=gl_renderer_coh_total();i>0&&i+32>gl_renderer_coh_total();){i--;if(gl_renderer_coh_get(i,&event)&&event.kind==GL_COH_ENSURE){found=1;break;}}
 check(found,"readback event");check(found&&(event.x1-event.x0+1)*(event.y1-event.y0+1)<=4,"single-pixel bounded transfer");
 verify("single pixel + unchanged background");
 for(int row=0;row<8;row++){
  glb_draw_flat_rect(13,17+row*9,7,3,0x1234+row);
  glb_vram_write(23,20+row*9,0x7654);verify("odd coordinates width and row stride");
 }
 glb_draw_flat_rect(40,40,16,16,0x4321);glb_vram_write(900,400,0x7117);glb_draw_flat_rect(600,410,13,7,0x2222);verify("disjoint upload inside readback union");
 glb_draw_flat_rect(512,0,16,16,0x1234);
 glb_draw_textured_rect(90,90,16,16,0,0,0,0,0x108);verify("texture pack does not clear CPU debt");
 glb_fill_rect(1016,508,24,8,0x3210);verify("wrapping fill");
 glb_copy_rect(40,40,42,41,12,12);verify("overlapping copy");
 for(int mode=0;mode<4;mode++){
  glb_set_mask_bits(1,0);glb_draw_flat_rect(111,151,7,3,0x4567);
  glb_set_mask_bits(0,1);glb_draw_flat_rect(109,150,12,6,0x2222);
  glb_set_mask_bits(0,0);glb_set_semi_transparency(1,mode);glb_draw_flat_rect(108,149,14,8,0x1123);glb_set_semi_transparency(0,0);verify("mask and blend");
 }
 glb_set_precise_triangle(1,31*65536+49152,201*65536+49152,63*65536+49152,201*65536+49152,31*65536+49152,219*65536+49152);
 glb_draw_flat_triangle(31,201,63,201,31,219,0x7abc);verify("precision bound margin");
 glb_set_draw_area(11,11,19,19);glb_draw_flat_rect(0,0,32,32,0x5aaa);verify("clipped primitive");
 glb_set_draw_area(0,0,1023,511);glb_draw_flat_rect(320,320,8,8,0x4444);
 for(int i=0;i<1024*512;i++)image[i]=(uint16_t)((i*23)&0x7fff);
 gl_renderer_restage_vram_after_savestate();verify("state restage with pending draw");
 /* Consecutive RGB888 movies can stay in depth24. The second player's tile
  * clear must reach the CPU scanout, without reading stale FBO words over the
  * new packed movie. Also cover entry directly through an upload, not a test
  * call to the mode policy before it. */
 static uint16_t movie_first[480*16], movie_next[480*8];
 for(int i=0;i<480*16;i++)movie_first[i]=0x2345;
 for(int i=0;i<480*8;i++)movie_next[i]=0x4567;
 glb_draw_flat_rect(900,400,2,2,0x4321);
 glb_draw_flat_rect(32,32,480,16,0x7117);
 test_depth24=1;
 glb_vram_transfer_in(32,32,480,16,movie_first);
 check(image[400*1024+900]==0x4321,"depth24 entry retains pending GPU pixels");
 check(image[32*1024+32]==0x2345,"entry sync precedes first packed upload");
 glb_draw_flat_rect(32,32,480,16,0);
 glb_vram_transfer_in(32,36,480,8,movie_next);
 check(image[33*1024+40]==0,"consecutive movie top bar cleared");
 check(image[46*1024+40]==0,"consecutive movie bottom bar cleared");
 gl_renderer_sync_cpu();
 check(image[38*1024+40]==0x4567,"movie survives primitive readback debt");
 glb_copy_rect(32,36,40,37,4,2);
 check(image[37*1024+40]==0x4567,"depth24 copy reads packed CPU source");
 glb_fill_rect(48,33,16,1,0);
 check(glb_vram_read(48,33)==0,"depth24 fill and read stay coherent");
 test_depth24=0;depth24_upload_policy();
 verify("consecutive movie return to GPU authority");
 /* Existing depth24 policy clears the skipped movie band on return to15-bit.
  * That GPU write must become visible without waiting for another primitive. */
 static uint16_t movie[480*16], texture[4]={0x3210,0x3210,0x3210,0x3210};
 for(int i=0;i<480*16;i++)movie[i]=0x1234;
 test_depth24=1;depth24_upload_policy();
 glb_vram_transfer_in(32,32,480,16,movie);
 glb_vram_transfer_in(33,33,2,2,texture);
 test_depth24=0;depth24_upload_policy();
 check(glb_vram_read(40,40)==0,"depth24 cleared band immediate CPU read");
 check(glb_vram_read(33,33)==0x3210,"newer overlapping texture survives clear");
 verify("depth24 leave coherence without subsequent primitive");
 /* Run retained-bank tests with the normal draw area before the wide-view
  * regression narrows it to the gameplay rectangle. */
 static uint16_t bank[256*128];
 bank[0]=0x001f; bank[1]=0x03e0; bank[2]=0x7c00;
 bank[16]=0x1111; /* 4-bit indices, CLUT at (0,0) */
 check(psx_mod_define_texture_bank(7,256,128,bank),"define retained bank");
 check(gl_renderer_select_texture_bank(7),"select retained bank");
 glb_draw_shaded_textured_triangle(100,250,64,0,0x808080,132,250,64,0,0x808080,100,282,64,0,0x808080,0,0,0,1);
 check(gl_renderer_select_texture_bank(0),"select original VRAM");
 glb_vram_write(512,0,0x7c00);
 glb_draw_shaded_textured_triangle(116,250,0,0,0x808080,148,250,0,0,0x808080,116,282,0,0,0x808080,0,0,0x108,1);
 check(glb_vram_read(102,252)==0x03e0,"retained 4-bit CLUT independent of guest VRAM");
 check(glb_vram_read(118,252)==0x7c00,"following stock texture wins overlap");
 check(gl_renderer_select_texture_bank(7),"reselect retained bank");
 glb_draw_shaded_textured_triangle(300,250,0,0,0x808080,332,250,0,0,0x808080,300,282,0,0,0x808080,0,0,0x100,1);
 check(gl_renderer_select_texture_bank(0),"reset bank after direct texture");
 check(glb_vram_read(302,252)==0x001f,"retained 16-bit texel");
 verify("retained banks and original VRAM ordered together");
 /* A streamed scene can retain indices while CLUT uploads/fades continue.
  * Alternate both modes of the SAME bank with pending draws: palette source
  * must participate in the batch key, and stock packets must reset it. */
 glb_vram_write(1,0,0x7c00);
 check(gl_renderer_select_texture_bank_live_clut(7),"select bank with live CLUT");
 glb_draw_shaded_textured_triangle(400,250,64,0,0x808080,432,250,64,0,0x808080,400,282,64,0,0x808080,0,0,0,1);
 check(gl_renderer_select_texture_bank(7),"same bank with retained CLUT");
 glb_draw_shaded_textured_triangle(440,250,64,0,0x808080,472,250,64,0,0x808080,440,282,64,0,0x808080,0,0,0,1);
 check(glb_vram_read(402,252)==0x7c00,"live guest palette used");
 check(glb_vram_read(442,252)==0x03e0,"same-bank palette source is a batch key");
 glb_vram_write(1,0,0x001f);
 check(gl_renderer_select_texture_bank_live_clut(7),"live CLUT after update");
 glb_draw_shaded_textured_triangle(480,250,64,0,0x808080,512,250,64,0,0x808080,480,282,64,0,0x808080,0,0,0,1);
 check(gl_renderer_select_texture_bank(0),"reset live-CLUT bank");
 check(glb_vram_read(482,252)==0x001f,"palette update visible without replacing indices");
 verify("retained indices with animated guest CLUT");
 verify_bank_batching();
 verify_oversize_wide_margins(scale);
 /* World and UI use different origins in an anchored wide frame. Keep the
  * canonical-center optimization enabled to catch an erroneous blit over the
  * completed mirror, and change origins with a pending flat batch. */
 glb_set_draw_area(0,0,319,239);glb_set_precise_triangle(0,0,0,0,0,0,0);
 glb_wide_configure(426,53);glb_wide_set_target(0);
 s_wide_fast=1;
 uint32_t *wide_pixels=calloc((size_t)426*240*scale*scale,sizeof(uint32_t));
 if(!wide_pixels)return 2;
 for(int shift=-53;shift<=53;shift+=53){
  glb_wide_clear(0,0,240,0);
  glb_wide_set_view(1,shift,0,0);
  glb_draw_flat_rect(-106,0,532,240,0x7c00);
  glb_draw_flat_rect(160,40,3,3,0x03e0);
  glb_wide_set_view(1,0,0,0);
  glb_draw_flat_rect(160,20,3,3,0x001f);
  check(glb_render_wide_display(wide_pixels,426*scale*4,0,0,240)>0,"anchored wide readback");
  check(wide_pixels[(40*scale)*(426*scale)+(213+shift)*scale]==0xff00f800u,"anchored world marker");
  check(wide_pixels[(20*scale)*(426*scale)+213*scale]==0xfff80000u,"centered dialogue marker");
  check(wide_pixels[(60*scale)*(426*scale)]==0xff0000f8u,"anchored left edge");
  check(wide_pixels[(60*scale)*(426*scale)+425*scale]==0xff0000f8u,"anchored right edge");
 }
 glb_wide_set_view(0,0,0,0);
 check(wide_dx()==53,"disabled view preserves original origin");
 free(wide_pixels);
 printf("checks=%d failures=%d\n",checks,failures);
 gl_renderer_shutdown();SDL_DestroyWindow(win);SDL_Quit();return failures?1:0;
}
