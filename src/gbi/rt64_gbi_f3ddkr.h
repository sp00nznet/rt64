//
// RT64
//
// F3DDKR: Diddy Kong Racing's graphics microcode (see rt64_gbi_f3ddkr.cpp).
//

#pragma once

#include "rt64_gbi.h"

// DKR-specific RSP opcodes
#define F3DDKR_G_SPNOOP           0x00
#define F3DDKR_G_MTX              0x01
#define F3DDKR_G_VTX              0x04  // 10-byte verts (no UVs); per-tri UVs come from G_TRIN
#define F3DDKR_G_TRIN             0x05  // Batched tris with per-tri UVs
#define F3DDKR_G_DL               0x06
#define F3DDKR_G_DMADL            0x07  // DMA-based DL call
#define F3DDKR_G_MOVEWORD         0xBC
#define F3DDKR_G_ENDDL            0xB8

// MoveWord param ids (G_MW_*) DKR uses
#define F3DDKR_G_MW_BILLBOARD     0x02
#define F3DDKR_G_MW_SEGMENT       0x06
#define F3DDKR_G_MW_FOG           0x08
#define F3DDKR_G_MW_MVPMATRIX     0x0A  // w1 >> 6 = the matrix slot to make active

namespace RT64 {
    namespace GBI_F3DDKR {
        void setup(GBI *ucode);
    };
};
