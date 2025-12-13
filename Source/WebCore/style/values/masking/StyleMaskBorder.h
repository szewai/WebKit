/*
 * Copyright (C) 2000 Lars Knoll (knoll@kde.org)
 *           (C) 2000 Antti Koivisto (koivisto@kde.org)
 *           (C) 2000 Dirk Mueller (mueller@kde.org)
 * Copyright (C) 2003-2017 Apple Inc. All rights reserved.
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

#pragma once

#include <WebCore/StyleMaskBorderOutset.h>
#include <WebCore/StyleMaskBorderRepeat.h>
#include <WebCore/StyleMaskBorderSlice.h>
#include <WebCore/StyleMaskBorderSource.h>
#include <WebCore/StyleMaskBorderWidth.h>
#include <wtf/DataRef.h>

namespace WebCore {

class RenderStyleProperties;

namespace Style {

// <'mask-border'> = <'mask-border-source'> || <'mask-border-slice'> [ / <'mask-border-width'>? [ / <'mask-border-outset'> ]? ]? || <'mask-border-repeat'> || <'mask-border-mode'>
// FIXME: Add support for `mask-border-mode`.
// https://drafts.fxtf.org/css-masking-1/#propdef-mask-border
struct MaskBorder {
    using Source = MaskBorderSource;
    using Slice = MaskBorderSlice;
    using Width = MaskBorderWidth;
    using Outset = MaskBorderOutset;
    using Repeat = MaskBorderRepeat;

    MaskBorder();
    MaskBorder(MaskBorderSource&&, MaskBorderSlice&&, MaskBorderWidth&&, MaskBorderOutset&&, MaskBorderRepeat&&);

    bool hasSource() const { return !m_data->maskBorderSource.isNone(); }
    const MaskBorderSource& source() const { return m_data->maskBorderSource; }
    void setSource(MaskBorderSource&& source) { m_data.access().maskBorderSource = WTFMove(source); }

    const MaskBorderSlice& slice() const { return m_data->maskBorderSlice; }
    void setSlice(MaskBorderSlice&& slice) { m_data.access().maskBorderSlice = WTFMove(slice); }

    const MaskBorderWidth& width() const { return m_data->maskBorderWidth; }
    void setWidth(MaskBorderWidth&& width) { m_data.access().maskBorderWidth = WTFMove(width); }

    const MaskBorderOutset& outset() const { return m_data->maskBorderOutset; }
    void setOutset(MaskBorderOutset&& outset) { m_data.access().maskBorderOutset = WTFMove(outset); }

    const MaskBorderRepeat& repeat() const { return m_data->maskBorderRepeat; }
    void setRepeat(MaskBorderRepeat&& repeat) { m_data.access().maskBorderRepeat = WTFMove(repeat); }

    void copySliceFrom(const MaskBorder& other)
    {
        m_data.access().maskBorderSlice = other.m_data->maskBorderSlice;
    }

    void copyWidthFrom(const MaskBorder& other)
    {
        m_data.access().maskBorderWidth = other.m_data->maskBorderWidth;
    }

    void copyOutsetFrom(const MaskBorder& other)
    {
        m_data.access().maskBorderOutset = other.m_data->maskBorderOutset;
    }

    void copyRepeatFrom(const MaskBorder& other)
    {
        m_data.access().maskBorderRepeat = other.m_data->maskBorderRepeat;
    }

    bool operator==(const MaskBorder&) const = default;

private:
    friend class WebCore::RenderStyleProperties;

    struct Data : RefCounted<Data> {
        static Ref<Data> create();
        static Ref<Data> create(MaskBorderSource&&, MaskBorderSlice&&, MaskBorderWidth&&, MaskBorderOutset&&, MaskBorderRepeat&&);
        Ref<Data> copy() const;

        bool operator==(const Data&) const;

        MaskBorderSource maskBorderSource;
        MaskBorderSlice maskBorderSlice;
        MaskBorderWidth maskBorderWidth;
        MaskBorderOutset maskBorderOutset;
        MaskBorderRepeat maskBorderRepeat;

    private:
        Data();
        Data(MaskBorderSource&&, MaskBorderSlice&&, MaskBorderWidth&&, MaskBorderOutset&&, MaskBorderRepeat&&);
        Data(const Data&);
    };

    static DataRef<Data>& defaultData();

    DataRef<Data> m_data;
};

// MARK: - Conversion

template<> struct CSSValueConversion<MaskBorder> { auto operator()(BuilderState&, const CSSValue&) -> MaskBorder; };
template<> struct CSSValueCreation<MaskBorder> { auto operator()(CSSValuePool&, const RenderStyle&, const MaskBorder&) -> Ref<CSSValue>; };

// MARK: - Serialization

template<> struct Serialize<MaskBorder> { void operator()(StringBuilder&, const CSS::SerializationContext&, const RenderStyle&, const MaskBorder&); };

// MARK: - Logging

WTF::TextStream& operator<<(WTF::TextStream&, const MaskBorder&);

} // namespace Style
} // namespace WebCore
