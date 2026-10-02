/* Captured MediEvil GT4 regression: saturating two different X projections
 * reverses the second triangle. Execute the GPU preparation path and verify
 * correction, unchanged packets/Y, and exact-provenance fallback conditions. */
#define main gpu_fixture_main
#include "test_gpu_textured_dot_nw_shift_exec.c"
#undef main

static int64_t area(const int32_t *p) {
    int64_t x0=p[0]/65536, y0=p[1]/65536;
    int64_t x1=p[2]/65536, y1=p[3]/65536;
    int64_t x2=p[4]/65536, y2=p[5]/65536;
    return (x1-x0)*(y2-y0)-(y1-y0)*(x2-x0);
}

int main(void) {
    reset_gpu_state_for_test();
    configure_native_wide_16_9();
    hres1=2; ws_cfg_num=125; ws_cfg_den=30; /* 512 pixels, owner's ultrawide view */
    gp0_cmd_source_addr=0x10004u;
    const int indices[4]={1,4,7,10};
    const int32_t x[4]={882,988,1023,1023}, y[4]={117,130,152,173};
    const int32_t projected[4]={882,988,1040,1203};
    for (unsigned i=0;i<4;i++) {
        uint32_t word=pack_vertex(x[i],y[i]);
        gp0_cmd_buf[indices[i]]=word;
        fixture_projections[i].addr=gp0_cmd_source_addr+indices[i]*4;
        fixture_projections[i].word=word;
        fixture_projections[i].x16=projected[i]*65536;
        fixture_projections[i].y16=y[i]*65536+17000;
        fixture_projections[i].z=900;
        fixture_projections[i].valid=1;
    }
    uint32_t original[12];memcpy(original,gp0_cmd_buf,sizeof original);
    const int32_t ax[3]={882,988,1023}, ay[3]={117,130,152};
    const int32_t bx[3]={1023,988,1023}, by[3]={152,130,173};
    const int32_t folded[6]={1023*65536,152*65536,988*65536,130*65536,
                             1023*65536,173*65536};
    assert(area(folded)==-735);
    prepare_precise_triangle(1,4,7,ax,ay);
    assert(!fixture_precise_triangle.enabled); /* default remains stock */
    psx_mod_set_native_wide_projection_correction(1);
    assert(fixture_pgxp_enabled);
    prepare_precise_triangle(1,4,7,ax,ay);
    assert(fixture_precise_triangle.enabled && area(fixture_precise_triangle.xy)>0);
    assert(fixture_precise_triangle.xy[5]==152*65536); /* no Y fraction correction */
    prepare_precise_triangle(7,4,10,bx,by);
    assert(fixture_precise_triangle.enabled && area(fixture_precise_triangle.xy)>0);
    assert(!memcmp(original,gp0_cmd_buf,sizeof original));
    const int32_t shifted_x[3]={1030,995,1030}, shifted_y[3]={149,127,170};
    prepare_precise_triangle(7,4,10,shifted_x,shifted_y);
    assert(fixture_precise_triangle.xy[4]==1210*65536);
    assert(fixture_precise_triangle.xy[5]==170*65536);
    uint64_t recovered=0;
    assert(gpu_ws_native_wide_projection_correction(&recovered) && recovered>0);
    int32_t px=0;
    uint32_t addr=fixture_projections[2].addr, word=fixture_projections[2].word;
    assert(native_wide_projection_x(addr,word,1023,152,&px));
    assert(!native_wide_projection_x(addr+4,word,1023,152,&px));
    assert(!native_wide_projection_x(addr,word^1,1023,152,&px));
    assert(!native_wide_projection_x(UINT32_MAX,word,1023,152,&px));
    assert(!native_wide_projection_x(addr,word,1000,152,&px));
    fixture_projections[2].z=0;
    assert(!native_wide_projection_x(addr,word,1023,152,&px));
    fixture_projections[2].z=900;
    assert(!native_wide_projection_x(addr,word,1023,151,&px));
    fixture_projections[2].x16=4096*65536-1;
    assert(!native_wide_projection_x(addr,word,1023,152,&px));
    fixture_projections[2].x16=1000*65536;
    assert(!native_wide_projection_x(addr,word,1023,152,&px));
    /* Negative saturation is recovered only when the shadow extends left. */
    fixture_projections[2].word=pack_vertex(-1024,152);
    fixture_projections[2].x16=-1200*65536;
    assert(native_wide_projection_x(addr,fixture_projections[2].word,-1024,152,&px));
    fixture_projections[2].x16=-4096*65536;
    assert(!native_wide_projection_x(addr,fixture_projections[2].word,-1024,152,&px));
    fixture_projections[2].x16=1040*65536;
    fixture_projections[2].word=word;
    ws_mode=0;
    prepare_precise_triangle(7,4,10,bx,by);
    assert(!fixture_precise_triangle.enabled); /* 4:3 remains stock */
    ws_mode=2; ws_cfg_num=4; ws_cfg_den=3;
    prepare_precise_triangle(7,4,10,bx,by);
    assert(!fixture_precise_triangle.enabled); /* native-wide selected, zero reveal */
    configure_native_wide_16_9();
    psx_mod_set_native_wide_projection_correction(0);
    assert(!fixture_pgxp_enabled);
    prepare_precise_triangle(7,4,10,bx,by);
    assert(!fixture_precise_triangle.enabled);
    puts("native_wide_projection_test: PASS");
    return 0;
}
