/*
 * Copyright (C) 2022 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "ImageBufferContextSwitcher.h"

#include "Filter.h"
#include "FilterResults.h"
#include "GraphicsContext.h"
#include "ImageBuffer.h"
#include <wtf/TZoneMallocInlines.h>

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(ImageBufferContextSwitcher);

ImageBufferContextSwitcher::ImageBufferContextSwitcher(GraphicsContext& destinationContext, const FloatRect& sourceImageRect, const DestinationColorSpace& colorSpace, RefPtr<Filter>&& filter, FilterResults* results)
    : GraphicsContextSwitcher(WTFMove(filter))
    , m_sourceImageRect(sourceImageRect)
    , m_results(results)
{
    if (sourceImageRect.isEmpty())
        return;

    if (m_filter)
        m_sourceImage = destinationContext.createScaledImageBuffer(m_sourceImageRect, m_filter->filterScale(), colorSpace, m_filter->renderingMode());
    else
        m_sourceImage = destinationContext.createAlignedImageBuffer(m_sourceImageRect, colorSpace);

    if (!m_sourceImage) {
        m_filter = nullptr;
        return;
    }

    auto state = destinationContext.state();
    m_sourceImage->context().mergeAllChanges(state);
}

GraphicsContext* ImageBufferContextSwitcher::drawingContext(GraphicsContext& context) const
{
    return m_sourceImage ? &m_sourceImage->context() : &context;
}

void ImageBufferContextSwitcher::beginClipAndDrawSourceImage(GraphicsContext& destinationContext, const FloatRect& repaintRect, const FloatRect&)
{
    if (auto* context = drawingContext(destinationContext)) {
        context->save();
        context->clearRect(repaintRect);
        context->clip(repaintRect);
    }
}

void ImageBufferContextSwitcher::endClipAndDrawSourceImage(GraphicsContext& destinationContext, const DestinationColorSpace& colorSpace)
{
    if (auto* context = drawingContext(destinationContext))
        context->restore();

    endDrawSourceImage(destinationContext, colorSpace);
}

void ImageBufferContextSwitcher::endDrawSourceImage(GraphicsContext& destinationContext, const DestinationColorSpace& colorSpace)
{
    if (!m_filter) {
        if (m_sourceImage)
            destinationContext.drawImageBuffer(*m_sourceImage, m_sourceImageRect, { destinationContext.compositeOperation(), destinationContext.blendMode() });
        return;
    }

    FilterResults results;
#if USE(CAIRO)
    // Cairo operates in SRGB which is why the SourceImage initially is in SRGB color space,
    // but before applying all filters it has to be transformed to LinearRGB to comply with
    // specification (https://www.w3.org/TR/filter-effects-1/#attr-valuedef-in-sourcegraphic).
    if (m_sourceImage)
        m_sourceImage->transformToColorSpace(colorSpace);
#else
    UNUSED_PARAM(colorSpace);
#endif
    destinationContext.drawFilteredImageBuffer(m_sourceImage.get(), m_sourceImageRect, Ref { *m_filter }, m_results ? *m_results : results);
}

} // namespace WebCore
