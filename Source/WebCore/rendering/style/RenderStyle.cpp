/*
 * Copyright (C) 1999 Antti Koivisto (koivisto@kde.org)
 * Copyright (C) 2004-2023 Apple Inc. All rights reserved.
 * Copyright (C) 2011 Adobe Systems Incorporated. All rights reserved.
 * Copyright (C) 2025 Samuel Weinig <sam@webkit.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#include "config.h"
#include "RenderStyle.h"

#include "AutosizeStatus.h"
#include "CSSCustomPropertyValue.h"
#include "CSSPropertyNames.h"
#include "CSSPropertyParser.h"
#include "CSSValuePool.h"
#include "ColorBlending.h"
#include "FloatRoundedRect.h"
#include "FontCascade.h"
#include "FontSelector.h"
#include "InlineIteratorTextBox.h"
#include "InlineTextBoxStyle.h"
#include "Logging.h"
#include "MotionPath.h"
#include "Pagination.h"
#include "PathTraversalState.h"
#include "RenderBlock.h"
#include "RenderElement.h"
#include "RenderStyleDifference.h"
#include "RenderStyleSetters.h"
#include "RenderTheme.h"
#include "SVGRenderStyle.h"
#include "ScaleTransformOperation.h"
#include "ScrollAxis.h"
#include "StyleCustomPropertyRegistry.h"
#include "StyleExtractor.h"
#include "StyleImage.h"
#include "StyleInheritedData.h"
#include "StylePrimitiveNumericTypes+Evaluation.h"
#include "StyleResolver.h"
#include "StyleScaleTransformFunction.h"
#include "StyleSelfAlignmentData.h"
#include "StyleTextDecorationLine.h"
#include "StyleTextTransform.h"
#include "StyleTreeResolver.h"
#include "TransformOperationData.h"
#include <algorithm>
#include <wtf/MathExtras.h>
#include <wtf/PointerComparison.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/TextStream.h>

#if ENABLE(TEXT_AUTOSIZING)
#include <wtf/text/StringHash.h>
#endif

namespace WebCore {

struct SameSizeAsBorderValue {
    Style::Color m_color;
    float m_width;
    int m_restBits;
};

static_assert(sizeof(BorderValue) == sizeof(SameSizeAsBorderValue), "BorderValue should not grow");

struct SameSizeAsRenderStyle : CanMakeCheckedPtr<SameSizeAsRenderStyle> {
    void* nonInheritedDataRefs[1];
    struct NonInheritedFlags {
        unsigned m_bitfields[3];
    } m_nonInheritedFlags;
    void* inheritedDataRefs[2];
    struct InheritedFlags {
        unsigned m_bitfields[2];
    } m_inheritedFlags;
    void* pseudos;
    void* dataRefSvgStyle;

#if ASSERT_ENABLED || ENABLE(SECURITY_ASSERTIONS)
    bool deletionCheck;
#endif
};

static_assert(sizeof(RenderStyle) == sizeof(SameSizeAsRenderStyle), "RenderStyle should stay small");

inline RenderStyleBase::RenderStyleBase(RenderStyleBase&&) = default;
inline RenderStyleBase& RenderStyleBase::operator=(RenderStyleBase&&) = default;

inline RenderStyleBase::RenderStyleBase(CreateDefaultStyleTag)
    : m_nonInheritedData(StyleNonInheritedData::create())
    , m_rareInheritedData(StyleRareInheritedData::create())
    , m_inheritedData(StyleInheritedData::create())
    , m_svgStyle(SVGRenderStyle::create())
{
    m_inheritedFlags.writingMode = WritingMode(RenderStyle::initialWritingMode(), RenderStyle::initialDirection(), RenderStyle::initialTextOrientation()).toData();
    m_inheritedFlags.emptyCells = static_cast<unsigned>(RenderStyle::initialEmptyCells());
    m_inheritedFlags.captionSide = static_cast<unsigned>(RenderStyle::initialCaptionSide());
    m_inheritedFlags.listStylePosition = static_cast<unsigned>(RenderStyle::initialListStylePosition());
    m_inheritedFlags.visibility = static_cast<unsigned>(RenderStyle::initialVisibility());
    m_inheritedFlags.textAlign = static_cast<unsigned>(RenderStyle::initialTextAlign());
    m_inheritedFlags.textTransform = RenderStyle::initialTextTransform().toRaw();
    m_inheritedFlags.textDecorationLineInEffect = RenderStyle::initialTextDecorationLine().toRaw();
    m_inheritedFlags.cursorType = static_cast<unsigned>(RenderStyle::initialCursor().predefined);
#if ENABLE(CURSOR_VISIBILITY)
    m_inheritedFlags.cursorVisibility = static_cast<unsigned>(RenderStyle::initialCursorVisibility());
#endif
    m_inheritedFlags.whiteSpaceCollapse = static_cast<unsigned>(RenderStyle::initialWhiteSpaceCollapse());
    m_inheritedFlags.textWrapMode = static_cast<unsigned>(RenderStyle::initialTextWrapMode());
    m_inheritedFlags.textWrapStyle = static_cast<unsigned>(RenderStyle::initialTextWrapStyle());
    m_inheritedFlags.borderCollapse = static_cast<unsigned>(RenderStyle::initialBorderCollapse());
    m_inheritedFlags.rtlOrdering = static_cast<unsigned>(RenderStyle::initialRTLOrdering());
    m_inheritedFlags.boxDirection = static_cast<unsigned>(RenderStyle::initialBoxDirection());
    m_inheritedFlags.printColorAdjust = static_cast<unsigned>(RenderStyle::initialPrintColorAdjust());
    m_inheritedFlags.pointerEvents = static_cast<unsigned>(RenderStyle::initialPointerEvents());
    m_inheritedFlags.insideLink = static_cast<unsigned>(InsideLink::NotInside);
#if ENABLE(TEXT_AUTOSIZING)
    m_inheritedFlags.autosizeStatus = 0;
#endif

    m_nonInheritedFlags.effectiveDisplay = static_cast<unsigned>(RenderStyle::initialDisplay());
    m_nonInheritedFlags.originalDisplay = static_cast<unsigned>(RenderStyle::initialDisplay());
    m_nonInheritedFlags.overflowX = static_cast<unsigned>(RenderStyle::initialOverflowX());
    m_nonInheritedFlags.overflowY = static_cast<unsigned>(RenderStyle::initialOverflowY());
    m_nonInheritedFlags.clear = static_cast<unsigned>(RenderStyle::initialClear());
    m_nonInheritedFlags.position = static_cast<unsigned>(RenderStyle::initialPosition());
    m_nonInheritedFlags.unicodeBidi = static_cast<unsigned>(RenderStyle::initialUnicodeBidi());
    m_nonInheritedFlags.floating = static_cast<unsigned>(RenderStyle::initialFloating());
    m_nonInheritedFlags.textDecorationLine = RenderStyle::initialTextDecorationLine().toRaw();
    m_nonInheritedFlags.usesViewportUnits = false;
    m_nonInheritedFlags.usesContainerUnits = false;
    m_nonInheritedFlags.useTreeCountingFunctions = false;
    m_nonInheritedFlags.hasExplicitlyInheritedProperties = false;
    m_nonInheritedFlags.disallowsFastPathInheritance = false;
    m_nonInheritedFlags.emptyState = false;
    m_nonInheritedFlags.firstChildState = false;
    m_nonInheritedFlags.lastChildState = false;
    m_nonInheritedFlags.isLink = false;
    m_nonInheritedFlags.pseudoElementType = 0;
    m_nonInheritedFlags.pseudoBits = 0;

    static_assert((sizeof(InheritedFlags) <= 8), "InheritedFlags does not grow");
    static_assert((sizeof(NonInheritedFlags) <= 12), "NonInheritedFlags does not grow");
}

inline RenderStyleBase::RenderStyleBase(const RenderStyleBase& other, CloneTag)
    : m_nonInheritedData(other.m_nonInheritedData)
    , m_nonInheritedFlags(other.m_nonInheritedFlags)
    , m_rareInheritedData(other.m_rareInheritedData)
    , m_inheritedData(other.m_inheritedData)
    , m_inheritedFlags(other.m_inheritedFlags)
    , m_svgStyle(other.m_svgStyle)
{
}

inline RenderStyleBase::RenderStyleBase(RenderStyleBase& a, RenderStyleBase&& b)
    : m_nonInheritedData(a.m_nonInheritedData.replace(WTF::move(b.m_nonInheritedData)))
    , m_nonInheritedFlags(std::exchange(a.m_nonInheritedFlags, b.m_nonInheritedFlags))
    , m_rareInheritedData(a.m_rareInheritedData.replace(WTF::move(b.m_rareInheritedData)))
    , m_inheritedData(a.m_inheritedData.replace(WTF::move(b.m_inheritedData)))
    , m_inheritedFlags(std::exchange(a.m_inheritedFlags, b.m_inheritedFlags))
    , m_cachedPseudoStyles(std::exchange(a.m_cachedPseudoStyles, WTF::move(b.m_cachedPseudoStyles)))
    , m_svgStyle(a.m_svgStyle.replace(WTF::move(b.m_svgStyle)))
{
}

inline void RenderStyleBase::NonInheritedFlags::copyNonInheritedFrom(const NonInheritedFlags& other)
{
    // Only some flags are copied because NonInheritedFlags contains things that are not actually style data.
    effectiveDisplay = other.effectiveDisplay;
    originalDisplay = other.originalDisplay;
    overflowX = other.overflowX;
    overflowY = other.overflowY;
    clear = other.clear;
    position = other.position;
    unicodeBidi = other.unicodeBidi;
    floating = other.floating;
    textDecorationLine = other.textDecorationLine;
    usesViewportUnits = other.usesViewportUnits;
    usesContainerUnits = other.usesContainerUnits;
    useTreeCountingFunctions = other.useTreeCountingFunctions;
    hasExplicitlyInheritedProperties = other.hasExplicitlyInheritedProperties;
    disallowsFastPathInheritance = other.disallowsFastPathInheritance;
}

RenderStyle::RenderStyle(RenderStyle&&) = default;
RenderStyle& RenderStyle::operator=(RenderStyle&&) = default;

inline RenderStyle::RenderStyle(CreateDefaultStyleTag tag)
    : RenderStyleProperties { tag }
{
}

inline RenderStyle::RenderStyle(const RenderStyle& other, CloneTag tag)
    : RenderStyleProperties { other, tag }
{
}

inline RenderStyle::RenderStyle(RenderStyle& a, RenderStyle&& b)
    : RenderStyleProperties { a, WTF::move(b) }
{
}

RenderStyle& RenderStyle::defaultStyleSingleton()
{
    static NeverDestroyed<RenderStyle> style { CreateDefaultStyle };
    return style;
}

RenderStyle RenderStyle::create()
{
    return clone(defaultStyleSingleton());
}

std::unique_ptr<RenderStyle> RenderStyle::createPtr()
{
    return clonePtr(defaultStyleSingleton());
}

std::unique_ptr<RenderStyle> RenderStyle::createPtrWithRegisteredInitialValues(const Style::CustomPropertyRegistry& registry)
{
    return clonePtr(registry.initialValuePrototypeStyle());
}

RenderStyle RenderStyle::clone(const RenderStyle& style)
{
    return RenderStyle(style, Clone);
}

RenderStyle RenderStyle::cloneIncludingPseudoElements(const RenderStyle& style)
{
    auto newStyle = RenderStyle(style, Clone);
    newStyle.copyPseudoElementsFrom(style);
    return newStyle;
}

std::unique_ptr<RenderStyle> RenderStyle::clonePtr(const RenderStyle& style)
{
    return makeUnique<RenderStyle>(style, Clone);
}

RenderStyle RenderStyle::createAnonymousStyleWithDisplay(const RenderStyle& parentStyle, DisplayType display)
{
    auto newStyle = create();
    newStyle.inheritFrom(parentStyle);
    newStyle.inheritUnicodeBidiFrom(parentStyle);
    newStyle.setDisplay(display);
    return newStyle;
}

RenderStyle RenderStyle::createStyleInheritingFromPseudoStyle(const RenderStyle& pseudoStyle)
{
    ASSERT(pseudoStyle.pseudoElementType() == PseudoElementType::Before || pseudoStyle.pseudoElementType() == PseudoElementType::After);

    auto style = create();
    style.inheritFrom(pseudoStyle);
    return style;
}

RenderStyle RenderStyle::replace(RenderStyle&& newStyle)
{
    return RenderStyle { *this, WTF::move(newStyle) };
}

void RenderStyle::inheritFrom(const RenderStyle& inheritParent)
{
    m_rareInheritedData = inheritParent.m_rareInheritedData;
    m_inheritedData = inheritParent.m_inheritedData;
    m_inheritedFlags = inheritParent.m_inheritedFlags;

    if (m_svgStyle != inheritParent.m_svgStyle)
        m_svgStyle.access().inheritFrom(inheritParent.m_svgStyle.get());
}

void RenderStyle::inheritIgnoringCustomPropertiesFrom(const RenderStyle& inheritParent)
{
    auto oldCustomProperties = m_rareInheritedData->customProperties;
    inheritFrom(inheritParent);
    if (oldCustomProperties != m_rareInheritedData->customProperties)
        m_rareInheritedData.access().customProperties = oldCustomProperties;
}

void RenderStyle::inheritUnicodeBidiFrom(const RenderStyle& inheritParent)
{
    m_nonInheritedFlags.unicodeBidi = inheritParent.m_nonInheritedFlags.unicodeBidi;
}

void RenderStyle::fastPathInheritFrom(const RenderStyle& inheritParent)
{
    ASSERT(!disallowsFastPathInheritance());

    // FIXME: Use this mechanism for other properties too, like variables.
    m_inheritedFlags.visibility = inheritParent.m_inheritedFlags.visibility;
    m_inheritedFlags.hasExplicitlySetColor = inheritParent.m_inheritedFlags.hasExplicitlySetColor;

    if (m_inheritedData.ptr() != inheritParent.m_inheritedData.ptr()) {
        if (m_inheritedData->nonFastPathInheritedEqual(*inheritParent.m_inheritedData)) {
            m_inheritedData = inheritParent.m_inheritedData;
            return;
        }
        m_inheritedData.access().fastPathInheritFrom(*inheritParent.m_inheritedData);
    }
}

void RenderStyle::copyNonInheritedFrom(const RenderStyle& other)
{
    m_nonInheritedData = other.m_nonInheritedData;
    m_nonInheritedFlags.copyNonInheritedFrom(other.m_nonInheritedFlags);

    if (m_svgStyle != other.m_svgStyle)
        m_svgStyle.access().copyNonInheritedFrom(other.m_svgStyle.get());

    ASSERT(zoom() == initialZoom());
}

void RenderStyle::copyContentFrom(const RenderStyle& other)
{
    if (!other.m_nonInheritedData->miscData->content.isData())
        return;
    m_nonInheritedData.access().miscData.access().content = other.m_nonInheritedData->miscData->content;
}

void RenderStyle::copyPseudoElementsFrom(const RenderStyle& other)
{
    if (!other.m_cachedPseudoStyles)
        return;

    for (auto& [key, pseudoElementStyle] : other.m_cachedPseudoStyles->styles) {
        if (!pseudoElementStyle) {
            ASSERT_NOT_REACHED();
            continue;
        }
        addCachedPseudoStyle(makeUnique<RenderStyle>(cloneIncludingPseudoElements(*pseudoElementStyle)));
    }
}

void RenderStyle::copyPseudoElementBitsFrom(const RenderStyle& other)
{
    m_nonInheritedFlags.pseudoBits = other.m_nonInheritedFlags.pseudoBits;
}

bool RenderStyle::operator==(const RenderStyle& other) const
{
    // compare everything except the pseudoStyle pointer
    return m_inheritedFlags == other.m_inheritedFlags
        && m_nonInheritedFlags == other.m_nonInheritedFlags
        && m_nonInheritedData == other.m_nonInheritedData
        && m_rareInheritedData == other.m_rareInheritedData
        && m_inheritedData == other.m_inheritedData
        && m_svgStyle == other.m_svgStyle;
}

RenderStyle* RenderStyle::getCachedPseudoStyle(const Style::PseudoElementIdentifier& pseudoElementIdentifier) const
{
    if (!m_cachedPseudoStyles)
        return nullptr;

    return m_cachedPseudoStyles->styles.get(pseudoElementIdentifier);
}

RenderStyle* RenderStyle::addCachedPseudoStyle(std::unique_ptr<RenderStyle> pseudo)
{
    if (!pseudo)
        return nullptr;

    ASSERT(pseudo->pseudoElementType());

    RenderStyle* result = pseudo.get();

    if (!m_cachedPseudoStyles)
        m_cachedPseudoStyles = makeUnique<PseudoStyleCache>();

    m_cachedPseudoStyles->styles.add(*result->pseudoElementIdentifier(), WTF::move(pseudo));

    return result;
}

bool RenderStyle::inheritedEqual(const RenderStyle& other) const
{
    return m_inheritedFlags == other.m_inheritedFlags
        && m_inheritedData == other.m_inheritedData
        && (m_svgStyle.ptr() == other.m_svgStyle.ptr() || m_svgStyle->inheritedEqual(other.m_svgStyle))
        && m_rareInheritedData == other.m_rareInheritedData;
}

bool RenderStyle::nonInheritedEqual(const RenderStyle& other) const
{
    return m_nonInheritedFlags == other.m_nonInheritedFlags
        && m_nonInheritedData == other.m_nonInheritedData
        && (m_svgStyle.ptr() == other.m_svgStyle.ptr() || m_svgStyle->nonInheritedEqual(other.m_svgStyle));
}

bool RenderStyle::fastPathInheritedEqual(const RenderStyle& other) const
{
    if (m_inheritedFlags.visibility != other.m_inheritedFlags.visibility)
        return false;
    if (m_inheritedFlags.hasExplicitlySetColor != other.m_inheritedFlags.hasExplicitlySetColor)
        return false;
    if (m_inheritedData.ptr() == other.m_inheritedData.ptr())
        return true;
    return m_inheritedData->fastPathInheritedEqual(*other.m_inheritedData);
}

bool RenderStyle::nonFastPathInheritedEqual(const RenderStyle& other) const
{
    auto withoutFastPathFlags = [](auto flags) {
        flags.visibility = { };
        flags.hasExplicitlySetColor = { };
        return flags;
    };
    if (withoutFastPathFlags(m_inheritedFlags) != withoutFastPathFlags(other.m_inheritedFlags))
        return false;
    if (m_inheritedData.ptr() != other.m_inheritedData.ptr() && !m_inheritedData->nonFastPathInheritedEqual(*other.m_inheritedData))
        return false;
    if (m_rareInheritedData != other.m_rareInheritedData)
        return false;
    if (m_svgStyle.ptr() != other.m_svgStyle.ptr() && !m_svgStyle->inheritedEqual(other.m_svgStyle))
        return false;
    return true;
}

bool RenderStyle::descendantAffectingNonInheritedPropertiesEqual(const RenderStyle& other) const
{
    if (m_nonInheritedData.ptr() == other.m_nonInheritedData.ptr()
        || m_nonInheritedData->miscData.ptr() == other.m_nonInheritedData->miscData.ptr())
        return true;

    if (m_nonInheritedData->miscData->alignItems != other.m_nonInheritedData->miscData->alignItems)
        return false;

    if (m_nonInheritedData->miscData->justifyItems != other.m_nonInheritedData->miscData->justifyItems)
        return false;

    if (m_nonInheritedData->miscData->usedAppearance != other.m_nonInheritedData->miscData->usedAppearance)
        return false;

    return true;
}

bool RenderStyle::borderAndBackgroundEqual(const RenderStyle& other) const
{
    return border() == other.border()
        && backgroundLayers() == other.backgroundLayers()
        && backgroundColor() == other.backgroundColor();
}

#if ENABLE(TEXT_AUTOSIZING)

static inline unsigned computeFontHash(const FontCascade& font)
{
    // FIXME: Would be better to hash the family name rather than hashing a hash of the family name. Also, should this use FontCascadeDescription::familyNameHash?
    return computeHash(ASCIICaseInsensitiveHash::hash(font.fontDescription().firstFamily()), font.fontDescription().specifiedSize());
}

unsigned RenderStyle::hashForTextAutosizing() const
{
    // FIXME: Not a very smart hash. Could be improved upon. See <https://bugs.webkit.org/show_bug.cgi?id=121131>.
    unsigned hash = m_nonInheritedData->miscData->usedAppearance;
    hash ^= m_nonInheritedData->rareData->lineClamp.valueForHash();
    hash ^= m_rareInheritedData->overflowWrap;
    hash ^= m_rareInheritedData->nbspMode;
    hash ^= m_rareInheritedData->lineBreak;
    hash ^= m_inheritedData->specifiedLineHeight.valueForHash();
    hash ^= computeFontHash(m_inheritedData->fontData->fontCascade);
    hash ^= WTF::FloatHash<float>::hash(m_inheritedData->borderHorizontalSpacing.unresolvedValue());
    hash ^= WTF::FloatHash<float>::hash(m_inheritedData->borderVerticalSpacing.unresolvedValue());
    hash ^= m_inheritedFlags.boxDirection;
    hash ^= m_inheritedFlags.rtlOrdering;
    hash ^= m_nonInheritedFlags.position;
    hash ^= m_nonInheritedFlags.floating;
    hash ^= m_nonInheritedData->miscData->textOverflow;
    hash ^= m_rareInheritedData->textSecurity;
    return hash;
}

bool RenderStyle::equalForTextAutosizing(const RenderStyle& other) const
{
    return m_nonInheritedData->miscData->usedAppearance == other.m_nonInheritedData->miscData->usedAppearance
        && m_nonInheritedData->rareData->lineClamp == other.m_nonInheritedData->rareData->lineClamp
        && m_rareInheritedData->textSizeAdjust == other.m_rareInheritedData->textSizeAdjust
        && m_rareInheritedData->overflowWrap == other.m_rareInheritedData->overflowWrap
        && m_rareInheritedData->nbspMode == other.m_rareInheritedData->nbspMode
        && m_rareInheritedData->lineBreak == other.m_rareInheritedData->lineBreak
        && m_rareInheritedData->textSecurity == other.m_rareInheritedData->textSecurity
        && m_inheritedData->specifiedLineHeight == other.m_inheritedData->specifiedLineHeight
        && m_inheritedData->fontData->fontCascade.equalForTextAutoSizing(other.m_inheritedData->fontData->fontCascade)
        && m_inheritedData->borderHorizontalSpacing == other.m_inheritedData->borderHorizontalSpacing
        && m_inheritedData->borderVerticalSpacing == other.m_inheritedData->borderVerticalSpacing
        && m_inheritedFlags.boxDirection == other.m_inheritedFlags.boxDirection
        && m_inheritedFlags.rtlOrdering == other.m_inheritedFlags.rtlOrdering
        && m_nonInheritedFlags.position == other.m_nonInheritedFlags.position
        && m_nonInheritedFlags.floating == other.m_nonInheritedFlags.floating
        && m_nonInheritedData->miscData->textOverflow == other.m_nonInheritedData->miscData->textOverflow;
}

bool RenderStyle::isIdempotentTextAutosizingCandidate() const
{
    return isIdempotentTextAutosizingCandidate(OptionSet<AutosizeStatus::Fields>::fromRaw(m_inheritedFlags.autosizeStatus));
}

bool RenderStyle::isIdempotentTextAutosizingCandidate(AutosizeStatus status) const
{
    // Refer to <rdar://problem/51826266> for more information regarding how this function was generated.
    auto fields = status.fields();

    if (fields.contains(AutosizeStatus::Fields::AvoidSubtree))
        return false;

    constexpr float smallMinimumDifferenceThresholdBetweenLineHeightAndSpecifiedFontSizeForBoostingText = 5;
    constexpr float largeMinimumDifferenceThresholdBetweenLineHeightAndSpecifiedFontSizeForBoostingText = 25;

    if (fields.contains(AutosizeStatus::Fields::FixedHeight)) {
        if (fields.contains(AutosizeStatus::Fields::FixedWidth)) {
            if (whiteSpaceCollapse() == WhiteSpaceCollapse::Collapse && textWrapMode() == TextWrapMode::NoWrap) {
                if (width().isFixed())
                    return false;
                if (auto fixedHeight = height().tryFixed(); fixedHeight && specifiedLineHeight().isFixed()) {
                    if (auto fixedSpecifiedLineHeight = specifiedLineHeight().tryFixed()) {
                        float specifiedSize = specifiedFontSize();
                        if (fixedHeight->resolveZoom(usedZoomForLength()) == specifiedSize && fixedSpecifiedLineHeight->resolveZoom(usedZoomForLength()) == specifiedSize)
                            return false;
                    }
                }
                return true;
            }
            if (fields.contains(AutosizeStatus::Fields::Floating)) {
                if (auto fixedHeight = height().tryFixed(); specifiedLineHeight().isFixed() && fixedHeight) {
                    if (auto fixedSpecifiedLineHeight = specifiedLineHeight().tryFixed()) {
                        float specifiedSize = specifiedFontSize();
                        if (fixedSpecifiedLineHeight->resolveZoom(Style::ZoomFactor { 1.0f, deviceScaleFactor() }) - specifiedSize > smallMinimumDifferenceThresholdBetweenLineHeightAndSpecifiedFontSizeForBoostingText
                            && fixedHeight->resolveZoom(usedZoomForLength()) - specifiedSize > smallMinimumDifferenceThresholdBetweenLineHeightAndSpecifiedFontSizeForBoostingText)
                            return true;
                    }
                }
                return false;
            }
            if (fields.contains(AutosizeStatus::Fields::OverflowXHidden))
                return false;
            return true;
        }
        if (fields.contains(AutosizeStatus::Fields::OverflowXHidden)) {
            if (fields.contains(AutosizeStatus::Fields::Floating))
                return false;
            return true;
        }
        return true;
    }

    if (width().isFixed()) {
        if (breakWords())
            return true;
        return false;
    }

    if (textSizeAdjust().isPercentage() && textSizeAdjust().percentage() == 100) {
        if (fields.contains(AutosizeStatus::Fields::Floating))
            return true;
        if (fields.contains(AutosizeStatus::Fields::FixedWidth))
            return true;
        if (auto fixedSpecifiedLineHeight = specifiedLineHeight().tryFixed(); fixedSpecifiedLineHeight && fixedSpecifiedLineHeight->resolveZoom(usedZoomForLength()) - specifiedFontSize() > largeMinimumDifferenceThresholdBetweenLineHeightAndSpecifiedFontSizeForBoostingText)
            return true;
        return false;
    }

    if (hasBackgroundImage() && backgroundLayers().usedFirst().repeat() == FillRepeat::NoRepeat)
        return false;

    return true;
}

#endif // ENABLE(TEXT_AUTOSIZING)

void RenderStyle::addCustomPaintWatchProperty(const AtomString& name)
{
    auto& data = m_nonInheritedData.access().rareData.access();
    data.customPaintWatchedProperties.add(name);
}

bool RenderStyle::scrollAnchoringSuppressionStyleDidChange(const RenderStyle* other) const
{
    // https://drafts.csswg.org/css-scroll-anchoring/#suppression-triggers
    // Determine if there are any style changes that should result in an scroll anchoring suppression
    if (!other)
        return false;

    if (m_nonInheritedData->boxData.ptr() != other->m_nonInheritedData->boxData.ptr()) {
        if (m_nonInheritedData->boxData->width != other->m_nonInheritedData->boxData->width
            || m_nonInheritedData->boxData->minWidth != other->m_nonInheritedData->boxData->minWidth
            || m_nonInheritedData->boxData->maxWidth != other->m_nonInheritedData->boxData->maxWidth
            || m_nonInheritedData->boxData->height != other->m_nonInheritedData->boxData->height
            || m_nonInheritedData->boxData->minHeight != other->m_nonInheritedData->boxData->minHeight
            || m_nonInheritedData->boxData->maxHeight != other->m_nonInheritedData->boxData->maxHeight)
            return true;
    }

    if (overflowAnchor() != other->overflowAnchor() && overflowAnchor() == OverflowAnchor::None)
        return true;

    if (position() != other->position())
        return true;

    if (m_nonInheritedData->surroundData.ptr() && other->m_nonInheritedData->surroundData.ptr() && m_nonInheritedData->surroundData != other->m_nonInheritedData->surroundData) {
        if (m_nonInheritedData->surroundData->margin != other->m_nonInheritedData->surroundData->margin)
            return true;

        if (m_nonInheritedData->surroundData->padding != other->m_nonInheritedData->surroundData->padding)
            return true;
    }

    if (position() != PositionType::Static) {
        if (m_nonInheritedData->surroundData->inset != other->m_nonInheritedData->surroundData->inset)
            return true;
    }

    if (hasTransformRelatedProperty() != other->hasTransformRelatedProperty() || transform() != other->transform())
        return true;

    return false;
}

bool RenderStyle::outOfFlowPositionStyleDidChange(const RenderStyle* other) const
{
    // https://drafts.csswg.org/css-scroll-anchoring/#suppression-triggers
    // Determine if there is a style change that causes an element to become or stop
    // being absolutely or fixed positioned
    return other && hasOutOfFlowPosition() != other->hasOutOfFlowPosition();
}

bool RenderStyle::affectedByTransformOrigin() const
{
    if (rotate().affectedByTransformOrigin())
        return true;

    if (scale().affectedByTransformOrigin())
        return true;

    if (transform().affectedByTransformOrigin())
        return true;

    if (hasOffsetPath())
        return true;

    return false;
}

FloatPoint RenderStyle::computePerspectiveOrigin(const FloatRect& boundingBox) const
{
    return boundingBox.location() + Style::evaluate<FloatPoint>(perspectiveOrigin(), boundingBox.size(), Style::ZoomNeeded { });
}

void RenderStyle::applyPerspective(TransformationMatrix& transform, const FloatPoint& originTranslate) const
{
    // https://www.w3.org/TR/css-transforms-2/#perspective
    // The perspective matrix is computed as follows:
    // 1. Start with the identity matrix.

    // 2. Translate by the computed X and Y values of perspective-origin
    transform.translate(originTranslate.x(), originTranslate.y());

    // 3. Multiply by the matrix that would be obtained from the perspective() transform function, where the length is provided by the value of the perspective property
    transform.applyPerspective(usedPerspective());

    // 4. Translate by the negated computed X and Y values of perspective-origin
    transform.translate(-originTranslate.x(), -originTranslate.y());
}

FloatPoint3D RenderStyle::computeTransformOrigin(const FloatRect& boundingBox) const
{
    FloatPoint3D originTranslate;
    originTranslate.setXY(boundingBox.location() + Style::evaluate<FloatPoint>(transformOrigin().xy(), boundingBox.size(), Style::ZoomNeeded { }));
    originTranslate.setZ(transformOriginZ().resolveZoom(Style::ZoomNeeded { }));
    return originTranslate;
}

void RenderStyle::applyTransformOrigin(TransformationMatrix& transform, const FloatPoint3D& originTranslate) const
{
    if (!originTranslate.isZero())
        transform.translate3d(originTranslate.x(), originTranslate.y(), originTranslate.z());
}

void RenderStyle::unapplyTransformOrigin(TransformationMatrix& transform, const FloatPoint3D& originTranslate) const
{
    if (!originTranslate.isZero())
        transform.translate3d(-originTranslate.x(), -originTranslate.y(), -originTranslate.z());
}

void RenderStyle::applyTransform(TransformationMatrix& transform, const TransformOperationData& transformData, OptionSet<RenderStyle::TransformOperationOption> options) const
{
    if (!options.contains(RenderStyle::TransformOperationOption::TransformOrigin) || !affectedByTransformOrigin()) {
        applyCSSTransform(transform, transformData, options);
        return;
    }

    auto originTranslate = computeTransformOrigin(transformData.boundingBox);
    applyTransformOrigin(transform, originTranslate);
    applyCSSTransform(transform, transformData, options);
    unapplyTransformOrigin(transform, originTranslate);
}

void RenderStyle::applyTransform(TransformationMatrix& transform, const TransformOperationData& transformData) const
{
    applyTransform(transform, transformData, allTransformOperations());
}

void RenderStyle::applyCSSTransform(TransformationMatrix& transform, const TransformOperationData& operationData, OptionSet<RenderStyle::TransformOperationOption> options) const
{
    // https://www.w3.org/TR/css-transforms-2/#ctm
    // The transformation matrix is computed from the transform, transform-origin, translate, rotate, scale, and offset properties as follows:
    // 1. Start with the identity matrix.

    // 2. Translate by the computed X, Y, and Z values of transform-origin.
    // (implemented in applyTransformOrigin)
    auto& boundingBox = operationData.boundingBox;

    // 3. Translate by the computed X, Y, and Z values of translate.
    if (options.contains(RenderStyle::TransformOperationOption::Translate))
        translate().apply(transform, boundingBox.size());

    // 4. Rotate by the computed <angle> about the specified axis of rotate.
    if (options.contains(RenderStyle::TransformOperationOption::Rotate))
        rotate().apply(transform, boundingBox.size());

    // 5. Scale by the computed X, Y, and Z values of scale.
    if (options.contains(RenderStyle::TransformOperationOption::Scale))
        scale().apply(transform, boundingBox.size());

    // 6. Translate and rotate by the transform specified by offset.
    if (options.contains(RenderStyle::TransformOperationOption::Offset))
        MotionPath::applyMotionPathTransform(transform, operationData, *this);

    // 7. Multiply by each of the transform functions in transform from left to right.
    this->transform().apply(transform, boundingBox.size());

    // 8. Translate by the negated computed X, Y and Z values of transform-origin.
    // (implemented in unapplyTransformOrigin)
}

void RenderStyle::setPageScaleTransform(float scale)
{
    if (scale == 1)
        return;

    setTransform(Style::Transform { Style::TransformFunction { Style::ScaleTransformFunction::create(scale, scale, Style::TransformFunctionType::Scale) } });
    setTransformOriginX(0_css_px);
    setTransformOriginY(0_css_px);
}

const AtomString& RenderStyle::hyphenString() const
{
    ASSERT(hyphens() != Hyphens::None);

    return WTF::switchOn(m_rareInheritedData->hyphenateCharacter,
        [&](const CSS::Keyword::Auto&) -> const AtomString& {
            // FIXME: This should depend on locale.
            static MainThreadNeverDestroyed<const AtomString> hyphenMinusString(span(hyphenMinus));
            static MainThreadNeverDestroyed<const AtomString> hyphenString(span(hyphen));

            return fontCascade().primaryFont()->glyphForCharacter(hyphen) ? hyphenString : hyphenMinusString;
        },
        [](const AtomString& string) -> const AtomString& {
            return string;
        }
    );
}

void RenderStyle::adjustAnimations()
{
    if (animations().isInitial())
        return;

    ensureAnimations().prepareForUse();
}

void RenderStyle::adjustTransitions()
{
    if (transitions().isInitial())
        return;

    ensureTransitions().prepareForUse();
}

void RenderStyle::adjustBackgroundLayers()
{
    if (backgroundLayers().isInitial())
        return;

    ensureBackgroundLayers().prepareForUse();
}

void RenderStyle::adjustMaskLayers()
{
    if (maskLayers().isInitial())
        return;

    ensureMaskLayers().prepareForUse();
}

float RenderStyle::computedLineHeight() const
{
    return computeLineHeight(lineHeight());
}

float RenderStyle::computeLineHeight(const Style::LineHeight& lineHeight) const
{
    return WTF::switchOn(lineHeight,
        [&](const CSS::Keyword::Normal&) -> float {
            return metricsOfPrimaryFont().lineSpacing();
        },
        [&](const Style::LineHeight::Fixed& fixed) -> float {
            return Style::evaluate<LayoutUnit>(fixed, usedZoomForLength()).toFloat();
        },
        [&](const Style::LineHeight::Percentage& percentage) -> float {
            return Style::evaluate<LayoutUnit>(percentage, LayoutUnit { computedFontSize() }).toFloat();
        },
        [&](const Style::LineHeight::Calc& calc) -> float {
            return Style::evaluate<LayoutUnit>(calc, LayoutUnit { computedFontSize() }, usedZoomForLength()).toFloat();
        }
    );
}

const Style::Color& RenderStyle::unresolvedColorForProperty(CSSPropertyID colorProperty, bool visitedLink) const
{
    switch (colorProperty) {
    case CSSPropertyAccentColor:
        return accentColor().colorOrCurrentColor();
    case CSSPropertyBackgroundColor:
        return visitedLink ? visitedLinkBackgroundColor() : backgroundColor();
    case CSSPropertyBorderBottomColor:
        return visitedLink ? visitedLinkBorderBottomColor() : borderBottomColor();
    case CSSPropertyBorderLeftColor:
        return visitedLink ? visitedLinkBorderLeftColor() : borderLeftColor();
    case CSSPropertyBorderRightColor:
        return visitedLink ? visitedLinkBorderRightColor() : borderRightColor();
    case CSSPropertyBorderTopColor:
        return visitedLink ? visitedLinkBorderTopColor() : borderTopColor();
    case CSSPropertyFill:
        return fill().colorDisregardingType();
    case CSSPropertyFloodColor:
        return floodColor();
    case CSSPropertyLightingColor:
        return lightingColor();
    case CSSPropertyOutlineColor:
        return visitedLink ? visitedLinkOutlineColor() : outlineColor();
    case CSSPropertyStopColor:
        return stopColor();
    case CSSPropertyStroke:
        return stroke().colorDisregardingType();
    case CSSPropertyStrokeColor:
        return visitedLink ? visitedLinkStrokeColor() : strokeColor();
    case CSSPropertyBorderBlockEndColor:
    case CSSPropertyBorderBlockStartColor:
    case CSSPropertyBorderInlineEndColor:
    case CSSPropertyBorderInlineStartColor:
        return unresolvedColorForProperty(CSSProperty::resolveDirectionAwareProperty(colorProperty, writingMode()));
    case CSSPropertyColumnRuleColor:
        return visitedLink ? visitedLinkColumnRuleColor() : columnRuleColor();
    case CSSPropertyTextEmphasisColor:
        return visitedLink ? visitedLinkTextEmphasisColor() : textEmphasisColor();
    case CSSPropertyWebkitTextFillColor:
        return visitedLink ? visitedLinkTextFillColor() : textFillColor();
    case CSSPropertyWebkitTextStrokeColor:
        return visitedLink ? visitedLinkTextStrokeColor() : textStrokeColor();
    case CSSPropertyTextDecorationColor:
        return visitedLink ? visitedLinkTextDecorationColor() : textDecorationColor();
    case CSSPropertyCaretColor:
        return visitedLink ? visitedLinkCaretColor() : caretColor();
    default:
        ASSERT_NOT_REACHED();
        break;
    }

    static NeverDestroyed<Style::Color> defaultColor { };
    return defaultColor;
}

Color RenderStyle::colorResolvingCurrentColor(CSSPropertyID colorProperty, bool visitedLink) const
{
    if (colorProperty == CSSPropertyColor)
        return visitedLink ? visitedLinkColor() : color();

    auto& result = unresolvedColorForProperty(colorProperty, visitedLink);
    if (result.isCurrentColor()) {
        if (colorProperty == CSSPropertyTextDecorationColor) {
            if (hasPositiveStrokeWidth()) {
                // Prefer stroke color if possible but not if it's fully transparent.
                auto strokeColor = colorResolvingCurrentColor(usedStrokeColorProperty(), visitedLink);
                if (strokeColor.isVisible())
                    return strokeColor;
            }

            return colorResolvingCurrentColor(CSSPropertyWebkitTextFillColor, visitedLink);
        }

        return visitedLink ? visitedLinkColor() : color();
    }

    return colorResolvingCurrentColor(result, visitedLink);
}

Color RenderStyle::colorResolvingCurrentColor(const Style::Color& color, bool visitedLink) const
{
    return color.resolveColor(visitedLink ? visitedLinkColor() : this->color());
}

Color RenderStyle::visitedDependentColor(CSSPropertyID colorProperty, OptionSet<PaintBehavior> paintBehavior) const
{
    Color unvisitedColor = colorResolvingCurrentColor(colorProperty, false);
    if (insideLink() != InsideLink::InsideVisited)
        return unvisitedColor;

    if (paintBehavior.contains(PaintBehavior::DontShowVisitedLinks))
        return unvisitedColor;

    if (isInSubtreeWithBlendMode())
        return unvisitedColor;

    Color visitedColor = colorResolvingCurrentColor(colorProperty, true);

    // FIXME: Technically someone could explicitly specify the color transparent, but for now we'll just
    // assume that if the background color is transparent that it wasn't set. Note that it's weird that
    // we're returning unvisited info for a visited link, but given our restriction that the alpha values
    // have to match, it makes more sense to return the unvisited background color if specified than it
    // does to return black. This behavior matches what Firefox 4 does as well.
    if (colorProperty == CSSPropertyBackgroundColor && visitedColor == Color::transparentBlack)
        return unvisitedColor;

    // Take the alpha from the unvisited color, but get the RGB values from the visited color.
    return visitedColor.colorWithAlpha(unvisitedColor.alphaAsFloat());
}

Color RenderStyle::visitedDependentColorWithColorFilter(CSSPropertyID colorProperty, OptionSet<PaintBehavior> paintBehavior) const
{
    if (!hasAppleColorFilter())
        return visitedDependentColor(colorProperty, paintBehavior);

    return colorByApplyingColorFilter(visitedDependentColor(colorProperty, paintBehavior));
}

Color RenderStyle::colorByApplyingColorFilter(const Color& color) const
{
    Color transformedColor = color;
    appleColorFilter().transformColor(transformedColor);
    return transformedColor;
}

Color RenderStyle::colorWithColorFilter(const Style::Color& color) const
{
    return colorByApplyingColorFilter(colorResolvingCurrentColor(color));
}

Color RenderStyle::usedAccentColor(OptionSet<StyleColorOptions> styleColorOptions) const
{
    return WTF::switchOn(accentColor(),
        [](const CSS::Keyword::Auto&) -> Color {
            return { };
        },
        [&](const Style::Color& color) -> Color {
            auto resolvedAccentColor = colorResolvingCurrentColor(color);

            if (!resolvedAccentColor.isOpaque()) {
                auto computedCanvasColor = RenderTheme::singleton().systemColor(CSSValueCanvas, styleColorOptions);
                resolvedAccentColor = blendSourceOver(computedCanvasColor, resolvedAccentColor);
            }

            if (hasAppleColorFilter())
                return colorByApplyingColorFilter(resolvedAccentColor);

            return resolvedAccentColor;
        }
    );
}

Color RenderStyle::usedScrollbarThumbColor() const
{
    return WTF::switchOn(scrollbarColor(),
        [&](const CSS::Keyword::Auto&) -> Color {
            return { };
        },
        [&](const auto& parts) -> Color {
            if (hasAppleColorFilter())
                return colorByApplyingColorFilter(colorResolvingCurrentColor(parts.thumb));
            return colorResolvingCurrentColor(parts.thumb);
        }
    );
}

Color RenderStyle::usedScrollbarTrackColor() const
{
    return WTF::switchOn(scrollbarColor(),
        [&](const CSS::Keyword::Auto&) -> Color {
            return { };
        },
        [&](const auto& parts) -> Color {
            if (hasAppleColorFilter())
                return colorByApplyingColorFilter(colorResolvingCurrentColor(parts.track));
            return colorResolvingCurrentColor(parts.track);
        }
    );
}

const BorderValue& RenderStyle::borderBefore(const WritingMode writingMode) const
{
    switch (writingMode.blockDirection()) {
    case FlowDirection::TopToBottom:
        return borderTop();
    case FlowDirection::BottomToTop:
        return borderBottom();
    case FlowDirection::LeftToRight:
        return borderLeft();
    case FlowDirection::RightToLeft:
        return borderRight();
    }
    ASSERT_NOT_REACHED();
    return borderTop();
}

const BorderValue& RenderStyle::borderAfter(const WritingMode writingMode) const
{
    switch (writingMode.blockDirection()) {
    case FlowDirection::TopToBottom:
        return borderBottom();
    case FlowDirection::BottomToTop:
        return borderTop();
    case FlowDirection::LeftToRight:
        return borderRight();
    case FlowDirection::RightToLeft:
        return borderLeft();
    }
    ASSERT_NOT_REACHED();
    return borderBottom();
}

const BorderValue& RenderStyle::borderStart(const WritingMode writingMode) const
{
    if (writingMode.isHorizontal())
        return writingMode.isInlineLeftToRight() ? borderLeft() : borderRight();
    return writingMode.isInlineTopToBottom() ? borderTop() : borderBottom();
}

const BorderValue& RenderStyle::borderEnd(const WritingMode writingMode) const
{
    if (writingMode.isHorizontal())
        return writingMode.isInlineLeftToRight() ? borderRight() : borderLeft();
    return writingMode.isInlineTopToBottom() ? borderBottom() : borderTop();
}

Style::LineWidth RenderStyle::borderBeforeWidth(const WritingMode writingMode) const
{
    switch (writingMode.blockDirection()) {
    case FlowDirection::TopToBottom:
        return borderTopWidth();
    case FlowDirection::BottomToTop:
        return borderBottomWidth();
    case FlowDirection::LeftToRight:
        return borderLeftWidth();
    case FlowDirection::RightToLeft:
        return borderRightWidth();
    }
    ASSERT_NOT_REACHED();
    return borderTopWidth();
}

Style::LineWidth RenderStyle::borderAfterWidth(const WritingMode writingMode) const
{
    switch (writingMode.blockDirection()) {
    case FlowDirection::TopToBottom:
        return borderBottomWidth();
    case FlowDirection::BottomToTop:
        return borderTopWidth();
    case FlowDirection::LeftToRight:
        return borderRightWidth();
    case FlowDirection::RightToLeft:
        return borderLeftWidth();
    }
    ASSERT_NOT_REACHED();
    return borderBottomWidth();
}

Style::LineWidth RenderStyle::borderStartWidth(const WritingMode writingMode) const
{
    if (writingMode.isHorizontal())
        return writingMode.isInlineLeftToRight() ? borderLeftWidth() : borderRightWidth();
    return writingMode.isInlineTopToBottom() ? borderTopWidth() : borderBottomWidth();
}

Style::LineWidth RenderStyle::borderEndWidth(const WritingMode writingMode) const
{
    if (writingMode.isHorizontal())
        return writingMode.isInlineLeftToRight() ? borderRightWidth() : borderLeftWidth();
    return writingMode.isInlineTopToBottom() ? borderBottomWidth() : borderTopWidth();
}

void RenderStyle::setMarginStart(Style::MarginEdge&& margin)
{
    if (writingMode().isHorizontal()) {
        if (writingMode().isInlineLeftToRight())
            setMarginLeft(WTF::move(margin));
        else
            setMarginRight(WTF::move(margin));
    } else {
        if (writingMode().isInlineTopToBottom())
            setMarginTop(WTF::move(margin));
        else
            setMarginBottom(WTF::move(margin));
    }
}

void RenderStyle::setMarginEnd(Style::MarginEdge&& margin)
{
    if (writingMode().isHorizontal()) {
        if (writingMode().isInlineLeftToRight())
            setMarginRight(WTF::move(margin));
        else
            setMarginLeft(WTF::move(margin));
    } else {
        if (writingMode().isInlineTopToBottom())
            setMarginBottom(WTF::move(margin));
        else
            setMarginTop(WTF::move(margin));
    }
}

void RenderStyle::setMarginBefore(Style::MarginEdge&& margin)
{
    switch (writingMode().blockDirection()) {
    case FlowDirection::TopToBottom:
        return setMarginTop(WTF::move(margin));
    case FlowDirection::BottomToTop:
        return setMarginBottom(WTF::move(margin));
    case FlowDirection::LeftToRight:
        return setMarginLeft(WTF::move(margin));
    case FlowDirection::RightToLeft:
        return setMarginRight(WTF::move(margin));
    }
}

void RenderStyle::setMarginAfter(Style::MarginEdge&& margin)
{
    switch (writingMode().blockDirection()) {
    case FlowDirection::TopToBottom:
        return setMarginBottom(WTF::move(margin));
    case FlowDirection::BottomToTop:
        return setMarginTop(WTF::move(margin));
    case FlowDirection::LeftToRight:
        return setMarginRight(WTF::move(margin));
    case FlowDirection::RightToLeft:
        return setMarginLeft(WTF::move(margin));
    }
}

void RenderStyle::setPaddingStart(Style::PaddingEdge&& padding)
{
    if (writingMode().isHorizontal()) {
        if (writingMode().isInlineLeftToRight())
            setPaddingLeft(WTF::move(padding));
        else
            setPaddingRight(WTF::move(padding));
    } else {
        if (writingMode().isInlineTopToBottom())
            setPaddingTop(WTF::move(padding));
        else
            setPaddingBottom(WTF::move(padding));
    }
}

void RenderStyle::setPaddingEnd(Style::PaddingEdge&& padding)
{
    if (writingMode().isHorizontal()) {
        if (writingMode().isInlineLeftToRight())
            setPaddingRight(WTF::move(padding));
        else
            setPaddingLeft(WTF::move(padding));
    } else {
        if (writingMode().isInlineTopToBottom())
            setPaddingBottom(WTF::move(padding));
        else
            setPaddingTop(WTF::move(padding));
    }
}

void RenderStyle::setPaddingBefore(Style::PaddingEdge&& padding)
{
    switch (writingMode().blockDirection()) {
    case FlowDirection::TopToBottom:
        return setPaddingTop(WTF::move(padding));
    case FlowDirection::BottomToTop:
        return setPaddingBottom(WTF::move(padding));
    case FlowDirection::LeftToRight:
        return setPaddingLeft(WTF::move(padding));
    case FlowDirection::RightToLeft:
        return setPaddingRight(WTF::move(padding));
    }
}

void RenderStyle::setPaddingAfter(Style::PaddingEdge&& padding)
{
    switch (writingMode().blockDirection()) {
    case FlowDirection::TopToBottom:
        return setPaddingBottom(WTF::move(padding));
    case FlowDirection::BottomToTop:
        return setPaddingTop(WTF::move(padding));
    case FlowDirection::LeftToRight:
        return setPaddingRight(WTF::move(padding));
    case FlowDirection::RightToLeft:
        return setPaddingLeft(WTF::move(padding));
    }
}

String RenderStyle::altFromContent() const
{
    if (auto* contentData = content().tryData())
        return contentData->altText.value_or(nullString());
    return { };
}

template<typename OutsetValue>
static LayoutUnit computeOutset(const OutsetValue& outsetValue, LayoutUnit borderWidth)
{
    return WTF::switchOn(outsetValue,
        [&](const typename OutsetValue::Number& number) {
            return LayoutUnit(number.value * borderWidth);
        },
        [&](const typename OutsetValue::Length& length) {
            return LayoutUnit(length.resolveZoom(Style::ZoomNeeded { }));
        }
    );
}

LayoutBoxExtent RenderStyle::imageOutsets(const Style::BorderImage& image) const
{
    return {
        computeOutset(image.outset().values.top(), Style::evaluate<LayoutUnit>(borderTopWidth(), Style::ZoomNeeded { })),
        computeOutset(image.outset().values.right(), Style::evaluate<LayoutUnit>(borderRightWidth(), Style::ZoomNeeded { })),
        computeOutset(image.outset().values.bottom(), Style::evaluate<LayoutUnit>(borderBottomWidth(), Style::ZoomNeeded { })),
        computeOutset(image.outset().values.left(), Style::evaluate<LayoutUnit>(borderLeftWidth(), Style::ZoomNeeded { })),
    };
}

LayoutBoxExtent RenderStyle::imageOutsets(const Style::MaskBorder& image) const
{
    return {
        computeOutset(image.outset().values.top(), Style::evaluate<LayoutUnit>(borderTopWidth(), Style::ZoomNeeded { })),
        computeOutset(image.outset().values.right(), Style::evaluate<LayoutUnit>(borderRightWidth(), Style::ZoomNeeded { })),
        computeOutset(image.outset().values.bottom(), Style::evaluate<LayoutUnit>(borderBottomWidth(), Style::ZoomNeeded { })),
        computeOutset(image.outset().values.left(), Style::evaluate<LayoutUnit>(borderLeftWidth(), Style::ZoomNeeded { })),
    };
}

void RenderStyle::setColumnStylesFromPaginationMode(PaginationMode paginationMode)
{
    if (paginationMode == Pagination::Mode::Unpaginated)
        return;
    
    setColumnFill(ColumnFill::Auto);
    
    switch (paginationMode) {
    case Pagination::Mode::LeftToRightPaginated:
        setColumnAxis(ColumnAxis::Horizontal);
        if (writingMode().isHorizontal())
            setColumnProgression(writingMode().isBidiLTR() ? ColumnProgression::Normal : ColumnProgression::Reverse);
        else
            setColumnProgression(writingMode().isBlockFlipped() ? ColumnProgression::Reverse : ColumnProgression::Normal);
        break;
    case Pagination::Mode::RightToLeftPaginated:
        setColumnAxis(ColumnAxis::Horizontal);
        if (writingMode().isHorizontal())
            setColumnProgression(writingMode().isBidiLTR() ? ColumnProgression::Reverse : ColumnProgression::Normal);
        else
            setColumnProgression(writingMode().isBlockFlipped() ? ColumnProgression::Normal : ColumnProgression::Reverse);
        break;
    case Pagination::Mode::TopToBottomPaginated:
        setColumnAxis(ColumnAxis::Vertical);
        if (writingMode().isHorizontal())
            setColumnProgression(writingMode().isBlockFlipped() ? ColumnProgression::Reverse : ColumnProgression::Normal);
        else
            setColumnProgression(writingMode().isBidiLTR() ? ColumnProgression::Normal : ColumnProgression::Reverse);
        break;
    case Pagination::Mode::BottomToTopPaginated:
        setColumnAxis(ColumnAxis::Vertical);
        if (writingMode().isHorizontal())
            setColumnProgression(writingMode().isBlockFlipped() ? ColumnProgression::Normal : ColumnProgression::Reverse);
        else
            setColumnProgression(writingMode().isBidiLTR() ? ColumnProgression::Reverse : ColumnProgression::Normal);
        break;
    case Pagination::Mode::Unpaginated:
        ASSERT_NOT_REACHED();
        break;
    }
}

void RenderStyle::deduplicateCustomProperties(const RenderStyle& other)
{
    auto deduplicate = [&] <typename T> (const DataRef<T>& data, const DataRef<T>& otherData) {
        auto& properties = const_cast<DataRef<Style::CustomPropertyData>&>(data->customProperties);
        auto& otherProperties = otherData->customProperties;
        if (properties.ptr() == otherProperties.ptr() || *properties != *otherProperties)
            return;
        properties = otherProperties;
    };

    deduplicate(m_rareInheritedData, other.m_rareInheritedData);
    deduplicate(m_nonInheritedData->rareData, other.m_nonInheritedData->rareData);
}

void RenderStyle::setCustomPropertyValue(Ref<const Style::CustomProperty>&& value, bool isInherited)
{
    auto& name = value->name();
    if (isInherited) {
        if (auto* existingValue = m_rareInheritedData->customProperties->get(name); !existingValue || *existingValue != value.get())
            m_rareInheritedData.access().customProperties.access().set(name, WTF::move(value));
    } else {
        if (auto* existingValue = m_nonInheritedData->rareData->customProperties->get(name); !existingValue || *existingValue != value.get())
            m_nonInheritedData.access().rareData.access().customProperties.access().set(name, WTF::move(value));
    }
}

const Style::CustomProperty* RenderStyle::customPropertyValue(const AtomString& name) const
{
    for (auto* map : { &nonInheritedCustomProperties(), &inheritedCustomProperties() }) {
        if (auto* value = map->get(name))
            return value;
    }
    return nullptr;
}

bool RenderStyle::customPropertyValueEqual(const RenderStyle& other, const AtomString& name) const
{
    if (&nonInheritedCustomProperties() == &other.nonInheritedCustomProperties() && &inheritedCustomProperties() == &other.inheritedCustomProperties())
        return true;

    auto* value = customPropertyValue(name);
    auto* otherValue = other.customPropertyValue(name);
    if (value == otherValue)
        return true;
    if (!value || !otherValue)
        return false;
    return *value == *otherValue;
}

bool RenderStyle::customPropertiesEqual(const RenderStyle& other) const
{
    return m_nonInheritedData->rareData->customProperties == other.m_nonInheritedData->rareData->customProperties
        && m_rareInheritedData->customProperties == other.m_rareInheritedData->customProperties;
}

bool RenderStyle::scrollSnapDataEquivalent(const RenderStyle& other) const
{
    if (m_nonInheritedData.ptr() == other.m_nonInheritedData.ptr()
        || m_nonInheritedData->rareData.ptr() == other.m_nonInheritedData->rareData.ptr())
        return true;

    return m_nonInheritedData->rareData->scrollMargin == other.m_nonInheritedData->rareData->scrollMargin
        && m_nonInheritedData->rareData->scrollSnapAlign == other.m_nonInheritedData->rareData->scrollSnapAlign
        && m_nonInheritedData->rareData->scrollSnapStop == other.m_nonInheritedData->rareData->scrollSnapStop
        && m_nonInheritedData->rareData->scrollSnapAlign == other.m_nonInheritedData->rareData->scrollSnapAlign;
}

float RenderStyle::outlineSize() const
{
    return std::max(0.0f, Style::evaluate<float>(outlineWidth(), Style::ZoomNeeded { }) + Style::evaluate<float>(outlineOffset(), Style::ZoomNeeded { }));
}

bool RenderStyle::shouldPlaceVerticalScrollbarOnLeft() const
{
    return !writingMode().isAnyLeftToRight();
}

float RenderStyle::computedStrokeWidth(const IntSize& viewportSize) const
{
    // Use the stroke-width and stroke-color value combination only if stroke-color has been explicitly specified.
    // Since there will be no visible stroke when stroke-color is not specified (transparent by default), we fall
    // back to the legacy Webkit text stroke combination in that case.
    if (!hasExplicitlySetStrokeColor())
        return Style::evaluate<float>(textStrokeWidth(), usedZoomForLength());

    return WTF::switchOn(strokeWidth(),
        [&](const Style::StrokeWidth::Fixed& fixedStrokeWidth) -> float {
            return Style::evaluate<float>(fixedStrokeWidth, Style::ZoomNeeded { });
        },
        [&](const Style::StrokeWidth::Percentage& percentageStrokeWidth) -> float {
            // According to the spec, https://drafts.fxtf.org/paint/#stroke-width, the percentage is relative to the scaled viewport size.
            // The scaled viewport size is the geometric mean of the viewport width and height.
            return percentageStrokeWidth.value * (viewportSize.width() + viewportSize.height()) / 200.0f;
        },
        [&](const Style::StrokeWidth::Calc& calcStrokeWidth) -> float {
            // FIXME: It is almost certainly wrong that calc and percentage are being handled differently - https://bugs.webkit.org/show_bug.cgi?id=296482
            return Style::evaluate<float>(calcStrokeWidth, viewportSize.width(), Style::ZoomNeeded { });
        }
    );
}

bool RenderStyle::hasPositiveStrokeWidth() const
{
    if (!hasExplicitlySetStrokeWidth())
        return textStrokeWidth().isPositive();
    return strokeWidth().isPossiblyPositive();
}

Color RenderStyle::computedStrokeColor() const
{
    return visitedDependentColor(usedStrokeColorProperty());
}

UsedClear RenderStyle::usedClear(const RenderElement& renderer)
{
    auto computedClear = renderer.style().clear();
    auto writingMode = renderer.containingBlock()->writingMode();
    switch (computedClear) {
    case Clear::None:
        return UsedClear::None;
    case Clear::Both:
        return UsedClear::Both;
    case Clear::Left:
        return writingMode.isLogicalLeftLineLeft() ? UsedClear::Left : UsedClear::Right;
    case Clear::Right:
        return writingMode.isLogicalLeftLineLeft() ? UsedClear::Right : UsedClear::Left;
    case Clear::InlineStart:
        return writingMode.isLogicalLeftInlineStart() ? UsedClear::Left : UsedClear::Right;
    case Clear::InlineEnd:
        return writingMode.isLogicalLeftInlineStart() ? UsedClear::Right : UsedClear::Left;
    }

    RELEASE_ASSERT_NOT_REACHED();
}

UsedFloat RenderStyle::usedFloat(const RenderElement& renderer)
{
    auto computedFloat = renderer.style().floating();
    auto writingMode = renderer.containingBlock()->writingMode();
    switch (computedFloat) {
    case Float::None:
        return UsedFloat::None;
    case Float::Left:
        return writingMode.isLogicalLeftLineLeft() ? UsedFloat::Left : UsedFloat::Right;
    case Float::Right:
        return writingMode.isLogicalLeftLineLeft() ? UsedFloat::Right : UsedFloat::Left;
    case Float::InlineStart:
        return writingMode.isLogicalLeftInlineStart() ? UsedFloat::Left : UsedFloat::Right;
    case Float::InlineEnd:
        return writingMode.isLogicalLeftInlineStart() ? UsedFloat::Right : UsedFloat::Left;
    }

    RELEASE_ASSERT_NOT_REACHED();
}

UserSelect RenderStyle::usedUserSelect() const
{
    if (effectiveInert())
        return UserSelect::None;

    auto value = userSelect();
    if (userModify() != UserModify::ReadOnly && userDrag() != UserDrag::Element)
        return value == UserSelect::None ? UserSelect::Text : value;

    return value;
}

std::optional<Style::PseudoElementIdentifier> RenderStyle::pseudoElementIdentifier() const
{
    if (!pseudoElementType())
        return { };
    return Style::PseudoElementIdentifier { *pseudoElementType(), pseudoElementNameArgument() };
}

void RenderStyle::adjustScrollTimelines()
{
    auto& names = scrollTimelineNames();
    if (names.isNone() && scrollTimelines().isEmpty())
        return;

    auto& axes = scrollTimelineAxes();
    auto numberOfAxes = axes.size();
    ASSERT(numberOfAxes > 0);

    m_nonInheritedData.access().rareData.access().scrollTimelines = { FixedVector<Ref<ScrollTimeline>>::createWithSizeFromGenerator(names.size(), [&](auto i) {
        return ScrollTimeline::create(names[i].value.value, axes[i % numberOfAxes]);
    }) };
}

void RenderStyle::adjustViewTimelines()
{
    auto& names = viewTimelineNames();
    if (names.isNone() && viewTimelines().isEmpty())
        return;

    auto& axes = viewTimelineAxes();
    auto numberOfAxes = axes.size();
    ASSERT(numberOfAxes > 0);

    auto& insets = viewTimelineInsets();
    auto numberOfInsets = insets.size();
    ASSERT(numberOfInsets > 0);

    m_nonInheritedData.access().rareData.access().viewTimelines = { FixedVector<Ref<ViewTimeline>>::createWithSizeFromGenerator(names.size(), [&](auto i) {
        return ViewTimeline::create(names[i].value.value, axes[i % numberOfAxes], insets[i % numberOfInsets]);
    }) };
}

} // namespace WebCore
