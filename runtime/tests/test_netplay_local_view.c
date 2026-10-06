#include "netplay_local_view.h"
#undef NDEBUG
#include <assert.h>

int main(void)
{
    PsxNetplayLocalView view;
    uint32_t x = 0, y = 0, w = 0, h = 0;
    psx_netplay_local_view_reset(&view);
    assert(!psx_netplay_local_view_get(&view, 10, 320, 240, &x, &y, &w, &h));

    /* Empty and out-of-VRAM rectangles are refused. */
    assert(!psx_netplay_local_view_set(&view, 10, 0, 0, 0, 120));
    assert(!psx_netplay_local_view_set(&view, 10, 1000, 0, 160, 120));
    assert(!view.valid);

    assert(psx_netplay_local_view_set(&view, 100, 160, 120, 160, 120));
    assert(psx_netplay_local_view_get(&view, 100, 320, 240, &x, &y, &w, &h));
    assert(x == 160 && y == 120 && w == 160 && h == 120);
    /* Held for PSX_NETPLAY_LOCAL_VIEW_HOLD ticks, then lapses. */
    assert(psx_netplay_local_view_get(&view, 100 + PSX_NETPLAY_LOCAL_VIEW_HOLD,
                                      320, 240, NULL, NULL, NULL, NULL));
    assert(!psx_netplay_local_view_get(&view,
                                       101 + PSX_NETPLAY_LOCAL_VIEW_HOLD,
                                       320, 240, NULL, NULL, NULL, NULL));
    /* A display smaller than the rectangle presents the full frame. */
    assert(!psx_netplay_local_view_get(&view, 100, 256, 240,
                                       NULL, NULL, NULL, NULL));

    /* Resimulating an older tick renews the rectangle, not the stamp. */
    assert(psx_netplay_local_view_set(&view, 96, 0, 0, 160, 120));
    assert(view.tick == 100);
    assert(psx_netplay_local_view_get(&view, 104, 320, 240, &x, &y, &w, &h));
    assert(x == 0 && y == 0);
    /* A present slightly behind a resimulated request still uses it. */
    assert(psx_netplay_local_view_get(&view, 98, 320, 240,
                                      NULL, NULL, NULL, NULL));
    /* A load far behind restarts the stamp. */
    assert(psx_netplay_local_view_set(&view, 10, 0, 0, 160, 120));
    assert(view.tick == 10);
    assert(!psx_netplay_local_view_get(&view, 200, 320, 240,
                                       NULL, NULL, NULL, NULL));

    psx_netplay_local_view_reset(&view);
    assert(!psx_netplay_local_view_get(&view, 10, 320, 240,
                                       NULL, NULL, NULL, NULL));
    return 0;
}
