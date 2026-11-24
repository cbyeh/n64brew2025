/**
 * C port of Tiny3D's RSP microcode (tiny3d/src/t3d/rsp/rsp_tiny3d.rspl).
 * Instead of transforming vertices and rasterizing triangles, it forwards the
 * untransformed geometry as F3DEX2 G_VTX/G_TRI with the same matrices, so the
 * emulator's graphics plugin does T&L on the host GPU.
 *
 * This runs on the emulated N64 CPU, in place of the RSP, so the hot paths
 * (triangles, vertex loads, matrices) are kept as cheap as possible.
 */
#include <libdragon.h>
#include <malloc.h>
#include <t3d/t3d.h>
#include <t3d/t3dmodel.h>
#include "rsp/rsp_tiny3d.h"
#include "rspq_internal.h"
#include "hle.h"
#include <math.h>
#include "gbi.h"

#define D(sym)      (RSP_T3D_##sym & 0xFFF)
#define VTX_SIZE    36
#define Q           ((rsp_queue_t*)__vrsp_dmem)

// The state a block replay saves and compares (replay.c), in one struct so it goes in one piece
static struct {
    hle_mat4_t mat_proj;
    int64_t proj_z[4];          // z row of the projection
    uint32_t fog_word;
    uint32_t stack_base;
    int cur_slot;               // ucode stack slot of the current matrix
    int gdepth;                 // GBI modelview stack depth (== cur_slot when in sync)
    bool state_dirty;           // geometry mode / texture need recomputing
    bool fog_dirty;             // the projection or a DMEM word changed since the last fog fit
    bool tri_has_pending;
} rs = { .stack_base = 0xFFFFFFFF, .state_dirty = true, .fog_dirty = true };
#define mat_proj        rs.mat_proj
#define proj_z          rs.proj_z
#define fog_word        rs.fog_word
#define stack_base      rs.stack_base
#define cur_slot        rs.cur_slot
#define gdepth          rs.gdepth
#define state_dirty     rs.state_dirty
#define fog_dirty       rs.fog_dirty
#define tri_has_pending rs.tri_has_pending
static hle_mat4_t mat_mv;

static inline uint32_t resolve_segment(uint32_t addr)
{
    return (addr + VDMEM32(D(SEGMENT_TABLE) + ((addr >> 24) & 0x1C))) & 0xFFFFFF;
}

// DMEM vertex address -> index; 36 does not divide cheaply, (x*1821)>>16 is exact here
__attribute__((always_inline)) static inline int vtx_index(uint32_t dmem)
{
    return ((((dmem & 0xFFF) - D(VERT_BUFFER)) & 0xFFF) * 1821) >> 16;
}

// ---- Matrices (inc/matrixStack.rspl) ----
//
// The ucode keeps a matrix stack where each slot is either loaded or multiplied
// by the slot below. That maps onto the F3DEX2 modelview stack: "multiply" is
// G_MTX(MUL|PUSH) from the slot below, "load" is G_MTX(LOAD), pops are POPMTX.
// So the plugin does the products; the CPU only keeps, per slot, the clip-space
// Z row (proj * modelview) needed for fog, plus how to rebuild the GBI stack
// after the plugin lost it (new task, 2D draws).

#define STACK_SIZE  16              // = dram_stack_size / 64 in vrdp.c

typedef struct { uint32_t addr; bool mul; } slot_t;
static slot_t slots[STACK_SIZE];
static uint32_t slot_raw[STACK_SIZE];   // the address the command gave (segmented), for replay
// Integer (16.16) so the emulated CPU avoids FPU conversions; values stay far below 2^47
static int64_t zrow[STACK_SIZE][4]; // clip z = sum_c zrow[k][c] * v[c]
static int zvalid = -1;             // zrow[0..zvalid] are up to date (computed lazily, for fog)
static uint32_t gbi_epoch = ~0u;    // gsp_epoch the GBI stack was built in

__attribute__((noinline)) void hle_mat_from_fp(float out[4][4], const void *fp_)
{
    const int16_t *p = fp_;
    for (int c = 0; c < 4; c++, p += 8) {
        for (int r = 0; r < 4; r++)
            out[c][r] = (int32_t)(((uint32_t)(uint16_t)p[r] << 16) | (uint16_t)p[4 + r]) * (1.0f / 65536.0f);
    }
}

void hle_mat_mul(float out[4][4], const float l[4][4], const float r[4][4])
{
    float res[4][4];
    for (int c = 0; c < 4; c++) {
        float r0 = r[c][0], r1 = r[c][1], r2 = r[c][2], r3 = r[c][3];
        for (int j = 0; j < 4; j++)
            res[c][j] = l[0][j]*r0 + l[1][j]*r1 + l[2][j]*r2 + l[3][j]*r3;
    }
    memcpy(out, res, sizeof(res));
}

/** T3DMat4FP in RDRAM -> GBI Mtx at dst (integer reshuffle only). */
void hle_t3d_mtx_conv_to(void *dst_, uint32_t addr)
{
    const uint32_t *src = hle_rdram(addr);      // per column: 2 words of int halves, 2 of frac halves
    uint32_t *dst = dst_;
    for (int c = 0; c < 4; c++) {
        dst[c*2+0] = src[c*4+0]; dst[c*2+1] = src[c*4+1];
        dst[8+c*2+0] = src[c*4+2]; dst[8+c*2+1] = src[c*4+3];
    }
}

/** Slot k's matrix (at addr) as a GBI Mtx in scratch. */
static uint32_t mtx_conv(uint32_t addr, int k)
{
    uint32_t *dst = gbi_alloc(sizeof(gbi_mtx_t));
    hle_t3d_mtx_conv_to(dst, addr);
    hle_rp_mtx(dst, k, slot_raw[k]);
    return PhysicalAddr(dst);
}

/** zrow of a slot: proj (load) or the slot below (multiply), times the matrix at addr. */
static inline int32_t fp_elem(const int16_t *col, int r)
{
    return ((uint32_t)(uint16_t)col[r] << 16) | (uint16_t)col[4 + r];
}

static void update_zrow(int k)
{
#ifdef HLE_TEST_PERF
    extern uint32_t hle_vl_zrow; hle_vl_zrow++;
#endif
    const int64_t *base = (slots[k].mul && k > 0) ? zrow[k - 1] : proj_z;
    if (!slots[k].addr) return;
    const int16_t *p = hle_rdram(slots[k].addr);
    for (int c = 0; c < 4; c++, p += 8)
        zrow[k][c] = (base[0] * fp_elem(p, 0) + base[1] * fp_elem(p, 1) +
                      base[2] * fp_elem(p, 2) + base[3] * fp_elem(p, 3)) >> 16;
}

/** Emits slot k on top of a GBI stack that holds slots 0..k-1 below the current depth. */
static void gbi_slot(int k)
{
    uint32_t m = slots[k].addr ? mtx_conv(slots[k].addr, k) : 0;
    if (!m) return;
    uint32_t op = (slots[k].mul && k > 0) ? G_MTX_MUL : G_MTX_LOAD;
    if (k > gdepth) {
        while (gdepth < k - 1) {            // empty slots in between: placeholders
            GBI_MTX(m, G_MTX_LOAD | G_MTX_PUSH | G_MTX_MODELVIEW);
            gdepth++;
        }
        GBI_MTX(m, op | G_MTX_PUSH | G_MTX_MODELVIEW);
        gdepth = k;
    } else {
        if (gdepth > k) {
            gbi((uint32_t)G_POPMTX << 24 | 0x380002, (gdepth - k) * 64);
            gdepth = k;
        }
        if (op == G_MTX_MUL) {              // multiply from the slot below
            gbi((uint32_t)G_POPMTX << 24 | 0x380002, 64);
            GBI_MTX(m, G_MTX_MUL | G_MTX_PUSH | G_MTX_MODELVIEW);
        } else {
            GBI_MTX(m, G_MTX_LOAD | G_MTX_NOPUSH | G_MTX_MODELVIEW);
        }
    }
}

/** Rebuilds the GBI modelview stack up to the current slot if the plugin lost it. */
static void sync_modelview(void)
{
    if (gbi_epoch == gsp_epoch) return;
    gbi((uint32_t)G_POPMTX << 24 | 0x380002, STACK_SIZE * 64);
    gdepth = 0;
    for (int k = 0; k <= cur_slot; k++) {
        if (k == 0) {
            uint32_t m = slots[0].addr ? mtx_conv(slots[0].addr, 0) : 0;
            if (m) GBI_MTX(m, G_MTX_LOAD | G_MTX_NOPUSH | G_MTX_MODELVIEW);
        } else {
            gbi_slot(k);
        }
    }
    gbi_epoch = gsp_epoch;
}

__attribute__((noinline)) static void matrix_stack(uint32_t a0, uint32_t a1)
{
    int16_t advance = (int16_t)(a0 >> 8);
    if (stack_base == 0xFFFFFFFF) stack_base = VDMEM32(D(MATRIX_STACK_PTR));
    int32_t ptr = (int32_t)VDMEM32(D(MATRIX_STACK_PTR)) + advance;
    VDMEM32(D(MATRIX_STACK_PTR)) = ptr;
    int slot = (ptr - (int32_t)stack_base) / (int)sizeof(T3DMat4FP);
    assertf(slot >= 0 && slot < STACK_SIZE, "HLE: t3d matrix stack out of range (%d)", slot);
    cur_slot = slot;
    if (a0 & 2) return;     // only move the stack pointer

    gbi_reserve(STACK_SIZE + 4, sizeof(gbi_mtx_t));
    bool in_sync = gbi_epoch == gsp_epoch;
    if (advance < 0) {
        // pop: the slot's matrix is already on the GBI stack
        if (in_sync && gdepth > slot) {
            gbi((uint32_t)G_POPMTX << 24 | 0x380002, (gdepth - slot) * 64);
            gdepth = slot;
        }
        return;
    }
    slots[slot].addr = resolve_segment(a1);
    slots[slot].mul = (a0 & 1) && slot > 0;
    slot_raw[slot] = a1;
    hle_rp_slot_written(slot);
    if (zvalid >= slot) zvalid = slot - 1;
    hle_rp_zvalid(slot - 1);
    if (in_sync)
        gbi_slot(slot);
}

/** Full modelview of the current slot (only tinypx needs it). */
static void full_modelview(float out[4][4])
{
    float m[4][4];
    for (int k = 0; k <= cur_slot; k++) {
        if (!slots[k].addr) continue;
        hle_mat_from_fp(m, hle_rdram(slots[k].addr));
        if (slots[k].mul && k > 0) hle_mat_mul(out, out, m);
        else memcpy(out, m, sizeof(m));
    }
}

// ---- Render state -> F3DEX2 state ----

static bool fog_enabled(void) { return VDMEM8(D(FOG_STORE_OFFSET)) == 0x0B; }

/**
 * Tiny3D fog is linear in clip-space Z and written into the vertex alpha, where
 * rdpq's fog blender reads it. GBI fog (G_FOG) is linear in NDC Z instead, which
 * fogs far too early, so the alpha is computed here like the ucode does:
 * alpha = offset + slope * clip_z, folded into one plane equation.
 */
static void fog_plane(int64_t k[4])
{
    int32_t slope = (int32_t)(((uint32_t)VDMEM16(D(SCREEN_SCALE) + 6) << 16) | VDMEM16(D(SCREEN_SCALE) + 14));
    int32_t offset = (int16_t)VDMEM16(D(SCREEN_OFFSET) + 6) + 32768;   // 0x80FF -> alpha 255
    for (; zvalid < cur_slot; zvalid++)
        update_zrow(zvalid + 1);
    for (int c = 0; c < 4; c++)
        k[c] = (zrow[cur_slot][c] * slope) >> 16;
    k[3] += (int64_t)offset << 16;
}

#ifndef HLE_FOG_GBI_ERR
#define HLE_FOG_GBI_ERR 0.08f   // most fog alpha (0..1) G_FOG may be off by, else CPU fog
#endif
/**
 * The same fog done by the graphics plugin (G_FOG), so vertices need no CPU work.
 * Tiny3D's alpha is affine in depth d, G_FOG's in NDC z (affine in 1/d): fit the
 * G_FOG closest over the fog range. Returns the G_MW_FOG word, 0 = keep CPU fog.
 * Refit only when the fog or projection values change.
 */
__attribute__((noinline)) static uint32_t fog_gbi_fit(void);
static uint32_t fog_gbi_refit(float sl, float ofs, const float (*P)[4]);
static inline uint32_t fog_gbi(void)
{
#ifdef HLE_FOG_CPU      // test aid: always the exact CPU fog
    return 0;
#endif
#ifdef HLE_TEST_FOG_AB  // test aid: autoplay flips between CPU and G_FOG every 120 frames
    extern bool hle_fog_ab_cpu;
    if (hle_fog_ab_cpu) return 0;
#endif
    return fog_dirty ? fog_gbi_fit() : fog_word;
}
static uint32_t fog_gbi_fit(void)
{
    static float key[6];
    uint32_t word = fog_word;
    fog_dirty = false;
    float sl = (int32_t)(((uint32_t)VDMEM16(D(SCREEN_SCALE) + 6) << 16) | VDMEM16(D(SCREEN_SCALE) + 14)) / 65536.0f;
    float ofs = (int16_t)VDMEM16(D(SCREEN_OFFSET) + 6) + 32768;
    const float (*P)[4] = mat_proj;
    float now[6] = { sl, ofs, P[2][2], P[3][2], P[2][3], P[3][3] };
    if (!memcmp(key, now, sizeof(now))) return word;
    memcpy(key, now, sizeof(now));
    return fog_word = fog_gbi_refit(sl, ofs, P);
}
/** The G_FOG closest to the current Tiny3D fog (0 = keep CPU fog). */
static uint32_t fog_gbi_refit(float sl, float ofs, const float (*P)[4])
{
    uint32_t word = 0;
    if (P[0][2] != 0 || P[1][2] != 0 || P[0][3] != 0 || P[1][3] != 0) return 0;   // not a plain perspective
    // eye point at depth d: clip z = az*d + bz, clip w = aw*d + bw; Tiny3D alpha = sl*clip_z + ofs
    float az = -P[2][2], bz = P[3][2], aw = -P[2][3], bw = P[3][3];
    if (sl * az == 0) return 0;
    // depths where the alpha leaves its near value (va) and reaches its far one (1 - va)
    float va = sl * az < 0;
    float dn = (255 * va - ofs - sl*bz) / (sl*az), df = (255 * (1 - va) - ofs - sl*bz) / (sl*az);
    if (!(dn > 0 && df > dn)) return 0;
    float w = df - dn, best = 2;
    for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) {
        float d0 = dn + w * (-0.1f + 0.4f * i / 15), d1 = df + w * (-0.1f + 0.6f * j / 15);
        if (d0 <= 0 || d1 <= d0) continue;
        float u0 = (az*d0 + bz) / (aw*d0 + bw), u1 = (az*d1 + bz) / (aw*d1 + bw);
        float fm = roundf(256 * (1 - 2 * va) / (u1 - u0)), fo = roundf(256 * va - u0 * fm);
        if (!(fabsf(fm) <= 32767 && fo >= -32768 && fo <= 32767)) continue;
        float err = 0;
        for (int n = 0; n < 24 && err < best; n++) {
            float d = dn + w * (-0.25f + 1.75f * n / 23);
            if (d <= 0) continue;
            float t = (sl*(az*d + bz) + ofs) / 255, g = (fm * (az*d + bz) / (aw*d + bw) + fo) / 256;
            t = t < 0 ? 0 : t > 1 ? 1 : t; g = g < 0 ? 0 : g > 1 ? 1 : g;
            err = fmaxf(err, fabsf(t - g));
        }
        if (err < best) { best = err; word = (uint32_t)(int)fm << 16 | (uint16_t)(int)fo; }
    }
    if (best > HLE_FOG_GBI_ERR) word = 0;
#ifdef HLE_LOG_FOG
    debugf("[fog] range %.0f-%.0f: G_FOG %s (error %.3f)\n", (double)dn, (double)df, word ? "on" : "off", (double)best);
#endif
    return word;
}

static void sync_state(void)
{
    if (state_dirty) {
        uint32_t op = (VDMEM32(D(TRI_COMMAND)) >> 8) & 0xFF;
        uint32_t geom = G_CLIPPING;
        if (op & 1) geom |= G_ZBUFFER;
        if (op & 4) geom |= G_SHADE | G_SHADING_SMOOTH;
        switch (VDMEM8(D(FACE_CULLING))) {
        case 0: geom |= G_CULL_FRONT; break;
        case 1: geom |= G_CULL_BACK; break;
        }
        gsp_3d.geom = geom;
        uint32_t levels = (((uint32_t*)&Q->rdp_mode.other_modes)[0] >> 24 & 0x38) >> 3;
        gsp_3d.tex = (op & 2) ? ((levels << 11) | (1 << 1)) : 0;
        state_dirty = false;
    }
    gsp_sync_3d();
    sync_modelview();
}

/**
 * Reserves room for `ncmds` commands / `nbytes` of scratch plus a full state
 * re-emit, then syncs the state: a task split can only happen here, before it.
 */
static void t3d_begin(int ncmds, int nbytes)
{
    gbi_reserve(ncmds + 3 * STACK_SIZE + 16, nbytes + (STACK_SIZE + 3) * (int)sizeof(gbi_mtx_t) + 32);
    sync_state();
}

__attribute__((noinline)) static void screen_size(const uint32_t *a)
{
    int guard = a[0] & 0xF; if (guard < 1) guard = 1;
    float norm_w = (a[3] & 0xFFFF) / 65535.0f;
    if (norm_w <= 0) norm_w = 1;
    float sx = ((int16_t)(a[2] >> 16) + (a[4] >> 16) / 65536.0f) / (norm_w * 4 * guard);
    float sy = -((int16_t)(a[2] & 0xFFFF) + (a[4] & 0xFFFF) / 65536.0f) / (norm_w * 4 * guard);
    int cx2 = (a[1] >> 17) & 0x7FFF;    // offset*2 + size == 2 * center
    int cy2 = (a[1] >> 1) & 0xFFFF;
    gsp_3d.vp[0] = (int16_t)(sx * 2); gsp_3d.vp[1] = (int16_t)(sy * 2);
    gsp_3d.vp[4] = cx2 * 2;           gsp_3d.vp[5] = cy2 * 2;
    gsp_3d.vp_ver++;
}

static void apply_depth_offset(uint32_t z)
{
    // SCREEN_OFFSET.z is 0x3FFF + offset (Tiny3D depth units, 0..0x7FFF)
    gsp_3d.vp[2] = 0x1FF;
    gsp_3d.vp[6] = 0x1FF + ((int)z - 0x3FFF) / 32;
    gsp_3d.vp_ver++;
}

// ---- Geometry ----

// T3DVertPacked (2 vertices): posA normA | posB normB | rgbaA | rgbaB | stA | stB
// gbi_vtx_t: ob[0..1] | ob[2] flag | tc | cn
// One copy loop per combination of UV offset, ambient and fog (always inlined with constants),
// so the per-vertex work has no branches on them; positions are read as int16 (no sign-extending)
__attribute__((always_inline)) static inline void vert_copy(uint32_t *o, const uint32_t *in, int count, bool uv, bool scale, bool fog,
                                                             int16_t uv_s, int16_t uv_t, const uint8_t (*amb_tbl)[256], const int64_t *k)
{
    for (int i = 0; i < count; i += 2, in += 8, o += 8) {
        o[0] = in[0]; o[1] = in[1] & 0xFFFF0000; o[2] = in[6]; o[3] = in[4];
        o[4] = in[2]; o[5] = in[3] & 0xFFFF0000; o[6] = in[7]; o[7] = in[5];
        if (uv) {
            o[2] = ((uint32_t)(uint16_t)((o[2] >> 16) + uv_s) << 16) | (uint16_t)(o[2] + uv_t);
            o[6] = ((uint32_t)(uint16_t)((o[6] >> 16) + uv_s) << 16) | (uint16_t)(o[6] + uv_t);
        }
        if (scale) {
            for (int j = 3; j <= 7; j += 4) {
                uint8_t *cn = (uint8_t*)&o[j];
                cn[0] = amb_tbl[0][cn[0]]; cn[1] = amb_tbl[1][cn[1]];
                cn[2] = amb_tbl[2][cn[2]]; cn[3] = amb_tbl[3][cn[3]];
            }
        }
        if (fog) {
            const int16_t *p = (const int16_t*)in;      // x y z of A at 0 1 2, of B at 4 5 6
            for (int j = 0; j < 2; j++, p += 4) {
                int64_t f = k[0] * p[0] + k[1] * p[1] + k[2] * p[2] + k[3];
                int32_t a = f >> 16;
                o[j*4+3] = (o[j*4+3] & ~0xFFu) | (a <= 0 ? 0 : a >= 255 ? 255 : a);
            }
        }
    }
}

// ---- Converted-vertex cache ----
//
// Model data (t3d_model_load) never changes, so without CPU fog a G_VTX copy of its
// vertices only depends on the UV offset and ambient, and a strip's triangles on
// nothing: both are made once and reused (strips as a display list called with G_DL).
// Buffers rewritten every frame (cloth, trails, chains) live outside models and are
// converted each time. The pool is only reused between tasks: the plugin reads a task's
// vertices when it is submitted.

#define VC_BYTES    (is_memory_expanded() ? 1024 * 1024 : 256 * 1024)   // converted vertices kept
#define VC_SLOTS    1024                // power of 2
#define VC_MODELS   2048                // Junk Runner's overworld keeps ~300-1000 meshes loaded

typedef struct {
    uint32_t src, uv, amb, out, bytes;  // strips: uv = amb = VC_STRIP
#ifdef HLE_TEST_VCACHE_VERIFY
    uint32_t sum;                       // the source when it was cached
#endif
} vc_ent_t;
#define VC_STRIP    0xFFFFFFFE
static vc_ent_t vc_tab[VC_SLOTS];
static uint8_t *vc_pool;
static uint32_t vc_used, vc_task;
static bool vc_reset_pending;
static struct { uint32_t lo, hi; } vc_models[VC_MODELS];
static int vc_nmodels;
#ifdef HLE_TEST_PERF
uint32_t hle_vc_hits, hle_vc_misses, hle_vc_resets, hle_vc_peak, hle_vc_strip_hits;
#endif

static uint32_t vc_gen_n;           // pool generations: a reset frees everything recorded with it
uint32_t hle_vc_gen(void) { return vc_gen_n + vc_reset_pending; }    // a pending reset already ends the generation: its lists may be overwritten later in the task
#ifdef HLE_TEST_DL_CHECK
bool hle_vc_pending(void) { return vc_reset_pending && vc_task != gsp_tasks; }
#endif

static void vc_reset_later(void)
{
#ifdef HLE_TEST_DL_CHECK
    debugf("[vcache] reset pending at task %lu (used %lu)\n", gsp_tasks, vc_used);
#endif
    memset(vc_tab, 0, sizeof(vc_tab));
    vc_reset_pending = true;
    vc_task = gsp_tasks;
}

#ifdef HLE_TEST_PERF
uint32_t hle_lit_white, hle_lit_all;
uint32_t hle_vl_why[4];     // test aid: vertex loads that were fogged, lit, cacheable, not in a model
#endif
void hle_vcache_model(const void *data, uint32_t bytes, bool add)
{
#ifdef HLE_TEST_PERF
    if (add && vc_nmodels == VC_MODELS) hle_warn_once("vcmodels", "vertex cache: more than %d models", VC_MODELS);
#endif
    uint32_t lo = PhysicalAddr(data), hi = lo + bytes;
    for (int i = 0; i < vc_nmodels; i++) {
        if (vc_models[i].lo != lo) continue;
        if (!add) { vc_models[i] = vc_models[--vc_nmodels]; vc_reset_later(); }
        return;
    }
    if (add && vc_nmodels < VC_MODELS) vc_models[vc_nmodels++] = (typeof(vc_models[0])){ lo, hi };
}

#ifdef HLE_TEST_VCACHE_VERIFY   // test aid: every hit checks its source is what was cached
uint32_t hle_vc_verify_hits, hle_vc_verify_bad;
static uint32_t vc_sum(uint32_t src, uint32_t bytes)
{
    const uint8_t *p = hle_rdram(src); uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < bytes; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}
#endif
/** The entry for this copy (out != 0 when cached), a free one to fill (out == 0), or NULL. */
static vc_ent_t *vc_find(uint32_t src, uint32_t bytes, uint32_t uv, uint32_t amb)
{
    if (vc_reset_pending && vc_task != gsp_tasks) { vc_used = 0; vc_reset_pending = false; vc_gen_n++; }
    uint32_t h = ((src >> 4) * 2654435761u) >> 22;
    for (int n = 0; n < 8; n++, h = (h + 1) & (VC_SLOTS - 1)) {
        vc_ent_t *e = &vc_tab[h];
        if (!e->out) {
            if (vc_reset_pending) return NULL;      // pool full until the next task
            int i = 0;
            while (i < vc_nmodels && (src < vc_models[i].lo || src + bytes > vc_models[i].hi)) i++;
            if (i == vc_nmodels) return NULL;
            e->src = src; e->uv = uv; e->amb = amb; e->bytes = bytes;
#ifdef HLE_TEST_VCACHE_VERIFY
            e->sum = vc_sum(src, bytes);
#endif
            return e;
        }
        if (e->src == src && e->uv == uv && e->amb == amb && e->bytes == bytes) {
#ifdef HLE_TEST_VCACHE_VERIFY
            hle_vc_verify_hits++;
            if (vc_sum(src, bytes) != e->sum) {
                if (hle_vc_verify_bad++ < 10) debugf("[vcache] CHANGED since cached: %06lx +%lu (%s)\n", src, bytes, uv == VC_STRIP ? "strip" : "vertices");
                e->sum = vc_sum(src, bytes);
            }
#endif
            return e;
        }
    }
    return NULL;
}

/** vc_find for data known not to change (no model check). */
static vc_ent_t *vc_find_any(uint32_t src, uint32_t bytes, uint32_t key)
{
    if (vc_reset_pending && vc_task != gsp_tasks) { vc_used = 0; vc_reset_pending = false; vc_gen_n++; }
    uint32_t h = ((src >> 4) * 2654435761u) >> 22;
    for (int n = 0; n < 8; n++, h = (h + 1) & (VC_SLOTS - 1)) {
        vc_ent_t *e = &vc_tab[h];
        if (!e->out) {
            if (vc_reset_pending) return NULL;
            e->src = src; e->uv = key; e->amb = key; e->bytes = bytes;
            return e;
        }
        if (e->src == src && e->uv == key && e->amb == key && e->bytes == bytes) return e;
    }
    return NULL;
}

static uint32_t *vc_alloc(int bytes)
{
    static uint32_t vc_bytes;
    if (!vc_pool) { vc_bytes = VC_BYTES; vc_pool = malloc_uncached(vc_bytes); }
    bytes = (bytes + 15) & ~15;
    if (!vc_pool || vc_used + bytes > vc_bytes) {
#ifdef HLE_TEST_PERF
        hle_vc_resets++;
#endif
        vc_reset_later();
        return NULL;
    }
    uint32_t *p = (uint32_t*)(vc_pool + vc_used);
    vc_used += bytes;
#ifdef HLE_TEST_PERF
    if (vc_used > hle_vc_peak) hle_vc_peak = vc_used;
#endif
    return p;
}

#ifdef HLE_TEST_PERF
uint32_t hle_vl_ticks[10], hle_vl_verts, hle_vl_amb, hle_vl_zrow, hle_vl_fog;     // test aid: vert_load's setup, t3d_begin, the loop, the G_VTX
#define VL_MARK(i) do { uint32_t _n = TICKS_READ(); hle_vl_ticks[i] += _n - _vl; _vl = _n; } while (0)
#else
#define VL_MARK(i) do {} while (0)
#endif
// ---- Directional lights (rsp_tiny3d.rspl, the light loop) ----
//
// GBI lighting would replace the vertex colour, but Tiny3D multiplies it: colour *
// (ambient + sum of max(0, n.L) * light colour), with the normal turned by the normalized
// modelview columns (MATRIX_NORMAL) and L already in view space. So the CPU does it, per load.
static struct { int n; float nm[3][3]; float dir[7][3]; int col[7][4]; int amb[4]; } lt;

/** MATRIX_NORMAL: the modelview's columns, normalized (normals to view space). */
static void normal_matrix(void)
{
    float mv[4][4];
    full_modelview(mv);
    for (int i = 0; i < 3; i++) {
        float l = sqrtf(mv[i][0]*mv[i][0] + mv[i][1]*mv[i][1] + mv[i][2]*mv[i][2]);
        float inv = l > 0 ? 1.0f / l : 0;
        for (int j = 0; j < 3; j++) lt.nm[i][j] = mv[i][j] * inv;
    }
}

static void light_setup(void)
{
    normal_matrix();
    const uint8_t *amb = VDMEM_PTR(D(COLOR_AMBIENT));
    for (int c = 0; c < 4; c++) lt.amb[c] = amb[c];
    int n = VDMEM8(D(ACTIVE_LIGHT_SIZE)) / 16;
    lt.n = 0;
    for (int i = 0; i < n && i < 7; i++) {
        const uint8_t *l = VDMEM_PTR(D(LIGHT_DIR_COLOR) + i * 16);
        if (l[15]) { hle_warn_once("t3dpoint", "Tiny3D point lights are ignored"); continue; }
        for (int c = 0; c < 4; c++) lt.col[lt.n][c] = l[c];
        for (int j = 0; j < 3; j++) lt.dir[lt.n][j] = (int8_t)l[8 + j] * (1.0f / 128.0f);
        lt.n++;
    }
}

/**
 * The same lights as F3DEX2 Light structs for the plugin, which lights a vertex as
 * ambient + sum of max(0, n.L) * colour like Tiny3D, but ignores the vertex colour: so
 * it is used only for loads whose vertices are all white (98% of the lit ones here).
 * False if a point light is on (CPU path).
 */
static bool lights_gbi(void)
{
    uint8_t l[sizeof(gsp_3d.lights)] = {0};
    int n = VDMEM8(D(ACTIVE_LIGHT_SIZE)) / 16;
    if (n > 7) return false;
    for (int i = 0; i < n; i++) {
        const uint8_t *t = VDMEM_PTR(D(LIGHT_DIR_COLOR) + i * 16);
        if (t[15]) return false;
        uint8_t *g = l + i * 16;
        g[0] = g[4] = t[0]; g[1] = g[5] = t[1]; g[2] = g[6] = t[2];
        g[8] = t[8]; g[9] = t[9]; g[10] = t[10];
    }
    const uint8_t *amb = VDMEM_PTR(D(COLOR_AMBIENT));
    uint8_t *g = l + n * 16;
    g[0] = g[4] = amb[0]; g[1] = g[5] = amb[1]; g[2] = g[6] = amb[2];
    if (n != gsp_3d.nlights || memcmp(l, gsp_3d.lights, 16 * (n + 1))) {
        memcpy(gsp_3d.lights, l, 16 * (n + 1));
        gsp_3d.nlights = n;
        gsp_3d.lights_ver++;
    }
    return true;
}

static bool load_white(const T3DVertPacked *src, int count)
{
    const uint32_t *in = (const uint32_t*)src;
    for (int i = 0; i < count; i += 2, in += 8)
        if ((in[4] & in[5]) < 0xFFFFFF00) return false;
    return true;
}

/** Puts the normals where GBI lighting reads them (colour bytes 0-2), keeping the alpha. */
static void vert_normals(uint32_t *o, const uint32_t *in, int count)
{
    for (int i = 0; i < count; i += 2, in += 8, o += 8)
        for (int v = 0; v < 2; v++) {
            int16_t pn = (int16_t)(in[1 + 2*v] & 0xFFFF);      // 5.6.5 -> s8
            uint8_t *cn = (uint8_t*)&o[3 + 4*v];
            cn[0] = (pn >> 11) * 8; cn[1] = ((int16_t)(pn << 5) >> 10) * 4; cn[2] = ((int16_t)(pn << 11) >> 11) * 8;
        }
}

/**
 * VertexFX_Spherical (vertexFX.rspl): UVs from the view-space normal, u = n.x * a0 + a0,
 * v = n.y * a1 + a0 (a0 = width * 16, a1 = -height * 16, 10.5 texels).
 * shortcut: the ucode also nudges the UV by the vertex's screen position (a parallax hint);
 * skipped, so reflections don't slide across the screen.
 */
static void vert_spherical(uint32_t *o, const uint32_t *in, int count, int16_t a0, int16_t a1)
{
    for (int i = 0; i < count; i += 2, in += 8, o += 8)
        for (int v = 0; v < 2; v++) {
            int16_t pn = (int16_t)(in[1 + 2*v] & 0xFFFF);
            float n0 = (pn >> 11) * (1.0f / 16.0f);
            float n1 = ((int16_t)(pn << 5) >> 10) * (1.0f / 32.0f);
            float n2 = ((int16_t)(pn << 11) >> 11) * (1.0f / 16.0f);
            float x = n0 * lt.nm[0][0] + n1 * lt.nm[1][0] + n2 * lt.nm[2][0];
            float y = n0 * lt.nm[0][1] + n1 * lt.nm[1][1] + n2 * lt.nm[2][1];
            int16_t s = (int16_t)(x * a0 + a0), t = (int16_t)(y * a1 + a0);
            o[2 + 4*v] = ((uint32_t)(uint16_t)s << 16) | (uint16_t)t;
        }
}

/** Lights the colours of a converted load in place (o: gbi_vtx_t pairs, in: T3DVertPacked). */
static void vert_light(uint32_t *o, const uint32_t *in, int count, bool keep_alpha)
{
    for (int i = 0; i < count; i += 2, in += 8, o += 8) {
        for (int v = 0; v < 2; v++) {
            int16_t pn = (int16_t)(in[1 + 2*v] & 0xFFFF);  // 5.6.5 normal
            float n0 = (pn >> 11) * (1.0f / 16.0f);
            float n1 = ((int16_t)(pn << 5) >> 10) * (1.0f / 32.0f);
            float n2 = ((int16_t)(pn << 11) >> 11) * (1.0f / 16.0f);
            float nv[3];
            for (int j = 0; j < 3; j++) nv[j] = n0 * lt.nm[0][j] + n1 * lt.nm[1][j] + n2 * lt.nm[2][j];
            float acc[4] = { lt.amb[0], lt.amb[1], lt.amb[2], lt.amb[3] };
            for (int k = 0; k < lt.n; k++) {
                float d = nv[0] * lt.dir[k][0] + nv[1] * lt.dir[k][1] + nv[2] * lt.dir[k][2];
                if (d <= 0) continue;
                for (int c = 0; c < 4; c++) acc[c] += d * lt.col[k][c];
            }
            uint8_t *cn = (uint8_t*)&o[3 + 4*v];
#ifdef HLE_TEST_PERF
            { extern uint32_t hle_lit_white, hle_lit_all; hle_lit_all++; hle_lit_white += cn[0] == 255 && cn[1] == 255 && cn[2] == 255; }
#endif
            for (int c = 0; c < (keep_alpha ? 3 : 4); c++) {
                int a = acc[c] >= 255.0f ? 255 : (int)acc[c];
                cn[c] = cn[c] * a / 255;
            }
        }
    }
}

/** The copy for a vertex load not in the cache (kept apart so the hit path stays light). */
__attribute__((noinline)) static void vert_load_copy(int dest, int count, const T3DVertPacked *src, vc_ent_t *e,
                                                    uint32_t uv_ofs, bool scale, uint32_t amb, bool fog, const int64_t *k, bool lit, bool normals, bool sph)
{
#ifdef HLE_TEST_PERF
    uint32_t _vl = TICKS_READ();
#endif
    int16_t uv_s = uv_ofs >> 16, uv_t = uv_ofs;
    static uint8_t amb_tbl[4][256];     // c * ambient / 255 per channel
    static uint32_t amb_tbl_for = 0xFFFFFFFF;
    if (scale && amb != amb_tbl_for) {
        for (int ch = 0; ch < 4; ch++)
            for (int v = 0; v < 256; v++)
                amb_tbl[ch][v] = v * ((amb >> (24 - 8*ch)) & 0xFF) / 255;
        amb_tbl_for = amb;
#ifdef HLE_TEST_PERF
        hle_vl_amb++;
#endif
    }
    t3d_begin(2, count * sizeof(gbi_vtx_t));
    VL_MARK(1);
    uint32_t *out = e ? vc_alloc(count * sizeof(gbi_vtx_t)) : NULL;
    if (out) {
        e->out = PhysicalAddr(out);
#ifdef HLE_TEST_PERF
        hle_vc_misses++;
#endif
    } else {
        if (e) e->src = 0;                  // the slot stays free (out == 0)
        out = gbi_alloc(count * sizeof(gbi_vtx_t));
        hle_rp_bad();                       // a copy for this task only (CPU fog, not a model) can't be replayed
    }
    const uint32_t *in = (const uint32_t*)src;
    switch ((uv_ofs != 0) | scale << 1 | fog << 2) {
    case 0: vert_copy(out, in, count, false, false, false, uv_s, uv_t, amb_tbl, k); break;
    case 1: vert_copy(out, in, count, true,  false, false, uv_s, uv_t, amb_tbl, k); break;
    case 2: vert_copy(out, in, count, false, true,  false, uv_s, uv_t, amb_tbl, k); break;
    case 3: vert_copy(out, in, count, true,  true,  false, uv_s, uv_t, amb_tbl, k); break;
    case 4: vert_copy(out, in, count, false, false, true,  uv_s, uv_t, amb_tbl, k); break;
    case 5: vert_copy(out, in, count, true,  false, true,  uv_s, uv_t, amb_tbl, k); break;
    case 6: vert_copy(out, in, count, false, true,  true,  uv_s, uv_t, amb_tbl, k); break;
    default: vert_copy(out, in, count, true, true,  true,  uv_s, uv_t, amb_tbl, k); break;
    }
    if (lit) vert_light(out, in, count, fog_enabled());
    if (normals) vert_normals(out, in, count);
    if (sph) { if (!lit) normal_matrix(); vert_spherical(out, in, count, uv_ofs >> 16, uv_ofs); }
#ifdef HLE_TEST_SPH_RED     // test aid: spherical-UV vertices drawn red
    if (sph) for (int i = 0; i < count; i++) out[i * 4 + 3] = 0xFF0000FF;
#endif
#ifdef HLE_TEST_T3D_EXACT   // test aid: the loop as it was, on the same input: every word must match
    {
        extern uint32_t hle_exact_calls, hle_exact_bad;
        static uint32_t ref[T3D_VERTEX_CACHE_SIZE * 4];
        const uint32_t *in = (const uint32_t*)src; uint32_t *o = ref;
        for (int i = 0; i < count; i += 2, in += 8, o += 8) {
            o[0] = in[0]; o[1] = in[1] & 0xFFFF0000; o[2] = in[6]; o[3] = in[4];
            o[4] = in[2]; o[5] = in[3] & 0xFFFF0000; o[6] = in[7]; o[7] = in[5];
            if (uv_ofs) {
                o[2] = ((uint32_t)(uint16_t)((o[2] >> 16) + uv_s) << 16) | (uint16_t)(o[2] + uv_t);
                o[6] = ((uint32_t)(uint16_t)((o[6] >> 16) + uv_s) << 16) | (uint16_t)(o[6] + uv_t);
            }
            if (scale) for (int j = 3; j <= 7; j += 4) {
                uint8_t *cn = (uint8_t*)&o[j];
                cn[0] = amb_tbl[0][cn[0]]; cn[1] = amb_tbl[1][cn[1]]; cn[2] = amb_tbl[2][cn[2]]; cn[3] = amb_tbl[3][cn[3]];
            }
            if (fog) for (int j = 0; j < 8; j += 4) {
                int64_t f = k[0] * (int16_t)(o[j] >> 16) + k[1] * (int16_t)o[j] + k[2] * (int16_t)(o[j+1] >> 16) + k[3];
                int32_t a = f >> 16;
                o[j+3] = (o[j+3] & ~0xFFu) | (a <= 0 ? 0 : a >= 255 ? 255 : a);
            }
        }
        hle_exact_calls++;
        if (memcmp(ref, out, count * sizeof(gbi_vtx_t))) hle_exact_bad++;
    }
#endif
    VL_MARK(2);
    GBI_VTX(dest, count, PhysicalAddr(out));
    VL_MARK(3);
#ifdef HLE_TEST_PERF
    hle_vl_verts += count;
#endif
}

__attribute__((noinline)) static void vert_load(const uint32_t *a)
{
#ifdef HLE_TEST_PERF
    uint32_t _vl = TICKS_READ();
#endif
    int count = (a[0] & 0xFFFF) / 16;   // always an even number of vertices
    int dest = vtx_index(a[2] & 0xFFFF);
    if (dest + count > T3D_VERTEX_CACHE_SIZE) count = T3D_VERTEX_CACHE_SIZE - dest;
    if (count <= 0) return;
    const T3DVertPacked *src = hle_rdram(resolve_segment(a[1]));
    if (a[1] >> 24) hle_rp_bad();       // through a segment: the buffer may differ next time

    uint32_t fx = VDMEM16(D(VERTEX_FX_FUNC));
    bool sph = fx == (RSP_T3D_CODE_VertexFX_Spherical & 0xFFF);
    if (!sph && fx != (RSP_T3D_CODE_LOOP_TRAIL & 0xFFF))
        hle_warn_once("t3dfx", "Tiny3D vertex FX %04lx not supported", fx);
    uint32_t uv_ofs = VDMEM32(D(UV_GEN_PARAMS));
    if (sph) hle_rp_bad();              // view dependent

    bool unlit = VDMEM8(D(TRI_COMMAND)) & 1;
    uint32_t amb = VDMEM32(D(COLOR_AMBIENT));
    // a white ambient saturates the light sum (colour * min(255, 255 + ...)): nothing to light
    bool lit = !unlit && VDMEM8(D(ACTIVE_LIGHT_SIZE)) && amb != 0xFFFFFFFF;
    bool scale = !unlit && !lit && amb != 0xFFFFFFFF;
    // white vertices: the plugin lights them (cacheable); others are lit here
    bool normals = lit && lights_gbi() && load_white(src, count);
    if (normals) { lit = false; hle_rp_bad(); }     // the lights change between frames
    gsp_3d.lit = normals;
    if (lit) light_setup();

    VL_MARK(4);
    bool fog = fog_enabled();
    gsp_3d.fog = fog ? fog_gbi() : 0;
    if (gsp_3d.fog) fog = false;
    int64_t k[4] = {0};
    if (fog) fog_plane(k);
    VL_MARK(5);
#ifdef HLE_TEST_PERF
    hle_vl_fog += fog;
#endif
    VL_MARK(0);
#ifdef HLE_NO_VCACHE    // test aid: copy every load
    vc_ent_t *e = NULL;
#else
    vc_ent_t *e = (fog || lit || sph) ? NULL : vc_find(PhysicalAddr(src), count * sizeof(gbi_vtx_t), uv_ofs,
                                                normals ? 0xFFFFFFFE : scale ? amb : 0xFFFFFFFF);   // FFFFFFFE: normals
#endif
    VL_MARK(6);
#ifdef HLE_TEST_PERF
    { extern uint32_t hle_vl_why[4]; hle_vl_why[fog ? 0 : lit ? 1 : e ? 2 : 3]++;
}
#endif
    if (e && e->out) {
        t3d_begin(2, 0);
        VL_MARK(7);
        GBI_VTX(dest, count, e->out);
        VL_MARK(8);
#ifdef HLE_TEST_PERF
        hle_vc_hits++;
        hle_vl_verts += count;
#endif
        return;
    }
    vert_load_copy(dest, count, src, e, uv_ofs, scale, amb, fog, k, lit, normals, sph);
}

// Triangles are paired into G_TRI2, also across commands: hle_t3d_flush() is
// called before anything else reaches the display list.
#ifdef HLE_TEST_T3D_EXACT
uint32_t hle_exact_calls, hle_exact_strips, hle_exact_bad;
#endif
static uint32_t tri_pending;

void hle_t3d_flush(void)
{
    if (!tri_has_pending) return;
    tri_has_pending = false;
    gbi((uint32_t)G_TRI1 << 24 | tri_pending, 0);
}

/** Triangle in RDPQ_Triangle vertex order; `flip` = culling flag inverted by the caller. */
static inline void tri_idx(int a, int b, int c, bool flip)
{
    uint32_t idx = flip ? GBI_TRI_IDX(a, c, b) : GBI_TRI_IDX(a, b, c);
    if (tri_has_pending) {
        tri_has_pending = false;
        gbi((uint32_t)G_TRI2 << 24 | tri_pending, idx);
    } else {
        tri_pending = idx;
        tri_has_pending = true;
    }
}

static inline void tri(uint32_t d0, uint32_t d1, uint32_t d2, bool flip)
{
    tri_idx(vtx_index(d0), vtx_index(d1), vtx_index(d2), flip);
}

__attribute__((noinline)) static void tri_strip(uint32_t a0, uint32_t a1)
{
    int count = ((a1 & 0xFFFF) + 1) / 2;
    const uint16_t *e = hle_rdram(a0 & 0xFFFFFF);
#ifdef HLE_NO_VCACHE
    vc_ent_t *ce = NULL;
#else
    vc_ent_t *ce = vc_find(a0 & 0xFFFFFF, count * 2, VC_STRIP, VC_STRIP);
#endif
    t3d_begin(count / 2 + 2, 0);
    if (ce && ce->out) {
        gbi((uint32_t)G_DL << 24, ce->out);
#ifdef HLE_TEST_PERF
        hle_vc_strip_hits++;
#endif
        return;
    }
    uint64_t *dl_start = __gbi_ptr;
    if (!ce) hle_rp_bad();              // indices outside models may change
    // Literal port of the STRIP_LOOP: strips alternate winding, and an index
    // with the MSB set restarts a new strip.
#ifdef HLE_TEST_T3D_EXACT   // test aid: the strip walked as it was: the same triangles, in order
    static uint32_t ref[4096]; int nref = 0;
    {
        int end = count - 2, p = 0; bool flip = true;
        while (p < end && nref < 4096) {
            int16_t i1 = (int16_t)e[p + 2];
            if (i1 < 0) { p += 2; flip = true; continue; }
            int a = vtx_index(e[p + 1]), b = vtx_index(i1), c = vtx_index(e[p]);
            ref[nref++] = flip ? GBI_TRI_IDX(a, c, b) : GBI_TRI_IDX(a, b, c);
            p += 1; flip = !flip;
        }
    }
    uint64_t *dl0 = __gbi_ptr; bool pend0 = tri_has_pending; uint32_t pidx0 = tri_pending;
#endif
    // Each index is converted once: a triangle shares two with the one before
    int end = count - 2;
    int p = 0;
    bool flip = true;
    int v0 = vtx_index(e[0]), v1 = vtx_index(e[1]);
    while (p < end) {
        int16_t i1 = (int16_t)e[p + 2];
        if (i1 < 0) {
            p += 2; flip = true;
            if (p < end) { v0 = vtx_index(e[p]); v1 = vtx_index(e[p + 1]); }
            continue;
        }
        int v2 = vtx_index(i1);
        tri_idx(v1, v2, v0, flip);      // = tri(e[p + 1], i1, e[p], flip)
        v0 = v1; v1 = v2;
        p += 1;
        flip = !flip;
    }
#ifdef HLE_TEST_T3D_EXACT
    {   // what went out: pairs of G_TRI2 (plus the one pending before and after)
        extern uint32_t hle_exact_strips, hle_exact_bad;
        static uint32_t got[4100]; int ngot = 0;
        if (pend0) got[ngot++] = pidx0;
        for (uint64_t *d = dl0; d < __gbi_ptr && ngot < 4098; d++) { got[ngot++] = (uint32_t)(*d >> 32) & 0xFFFFFF; got[ngot++] = (uint32_t)*d; }
        if (tri_has_pending) got[ngot++] = tri_pending;
        int off = pend0 ? 1 : 0;
        hle_exact_strips++;
        if (nref < 4096 && (ngot - off != nref || memcmp(got + off, ref, nref * 4))) hle_exact_bad++;
    }
#endif
    if (ce) {   // move what went out into the pool, as a list called from here
        hle_t3d_flush();
        int n = __gbi_ptr - dl_start;
        uint64_t *dst = (uint64_t*)vc_alloc((n + 1) * 8);
        if (!dst) { ce->src = 0; hle_rp_bad(); return; }
        memcpy(dst, dl_start, n * 8);
        dst[n] = (uint64_t)G_ENDDL << 56;
        __gbi_ptr = dl_start;
        ce->out = PhysicalAddr(dst);
        gbi((uint32_t)G_DL << 24, ce->out);
#ifdef HLE_TEST_PERF
        hle_vc_misses++;
#endif
    }
}

__attribute__((noinline)) static void tri_seq(uint32_t a0, uint32_t a1)
{
    uint32_t base = a0 & 0xFFFF, end = a1 & 0xFFFF;
    uint32_t is_quad = a1 >> 16;
    uint32_t incr = is_quad + VTX_SIZE * 3;
    t3d_begin(((end - base) / VTX_SIZE) + 4, 0);
    for (uint32_t b = base; b != end; b += incr)
        tri(b + VTX_SIZE * 2, b + VTX_SIZE, b, false);
    if (is_quad) {
        for (uint32_t b = base + VTX_SIZE; b != end + VTX_SIZE; b += incr)
            tri(b + VTX_SIZE * 2, b + VTX_SIZE, b, true);
    }
}

void hle_t3d_matrices(const float (**proj)[4], const float (**mv)[4])
{
    hle_rp_bad();
    full_modelview(mat_mv);
    *proj = mat_proj;
    *mv = mat_mv;
}

// Runs of single triangles inside recorded blocks (which never change) are converted once
// and called as a list after that, like strips. VC_TRIS: their cache key.
#define VC_TRIS     0xFFFFFFFD
int hle_block_depth;                    // > 0: executing a block (rspq_hle.c)
static bool tris_blocks_cached;
void hle_tris_block_gone(void)
{
    if (tris_blocks_cached) { vc_reset_later(); tris_blocks_cached = false; }
}

static int tri_run_convert(const uint32_t *a, uint32_t cmd_byte);

int hle_t3d_tri_run(const uint32_t *a, uint32_t cmd_byte)
{
#ifndef HLE_NO_VCACHE
    if (hle_block_depth > 0) {
        const uint32_t *p = a;
        while ((p[0] >> 24) == cmd_byte) p += 2;
        int bytes = (p - a) * 4, n = bytes / 8;
        vc_ent_t *ce = vc_find_any(PhysicalAddr(a), bytes, VC_TRIS);
        if (ce) {
            t3d_begin(n / 2 + 4, 0);
            if (ce->out) { gbi((uint32_t)G_DL << 24, ce->out); return bytes; }
            hle_t3d_flush();
            uint64_t *dl_start = __gbi_ptr;
            tri_run_convert(a, cmd_byte);
            hle_t3d_flush();
            int m = __gbi_ptr - dl_start;
            uint64_t *dst = (uint64_t*)vc_alloc((m + 1) * 8);
            if (!dst) { ce->src = 0; return bytes; }    // already emitted in place
            memcpy(dst, dl_start, m * 8);
            dst[m] = (uint64_t)G_ENDDL << 56;
            __gbi_ptr = dl_start;
            ce->out = PhysicalAddr(dst);
            tris_blocks_cached = true;
            gbi((uint32_t)G_DL << 24, ce->out);
            return bytes;
        }
    }
#endif
    return tri_run_convert(a, cmd_byte);
}

static int tri_run_convert(const uint32_t *a, uint32_t cmd_byte)
{
    // the ucode feeds RDPQ_Triangle with (v0, v2, v1)
    const uint32_t *p = a;
    do {
        t3d_begin(64, 0);
        for (int i = 0; i < 64; i++) {
            tri(p[0] & 0xFFFF, p[1] & 0xFFFF, p[1] >> 16, false);
            p += 2;
            if ((p[0] >> 24) != cmd_byte) break;
        }
    } while ((p[0] >> 24) == cmd_byte);
    return (p - a) * 4;
}

void ovl_t3d(uint32_t cmd, const uint32_t *a, int n)
{
    hle_t3d_flush();

    switch (cmd) {
    case T3D_CMD_SCREEN_SIZE:
        screen_size(a);
        VDMEM16(D(NORM_SCALE_W) + 6) = a[3] & 0xFFFF;
        return;
    case T3D_CMD_MATRIX_STACK:
        matrix_stack(a[0], a[1]);
        return;
    case T3D_CMD_SET_WORD: {
        uint32_t addr = a[0] & 0xFFFF;
        if ((addr & 0xFFF) < __vrsp_state_start || (addr & 0xFFF) + 4 > __vrsp_state_end) hle_rp_bad();   // outside what replay restores
        if ((addr & 0xFFF) >= D(SEGMENT_TABLE) && (addr & 0xFFF) < D(SEGMENT_TABLE) + 32) hle_rp_bad();     // segments aren't in signatures
        if (addr & 0x8000) VDMEM16(addr) = a[1];
        else VDMEM32(addr) = a[1];
        fog_dirty = true;
        if ((addr & 0xFFE) == D(SCREEN_OFFSET) + 4)
            apply_depth_offset(VDMEM16(D(SCREEN_OFFSET) + 4));
        return;
    }
    case T3D_CMD_VERT_LOAD:
        vert_load(a);
        return;
    case T3D_CMD_LIGHT_SET: {
        uint32_t addr = a[0] & 0xFFF;
        if (addr < __vrsp_state_start || addr + 16 > __vrsp_state_end) hle_rp_bad();
        VDMEM32(addr) = a[1];
        if (a[2]) { VDMEM32(addr + 8) = a[2]; VDMEM32(addr + 12) = a[3]; }
        VDMEM32(addr + 4) = a[1];
        return;
    }
    case T3D_CMD_DRAWFLAGS: {
        VDMEM8(D(FACE_CULLING)) = a[0] & 0xFF;
        uint32_t mip = (((uint32_t*)&Q->rdp_mode.other_modes)[0] >> 24) & 0x38;
        VDMEM32(D(TRI_COMMAND)) = a[1] | mip;
        state_dirty = true;
        return;
    }
    case T3D_CMD_PROJ_SET:
        hle_mat_from_fp(mat_proj, hle_rdram(a[0] & 0xFFFFFF));
        gsp_3d_proj(&mat_proj);
        for (int m = 0; m < 4; m++)
            proj_z[m] = fp_elem((const int16_t*)hle_rdram(a[0] & 0xFFFFFF) + m * 8, 2);
        zvalid = -1;                            // fog rows depend on the projection
        hle_rp_zvalid(-1);
        fog_dirty = true;
        return;
    case T3D_CMD_FOG_STATE:
        VDMEM8(D(FOG_STORE_OFFSET)) = a[0] & 0xFF;
        return;
    case T3D_CMD_TRI_SYNC:
        return;
    case T3D_CMD_TRI_STRIP:
        tri_strip(a[0], a[1]);
        return;
    case T3D_CMD_TRI_SEQ:
        tri_seq(a[0], a[1]);
        return;
    default:    // IMEM patches (lighting mode / cull invert), metrics
        hle_warn_once("t3dcmd", "Tiny3D command %ld ignored", cmd);
        return;
    }
}

// Models register their vertex buffer with the cache (linked with --wrap)
T3DModel *__real_t3d_model_load(const char *path);
void __real_t3d_model_free(T3DModel *model);
T3DModel *__wrap_t3d_model_load(const char *path)
{
    T3DModel *m = __real_t3d_model_load(path);
    hle_vcache_model(m, malloc_usable_size(m), true);      // vertices and strip indices
    return m;
}
void __wrap_t3d_model_free(T3DModel *model)
{
    hle_vcache_model(model, 0, false);
    __real_t3d_model_free(model);
}

// ---- Block replay: this overlay's state ----

void hle_t3d_state(hle_st_t *s, bool sig)
{
    HLE_ST(s, rs);
    struct {
        uint32_t pend;          // stale when nothing is pending
        uint32_t rel;           // in sync or not, not the epoch itself
        uint32_t slots;         // signatures: which slots hold a matrix and how it combines (re-read on replay)
    } x = { tri_has_pending ? tri_pending : 0, gbi_epoch - gsp_epoch, 0 };
    if (sig) for (int k = 0; k < STACK_SIZE; k++) x.slots |= (slots[k].mul | (slots[k].addr != 0) << 1) << (2 * k);
    HLE_ST(s, x);
    if (s->load) { tri_pending = x.pend; gbi_epoch = gsp_epoch + x.rel; }
}
/** A slot the block set: the address it gave, resolved again on replay (segments may point elsewhere). */
uint32_t hle_t3d_slot_io(uint8_t *buf, int k, bool load)
{
    struct { uint32_t raw; bool mul; } v = { slot_raw[k], slots[k].mul };
    if (load) {
        memcpy(&v, buf, sizeof(v));
        slot_raw[k] = v.raw; slots[k].mul = v.mul; slots[k].addr = resolve_segment(v.raw);
    } else memcpy(buf, &v, sizeof(v));
    return sizeof(v);
}
/** resolve_segment with Tiny3D's own segment table (a replay runs while any overlay may be current). */
uint32_t hle_t3d_resolve(uint32_t raw)
{
    const uint8_t *img = hle_vrsp_t3d_img();
    return (raw + *(const uint32_t*)(img + D(SEGMENT_TABLE) + ((raw >> 24) & 0x1C))) & 0xFFFFFF;
}
/**
 * The DMEM this port ever reads, all a replay must match and restore: SCREEN_SCALE .. the
 * lights (which it ignores), then TRI_COMMAND .. VERTEX_FX_FUNC.
 */
void hle_t3d_dmem_range(uint32_t *lo, uint32_t *hi, uint32_t *lo2, uint32_t *hi2)
{
    *lo = D(SCREEN_SCALE); *hi = D(LIGHT_DIR_COLOR);
    *lo2 = D(TRI_COMMAND); *hi2 = D(VERTEX_FX_FUNC) + 4;
}
void hle_t3d_seg_mask(uint8_t *state_copy, uint32_t state_start)
{
    memset(state_copy + D(SEGMENT_TABLE) - state_start, 0, 32);
}
uint32_t hle_t3d_seg_addr(void) { return D(SEGMENT_TABLE); }
void hle_t3d_seg_keep(uint8_t *img, bool restore)
{
    static uint8_t seg[32];
    if (restore) memcpy(img + D(SEGMENT_TABLE), seg, 32); else memcpy(seg, img + D(SEGMENT_TABLE), 32);
}
uint32_t hle_t3d_slot_addr(int k) { return slots[k].addr; }
void hle_t3d_zvalid_min(int v) { if (v < zvalid) zvalid = v; }
