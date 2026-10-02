# Native-wide projection correction

Trusted title plugins can call
`psx_mod_set_native_wide_projection_correction(1)` to recover horizontally
saturated GTE vertices during native-wide rendering. The default is disabled.
It is independent of the user-facing geometry and perspective-texture options.

The PS1 clamps projected screen X to -1024 through 1023. Two different vertices
can therefore collapse onto one X edge, folding a planar quad when a wider view
reveals its triangles. The renderer recovers the original horizontal projection
from the existing PGXP transport only when the packet address and complete word
match, depth is present, the integer Y still agrees, and X actually extends past
the corresponding saturation edge. Missing or stale provenance stays stock.
The bounded projection transport's own +/-4096 endpoints are rejected.

This changes host triangle positions only. Guest SXY registers, packet words,
vertical coordinates, NCLIP, collision and ordering-table behavior stay intact.
With zero native-wide reveal, including a selected 4:3 view, correction is
inactive. Existing geometry correction can still supply subpixel coordinates
for ordinary vertices when the user enables it separately.

`ws_nw projection_correction=0|1` provides a diagnostic A/B toggle. The reply
reports the selected policy and the cumulative corrected vertex count, reset
on a policy change. It does not toggle geometry or texture correction.

The executable GPU regression uses a captured folding GT4 shape, verifies both
triangle windings after recovery, and checks unchanged packet/Y data, both
saturation edges, zero reveal, disabling the policy, missing depth, stale words,
wrong addresses and transport endpoints. The PGXP regression separately verifies
that a saturated MFC2/store retains its exact extended projection while the
ordinary conservative geometry consumer continues to reject it.

This policy cannot recover a projection whose provenance has already been lost
through unsupported CPU arithmetic, or repair a guest culling decision made
before GPU submission. Those paths still need title-specific qualification.
