// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AVDexedNativeFade.h"

#if JUCE_MAC
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

// Each picture is a Metal layer made like the one JUCE shows its window in
// (CoreGraphicsMetalLayerRenderer: BGRA8 sRGB pixels, no colour space, the
// CPU-painted pixels copied in as they are). The compositor then treats the
// pictures exactly as the window's own content (a CALayer holding a CGImage
// went through colour matching instead, and dark tones came out a few levels
// apart), and only multiplies them by their opacity.

bool AVDexedNativeFade::start(juce::ComponentPeer& peer, float backing, const std::vector<Picture>& pictures, double durationMs) {
    stop();
    if (! enabled || backing <= 0.0f || pictures.empty()) return false;
    auto* view = (NSView*) peer.getNativeHandle();
    CALayer* host = view != nil ? view.layer : nil;
    if (host == nil) return false;
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil) return false;
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];

    [CATransaction begin];
    [CATransaction setDisableActions: YES];
    CALayer* layers = [[CALayer alloc] init];
    layers.frame = host.bounds;
    layers.zPosition = 10000.0f;
    // Sublayer positions follow the view's own (flipped) coordinates only when
    // the host layer's geometry is flipped; otherwise y grows upwards.
    const bool flipped = host.geometryFlipped;
    const double height = host.bounds.size.height;
    const CFTimeInterval now = CACurrentMediaTime();
    bool complete = true;
    for (const auto& picture : pictures) {
        using namespace juce;
        if (picture.image.isNull() || picture.image.getFormat() != Image::ARGB) {
            complete = false;
            break;
        }
        const Image::BitmapData pixels(picture.image, Image::BitmapData::readOnly);
        auto* description = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat: MTLPixelFormatBGRA8Unorm_sRGB
                                                                                width: (NSUInteger) pixels.width
                                                                               height: (NSUInteger) pixels.height
                                                                            mipmapped: NO];
        description.storageMode = MTLStorageModeManaged;
        description.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> source = [device newTextureWithDescriptor: description];
        CAMetalLayer* layer = [CAMetalLayer layer];
        layer.device = device;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;
        layer.framebufferOnly = NO;
        layer.opaque = YES;
        layer.contentsScale = backing;
        layer.magnificationFilter = kCAFilterNearest;
        layer.minificationFilter = kCAFilterNearest;
        layer.drawableSize = CGSizeMake(pixels.width, pixels.height);
        const auto area = picture.pixels.toDouble() / (double) backing;
        layer.frame = CGRectMake(area.getX(), flipped ? area.getY() : height - area.getBottom(), area.getWidth(), area.getHeight());
        id<CAMetalDrawable> drawable = source != nil ? [layer nextDrawable] : nil;
        if (drawable == nil) {
            [source release];
            complete = false;
            break;
        }
        [source replaceRegion: MTLRegionMake2D(0, 0, (NSUInteger) pixels.width, (NSUInteger) pixels.height)
                  mipmapLevel: 0
                    withBytes: pixels.data
                  bytesPerRow: (NSUInteger) pixels.lineStride];
        [blit copyFromTexture: source sourceSlice: 0 sourceLevel: 0 sourceOrigin: MTLOriginMake(0, 0, 0)
                   sourceSize: MTLSizeMake((NSUInteger) pixels.width, (NSUInteger) pixels.height, 1)
                    toTexture: drawable.texture destinationSlice: 0 destinationLevel: 0
            destinationOrigin: MTLOriginMake(0, 0, 0)];
        [source release];
        [commands presentDrawable: drawable];
        layer.opacity = picture.toAlpha;
        if (picture.fromAlpha != picture.toAlpha) {
            CABasicAnimation* fade = [CABasicAnimation animationWithKeyPath: @"opacity"];
            fade.fromValue = @(picture.fromAlpha);
            fade.toValue = @(picture.toAlpha);
            fade.duration = juce::jmax(0.001, durationMs / 1000.0);
            fade.beginTime = now;
            fade.timingFunction = [CAMediaTimingFunction functionWithName: kCAMediaTimingFunctionLinear];
            fade.fillMode = kCAFillModeBoth;
            fade.removedOnCompletion = NO;
            [layer addAnimation: fade forKey: @"fade"];
        }
        [layers addSublayer: layer];
    }
    [blit endEncoding];
    if (complete) {
        [commands commit];
        [commands waitUntilScheduled];
        [host addSublayer: layers];
    }
    [CATransaction commit];
    [queue release];
    [device release];
    if (! complete) {
        [layers release];
        return false;
    }
    container = (void*) layers;   // keeps the +1 from alloc
    return true;
}

void AVDexedNativeFade::stop() {
    if (container == nullptr) return;
    auto* layers = (CALayer*) container;
    container = nullptr;
    [CATransaction begin];
    [CATransaction setDisableActions: YES];
    [layers removeFromSuperlayer];
    [CATransaction commit];
    [layers release];
}
#endif
