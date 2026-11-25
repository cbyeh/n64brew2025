/**
 * C port of Tiny3D's particle microcode (tiny3d/src/t3d/rsp/rsp_tinypx.rspl).
 * Like the ucode, each particle becomes SET_PRIM_COLOR + SET_PRIM_DEPTH + a
 * fill or texture rectangle; the math is done in float on the CPU.
 */
#include <libdragon.h>
#include <t3d/t3d.h>
#include <t3d/tpx.h>
#include "rsp/rsp_tinypx.h"
#include "rspq_internal.h"
#include "hle.h"
#include "gbi.h"

#define D(sym)  (RSP_TPX_##sym & 0xFFF)
#define Q       ((rsp_queue_t*)__vrsp_dmem)

static hle_mat4_t proj, mvp, stack[16];
static int sp;

static void update_mvp(void) { hle_mat_mul(mvp, proj, stack[sp]); }

static void matrix_stack(uint32_t a0, uint32_t a1)
{
    int advance = (int16_t)(a0 >> 8) / (int)sizeof(T3DMat4FP);
    sp += advance;
    assertf(sp >= 0 && sp < 16, "HLE: tpx matrix stack out of range");
    if (a0 & 2) return;
    if (advance >= 0) {
        float m[4][4];
        hle_mat_from_fp(m, hle_rdram(a1));
        if ((a0 & 1) && sp > 0) hle_mat_mul(stack[sp], stack[sp - 1], m);
        else memcpy(stack[sp], m, sizeof(m));
    }
    update_mvp();
}

static void sync_t3d(uint32_t w_norm)
{
    const float (*p)[4], (*mv)[4];
    hle_t3d_matrices(&p, &mv);
    memcpy(proj, p, sizeof(proj));
    memcpy(stack[sp], mv, sizeof(stack[sp]));
    VDMEM16(D(NORM_SCALE_W) + 6) = w_norm;
    update_mvp();
}

// Particles as screen-space triangles, QMAX per G_VTX (corners in 1/4 px, UVs in texels)
// GLideN64 has 80 vertex slots and Tiny3D's cache uses 0..69: two quads per load (72..79)
#ifndef QMAX
#define QMAX 2
#endif
#define QBASE (80 - QMAX * 4)
static gbi_vtx_t qbuf[QMAX * 4];
static int qn;                  // quads in qbuf
static uint32_t quad_epoch;     // gsp_epoch of the 2D camera the quads use (reset per draw)
static uint32_t quad_tex;       // G_TEXTURE bits for this draw

static void quad_flush(void)
{
    if (!qn) return;
    gbi_reserve(12 + qn, qn * 4 * (int)sizeof(gbi_vtx_t) + 2 * (int)sizeof(gbi_mtx_t) + 32);
    if (quad_epoch != gsp_epoch) {          // once per draw, again after a task split
        gsp_sync_2d_z(G_ZBUFFER, quad_tex, 1.0f / 32767.0f);
        quad_epoch = gsp_epoch;
    }
    gbi_vtx_t *v = gbi_alloc(qn * 4 * sizeof(gbi_vtx_t));
    memcpy(v, qbuf, qn * 4 * sizeof(gbi_vtx_t));
    // high slots, so Tiny3D's vertex cache (0..69) survives
    GBI_VTX(QBASE, qn * 4, PhysicalAddr(v));
    for (int k = 0; k < qn; k++) {
        int b = QBASE + k * 4;
        gbi((uint32_t)G_TRI2 << 24 | GBI_TRI_IDX(b, b + 1, b + 2), GBI_TRI_IDX(b, b + 2, b + 3));
    }
    qn = 0;
}

static void quad(int x0, int y0, int x1, int y1, float s0, float t0, float s1, float t1, float tcs, const uint8_t *col, float zndc)
{
    int16_t z = (int16_t)(zndc * 32767.0f);
    gbi_vtx_t *v = &qbuf[qn * 4];
    int xs[4] = { x0, x1, x1, x0 }, ys[4] = { y0, y0, y1, y1 };
    float ss[4] = { s0, s1, s1, s0 }, ts[4] = { t0, t0, t1, t1 };
    for (int i = 0; i < 4; i++) {
        v[i].ob[0] = xs[i]; v[i].ob[1] = ys[i]; v[i].ob[2] = z; v[i].flag = 0;
        v[i].tc[0] = (int16_t)(ss[i] * tcs); v[i].tc[1] = (int16_t)(ts[i] * tcs);
        v[i].cn[0] = col[0]; v[i].cn[1] = col[1]; v[i].cn[2] = col[2]; v[i].cn[3] = col[3];
    }
    if (++qn == QMAX) quad_flush();
}

/** Does either cycle of the colour combiner read PRIM (colour or alpha)? */
static bool combiner_uses_prim(uint64_t c)
{
    uint32_t w0 = (c >> 32) & 0xFFFFFF, w1 = (uint32_t)c;
    #define F(v, s, n) (((v) >> (s)) & ((1u << (n)) - 1))
    uint32_t rgb_ab[4] = { F(w0,20,4), F(w1,28,4), F(w0,5,4), F(w1,24,4) };
    uint32_t rgb_c[2] = { F(w0,15,5), F(w0,0,5) }, rgb_d[2] = { F(w1,15,3), F(w1,6,3) };
    uint32_t al[8] = { F(w0,12,3), F(w1,12,3), F(w0,9,3), F(w1,9,3), F(w1,21,3), F(w1,3,3), F(w1,18,3), F(w1,0,3) };
    #undef F
    for (int i = 0; i < 4; i++) if (rgb_ab[i] == 3) return true;
    for (int i = 0; i < 2; i++) if (rgb_c[i] == 3 || rgb_c[i] == 10 || rgb_d[i] == 3) return true;
    for (int i = 0; i < 8; i++) if (al[i] == 3) return true;
    return false;
}

static void draw(uint32_t a0, uint32_t a1, bool textured)
{
    bool s16 = (int32_t)a1 < 0;
    const uint8_t *in = hle_rdram(a1 & 0xFFFFFF);
    int stride = s16 ? sizeof(TPXParticleS16) : sizeof(TPXParticleS8);
    int count = (a0 & 0xFFFF) / stride * 2;

    // Screen mapping, identical to Tiny3D's (gsp_3d.vp is its viewport, 1/4 px units)
    float cx = gsp_3d.vp[4], cy = gsp_3d.vp[5], hx = gsp_3d.vp[0], hy = gsp_3d.vp[1];
    float norm_w = VDMEM16(D(NORM_SCALE_W) + 6) / 65535.0f;
    if (norm_w <= 0) norm_w = 1;
    float scale_x = VDMEM16(D(PARTICLE_SCALE)) / 32767.0f, scale_y = VDMEM16(D(PARTICLE_SCALE) + 2) / 32767.0f;
    float base = VDMEM16(D(BASE_SIZE));
    int tex_offset = (int16_t)VDMEM16(D(TEX_OFFSET));

    uint64_t sc = Q->rdp_scissor_rect;
    int min_x = (sc >> 44) & 0xFFF, min_y = (sc >> 32) & 0xFFF;
    int max_x = (sc >> 12) & 0xFFF, max_y = sc & 0xFFF;

    float m[4][3], mw[4];
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 3; r++) m[c][r] = mvp[c][r];
        mw[c] = mvp[c][3];
    }

    uint32_t out[8];
    quad_epoch = gsp_epoch - 1;
    qn = 0;
    // tile 0; the mip levels as the 3D path sets them (sync_state in ovl_t3d.c)
    uint32_t som_hi = ((uint32_t*)&Q->rdp_mode.other_modes)[0];
    quad_tex = textured ? ((((som_hi >> 24 & 0x38) >> 3) << 11) | (1 << 1)) : 0;
    // GLideN64 halves triangle UVs when texture perspective is off (particles turn it off)
    float tcs = (som_hi & (1u << 19)) ? 32.0f : 64.0f;
    bool use_prim = combiner_uses_prim(Q->rdp_mode.combiner);
    uint32_t last_prim = 0; bool prim_sent = false;
#ifndef HLE_TPX_TEXRECT
    bool zprim = ((uint32_t*)&Q->rdp_mode.other_modes)[1] & 4;    // G_ZS_PRIM
    if (zprim) { gbi_reserve(1, 0); gbi(0xE2001D00, 0); }            // G_SETOTHERMODE_L zsrc = pixel
#endif
#ifdef HLE_LOG_TPX
    { static int n; if (n++ < 6) debugf("[tpx] comb %016llx som %016llx tex %d\n", (unsigned long long)Q->rdp_mode.combiner, (unsigned long long)Q->rdp_mode.other_modes, textured); }
#endif
    for (int i = 0; i < count; i++) {
        const uint8_t *pt = in + (i >> 1) * stride;
        float x, y, z; int size, toff; const uint8_t *col;
        if (s16) {
            const TPXParticleS16 *p = (const TPXParticleS16*)pt;
            const int16_t *pos = (i & 1) ? p->posB : p->posA;
            x = pos[0]; y = pos[1]; z = pos[2];
            size = (i & 1) ? p->sizeB : p->sizeA;
            toff = (i & 1) ? p->texOffsetB : p->texOffsetA;
            col = (i & 1) ? p->colorB : p->colorA;
        } else {
            const TPXParticleS8 *p = (const TPXParticleS8*)pt;
            const int8_t *pos = (i & 1) ? p->posB : p->posA;
            x = pos[0]; y = pos[1]; z = pos[2];
            size = (i & 1) ? p->sizeB : p->sizeA;
            col = (i & 1) ? p->colorB : p->colorA;
            toff = col[3];
        }
        float w = mw[0]*x + mw[1]*y + mw[2]*z + mw[3];
        if (w <= 0) continue;
        float cz = m[0][2]*x + m[1][2]*y + m[2][2]*z + m[3][2];
        if (cz > w || cz < -w) continue;            // outside near/far
        float inv = 1.0f / w;
        float px = cx + (m[0][0]*x + m[1][0]*y + m[2][0]*z + m[3][0]) * inv * hx;
        float py = cy - (m[0][1]*x + m[1][1]*y + m[2][1]*z + m[3][1]) * inv * hy;
        float half = (size / 128.0f) * base * 0.5f / (w * norm_w);   // 1/4 px
        float hw = half * scale_x, hh = half * scale_y;
        int x0 = px - hw, y0 = py - hh, x1 = px + hw, y1 = py + hh;
        int cx0 = x0 < min_x ? min_x : x0, cy0 = y0 < min_y ? min_y : y0;
        int cx1 = x1 > max_x ? max_x : x1, cy1 = y1 > max_y ? max_y : y1;
        if (cx0 >= cx1 || cy0 >= cy1) continue;

        int depth = 0x3FFF + (int)(cz * inv * 0x3FFF);
        depth = depth < 0 ? 0 : depth > 0x7FFF ? 0x7FFF : depth;
#ifdef HLE_TEST_TPX_NEAR    // test aid: particles at the nearest depth (never hidden)
        depth = 0;
#endif
#ifdef HLE_TEST_TPX_FILL    // test aid: particles as plain colour squares
        textured = false;
#endif
        int n = 0;
        out[n++] = 0xFA000000;
        out[n++] = ((uint32_t)col[0] << 24) | (col[1] << 16) | (col[2] << 8) | col[3];
        out[n++] = 0xEE000000;
        out[n++] = (uint32_t)depth << 16;
#ifndef HLE_TPX_TEXRECT
        // As two screen-space triangles with a real per-vertex depth (Z source switched
        // to pixel around the quads). Prim depth needs fragment depth writes, which
        // GLideN64 turns off on iOS/Android: there the particles sank behind the scene.
        // prim colour: the first one also pushes the material's pending render state out,
        // then only when it changes and the combiner reads it
        if (!prim_sent || (use_prim && out[1] != last_prim)) {
            quad_flush();
            vrsp_rdpq_send(out, 2 * 4);
            last_prim = out[1]; prim_sent = true;
        }
        float s0 = 0, t0 = 0, s1 = 0, t1 = 0;
        if (textured) {
            float span_x = (x1 - x0) / 4.0f, span_y = (y1 - y0) / 4.0f;
            float dsdx = span_x > 0 ? 8.0f / span_x : 0, dtdy = span_y > 0 ? 8.0f / span_y : 0;
            s0 = (toff * 8 + tex_offset / 4.0f) + (cx0 - x0) / 4.0f * dsdx;
            t0 = (cy0 - y0) / 4.0f * dtdy;
            s1 = s0 + (cx1 - cx0) / 4.0f * dsdx;
            t1 = t0 + (cy1 - cy0) / 4.0f * dtdy;
        }
        quad(cx0, cy0, cx1, cy1, s0, t0, s1, t1, tcs, col, cz * inv);
        continue;
#endif
        if (!textured) {
            out[n++] = 0xF6000000 | (cx1 << 12) | cy1;
            out[n++] = (cx0 << 12) | cy0;
        } else {
            // One 8x8 texel section per particle, offset by the per-particle index
            float span_x = (x1 - x0) / 4.0f, span_y = (y1 - y0) / 4.0f;   // pixels
            float dsdx = span_x > 0 ? 8.0f / span_x : 0, dtdy = span_y > 0 ? 8.0f / span_y : 0;
            float s = (toff * 8 + tex_offset / 4.0f) + (cx0 - x0) / 4.0f * dsdx;
            float t = (cy0 - y0) / 4.0f * dtdy;
            out[n++] = 0xE4000000 | (cx1 << 12) | cy1;
            out[n++] = (cx0 << 12) | cy0;                   // tile 0
            out[n++] = ((uint32_t)(uint16_t)(int16_t)(s * 32) << 16) | (uint16_t)(int16_t)(t * 32);
            out[n++] = ((uint32_t)(uint16_t)(int16_t)(dsdx * 1024) << 16) | (uint16_t)(int16_t)(dtdy * 1024);
        }
        vrsp_rdpq_send(out, n * 4);
    }
#ifndef HLE_TPX_TEXRECT
    quad_flush();
    if (zprim) { gbi_reserve(1, 0); gbi(0xE2001D00, 4); }            // back to G_ZS_PRIM
#endif
}

void ovl_tpx(uint32_t cmd, const uint32_t *a, int n)
{
    switch (cmd) {
    case TPX_CMD_SYNC_T3D:
        sync_t3d(a[2] & 0xFFFF);
        return;
    case TPX_CMD_DRAW_COLOR:
        draw(a[0], a[1], false);
        return;
    case TPX_CMD_MATRIX_STACK:
        matrix_stack(a[0], a[1]);
        return;
    case TPX_CMD_SET_DMEM: {
        uint32_t addr = a[0] & 0xFFFF;
        if (addr & 0x8000) VDMEM16(addr) = a[1];
        else VDMEM32(addr) = a[1];
        return;
    }
    case TPX_CMD_DRAW_TEXTURE:
        draw(a[0], a[1], true);
        return;
    }
}
