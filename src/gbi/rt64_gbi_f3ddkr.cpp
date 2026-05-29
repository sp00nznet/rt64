//
// RT64
//
// F3DDKR — Diddy Kong Racing's custom Rare microcode (xbus variant).
//
// Stage 5: DKR-aware G_MTX and G_MOVEWORD on top of the Stage 4 vertex/
// triangle/DMA work. DKR uses a 4-slot indexed matrix model rather than
// F3D's projection/modelview stacks, and its MOVEWORD index encoding is
// different from F3D. We map both onto RT64's matrix API best-effort:
//   - Matrix slot 0 (camera VP) → RT64 projection LOAD
//   - Slot >0 with multiply flag → RT64 modelview LOAD; RT64 then computes
//     MVP = projection * modelview = slot0 * loaded, which matches DKR's
//     `result = slot[0] * loaded` semantic.
//   - Slot >0 without multiply → modelview LOAD (DKR expects pre-combined
//     MVP here; we accept that projection will still be slot 0 — visible
//     bug in the rare scenes that use this path, mostly UI overlays).
//
// G_MOVEWORD: segment + fog map cleanly to RT64. Billboard flag is
// recorded; honoring it would need a per-vertex post-MVP hook RT64 does
// not currently expose. MVPMATRIX (switch active matrix without reloading
// data) is not honored — most scenes implicitly re-activate via G_MTX.
//

#include "rt64_gbi_f3ddkr.h"

#include "hle/rt64_interpreter.h"
#include "rt64_gbi_f3d.h"
#include "shared/rt64_f3d_defines.h"

namespace RT64 {
    namespace GBI_F3DDKR {

        // Per-task DKR state. RT64 is single-threaded through the interpreter
        // so a namespace-local is safe. Reset by `reset()` at task start.
        struct DkrState {
            uint32_t vertexDmaOffset = 0;
            uint32_t matrixDmaOffset = 0;
            uint32_t nextAppendIndex = 0;  // tracks G_VTX append cursor
            bool billboardEnabled = false; // Stage 5: recorded, not honored
        };
        static DkrState gDkr;


        static void reset(State *state) {
            gDkr = DkrState{};
            // F3D::reset isn't exported; inline its two calls so we keep RSP
            // defaults sane after replacing resetFromTask below.
            state->rsp->setLookAtVectors(hlslpp::float3(0.0f, 1.0f, 0.0f), hlslpp::float3(1.0f, 0.0f, 0.0f));
            state->rsp->setFog(0x0100, 0x0000);
        }

        // --- G_MTX (0x01) ---
        // DKR encoding:
        //   w0[23:22] = matrix slot index (0-3)
        //   w0[21]    = multiply flag (1 = slot[idx] = slot[0] * loaded)
        //   w0[15:0]  = byte length (always 64 for a single matrix)
        //   w1        = segmented address of FixedMatrix (+ matrixDmaOffset)
        // After loading, the slot becomes the active vertex transform.
        static void mtx(State *state, DisplayList **dl) {
            const uint32_t w0 = (*dl)->w0;
            const uint32_t w1 = (*dl)->w1;
            const uint32_t index = (w0 >> 22) & 0x3;
            const uint32_t multiply = (w0 >> 21) & 0x1;
            const uint32_t address = w1 + gDkr.matrixDmaOffset;

            // RT64's RSP matrix() reads 64-byte FixedMatrix and dispatches to
            // matrixCommon with F3D-style param bits. Construct those bits
            // from DKR's slot/multiply.
            uint8_t params = 0;
            if (index == 0) {
                // Slot 0 → projection LOAD (G_MTX_PROJECTION | G_MTX_LOAD).
                params = uint8_t(state->rsp->projMask | state->rsp->loadMask);
            } else {
                // Slot N>0 → modelview LOAD. If DKR's multiply flag is set,
                // the loaded matrix is just the model component and RT64's
                // projection * modelview pipeline reconstructs the MVP. If
                // not set, the loaded matrix is pre-combined MVP — we still
                // load it as modelview and accept the bug (see file header).
                params = uint8_t(state->rsp->loadMask);
                (void)multiply;
            }

            state->rsp->matrix(address, params);
        }

        // --- G_MOVEWORD (0xBC) ---
        // DKR encoding:
        //   w0[7:0]   = index (which kind of word)
        //   w0[23:8]  = sub-offset
        //   w1        = value
        // Indices DKR uses:
        //   0x02 BILLBOARD: w1[0] = enable flag
        //   0x06 SEGMENT:   offset/4 = segment id, w1 = segment value
        //   0x08 FOG:       w1[31:16] = mul, w1[15:0] = offset
        //   0x0A MVPMATRIX: w1[7:6] = active matrix slot (not honored)
        static void moveWord(State *state, DisplayList **dl) {
            const uint32_t w0 = (*dl)->w0;
            const uint32_t w1 = (*dl)->w1;
            const uint8_t  index  = uint8_t(w0 & 0xFF);
            const uint16_t offset = uint16_t((w0 >> 8) & 0xFFFF);

            switch (index) {
            case F3DDKR_G_MW_BILLBOARD:
                gDkr.billboardEnabled = (w1 & 1) != 0;
                break;
            case F3DDKR_G_MW_SEGMENT: {
                const uint32_t seg = offset / 4;
                if (seg < 16) {
                    state->rsp->setSegment(seg, w1);
                }
                break;
            }
            case F3DDKR_G_MW_FOG:
                state->rsp->setFog(int16_t(w1 >> 16), int16_t(w1 & 0xFFFF));
                break;
            // F3DDKR_G_MW_MATRIX (0x0A): see file header — not honored.
            default:
                break;
            }
        }

        // --- G_VTX (0x04) ---
        // gSPVertexDKR encodes:
        //   w0[23:19] = (count-1) [5 bits, up to 32 verts]
        //   w0[16]    = append flag (1 = append, 0 = replace from index 0)
        //   w1        = segmented address of vertex data (plus DMA offset)
        static void vtx(State *state, DisplayList **dl) {
            const uint32_t w0 = (*dl)->w0;
            const uint32_t w1 = (*dl)->w1;
            const uint32_t count  = ((w0 >> 19) & 0x1F) + 1;
            const uint32_t append = (w0 >> 16) & 0x1;

            const uint32_t dstIndex = append ? gDkr.nextAppendIndex : 0;
            const uint32_t addr = w1 + gDkr.vertexDmaOffset;

            state->rsp->setVertexDKR(addr, count, dstIndex, gDkr.billboardEnabled);
            gDkr.nextAppendIndex = dstIndex + count;
        }

        // --- G_TRIN (0x05) ---
        // gSPPolygon encodes:
        //   w0[23:20] = (numTris-1)
        //   w0[16]    = tex_enabled (informational)
        //   w1        = segmented address of triangle array
        //
        // Each tri struct (16 bytes):
        //   flags(u8) vi0(u8) vi1(u8) vi2(u8) uv0_s(s16) uv0_t(s16)
        //   uv1_s(s16) uv1_t(s16) uv2_s(s16) uv2_t(s16)
        //
        // flags bit 0x40 = draw backface (skip culling for this tri).
        static void trin(State *state, DisplayList **dl) {
            const uint32_t w0 = (*dl)->w0;
            const uint32_t w1 = (*dl)->w1;
            const uint32_t count = ((w0 >> 20) & 0xF) + 1;

            const uint32_t rdramAddr = state->rsp->fromSegmentedMasked(w1);
            uint8_t *RDRAM = state->RDRAM;

            auto read_u8 = [&](uint32_t a) -> uint8_t {
                return RDRAM[a ^ 3];
            };
            auto read_s16 = [&](uint32_t a) -> int16_t {
                uint16_t hi = RDRAM[(a    ) ^ 3];
                uint16_t lo = RDRAM[(a + 1) ^ 3];
                return int16_t((hi << 8) | lo);
            };

            // F3D's G_CULL_BOTH constant from gbi_f3d.cpp setup.
            constexpr uint32_t G_CULL_BOTH_F3D = 0x00003000;

            for (uint32_t i = 0; i < count; i++) {
                const uint32_t triAddr = rdramAddr + i * 16;
                const uint8_t flags = read_u8(triAddr + 0);
                const uint8_t vi0 = read_u8(triAddr + 1);
                const uint8_t vi1 = read_u8(triAddr + 2);
                const uint8_t vi2 = read_u8(triAddr + 3);

                const int16_t s0 = read_s16(triAddr + 4);
                const int16_t t0 = read_s16(triAddr + 6);
                const int16_t s1 = read_s16(triAddr + 8);
                const int16_t t1 = read_s16(triAddr + 10);
                const int16_t s2 = read_s16(triAddr + 12);
                const int16_t t2 = read_s16(triAddr + 14);

                state->rsp->modifyVertex(vi0, G_MWO_POINT_ST,
                    (uint32_t(uint16_t(s0)) << 16) | uint16_t(t0));
                state->rsp->modifyVertex(vi1, G_MWO_POINT_ST,
                    (uint32_t(uint16_t(s1)) << 16) | uint16_t(t1));
                state->rsp->modifyVertex(vi2, G_MWO_POINT_ST,
                    (uint32_t(uint16_t(s2)) << 16) | uint16_t(t2));

                if (flags & 0x40) {
                    state->rsp->clearGeometryMode(G_CULL_BOTH_F3D);
                    state->rsp->drawIndexedTri(vi0, vi1, vi2);
                    state->rsp->setGeometryMode(G_CULL_BOTH_F3D);
                } else {
                    state->rsp->drawIndexedTri(vi0, vi1, vi2);
                }
            }
        }

        // --- G_DMADL (0x07) ---
        // Run N inline sub-DL commands from RDRAM. Sub-DL is count-bounded
        // (no G_ENDDL terminator), so we iterate manually instead of going
        // through runDl/endDl.
        //   w0[23:16] = numCommands
        //   w1        = segmented address of sub-DL
        static void dmaDl(State *state, DisplayList **dl) {
            const uint32_t w0 = (*dl)->w0;
            const uint32_t w1 = (*dl)->w1;
            const uint32_t numCommands = (w0 >> 16) & 0xFF;
            if (numCommands == 0) return;

            const uint32_t rdramAddr = state->rsp->fromSegmentedMasked(w1);
            DisplayList *subDl = reinterpret_cast<DisplayList *>(state->fromRDRAM(rdramAddr));

            GBI *gbi = state->ext.interpreter->hleGBI;
            for (uint32_t i = 0; i < numCommands; i++) {
                DisplayList *cur = &subDl[i];
                const uint8_t op = uint8_t(cur->w0 >> 24);
                GBIFunction func = gbi->map[op];
                if (func == nullptr) {
                    continue;
                }
                DisplayList *cursor = cur;
                func(state, &cursor);
            }
        }

        // --- G_SETCIMG (0xFF) ---
        // DKR sends SEGMENTED addresses (segment id in bits 27:24) where
        // standard F3D usage is physical. F3D's RDP::setColorImage takes
        // w1 raw, so RT64's framebuffer manager would key by the segmented
        // bits and never match VI_ORIGIN's physical address. Resolve here.
        static void setColorImageResolved(State *state, DisplayList **dl) {
            const uint32_t w0 = (*dl)->w0;
            const uint8_t fmt = (w0 >> 21) & 0x7;
            const uint8_t siz = (w0 >> 19) & 0x3;
            const uint16_t width = (w0 & 0xFFF) + 1;
            const uint32_t resolved = state->rsp->fromSegmentedMasked((*dl)->w1);
            state->rdp->setColorImage(fmt, siz, width, resolved);
        }

        // G_SETZIMG (0xFE): DKR also uses segmented addresses here.
        static void setDepthImageResolved(State *state, DisplayList **dl) {
            const uint32_t resolved = state->rsp->fromSegmentedMasked((*dl)->w1);
            state->rdp->setDepthImage(resolved);
        }

        // G_SETTIMG (0xFD): DKR uses segmented addresses for texture image
        // pointers too — these feed loadBlock/loadTile.
        static void setTextureImageResolved(State *state, DisplayList **dl) {
            const uint32_t w0 = (*dl)->w0;
            const uint32_t resolved = state->rsp->fromSegmentedMasked((*dl)->w1);
            const uint8_t fmt = (w0 >> 21) & 0x7;
            const uint8_t siz = (w0 >> 19) & 0x3;
            const uint16_t width = (w0 & 0xFFF) + 1;
            state->rdp->setTextureImage(fmt, siz, width, resolved);
        }

        // --- G_DMA_OFFSETS (0xBF) ---
        //   w0 & 0x00FFFFFF = matrix DMA offset
        //   w1 & 0x00FFFFFF = vertex DMA offset
        static void dmaOffsets(State *state, DisplayList **dl) {
            const uint32_t w0 = (*dl)->w0;
            const uint32_t w1 = (*dl)->w1;
            gDkr.matrixDmaOffset = w0 & 0x00FFFFFF;
            gDkr.vertexDmaOffset = w1 & 0x00FFFFFF;
        }

        void setup(GBI *gbi) {
            // Inherit F3D's full opcode set, then override DKR-specific ones.
            GBI_F3D::setup(gbi);

            gbi->map[F3DDKR_G_MTX]          = &mtx;
            gbi->map[F3DDKR_G_VTX]          = &vtx;
            gbi->map[F3DDKR_G_TRIN]         = &trin;
            gbi->map[F3DDKR_G_DMADL]        = &dmaDl;
            gbi->map[F3DDKR_G_MOVEWORD]     = &moveWord;
            gbi->map[F3DDKR_G_DMA_OFFSETS]  = &dmaOffsets;
            // DKR sends segmented addresses to G_SETCIMG / G_SETZIMG / G_SETTIMG.
            // Override F3D's bindings (which take w1 raw) with segment-resolving
            // wrappers so RT64's framebuffer manager and texture cache can match
            // by physical address.
            gbi->map[0xFF /* G_SETCIMG */]  = &setColorImageResolved;
            gbi->map[0xFE /* G_SETZIMG */]  = &setDepthImageResolved;
            gbi->map[0xFD /* G_SETTIMG */]  = &setTextureImageResolved;
            // 0xBF in F3D is G_TRI1; in F3DDKR it is G_DMA_OFFSETS. The line
            // above replaces F3D's tri1 binding, which is correct — DKR
            // never sends a G_TRI1 there.

            gbi->resetFromTask = &reset;
        }

    }  // namespace GBI_F3DDKR
}  // namespace RT64
