//
// RT64
//

#include "rt64_enhancement_configuration.h"

namespace RT64 {
    // EnhancementConfiguration
    
    EnhancementConfiguration::EnhancementConfiguration() {
        framebuffer.reinterpretFixULS = true;
        presentation.mode = Presentation::Mode::Console;  // Rampage: respect double-buffering to avoid ghost images
        presentation.removeBlackBorders = false;  // Rampage: 400x240 FB needs 4:3 via viewport, not cropping
        rect.fixRectLR = true;
        f3dex.forceBranch = false;
        s2dex.fixBilerpMismatch = true;
        s2dex.framebufferFastPath = true;
        textureLOD.scale = false;
    }
};