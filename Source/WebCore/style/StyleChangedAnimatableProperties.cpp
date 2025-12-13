/*
 * Copyright (C) 2004-2023 Apple Inc. All rights reserved.
 * Copyright (C) 2025 Samuel Weinig <sam@webkit.org>
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
#include "StyleChangedAnimatableProperties.h"

#include "RenderStyleInlines.h"
#include "WebAnimationTypes.h"

namespace WebCore {
namespace Style {

void conservativelyCollectChangedAnimatableProperties(const RenderStyle& a, const RenderStyle& b, CSSPropertiesBitSet& changingProperties)
{
    // FIXME: Consider auto-generating this function from CSSProperties.json.

    // This function conservatively answers what CSS properties we should visit for CSS transitions.
    // We do not need to precisely check equivalence before saying "this property needs to be visited".
    // Right now, we are designing this based on Speedometer3.0 data.

    auto conservativelyCollectChangedAnimatablePropertiesViaInheritedFlags = [&](auto& a, auto& b) {
        if (a.emptyCells != b.emptyCells)
            changingProperties.m_properties.set(CSSPropertyEmptyCells);
        if (a.captionSide != b.captionSide)
            changingProperties.m_properties.set(CSSPropertyCaptionSide);
        if (a.listStylePosition != b.listStylePosition)
            changingProperties.m_properties.set(CSSPropertyListStylePosition);
        if (a.visibility != b.visibility)
            changingProperties.m_properties.set(CSSPropertyVisibility);
        if (a.textAlign != b.textAlign)
            changingProperties.m_properties.set(CSSPropertyTextAlign);
        if (a.textTransform != b.textTransform)
            changingProperties.m_properties.set(CSSPropertyTextTransform);
        if (a.textDecorationLineInEffect != b.textDecorationLineInEffect)
            changingProperties.m_properties.set(CSSPropertyTextDecorationLine);
        if (a.cursorType != b.cursorType)
            changingProperties.m_properties.set(CSSPropertyCursor);
        if (a.whiteSpaceCollapse != b.whiteSpaceCollapse)
            changingProperties.m_properties.set(CSSPropertyWhiteSpaceCollapse);
        if (a.textWrapMode != b.textWrapMode)
            changingProperties.m_properties.set(CSSPropertyTextWrapMode);
        if (a.textWrapStyle != b.textWrapStyle)
            changingProperties.m_properties.set(CSSPropertyTextWrapStyle);
        if (a.borderCollapse != b.borderCollapse)
            changingProperties.m_properties.set(CSSPropertyBorderCollapse);
        if (a.printColorAdjust != b.printColorAdjust)
            changingProperties.m_properties.set(CSSPropertyPrintColorAdjust);
        if (a.pointerEvents != b.pointerEvents)
            changingProperties.m_properties.set(CSSPropertyPointerEvents);

        // Writing mode changes conversion of logical -> physical properties.
        // Thus we need to list up all physical properties.
        if (a.writingMode != b.writingMode) {
            changingProperties.m_properties.merge(CSSProperty::physicalProperties);
            if (WritingMode(a.writingMode).isVerticalTypographic() != WritingMode(b.writingMode).isVerticalTypographic())
                changingProperties.m_properties.set(CSSPropertyTextEmphasisStyle);
        }

        // insideLink changes visited / non-visited colors.
        // Thus we need to list up all color properties.
        if (a.insideLink != b.insideLink)
            changingProperties.m_properties.merge(CSSProperty::colorProperties);

        // Non animated styles are followings.
        // cursorVisibility
        // boxDirection
        // rtlOrdering
        // autosizeStatus
        // hasExplicitlySetColor
    };

    auto conservativelyCollectChangedAnimatablePropertiesViaNonInheritedFlags = [&](auto& a, auto& b) {
        if (a.overflowX != b.overflowX)
            changingProperties.m_properties.set(CSSPropertyOverflowX);
        if (a.overflowY != b.overflowY)
            changingProperties.m_properties.set(CSSPropertyOverflowY);
        if (a.clear != b.clear)
            changingProperties.m_properties.set(CSSPropertyClear);
        if (a.position != b.position)
            changingProperties.m_properties.set(CSSPropertyPosition);
        if (a.effectiveDisplay != b.effectiveDisplay)
            changingProperties.m_properties.set(CSSPropertyDisplay);
        if (a.floating != b.floating)
            changingProperties.m_properties.set(CSSPropertyFloat);
        if (a.textDecorationLine != b.textDecorationLine)
            changingProperties.m_properties.set(CSSPropertyTextDecorationLine);

        // Non animated styles are followings.
        // originalDisplay
        // unicodeBidi
        // usesViewportUnits
        // usesContainerUnits
        // useTreeCountingFunctions
        // hasExplicitlyInheritedProperties
        // disallowsFastPathInheritance
        // hasContentNone
        // emptyState
        // firstChildState
        // lastChildState
        // isLink
        // pseudoElementType
        // pseudoBits
    };

    auto conservativelyCollectChangedAnimatablePropertiesViaTransformData = [&](auto& a, auto& b) {
        if (a.origin.x != b.origin.x)
            changingProperties.m_properties.set(CSSPropertyTransformOriginX);
        if (a.origin.y != b.origin.y)
            changingProperties.m_properties.set(CSSPropertyTransformOriginY);
        if (a.origin.z != b.origin.z)
            changingProperties.m_properties.set(CSSPropertyTransformOriginZ);
        if (static_cast<TransformBox>(a.transformBox) != static_cast<TransformBox>(b.transformBox))
            changingProperties.m_properties.set(CSSPropertyTransformBox);
        if (a.transform != b.transform)
            changingProperties.m_properties.set(CSSPropertyTransform);
    };

    auto conservativelyCollectChangedAnimatablePropertiesViaNonInheritedBoxData = [&](auto& a, auto& b) {
        if (a.width != b.width)
            changingProperties.m_properties.set(CSSPropertyWidth);
        if (a.height != b.height)
            changingProperties.m_properties.set(CSSPropertyHeight);
        if (a.minWidth != b.minWidth)
            changingProperties.m_properties.set(CSSPropertyMinWidth);
        if (a.maxWidth != b.maxWidth)
            changingProperties.m_properties.set(CSSPropertyMaxWidth);
        if (a.minHeight != b.minHeight)
            changingProperties.m_properties.set(CSSPropertyMinHeight);
        if (a.maxHeight != b.maxHeight)
            changingProperties.m_properties.set(CSSPropertyMaxHeight);
        if (a.verticalAlign != b.verticalAlign)
            changingProperties.m_properties.set(CSSPropertyVerticalAlign);
        if (a.specifiedZIndex() != b.specifiedZIndex())
            changingProperties.m_properties.set(CSSPropertyZIndex);
        if (static_cast<BoxSizing>(a.boxSizing) != static_cast<BoxSizing>(b.boxSizing))
            changingProperties.m_properties.set(CSSPropertyBoxSizing);
        if (static_cast<BoxDecorationBreak>(a.boxDecorationBreak) != static_cast<BoxDecorationBreak>(b.boxDecorationBreak))
            changingProperties.m_properties.set(CSSPropertyWebkitBoxDecorationBreak);
        // Non animated styles are followings.
        // usedZIndex
        // hasAutoUsedZIndex
    };

    auto conservativelyCollectChangedAnimatablePropertiesViaNonInheritedBackgroundData = [&](auto& a, auto& b) {
        if (a.background != b.background) {
            changingProperties.m_properties.set(CSSPropertyBackgroundImage);
            changingProperties.m_properties.set(CSSPropertyBackgroundPositionX);
            changingProperties.m_properties.set(CSSPropertyBackgroundPositionY);
            changingProperties.m_properties.set(CSSPropertyBackgroundSize);
            changingProperties.m_properties.set(CSSPropertyBackgroundAttachment);
            changingProperties.m_properties.set(CSSPropertyBackgroundClip);
            changingProperties.m_properties.set(CSSPropertyBackgroundOrigin);
            changingProperties.m_properties.set(CSSPropertyBackgroundRepeat);
            changingProperties.m_properties.set(CSSPropertyBackgroundBlendMode);
        }
        if (a.backgroundColor != b.backgroundColor)
            changingProperties.m_properties.set(CSSPropertyBackgroundColor);
        if (a.outline != b.outline) {
            changingProperties.m_properties.set(CSSPropertyOutlineColor);
            changingProperties.m_properties.set(CSSPropertyOutlineStyle);
            changingProperties.m_properties.set(CSSPropertyOutlineWidth);
            changingProperties.m_properties.set(CSSPropertyOutlineOffset);
        }
    };

    auto conservativelyCollectChangedAnimatablePropertiesViaNonInheritedSurroundData = [&](auto& a, auto& b) {
        if (a.inset.top() != b.inset.top())
            changingProperties.m_properties.set(CSSPropertyTop);
        if (a.inset.left() != b.inset.left())
            changingProperties.m_properties.set(CSSPropertyLeft);
        if (a.inset.bottom() != b.inset.bottom())
            changingProperties.m_properties.set(CSSPropertyBottom);
        if (a.inset.right() != b.inset.right())
            changingProperties.m_properties.set(CSSPropertyRight);

        if (a.margin.top() != b.margin.top())
            changingProperties.m_properties.set(CSSPropertyMarginTop);
        if (a.margin.left() != b.margin.left())
            changingProperties.m_properties.set(CSSPropertyMarginLeft);
        if (a.margin.bottom() != b.margin.bottom())
            changingProperties.m_properties.set(CSSPropertyMarginBottom);
        if (a.margin.right() != b.margin.right())
            changingProperties.m_properties.set(CSSPropertyMarginRight);

        if (a.padding.top() != b.padding.top())
            changingProperties.m_properties.set(CSSPropertyPaddingTop);
        if (a.padding.left() != b.padding.left())
            changingProperties.m_properties.set(CSSPropertyPaddingLeft);
        if (a.padding.bottom() != b.padding.bottom())
            changingProperties.m_properties.set(CSSPropertyPaddingBottom);
        if (a.padding.right() != b.padding.right())
            changingProperties.m_properties.set(CSSPropertyPaddingRight);

        if (a.border != b.border) {
            if (a.border.top() != b.border.top()) {
                changingProperties.m_properties.set(CSSPropertyBorderTopWidth);
                changingProperties.m_properties.set(CSSPropertyBorderTopColor);
                changingProperties.m_properties.set(CSSPropertyBorderTopStyle);
            }
            if (a.border.left() != b.border.left()) {
                changingProperties.m_properties.set(CSSPropertyBorderLeftWidth);
                changingProperties.m_properties.set(CSSPropertyBorderLeftColor);
                changingProperties.m_properties.set(CSSPropertyBorderLeftStyle);
            }
            if (a.border.bottom() != b.border.bottom()) {
                changingProperties.m_properties.set(CSSPropertyBorderBottomWidth);
                changingProperties.m_properties.set(CSSPropertyBorderBottomColor);
                changingProperties.m_properties.set(CSSPropertyBorderBottomStyle);
            }
            if (a.border.right() != b.border.right()) {
                changingProperties.m_properties.set(CSSPropertyBorderRightWidth);
                changingProperties.m_properties.set(CSSPropertyBorderRightColor);
                changingProperties.m_properties.set(CSSPropertyBorderRightStyle);
            }
            if (a.border.image() != b.border.image()) {
                changingProperties.m_properties.set(CSSPropertyBorderImageSlice);
                changingProperties.m_properties.set(CSSPropertyBorderImageWidth);
                changingProperties.m_properties.set(CSSPropertyBorderImageRepeat);
                changingProperties.m_properties.set(CSSPropertyBorderImageSource);
                changingProperties.m_properties.set(CSSPropertyBorderImageOutset);
            }
            if (a.border.topLeftRadius() != b.border.topLeftRadius())
                changingProperties.m_properties.set(CSSPropertyBorderTopLeftRadius);
            if (a.border.topRightRadius() != b.border.topRightRadius())
                changingProperties.m_properties.set(CSSPropertyBorderTopRightRadius);
            if (a.border.bottomLeftRadius() != b.border.bottomLeftRadius())
                changingProperties.m_properties.set(CSSPropertyBorderBottomLeftRadius);
            if (a.border.bottomRightRadius() != b.border.bottomRightRadius())
                changingProperties.m_properties.set(CSSPropertyBorderBottomRightRadius);

            if (a.border.topLeftCornerShape() != b.border.topLeftCornerShape())
                changingProperties.m_properties.set(CSSPropertyCornerTopLeftShape);
            if (a.border.topRightCornerShape() != b.border.topRightCornerShape())
                changingProperties.m_properties.set(CSSPropertyCornerTopRightShape);
            if (a.border.bottomLeftCornerShape() != b.border.bottomLeftCornerShape())
                changingProperties.m_properties.set(CSSPropertyCornerBottomLeftShape);
            if (a.border.bottomRightCornerShape() != b.border.bottomRightCornerShape())
                changingProperties.m_properties.set(CSSPropertyCornerBottomRightShape);
        }

        // Non animated styles are followings.
        // hasExplicitlySetBorderBottomLeftRadius
        // hasExplicitlySetBorderBottomRightRadius
        // hasExplicitlySetBorderTopLeftRadius
        // hasExplicitlySetBorderTopRightRadius
    };

    auto conservativelyCollectChangedAnimatablePropertiesViaNonInheritedMiscData = [&](auto& a, auto& b) {
        if (a.opacity != b.opacity)
            changingProperties.m_properties.set(CSSPropertyOpacity);

        if (a.flexibleBox != b.flexibleBox) {
            changingProperties.m_properties.set(CSSPropertyFlexBasis);
            changingProperties.m_properties.set(CSSPropertyFlexDirection);
            changingProperties.m_properties.set(CSSPropertyFlexGrow);
            changingProperties.m_properties.set(CSSPropertyFlexShrink);
            changingProperties.m_properties.set(CSSPropertyFlexWrap);
        }

        if (a.multiCol != b.multiCol) {
            changingProperties.m_properties.set(CSSPropertyColumnCount);
            changingProperties.m_properties.set(CSSPropertyColumnFill);
            changingProperties.m_properties.set(CSSPropertyColumnSpan);
            changingProperties.m_properties.set(CSSPropertyColumnWidth);
            changingProperties.m_properties.set(CSSPropertyColumnRuleColor);
            changingProperties.m_properties.set(CSSPropertyColumnRuleStyle);
            changingProperties.m_properties.set(CSSPropertyColumnRuleWidth);
        }

        if (a.filter != b.filter)
            changingProperties.m_properties.set(CSSPropertyFilter);

        if (a.mask != b.mask) {
            changingProperties.m_properties.set(CSSPropertyMaskImage);
            changingProperties.m_properties.set(CSSPropertyMaskClip);
            changingProperties.m_properties.set(CSSPropertyMaskComposite);
            changingProperties.m_properties.set(CSSPropertyMaskMode);
            changingProperties.m_properties.set(CSSPropertyMaskOrigin);
            changingProperties.m_properties.set(CSSPropertyWebkitMaskPositionX);
            changingProperties.m_properties.set(CSSPropertyWebkitMaskPositionY);
            changingProperties.m_properties.set(CSSPropertyMaskSize);
            changingProperties.m_properties.set(CSSPropertyMaskRepeat);
        }

        if (a.visitedLinkColor.ptr() != b.visitedLinkColor.ptr()) {
            if (a.visitedLinkColor->visitedLinkBackgroundColor != b.visitedLinkColor->visitedLinkBackgroundColor)
                changingProperties.m_properties.set(CSSPropertyBackgroundColor);
            if (a.visitedLinkColor->visitedLinkBorderColors.left() != b.visitedLinkColor->visitedLinkBorderColors.left())
                changingProperties.m_properties.set(CSSPropertyBorderLeftColor);
            if (a.visitedLinkColor->visitedLinkBorderColors.right() != b.visitedLinkColor->visitedLinkBorderColors.right())
                changingProperties.m_properties.set(CSSPropertyBorderRightColor);
            if (a.visitedLinkColor->visitedLinkBorderColors.top() != b.visitedLinkColor->visitedLinkBorderColors.top())
                changingProperties.m_properties.set(CSSPropertyBorderTopColor);
            if (a.visitedLinkColor->visitedLinkBorderColors.bottom() != b.visitedLinkColor->visitedLinkBorderColors.bottom())
                changingProperties.m_properties.set(CSSPropertyBorderBottomColor);
            if (a.visitedLinkColor->visitedLinkTextDecorationColor != b.visitedLinkColor->visitedLinkTextDecorationColor)
                changingProperties.m_properties.set(CSSPropertyTextDecorationColor);
            if (a.visitedLinkColor->visitedLinkOutlineColor != b.visitedLinkColor->visitedLinkOutlineColor)
                changingProperties.m_properties.set(CSSPropertyOutlineColor);
        }

        if (a.content != b.content)
            changingProperties.m_properties.set(CSSPropertyContent);

        if (a.boxShadow != b.boxShadow) {
            changingProperties.m_properties.set(CSSPropertyBoxShadow);
            changingProperties.m_properties.set(CSSPropertyWebkitBoxShadow);
        }

        if (a.aspectRatio != b.aspectRatio)
            changingProperties.m_properties.set(CSSPropertyAspectRatio);
        if (a.alignContent != b.alignContent)
            changingProperties.m_properties.set(CSSPropertyAlignContent);
        if (a.alignItems != b.alignItems)
            changingProperties.m_properties.set(CSSPropertyAlignItems);
        if (a.alignSelf != b.alignSelf)
            changingProperties.m_properties.set(CSSPropertyAlignSelf);
        if (a.justifyContent != b.justifyContent)
            changingProperties.m_properties.set(CSSPropertyJustifyContent);
        if (a.justifyItems != b.justifyItems)
            changingProperties.m_properties.set(CSSPropertyJustifyItems);
        if (a.justifySelf != b.justifySelf)
            changingProperties.m_properties.set(CSSPropertyJustifySelf);
        if (a.order != b.order)
            changingProperties.m_properties.set(CSSPropertyOrder);
        if (a.objectPosition != b.objectPosition)
            changingProperties.m_properties.set(CSSPropertyObjectPosition);
        if (a.textOverflow != b.textOverflow)
            changingProperties.m_properties.set(CSSPropertyTextOverflow);
        if (a.resize != b.resize)
            changingProperties.m_properties.set(CSSPropertyResize);
        if (a.objectFit != b.objectFit)
            changingProperties.m_properties.set(CSSPropertyObjectFit);
        if (a.appearance != b.appearance)
            changingProperties.m_properties.set(CSSPropertyAppearance);
        if (a.tableLayout != b.tableLayout)
            changingProperties.m_properties.set(CSSPropertyTableLayout);

        if (a.transform.ptr() != b.transform.ptr())
            conservativelyCollectChangedAnimatablePropertiesViaTransformData(*a.transform, *b.transform);

        // Non animated styles are followings.
        // deprecatedFlexibleBox
        // hasAttrContent
        // hasExplicitlySetColorScheme
        // hasExplicitlySetDirection
        // hasExplicitlySetWritingMode
        // usedAppearance
        // userDrag
    };

    auto conservativelyCollectChangedAnimatablePropertiesViaNonInheritedRareData = [&](auto& a, auto& b) {
        if (a.blockStepAlign != b.blockStepAlign)
            changingProperties.m_properties.set(CSSPropertyBlockStepAlign);
        if (a.blockStepInsert != b.blockStepInsert)
            changingProperties.m_properties.set(CSSPropertyBlockStepInsert);
        if (a.blockStepRound != b.blockStepRound)
            changingProperties.m_properties.set(CSSPropertyBlockStepRound);
        if (a.blockStepSize != b.blockStepSize)
            changingProperties.m_properties.set(CSSPropertyBlockStepSize);
        if (a.containIntrinsicWidth != b.containIntrinsicWidth)
            changingProperties.m_properties.set(CSSPropertyContainIntrinsicWidth);
        if (a.containIntrinsicHeight != b.containIntrinsicHeight)
            changingProperties.m_properties.set(CSSPropertyContainIntrinsicHeight);
        if (a.perspectiveOrigin.x != b.perspectiveOrigin.x)
            changingProperties.m_properties.set(CSSPropertyPerspectiveOriginX);
        if (a.perspectiveOrigin.y != b.perspectiveOrigin.y)
            changingProperties.m_properties.set(CSSPropertyPerspectiveOriginY);
        if (a.initialLetter != b.initialLetter)
            changingProperties.m_properties.set(CSSPropertyWebkitInitialLetter);
        if (a.backdropFilter != b.backdropFilter)
            changingProperties.m_properties.set(CSSPropertyWebkitBackdropFilter);
        if (a.grid != b.grid) {
            changingProperties.m_properties.set(CSSPropertyGridAutoColumns);
            changingProperties.m_properties.set(CSSPropertyGridAutoFlow);
            changingProperties.m_properties.set(CSSPropertyGridAutoRows);
            changingProperties.m_properties.set(CSSPropertyGridTemplateColumns);
            changingProperties.m_properties.set(CSSPropertyGridTemplateRows);
            changingProperties.m_properties.set(CSSPropertyGridTemplateAreas);
        }
        if (a.gridItem != b.gridItem) {
            changingProperties.m_properties.set(CSSPropertyGridColumnStart);
            changingProperties.m_properties.set(CSSPropertyGridColumnEnd);
            changingProperties.m_properties.set(CSSPropertyGridRowStart);
            changingProperties.m_properties.set(CSSPropertyGridRowEnd);
        }
        if (a.clip != b.clip)
            changingProperties.m_properties.set(CSSPropertyClip);
        if (a.counterDirectives != b.counterDirectives) {
            changingProperties.m_properties.set(CSSPropertyCounterIncrement);
            changingProperties.m_properties.set(CSSPropertyCounterReset);
            changingProperties.m_properties.set(CSSPropertyCounterSet);
        }
        if (a.maskBorder != b.maskBorder) {
            changingProperties.m_properties.set(CSSPropertyMaskBorderSource);
            changingProperties.m_properties.set(CSSPropertyMaskBorderSlice);
            changingProperties.m_properties.set(CSSPropertyMaskBorderWidth);
            changingProperties.m_properties.set(CSSPropertyMaskBorderOutset);
            changingProperties.m_properties.set(CSSPropertyMaskBorderRepeat);
            changingProperties.m_properties.set(CSSPropertyWebkitMaskBoxImage);
        }
        if (a.shapeOutside != b.shapeOutside)
            changingProperties.m_properties.set(CSSPropertyShapeOutside);
        if (a.shapeMargin != b.shapeMargin)
            changingProperties.m_properties.set(CSSPropertyShapeMargin);
        if (a.shapeImageThreshold != b.shapeImageThreshold)
            changingProperties.m_properties.set(CSSPropertyShapeImageThreshold);
        if (a.perspective != b.perspective)
            changingProperties.m_properties.set(CSSPropertyPerspective);
        if (a.clip != b.clip)
            changingProperties.m_properties.set(CSSPropertyClip);
        if (a.clipPath != b.clipPath)
            changingProperties.m_properties.set(CSSPropertyClipPath);
        if (a.textDecorationColor != b.textDecorationColor)
            changingProperties.m_properties.set(CSSPropertyTextDecorationColor);
        if (a.rotate != b.rotate)
            changingProperties.m_properties.set(CSSPropertyRotate);
        if (a.scale != b.scale)
            changingProperties.m_properties.set(CSSPropertyScale);
        if (a.translate != b.translate)
            changingProperties.m_properties.set(CSSPropertyTranslate);
        if (a.columnGap != b.columnGap)
            changingProperties.m_properties.set(CSSPropertyColumnGap);
        if (a.rowGap != b.rowGap)
            changingProperties.m_properties.set(CSSPropertyRowGap);
        if (a.offsetPath != b.offsetPath)
            changingProperties.m_properties.set(CSSPropertyOffsetPath);
        if (a.offsetDistance != b.offsetDistance)
            changingProperties.m_properties.set(CSSPropertyOffsetDistance);
        if (a.offsetPosition != b.offsetPosition)
            changingProperties.m_properties.set(CSSPropertyOffsetPosition);
        if (a.offsetAnchor != b.offsetAnchor)
            changingProperties.m_properties.set(CSSPropertyOffsetAnchor);
        if (a.offsetRotate != b.offsetRotate)
            changingProperties.m_properties.set(CSSPropertyOffsetRotate);
        if (a.textDecorationThickness != b.textDecorationThickness)
            changingProperties.m_properties.set(CSSPropertyTextDecorationThickness);
        if (a.touchAction != b.touchAction)
            changingProperties.m_properties.set(CSSPropertyTouchAction);
        if (a.marginTrim != b.marginTrim)
            changingProperties.m_properties.set(CSSPropertyMarginTrim);
        if (a.scrollbarGutter != b.scrollbarGutter)
            changingProperties.m_properties.set(CSSPropertyScrollbarGutter);
        if (a.scrollbarWidth != b.scrollbarWidth)
            changingProperties.m_properties.set(CSSPropertyScrollbarWidth);
        if (a.transformStyle3D != b.transformStyle3D)
            changingProperties.m_properties.set(CSSPropertyTransformStyle);
        if (a.backfaceVisibility != b.backfaceVisibility)
            changingProperties.m_properties.set(CSSPropertyBackfaceVisibility);
        if (a.scrollBehavior != b.scrollBehavior)
            changingProperties.m_properties.set(CSSPropertyScrollBehavior);
        if (a.textDecorationStyle != b.textDecorationStyle)
            changingProperties.m_properties.set(CSSPropertyTextDecorationStyle);
        if (a.textGroupAlign != b.textGroupAlign)
            changingProperties.m_properties.set(CSSPropertyTextGroupAlign);
        if (a.effectiveBlendMode != b.effectiveBlendMode)
            changingProperties.m_properties.set(CSSPropertyMixBlendMode);
        if (a.isolation != b.isolation)
            changingProperties.m_properties.set(CSSPropertyIsolation);
        if (a.breakAfter != b.breakAfter)
            changingProperties.m_properties.set(CSSPropertyBreakAfter);
        if (a.breakBefore != b.breakBefore)
            changingProperties.m_properties.set(CSSPropertyBreakBefore);
        if (a.breakInside != b.breakInside)
            changingProperties.m_properties.set(CSSPropertyBreakInside);
        if (a.textBoxTrim != b.textBoxTrim)
            changingProperties.m_properties.set(CSSPropertyTextBoxTrim);
        if (a.overflowAnchor != b.overflowAnchor)
            changingProperties.m_properties.set(CSSPropertyOverflowAnchor);
        if (a.viewTransitionClasses != b.viewTransitionClasses)
            changingProperties.m_properties.set(CSSPropertyViewTransitionClass);
        if (a.viewTransitionName != b.viewTransitionName)
            changingProperties.m_properties.set(CSSPropertyViewTransitionName);
        if (a.contentVisibility != b.contentVisibility)
            changingProperties.m_properties.set(CSSPropertyContentVisibility);
        if (a.anchorNames != b.anchorNames)
            changingProperties.m_properties.set(CSSPropertyAnchorName);
        if (a.anchorScope != b.anchorScope)
            changingProperties.m_properties.set(CSSPropertyAnchorScope);
        if (a.positionAnchor != b.positionAnchor)
            changingProperties.m_properties.set(CSSPropertyPositionAnchor);
        if (a.positionArea != b.positionArea)
            changingProperties.m_properties.set(CSSPropertyPositionArea);
        if (a.positionTryFallbacks != b.positionTryFallbacks)
            changingProperties.m_properties.set(CSSPropertyPositionTryFallbacks);
        if (a.positionTryOrder != b.positionTryOrder)
            changingProperties.m_properties.set(CSSPropertyPositionTryOrder);
        if (a.positionVisibility != b.positionVisibility)
            changingProperties.m_properties.set(CSSPropertyPositionVisibility);
        if (a.scrollSnapAlign != b.scrollSnapAlign)
            changingProperties.m_properties.set(CSSPropertyScrollSnapAlign);
        if (a.scrollSnapStop != b.scrollSnapStop)
            changingProperties.m_properties.set(CSSPropertyScrollSnapStop);
        if (a.scrollSnapType != b.scrollSnapType)
            changingProperties.m_properties.set(CSSPropertyScrollSnapType);
        if (a.maxLines != b.maxLines)
            changingProperties.m_properties.set(CSSPropertyMaxLines);
        if (a.overflowContinue != b.overflowContinue)
            changingProperties.m_properties.set(CSSPropertyContinue);

        // Non animated styles are followings.
        // customProperties
        // customPaintWatchedProperties
        // zoom
        // contain
        // containerNames
        // scrollMargin
        // scrollPadding
        // lineClamp
        // willChange
        // marquee
        // boxReflect
        // pageSize
        // overscrollBehaviorX
        // overscrollBehaviorY
        // applePayButtonStyle
        // applePayButtonType
        // inputSecurity
        // containerType
        // transformStyleForcedToFlat
    };

    auto conservativelyCollectChangedAnimatablePropertiesViaInheritedData = [&](auto& a, auto& b) {
        if (a.lineHeight != b.lineHeight)
            changingProperties.m_properties.set(CSSPropertyLineHeight);

#if ENABLE(TEXT_AUTOSIZING)
        if (a.specifiedLineHeight != b.specifiedLineHeight)
            changingProperties.m_properties.set(CSSPropertyLineHeight);
#endif

        if (a.fontData != b.fontData) {
            changingProperties.m_properties.set(CSSPropertyWordSpacing);
            changingProperties.m_properties.set(CSSPropertyLetterSpacing);
            changingProperties.m_properties.set(CSSPropertyTextRendering);
            changingProperties.m_properties.set(CSSPropertyTextSpacingTrim);
            changingProperties.m_properties.set(CSSPropertyTextAutospace);
            changingProperties.m_properties.set(CSSPropertyFontStyle);
#if ENABLE(VARIATION_FONTS)
            changingProperties.m_properties.set(CSSPropertyFontOpticalSizing);
            changingProperties.m_properties.set(CSSPropertyFontVariationSettings);
#endif
            changingProperties.m_properties.set(CSSPropertyFontWeight);
            changingProperties.m_properties.set(CSSPropertyFontSizeAdjust);
            changingProperties.m_properties.set(CSSPropertyFontFamily);
            changingProperties.m_properties.set(CSSPropertyFontFeatureSettings);
            changingProperties.m_properties.set(CSSPropertyFontVariantEastAsian);
            changingProperties.m_properties.set(CSSPropertyFontVariantLigatures);
            changingProperties.m_properties.set(CSSPropertyFontVariantNumeric);
            changingProperties.m_properties.set(CSSPropertyFontSize);
            changingProperties.m_properties.set(CSSPropertyFontWidth);
            changingProperties.m_properties.set(CSSPropertyFontPalette);
            changingProperties.m_properties.set(CSSPropertyFontKerning);
            changingProperties.m_properties.set(CSSPropertyFontSynthesisWeight);
            changingProperties.m_properties.set(CSSPropertyFontSynthesisStyle);
            changingProperties.m_properties.set(CSSPropertyFontSynthesisSmallCaps);
            changingProperties.m_properties.set(CSSPropertyFontVariantAlternates);
            changingProperties.m_properties.set(CSSPropertyFontVariantPosition);
            changingProperties.m_properties.set(CSSPropertyFontVariantCaps);
            changingProperties.m_properties.set(CSSPropertyFontVariantEmoji);
        }

        if (a.borderHorizontalSpacing != b.borderHorizontalSpacing)
            changingProperties.m_properties.set(CSSPropertyWebkitBorderHorizontalSpacing);

        if (a.borderVerticalSpacing != b.borderVerticalSpacing)
            changingProperties.m_properties.set(CSSPropertyWebkitBorderVerticalSpacing);

        if (a.color != b.color || a.visitedLinkColor != b.visitedLinkColor)
            changingProperties.m_properties.set(CSSPropertyColor);
    };

    auto conservativelyCollectChangedAnimatablePropertiesViaRareInheritedData = [&](auto& a, auto& b) {
        if (a.textStrokeColor != b.textStrokeColor || a.visitedLinkTextStrokeColor != b.visitedLinkTextStrokeColor)
            changingProperties.m_properties.set(CSSPropertyWebkitTextStrokeColor);
        if (a.textFillColor != b.textFillColor || a.visitedLinkTextFillColor != b.visitedLinkTextFillColor)
            changingProperties.m_properties.set(CSSPropertyWebkitTextFillColor);
        if (a.textEmphasisColor != b.textEmphasisColor || a.visitedLinkTextEmphasisColor != b.visitedLinkTextEmphasisColor)
            changingProperties.m_properties.set(CSSPropertyTextEmphasisColor);
        if (a.caretColor != b.caretColor || a.visitedLinkCaretColor != b.visitedLinkCaretColor || a.hasAutoCaretColor != b.hasAutoCaretColor || a.hasVisitedLinkAutoCaretColor != b.hasVisitedLinkAutoCaretColor)
            changingProperties.m_properties.set(CSSPropertyCaretColor);
        if (a.accentColor != b.accentColor)
            changingProperties.m_properties.set(CSSPropertyAccentColor);
        if (a.textShadow != b.textShadow)
            changingProperties.m_properties.set(CSSPropertyTextShadow);
        if (a.textIndent != b.textIndent)
            changingProperties.m_properties.set(CSSPropertyTextIndent);
        if (a.textUnderlineOffset != b.textUnderlineOffset)
            changingProperties.m_properties.set(CSSPropertyTextUnderlineOffset);
        if (a.strokeMiterLimit != b.strokeMiterLimit)
            changingProperties.m_properties.set(CSSPropertyStrokeMiterlimit);
        if (a.widows != b.widows)
            changingProperties.m_properties.set(CSSPropertyWidows);
        if (a.orphans != b.orphans)
            changingProperties.m_properties.set(CSSPropertyOrphans);
        if (a.wordBreak != b.wordBreak)
            changingProperties.m_properties.set(CSSPropertyWordBreak);
        if (a.overflowWrap != b.overflowWrap)
            changingProperties.m_properties.set(CSSPropertyOverflowWrap);
        if (a.lineBreak != b.lineBreak)
            changingProperties.m_properties.set(CSSPropertyLineBreak);
        if (a.hangingPunctuation != b.hangingPunctuation)
            changingProperties.m_properties.set(CSSPropertyHangingPunctuation);
        if (a.hyphens != b.hyphens)
            changingProperties.m_properties.set(CSSPropertyHyphens);
        if (a.textEmphasisPosition != b.textEmphasisPosition)
            changingProperties.m_properties.set(CSSPropertyTextEmphasisPosition);
#if ENABLE(DARK_MODE_CSS)
        if (a.colorScheme != b.colorScheme)
            changingProperties.m_properties.set(CSSPropertyColorScheme);
#endif
        if (a.dynamicRangeLimit != b.dynamicRangeLimit)
            changingProperties.m_properties.set(CSSPropertyDynamicRangeLimit);
        if (a.textEmphasisStyle != b.textEmphasisStyle)
            changingProperties.m_properties.set(CSSPropertyTextEmphasisStyle);
        if (a.quotes != b.quotes)
            changingProperties.m_properties.set(CSSPropertyQuotes);
        if (a.appleColorFilter != b.appleColorFilter)
            changingProperties.m_properties.set(CSSPropertyAppleColorFilter);
        if (a.tabSize != b.tabSize)
            changingProperties.m_properties.set(CSSPropertyTabSize);
        if (a.imageOrientation != b.imageOrientation)
            changingProperties.m_properties.set(CSSPropertyImageOrientation);
        if (a.imageRendering != b.imageRendering)
            changingProperties.m_properties.set(CSSPropertyImageRendering);
        if (a.textAlignLast != b.textAlignLast)
            changingProperties.m_properties.set(CSSPropertyTextAlignLast);
        if (a.textBoxEdge != b.textBoxEdge)
            changingProperties.m_properties.set(CSSPropertyTextBoxEdge);
        if (a.lineFitEdge != b.lineFitEdge)
            changingProperties.m_properties.set(CSSPropertyLineFitEdge);
        if (a.textJustify != b.textJustify)
            changingProperties.m_properties.set(CSSPropertyTextJustify);
        if (a.textDecorationSkipInk != b.textDecorationSkipInk)
            changingProperties.m_properties.set(CSSPropertyTextDecorationSkipInk);
        if (a.textUnderlinePosition != b.textUnderlinePosition)
            changingProperties.m_properties.set(CSSPropertyTextUnderlinePosition);
        if (a.rubyPosition != b.rubyPosition)
            changingProperties.m_properties.set(CSSPropertyRubyPosition);
        if (a.rubyAlign != b.rubyAlign)
            changingProperties.m_properties.set(CSSPropertyRubyAlign);
        if (a.rubyOverhang != b.rubyOverhang)
            changingProperties.m_properties.set(CSSPropertyRubyOverhang);
        if (a.strokeColor != b.strokeColor)
            changingProperties.m_properties.set(CSSPropertyStrokeColor);
        if (a.paintOrder != b.paintOrder)
            changingProperties.m_properties.set(CSSPropertyPaintOrder);
        if (a.capStyle != b.capStyle)
            changingProperties.m_properties.set(CSSPropertyStrokeLinecap);
        if (a.joinStyle != b.joinStyle)
            changingProperties.m_properties.set(CSSPropertyStrokeLinejoin);
        if (a.hasExplicitlySetStrokeWidth != b.hasExplicitlySetStrokeWidth || a.strokeWidth != b.strokeWidth)
            changingProperties.m_properties.set(CSSPropertyStrokeWidth);
        if (a.listStyleImage != b.listStyleImage)
            changingProperties.m_properties.set(CSSPropertyListStyleImage);
        if (a.scrollbarColor != b.scrollbarColor)
            changingProperties.m_properties.set(CSSPropertyScrollbarColor);
        if (a.listStyleType != b.listStyleType)
            changingProperties.m_properties.set(CSSPropertyListStyleType);
        if (a.hyphenateCharacter != b.hyphenateCharacter)
            changingProperties.m_properties.set(CSSPropertyHyphenateCharacter);
        if (a.blockEllipsis != b.blockEllipsis)
            changingProperties.m_properties.set(CSSPropertyBlockEllipsis);

        // customProperties is handled separately.
        // Non animated styles are followings.
        //
        // textStrokeWidth
        // mathStyle
        // hyphenateLimitBefore
        // hyphenateLimitAfter
        // hyphenateLimitLines
        // tapHighlightColor
        // nbspMode
        // webkitOverflowScrolling
        // textSizeAdjust
        // userSelect
        // isInSubtreeWithBlendMode
        // usedTouchAction
        // eventListenerRegionTypes
        // effectiveInert
        // usedContentVisibility
        // visitedLinkStrokeColor
        // hasExplicitlySetStrokeColor
        // usedZoom
        // textSecurity
        // userModify
        // speakAs
        // textCombine
        // lineBoxContain
        // webkitTouchCallout
        // lineGrid
        // textZoom
        // lineSnap
        // lineAlign
        // cursorData
        // insideDefaultButton
        // insideDisabledSubmitButton
    };

    auto conservativelyCollectChangedAnimatablePropertiesViaSVGFillData = [&](auto& a, auto& b) {
        if (a.fillOpacity != b.fillOpacity)
            changingProperties.m_properties.set(CSSPropertyFillOpacity);
        if (a.fill != b.fill || a.visitedLinkFill != b.visitedLinkFill)
            changingProperties.m_properties.set(CSSPropertyFill);
    };

    auto conservativelyCollectChangedAnimatablePropertiesViaSVGStrokeData = [&](auto& a, auto& b) {
        if (a.strokeOpacity != b.strokeOpacity)
            changingProperties.m_properties.set(CSSPropertyStrokeOpacity);
        if (a.strokeDashOffset != b.strokeDashOffset)
            changingProperties.m_properties.set(CSSPropertyStrokeDashoffset);
        if (a.strokeDashArray != b.strokeDashArray)
            changingProperties.m_properties.set(CSSPropertyStrokeDasharray);
        if (a.stroke != b.stroke || a.visitedLinkStroke != b.visitedLinkStroke)
            changingProperties.m_properties.set(CSSPropertyStroke);
    };

    auto conservativelyCollectChangedAnimatablePropertiesViaSVGStopData = [&](auto& a, auto& b) {
        if (a.stopOpacity != b.stopOpacity)
            changingProperties.m_properties.set(CSSPropertyStopOpacity);
        if (a.stopColor != b.stopColor)
            changingProperties.m_properties.set(CSSPropertyStopColor);
    };

    auto conservativelyCollectChangedAnimatablePropertiesViaSVGMiscData = [&](auto& a, auto& b) {
        if (a.floodOpacity != b.floodOpacity)
            changingProperties.m_properties.set(CSSPropertyFloodOpacity);
        if (a.floodColor != b.floodColor)
            changingProperties.m_properties.set(CSSPropertyFloodColor);
        if (a.lightingColor != b.lightingColor)
            changingProperties.m_properties.set(CSSPropertyLightingColor);
        if (a.baselineShift != b.baselineShift)
            changingProperties.m_properties.set(CSSPropertyBaselineShift);
    };

    auto conservativelyCollectChangedAnimatablePropertiesViaSVGLayoutData = [&](auto& a, auto& b) {
        if (a.cx != b.cx)
            changingProperties.m_properties.set(CSSPropertyCx);
        if (a.cy != b.cy)
            changingProperties.m_properties.set(CSSPropertyCy);
        if (a.r != b.r)
            changingProperties.m_properties.set(CSSPropertyR);
        if (a.rx != b.rx)
            changingProperties.m_properties.set(CSSPropertyRx);
        if (a.ry != b.ry)
            changingProperties.m_properties.set(CSSPropertyRy);
        if (a.x != b.x)
            changingProperties.m_properties.set(CSSPropertyX);
        if (a.y != b.y)
            changingProperties.m_properties.set(CSSPropertyY);
        if (a.d != b.d)
            changingProperties.m_properties.set(CSSPropertyD);
    };

    auto conservativelyCollectChangedAnimatablePropertiesViaSVGInheritedResourceData = [&](auto& a, auto& b) {
        if (a.markerStart != b.markerStart)
            changingProperties.m_properties.set(CSSPropertyMarkerStart);
        if (a.markerMid != b.markerMid)
            changingProperties.m_properties.set(CSSPropertyMarkerMid);
        if (a.markerEnd != b.markerEnd)
            changingProperties.m_properties.set(CSSPropertyMarkerEnd);
    };

    auto conservativelyCollectChangedAnimatablePropertiesViaSVGInheritedFlags = [&](auto& a, auto& b) {
        if (a.shapeRendering != b.shapeRendering)
            changingProperties.m_properties.set(CSSPropertyShapeRendering);
        if (a.clipRule != b.clipRule)
            changingProperties.m_properties.set(CSSPropertyClipRule);
        if (a.fillRule != b.fillRule)
            changingProperties.m_properties.set(CSSPropertyFillRule);
        if (a.textAnchor != b.textAnchor)
            changingProperties.m_properties.set(CSSPropertyTextAnchor);
        if (a.colorInterpolation != b.colorInterpolation)
            changingProperties.m_properties.set(CSSPropertyColorInterpolation);
        if (a.colorInterpolationFilters != b.colorInterpolationFilters)
            changingProperties.m_properties.set(CSSPropertyColorInterpolationFilters);

        // Non animated styles are followings.
        // glyphOrientationHorizontal
        // glyphOrientationVertical
    };

    auto conservativelyCollectChangedAnimatablePropertiesViaSVGNonInheritedFlags = [&](auto& a, auto& b) {
        if (a.alignmentBaseline != b.alignmentBaseline)
            changingProperties.m_properties.set(CSSPropertyAlignmentBaseline);
        if (a.bufferedRendering != b.bufferedRendering)
            changingProperties.m_properties.set(CSSPropertyBufferedRendering);
        if (a.dominantBaseline != b.dominantBaseline)
            changingProperties.m_properties.set(CSSPropertyDominantBaseline);
        if (a.maskType != b.maskType)
            changingProperties.m_properties.set(CSSPropertyMaskType);
        if (a.vectorEffect != b.vectorEffect)
            changingProperties.m_properties.set(CSSPropertyVectorEffect);
    };

    if (a.inheritedFlags() != b.inheritedFlags())
        conservativelyCollectChangedAnimatablePropertiesViaInheritedFlags(a.inheritedFlags(), b.inheritedFlags());

    if (a.nonInheritedFlags() != b.nonInheritedFlags())
        conservativelyCollectChangedAnimatablePropertiesViaNonInheritedFlags(a.nonInheritedFlags(), b.nonInheritedFlags());

    if (&a.nonInheritedData() != &b.nonInheritedData()) {
        if (a.nonInheritedData().boxData.ptr() != b.nonInheritedData().boxData.ptr())
            conservativelyCollectChangedAnimatablePropertiesViaNonInheritedBoxData(*a.nonInheritedData().boxData, *b.nonInheritedData().boxData);

        if (a.nonInheritedData().backgroundData.ptr() != b.nonInheritedData().backgroundData.ptr())
            conservativelyCollectChangedAnimatablePropertiesViaNonInheritedBackgroundData(*a.nonInheritedData().backgroundData, *b.nonInheritedData().backgroundData);

        if (a.nonInheritedData().surroundData.ptr() != b.nonInheritedData().surroundData.ptr())
            conservativelyCollectChangedAnimatablePropertiesViaNonInheritedSurroundData(*a.nonInheritedData().surroundData, *b.nonInheritedData().surroundData);

        if (a.nonInheritedData().miscData.ptr() != b.nonInheritedData().miscData.ptr())
            conservativelyCollectChangedAnimatablePropertiesViaNonInheritedMiscData(*a.nonInheritedData().miscData, *b.nonInheritedData().miscData);

        if (a.nonInheritedData().rareData.ptr() != b.nonInheritedData().rareData.ptr())
            conservativelyCollectChangedAnimatablePropertiesViaNonInheritedRareData(*a.nonInheritedData().rareData, *b.nonInheritedData().rareData);
    }

    if (&a.rareInheritedData() != &b.rareInheritedData())
        conservativelyCollectChangedAnimatablePropertiesViaRareInheritedData(a.rareInheritedData(), b.rareInheritedData());

    if (&a.inheritedData() != &b.inheritedData())
        conservativelyCollectChangedAnimatablePropertiesViaInheritedData(a.inheritedData(), b.inheritedData());

    if (&a.svgStyle() != &b.svgStyle()) {
        if (a.svgStyle().fillData.ptr() != b.svgStyle().fillData.ptr())
            conservativelyCollectChangedAnimatablePropertiesViaSVGFillData(*a.svgStyle().fillData, *b.svgStyle().fillData);

        if (a.svgStyle().strokeData != b.svgStyle().strokeData)
            conservativelyCollectChangedAnimatablePropertiesViaSVGStrokeData(*a.svgStyle().strokeData, *b.svgStyle().strokeData);

        if (a.svgStyle().stopData != b.svgStyle().stopData)
            conservativelyCollectChangedAnimatablePropertiesViaSVGStopData(*a.svgStyle().stopData, *b.svgStyle().stopData);

        if (a.svgStyle().miscData != b.svgStyle().miscData)
            conservativelyCollectChangedAnimatablePropertiesViaSVGMiscData(*a.svgStyle().miscData, *b.svgStyle().miscData);

        if (a.svgStyle().layoutData != b.svgStyle().layoutData)
            conservativelyCollectChangedAnimatablePropertiesViaSVGLayoutData(*a.svgStyle().layoutData, *b.svgStyle().layoutData);

        if (a.svgStyle().inheritedResourceData != b.svgStyle().inheritedResourceData)
            conservativelyCollectChangedAnimatablePropertiesViaSVGInheritedResourceData(*a.svgStyle().inheritedResourceData, *b.svgStyle().inheritedResourceData);

        if (a.svgStyle().inheritedFlags != b.svgStyle().inheritedFlags)
            conservativelyCollectChangedAnimatablePropertiesViaSVGInheritedFlags(a.svgStyle().inheritedFlags, b.svgStyle().inheritedFlags);

        if (a.svgStyle().nonInheritedFlags != b.svgStyle().nonInheritedFlags)
            conservativelyCollectChangedAnimatablePropertiesViaSVGNonInheritedFlags(a.svgStyle().nonInheritedFlags, b.svgStyle().nonInheritedFlags);
    }
}

} // namespace Style
} // namespace WebCore
