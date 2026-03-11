//
// RT64
//

#include "rt64_vi_renderer.h"

#include "shared/rt64_hlsl.h"
#include "shared/rt64_video_interface.h"

namespace RT64 {
    // VIRenderer

    VIRenderer::VIRenderer() { }

    VIRenderer::~VIRenderer() { }

    inline hlslpp::float2 computeHDSize(hlslpp::float2 sdSize, hlslpp::float2 resolutionScale, uint32_t downsamplingScale) {
        return (sdSize * resolutionScale) / float(downsamplingScale);
    }

    inline hlslpp::float2 fromSDtoHD(hlslpp::float2 coordinate, hlslpp::float2 sdSize, hlslpp::float2 hdSize) {
        const hlslpp::float2 relativeScale = hdSize / sdSize;
        return coordinate * relativeScale;
    }

    inline hlslpp::float2 fromHDtoWindow(hlslpp::float2 coordinate, hlslpp::float2 hdSize, hlslpp::float2 windowSize) {
        const hlslpp::float2 hdCenter = hdSize / 2;
        const hlslpp::float2 windowCenter = windowSize / 2;
        const hlslpp::float2 relativeCoordinate = { coordinate.x - hdCenter.x, coordinate.y - hdCenter.y };

        // Window is wider than virtual HD TV, we do pillarboxing.
        float relativeScale;
        if ((windowSize.x / windowSize.y) > (hdSize.x / hdSize.y)) {
            relativeScale = windowSize.y / hdSize.y;
        }
        // Window is taller than virtual HD TV, we do letterboxing.
        else {
            relativeScale = windowSize.x / hdSize.x;
        }

        return windowCenter + relativeCoordinate * relativeScale;
    }

    void VIRenderer::render(const RenderParams &p) {
        static int vi_render_count = 0;
        vi_render_count++;
        if (vi_render_count <= 5 || vi_render_count % 300 == 0) {
            auto fbSz = p.vi->fbSize();
            fprintf(stderr, "[VI-RENDER] #%d: texW=%u texH=%u fbSize=(%u,%u) resScale=(%.2f,%.2f) ds=%u rmBB=%d gamma=%.3f\n",
                    vi_render_count, p.textureWidth, p.textureHeight,
                    (uint32_t)fbSz.x, (uint32_t)fbSz.y,
                    (float)p.resolutionScale.x, (float)p.resolutionScale.y,
                    p.downsamplingScale, (int)p.removeBlackBorders, p.vi->gamma());
            fflush(stderr);
        }
        const ShaderRecord *shader = nullptr;
        const RenderSampler *sampler = nullptr;
        switch (p.filtering) {
        case UserConfiguration::Filtering::Nearest:
            shader = &p.shaderLibrary->videoInterfaceNearest;
            sampler = p.shaderLibrary->samplerLibrary.nearest.borderBorder.get();
            break;
        case UserConfiguration::Filtering::AntiAliasedPixelScaling:
            shader = &p.shaderLibrary->videoInterfacePixel;
            sampler = p.shaderLibrary->samplerLibrary.linear.borderBorder.get();
            break;
        case UserConfiguration::Filtering::Linear:
        default:
            shader = &p.shaderLibrary->videoInterfaceLinear;
            sampler = p.shaderLibrary->samplerLibrary.linear.borderBorder.get();
            break;
        }

        if ((descriptorSet == nullptr) || (descriptorSetSampler != sampler)) {
            descriptorSet = std::make_unique<VideoInterfaceDescriptorSet>(sampler, p.device);
            descriptorSetSampler = sampler;
        }

        descriptorSet->setTexture(descriptorSet->gInput, p.texture, RenderTextureLayout::SHADER_READ);

        RenderViewport viewport;
        RenderRect scissor;
        getViewportAndScissor(p.swapChain, *p.vi, p.resolutionScale, p.downsamplingScale, p.removeBlackBorders, viewport, scissor);
        p.commandList->setViewports(viewport);
        p.commandList->setScissors(scissor);

        interop::VideoInterfaceCB pushConstants;
        pushConstants.textureResolution = { float(p.textureWidth), float(p.textureHeight) };
        // Set videoResolution to match textureResolution so the shader samples the
        // full texture across the viewport. The viewRectangle/cropRectangle handle
        // the 4:3 aspect ratio correction by sizing the viewport appropriately.
        pushConstants.videoResolution = pushConstants.textureResolution;
        pushConstants.gamma = p.vi->gamma();

        p.commandList->setPipeline(shader->pipeline.get());
        p.commandList->setGraphicsPipelineLayout(shader->pipelineLayout.get());
        p.commandList->setGraphicsDescriptorSet(descriptorSet->get(), 0);
        p.commandList->setGraphicsPushConstants(0, &pushConstants);
        p.commandList->setVertexBuffers(0, nullptr, 0, nullptr);
        p.commandList->drawInstanced(3, 1, 0, 0);
    }

    void VIRenderer::getViewportAndScissor(const RenderSwapChain *swapChain, const VI &vi, hlslpp::float2 resolutionScale, uint32_t downsamplingScale, bool removeBlackBorders, RenderViewport &viewport, RenderRect &scissor) {
        // We define three different coordinate spaces to work with to translate the VI parameters into the Window.
        //
        // VideoSD: This corresponds to the SD TV Scanline space, which is what the VI natively works on.
        // 
        // VideoHD: This corresponds to what the imaginary "HD" TV would be if it supported the amount of scanlines
        // desired by the resolution scale (divided by the downsampling scale) and a wider aspect ratio.
        // 
        // Window: The native coordinate space of the device's render target.
        //
        // The provided buffer doesn't necessarily have the same dimensions that the VI will sample to display it 
        // on the screen. To work around that, the viewport the buffer will be drawn in will be expanded so only
        // the region of interest is rendered. A scissor will cut it off correctly according to the coordinates
        // specified by the VI.
        const hlslpp::float2 sdSize = removeBlackBorders ? hlslpp::float2(vi.fbSize()) : hlslpp::float2(320.0f, 240.0f);
        const hlslpp::float2 hdSize = computeHDSize(sdSize, resolutionScale, downsamplingScale);
        const hlslpp::float2 windowSize = { float(swapChain->getWidth()), float(swapChain->getHeight()) };

        // Query the VI for the current rendering area.
        hlslpp::float4 viViewRect = vi.viewRectangle() * sdSize.xyxy;
        hlslpp::float4 viCropRect = vi.cropRectangle() * sdSize.xyxy;

        // Scale all the rectangles to the space of the Window.
        hlslpp::float2 topLeftViewport = fromSDtoHD({ float(viViewRect.x), float(viViewRect.y) }, sdSize, hdSize);
        hlslpp::float2 bottomRightViewport = fromSDtoHD({ float(viViewRect.x + viViewRect.z), float(viViewRect.y + viViewRect.w) }, sdSize, hdSize);
        topLeftViewport = fromHDtoWindow(topLeftViewport, hdSize, windowSize);
        bottomRightViewport = fromHDtoWindow(bottomRightViewport, hdSize, windowSize);

        hlslpp::float2 topLeftScissor = fromSDtoHD({ float(viCropRect.x), float(viCropRect.y) }, sdSize, hdSize);
        hlslpp::float2 bottomRightScissor = fromSDtoHD({ float(viCropRect.x + viCropRect.z), float(viCropRect.y + viCropRect.w) }, sdSize, hdSize);
        topLeftScissor = fromHDtoWindow(topLeftScissor, hdSize, windowSize);
        bottomRightScissor = fromHDtoWindow(bottomRightScissor, hdSize, windowSize);

        viewport = RenderViewport(topLeftViewport.x, topLeftViewport.y, bottomRightViewport.x - topLeftViewport.x, bottomRightViewport.y - topLeftViewport.y);
        scissor = RenderRect(lround(topLeftScissor.x), lround(topLeftScissor.y), lround(bottomRightScissor.x), lround(bottomRightScissor.y));

        static int gvs_count = 0;
        gvs_count++;
        if (gvs_count <= 5 || gvs_count % 300 == 0) {
            fprintf(stderr, "[VI-VIEWPORT] #%d: sdSize=(%.0f,%.0f) hdSize=(%.0f,%.0f) winSize=(%.0f,%.0f) "
                    "viewRect=(%.1f,%.1f,%.1f,%.1f) cropRect=(%.1f,%.1f,%.1f,%.1f) "
                    "viewport=(%.1f,%.1f,%.1f,%.1f) scissor=(%ld,%ld,%ld,%ld) rmBB=%d\n",
                    gvs_count,
                    (float)sdSize.x, (float)sdSize.y, (float)hdSize.x, (float)hdSize.y,
                    (float)windowSize.x, (float)windowSize.y,
                    (float)viViewRect.x, (float)viViewRect.y, (float)viViewRect.z, (float)viViewRect.w,
                    (float)viCropRect.x, (float)viCropRect.y, (float)viCropRect.z, (float)viCropRect.w,
                    viewport.x, viewport.y, viewport.width, viewport.height,
                    scissor.left, scissor.top, scissor.right, scissor.bottom,
                    (int)removeBlackBorders);
            fflush(stderr);
        }
    }
};