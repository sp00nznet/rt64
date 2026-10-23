//
// RT64
//
// F3DDKR: Diddy Kong Racing's graphics microcode (Rare's "xbus" variant of Fast3D).
//
// Encodings follow the DKR decompilation's include/f3ddkr.h (CC0,
// https://github.com/DavidSM64/Diddy-Kong-Racing), which documents what the game sends:
//
//  - Matrices live in four slots. G_MTX loads a complete model-view-projection matrix into a slot
//    and makes it the active one; G_MOVEWORD G_MW_MVPMATRIX makes another slot active. There is no
//    projection/modelview split, so the active slot is handed to RT64 as the modelview over a fixed
//    projection (see selectSlot).
//  - Vertices are 10 bytes (s16 x, y, z; u8 r, g, b, a) with no texture coordinates; G_TRIN
//    triangles carry each corner's s/t, and a flag that turns backface culling off for that triangle.
//  - Billboards: while enabled, each vertex loaded after the anchor (vertex 0) gets the anchor's
//    clip-space position added after its own transform, before the perspective divide. RT64
//    transforms vertices on the GPU, so the result is passed as a screen-position override.
//  - G_DMADL runs a count-bounded block of commands from memory (no G_ENDDL).
//  - G_SETCIMG, G_SETZIMG and G_SETTIMG get segmented addresses.
//

#include "rt64_gbi_f3ddkr.h"

#include "hle/rt64_interpreter.h"
#include "rt64_gbi_f3d.h"
#include "shared/rt64_f3d_defines.h"

namespace RT64 { struct DisplayList; }
namespace RT64 {
    namespace GBI_F3DDKR {
        constexpr uint32_t CullFront = 0x1000; // F3D G_CULL_FRONT
        constexpr uint32_t CullBack = 0x2000;  // F3D G_CULL_BACK
        constexpr uint32_t CullBoth = CullFront | CullBack;

        struct DkrState {
            hlslpp::float4x4 slots[4];
            uint32_t slotAddress[4] = {};
            uint32_t vertexCount = 0; // size of the last non-appending G_VTX: appends land right after it
            bool billboard = false;
        };
        static DkrState dkr; // the interpreter runs one display list at a time

        // RT64 tags each vertex with the modelview current when it's loaded, but picks up projection
        // changes only when a triangle is drawn, and DKR switches matrices between vertex loads (one
        // per bone). So the projection stays fixed for the whole task: a plain perspective matrix,
        // which also keeps RT64 treating the scene as 3D (depth, clipping). Each slot goes in as the
        // modelview slot * inverse(projection), so modelview * projection is exactly the slot.
        static const hlslpp::float4x4 FixedProj(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, -1.0f, 0.0f, 0.0f, -2.0f, 0.0f);
        static const hlslpp::float4x4 FixedProjInverse = hlslpp::inverse(FixedProj);

        static void selectSlot(State *state, uint32_t slot) {
            RSP *rsp = state->rsp.get();
            rsp->matrixCommon(hlslpp::mul(dkr.slots[slot], FixedProjInverse), dkr.slotAddress[slot], rsp->loadMask);
        }

        static void reset(State *state) {
            dkr = DkrState{};
            for (auto &m : dkr.slots) m = hlslpp::float4x4::identity();
            RSP *rsp = state->rsp.get();
            rsp->matrixCommon(FixedProj, 0, uint8_t(rsp->projMask | rsp->loadMask));
            state->rsp->setLookAtVectors(hlslpp::float3(0.0f, 1.0f, 0.0f), hlslpp::float3(1.0f, 0.0f, 0.0f));
            state->rsp->setFog(0x0100, 0x0000);
        }

        // G_MTX (0x01): w0[23:22] = slot, w1 = address of a fixed-point Mtx.
        static void mtx(State *state, DisplayList **dl) {
            const uint32_t slot = ((*dl)->w0 >> 22) & 0x3;
            const uint32_t address = (*dl)->w1;
            const uint32_t rdramAddress = state->rsp->fromSegmentedMasked(address);
            dkr.slots[slot] = reinterpret_cast<const FixedMatrix *>(state->fromRDRAM(rdramAddress))->toMatrix4x4();
            dkr.slotAddress[slot] = address;
            selectSlot(state, slot);
        }

        // G_MOVEWORD (0xBC): w0[7:0] = index, w0[23:8] = offset, w1 = value.
        static void moveWord(State *state, DisplayList **dl) {
            const uint32_t w0 = (*dl)->w0;
            const uint32_t w1 = (*dl)->w1;
            const uint16_t offset = uint16_t((w0 >> 8) & 0xFFFF);
            switch (w0 & 0xFF) {
            case F3DDKR_G_MW_BILLBOARD:
                dkr.billboard = (w1 & 1) != 0;
                break;
            case F3DDKR_G_MW_MVPMATRIX:
                selectSlot(state, (w1 >> 6) & 0x3);
                break;
            case F3DDKR_G_MW_SEGMENT:
                if ((offset / 4) < 16) state->rsp->setSegment(offset / 4, w1);
                break;
            case F3DDKR_G_MW_FOG:
                state->rsp->setFog(int16_t(w1 >> 16), int16_t(w1 & 0xFFFF));
                break;
            default: // clip ratio and the rest: nothing RT64 uses
                break;
            }
        }

        // G_VTX (0x04): w0[23:19] = count - 1, w0[16] = append, w1 = address.
        static void vtx(State *state, DisplayList **dl) {
            const uint32_t w0 = (*dl)->w0;
            const uint32_t count = ((w0 >> 19) & 0x1F) + 1;
            const uint32_t dst = ((w0 >> 16) & 0x1) ? dkr.vertexCount : 0;
            if (dst + count > RSP_MAX_VERTICES) return;
            state->rsp->setVertexDKR((*dl)->w1, count, dst, dkr.billboard);
            if (dst == 0) dkr.vertexCount = count; // appends never move the cursor (f3ddkr.h)
        }

        // G_TRIN (0x05): w0[23:20] = count - 1, w0[16] = textured, w1 = address of 16-byte Triangles:
        // flags (0x40 = draw the back face), three vertex indices, then three s16 s/t pairs.
        static void trin(State *state, DisplayList **dl) {
            const uint32_t w0 = (*dl)->w0;
            const uint32_t count = ((w0 >> 20) & 0xF) + 1;
            const bool textured = ((w0 >> 16) & 0x1) != 0;
            const uint32_t base = state->rsp->fromSegmentedMasked((*dl)->w1);
            const uint8_t *RDRAM = state->RDRAM;
            auto u8 = [&](uint32_t a) { return RDRAM[a ^ 3]; };
            auto u16 = [&](uint32_t a) { return uint32_t((u8(a) << 8) | u8(a + 1)); };

            RSP *rsp = state->rsp.get();
            const uint32_t mode = rsp->geometryModeStack[rsp->geometryModeStackSize - 1];
            // The game turns on G_CULL_FRONT for mirrored tracks, where the winding flips.
            const uint32_t cull = (mode & CullFront) ? CullFront : CullBack;
            uint32_t current = mode & CullBoth;
            for (uint32_t i = 0; i < count; i++) {
                const uint32_t tri = base + i * 16;
                const uint8_t vi[3] = { u8(tri + 1), u8(tri + 2), u8(tri + 3) };
                if ((vi[0] >= RSP_MAX_VERTICES) || (vi[1] >= RSP_MAX_VERTICES) || (vi[2] >= RSP_MAX_VERTICES)) continue;
                if (textured) {
                    for (int c = 0; c < 3; c++) rsp->modifyVertex(vi[c], G_MWO_POINT_ST, (u16(tri + 4 + c * 4) << 16) | u16(tri + 6 + c * 4));
                }
                const uint32_t want = (u8(tri) & 0x40) ? 0 : cull;
                if (want != current) {
                    rsp->modifyGeometryMode(~CullBoth, want);
                    current = want;
                }
                rsp->drawIndexedTri(vi[0], vi[1], vi[2]);
            }
            if (current != (mode & CullBoth)) rsp->modifyGeometryMode(~CullBoth, mode & CullBoth);
        }

        // G_DMADL (0x07): w0[23:16] = number of commands, w1 = their address.
        static void dmaDl(State *state, DisplayList **dl) {
            const uint32_t count = ((*dl)->w0 >> 16) & 0xFF;
            DisplayList *cmd = reinterpret_cast<DisplayList *>(state->fromRDRAM(state->rsp->fromSegmentedMasked((*dl)->w1)));
            DisplayList *end = cmd + count;
            GBI *gbi = state->ext.interpreter->hleGBI;
            for (; cmd < end; cmd++) {
                if (GBIFunction func = gbi->map[cmd->w0 >> 24]) func(state, &cmd); // may consume extra words
            }
        }

        static void setColorImage(State *state, DisplayList **dl) {
            const uint32_t w0 = (*dl)->w0;
            state->rdp->setColorImage((w0 >> 21) & 0x7, (w0 >> 19) & 0x3, (w0 & 0xFFF) + 1, state->rsp->fromSegmentedMasked((*dl)->w1));
        }

        static void setDepthImage(State *state, DisplayList **dl) {
            state->rdp->setDepthImage(state->rsp->fromSegmentedMasked((*dl)->w1));
        }

        static void setTextureImage(State *state, DisplayList **dl) {
            const uint32_t w0 = (*dl)->w0;
            state->rdp->setTextureImage((w0 >> 21) & 0x7, (w0 >> 19) & 0x3, (w0 & 0xFFF) + 1, state->rsp->fromSegmentedMasked((*dl)->w1));
        }

        void setup(GBI *gbi) {
            GBI_F3D::setup(gbi); // everything else is Fast3D's
            gbi->map[F3DDKR_G_MTX] = &mtx;
            gbi->map[F3DDKR_G_VTX] = &vtx;
            gbi->map[F3DDKR_G_TRIN] = &trin;
            gbi->map[F3DDKR_G_DMADL] = &dmaDl;
            gbi->map[F3DDKR_G_MOVEWORD] = &moveWord;
            gbi->map[0xFF] = &setColorImage;   // G_SETCIMG
            gbi->map[0xFE] = &setDepthImage;   // G_SETZIMG
            gbi->map[0xFD] = &setTextureImage; // G_SETTIMG
            gbi->resetFromTask = &reset;
        }
    }
}
