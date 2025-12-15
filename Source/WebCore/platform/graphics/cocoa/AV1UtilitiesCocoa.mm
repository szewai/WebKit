/*
 * Copyright (C) 2023 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "AV1UtilitiesCocoa.h"

#if PLATFORM(COCOA) && ENABLE(AV1)

#import "AV1Utilities.h"
#import "BitReader.h"
#import "MediaCapabilitiesInfo.h"
#import <wtf/cf/TypeCastsCF.h>
#import <wtf/cf/VectorCF.h>
#import <wtf/cocoa/TypeCastsCocoa.h>
#import <wtf/text/StringToIntegerConversion.h>

#import "VideoToolboxSoftLink.h"
#import <pal/cf/CoreMediaSoftLink.h>

namespace WebCore {

static bool isConfigurationRecordHDR(const AV1CodecConfigurationRecord& record)
{
    if (record.bitDepth < 10)
        return false;

    if (record.colorPrimaries != static_cast<uint8_t>(AV1ConfigurationColorPrimaries::BT_2020_Nonconstant_Luminance))
        return false;

    if (record.transferCharacteristics != static_cast<uint8_t>(AV1ConfigurationTransferCharacteristics::BT_2020_10bit)
        && record.transferCharacteristics != static_cast<uint8_t>(AV1ConfigurationTransferCharacteristics::BT_2020_12bit)
        && record.transferCharacteristics != static_cast<uint8_t>(AV1ConfigurationTransferCharacteristics::SMPTE_ST_2084)
        && record.transferCharacteristics != static_cast<uint8_t>(AV1ConfigurationTransferCharacteristics::BT_2100_HLG))
        return false;

    if (record.matrixCoefficients != static_cast<uint8_t>(AV1ConfigurationMatrixCoefficients::BT_2020_Nonconstant_Luminance)
        && record.matrixCoefficients != static_cast<uint8_t>(AV1ConfigurationMatrixCoefficients::BT_2020_Constant_Luminance)
        && record.matrixCoefficients != static_cast<uint8_t>(AV1ConfigurationMatrixCoefficients::BT_2100_ICC))
        return false;

    return true;
}

std::optional<MediaCapabilitiesInfo> validateAV1Parameters(const AV1CodecConfigurationRecord& record, const VideoConfiguration& configuration)
{
    if (!validateAV1ConfigurationRecord(record))
        return std::nullopt;

    if (!validateAV1PerLevelConstraints(record, configuration))
        return std::nullopt;

    if (!canLoad_VideoToolbox_VTCopyAV1DecoderCapabilitiesDictionary()
        || !canLoad_VideoToolbox_kVTDecoderCodecCapability_SupportedProfiles()
        || !canLoad_VideoToolbox_kVTDecoderCodecCapability_PerProfileSupport()
        || !canLoad_VideoToolbox_kVTDecoderProfileCapability_IsHardwareAccelerated()
        || !canLoad_VideoToolbox_kVTDecoderProfileCapability_MaxDecodeLevel()
        || !canLoad_VideoToolbox_kVTDecoderProfileCapability_MaxPlaybackLevel()
        || !canLoad_VideoToolbox_kVTDecoderCapability_ChromaSubsampling()
        || !canLoad_VideoToolbox_kVTDecoderCapability_ColorDepth())
        return std::nullopt;

    auto capabilities = adoptCF(softLink_VideoToolbox_VTCopyAV1DecoderCapabilitiesDictionary());
    if (!capabilities)
        return std::nullopt;

    auto supportedProfiles = dynamic_cf_cast<CFArrayRef>(CFDictionaryGetValue(capabilities.get(), kVTDecoderCodecCapability_SupportedProfiles));
    if (!supportedProfiles)
        return std::nullopt;

    int16_t profile = static_cast<int16_t>(record.profile);
    auto cfProfile = adoptCF(CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt16Type, &profile));
    auto searchRange = CFRangeMake(0, CFArrayGetCount(supportedProfiles));
    if (!CFArrayContainsValue(supportedProfiles, searchRange, cfProfile.get()))
        return std::nullopt;

    auto perProfileSupport = dynamic_cf_cast<CFDictionaryRef>(CFDictionaryGetValue(capabilities.get(), kVTDecoderCodecCapability_PerProfileSupport));
    if (!perProfileSupport)
        return std::nullopt;

    auto profileString = String::number(profile).createCFString();
    auto profileSupport = dynamic_cf_cast<CFDictionaryRef>(CFDictionaryGetValue(perProfileSupport, profileString.get()));
    if (!profileSupport)
        return std::nullopt;

    MediaCapabilitiesInfo info;

    info.supported = true;

    info.powerEfficient = CFDictionaryGetValue(profileSupport, kVTDecoderProfileCapability_IsHardwareAccelerated) == kCFBooleanTrue;

    if (auto cfMaxDecodeLevel = dynamic_cf_cast<CFNumberRef>(CFDictionaryGetValue(profileSupport, kVTDecoderProfileCapability_MaxDecodeLevel))) {
        int16_t maxDecodeLevel = 0;
        if (!CFNumberGetValue(cfMaxDecodeLevel, kCFNumberSInt16Type, &maxDecodeLevel))
            return std::nullopt;

        if (static_cast<int16_t>(record.level) > maxDecodeLevel)
            return std::nullopt;
    }

    if (auto cfSupportedChromaSubsampling = dynamic_cf_cast<CFArrayRef>(CFDictionaryGetValue(profileSupport, kVTDecoderCapability_ChromaSubsampling))) {
        auto supportedChromaSubsampling = makeVector(cfSupportedChromaSubsampling, [](CFStringRef chromaSubsamplingString) {
            return parseInteger<uint8_t>(String(chromaSubsamplingString));
        });

        // CoreMedia defines the kVTDecoderCapability_ChromaSubsampling value as
        // three decimal digits consisting of, in order from highest digit to lowest:
        // [subsampling_x, subsampling_y, mono_chrome]. This conflicts with AV1's
        // definition of chromaSubsampling in the Codecs Parameter String:
        // "The chromaSubsampling parameter value, represented by a three-digit decimal,
        // SHALL have its first digit equal to subsampling_x and its second digit equal to
        // subsampling_y. If both subsampling_x and subsampling_y are set to 1, then the third
        // digit SHALL be equal to chroma_sample_position, otherwise it SHALL be set to 0."

        // CoreMedia supports all values of chroma_sample_position, so to reconcile this
        // discrepency, construct a "chroma subsampling" query out of the high-order digits
        // of the AV1CodecConfigurationRecord.chromaSubsampling field, and use the
        // AV1CodecConfigurationRecord.monochrome field as the low-order digit.

        uint8_t subsamplingXandY = record.chromaSubsampling - (record.chromaSubsampling % 10);
        uint8_t subsamplingQuery = subsamplingXandY + (record.monochrome ? 1 : 0);

        if (!supportedChromaSubsampling.contains(subsamplingQuery))
            return std::nullopt;
    }

    if (auto cfSupportedColorDepths = dynamic_cf_cast<CFArrayRef>(CFDictionaryGetValue(profileSupport, kVTDecoderCapability_ColorDepth))) {
        auto supportedColorDepths = makeVector(cfSupportedColorDepths, [](CFStringRef colorDepthString) {
            return parseInteger<uint8_t>(String(colorDepthString));
        });
        if (!supportedColorDepths.contains(static_cast<uint8_t>(record.bitDepth)))
            return std::nullopt;
    }

    if (auto cfMaxPlaybackLevel = dynamic_cf_cast<CFNumberRef>(CFDictionaryGetValue(profileSupport, kVTDecoderProfileCapability_MaxPlaybackLevel))) {
        int16_t maxPlaybackLevel = 0;
        if (!CFNumberGetValue(cfMaxPlaybackLevel, kCFNumberSInt16Type, &maxPlaybackLevel))
            return std::nullopt;

        info.smooth = static_cast<int16_t>(record.level) <= maxPlaybackLevel;
    }

    if (canLoad_VideoToolbox_kVTDecoderProfileCapability_MaxHDRPlaybackLevel() && isConfigurationRecordHDR(record)) {
        if (auto cfMaxHDRPlaybackLevel = dynamic_cf_cast<CFNumberRef>(CFDictionaryGetValue(profileSupport, kVTDecoderProfileCapability_MaxHDRPlaybackLevel))) {
            int16_t maxHDRPlaybackLevel = 0;
            if (!CFNumberGetValue(cfMaxHDRPlaybackLevel, kCFNumberSInt16Type, &maxHDRPlaybackLevel))
                return std::nullopt;

            info.smooth = static_cast<int16_t>(record.level) <= maxHDRPlaybackLevel;
        }
    }

    return info;
}

static std::optional<bool> s_av1HardwareDecoderAvailable = { };
void setAV1HardwareDecoderAvailable(bool value)
{
    ASSERT(isMainThread());

    ASSERT(!s_av1HardwareDecoderAvailable || *s_av1HardwareDecoderAvailable == value);
    s_av1HardwareDecoderAvailable = value;
}

bool av1HardwareDecoderAvailable()
{
    ASSERT(isMainThread() || !!s_av1HardwareDecoderAvailable);

    if (!s_av1HardwareDecoderAvailable)
        s_av1HardwareDecoderAvailable = canLoad_VideoToolbox_VTIsHardwareDecodeSupported() && VTIsHardwareDecodeSupported('av01');

    return *s_av1HardwareDecoderAvailable;
}

static size_t readULEBSize(std::span<const uint8_t> data, size_t& index)
{
    size_t value = 0;
    for (size_t cptr = 0; cptr < 8; ++cptr) {
        if (index >= data.size())
            return 0;

        uint8_t dataByte = data[index++];
        uint8_t decodedByte = dataByte & 0x7f;
        value |= decodedByte << (7 * cptr);
        if (value >= std::numeric_limits<uint32_t>::max())
            return 0;
        if (!(dataByte & 0x80))
            break;
    }
    return value;
}

static std::optional<std::pair<std::span<const uint8_t>, std::span<const uint8_t>>> getSequenceHeaderOBU(std::span<const uint8_t> data)
{
    size_t index = 0;
    do {
        if (index >= data.size())
            return { };

        auto startIndex = index;
        auto value = data[index++];
        if (value >> 7)
            return { };
        auto headerType = value >> 3;
        bool hasPayloadSize = value & 0x02;
        if (!hasPayloadSize)
            return { };

        bool hasExtension = value & 0x04;
        if (hasExtension)
            ++index;

        Checked<size_t> payloadSize = readULEBSize(data, index);
        if (index + payloadSize >= data.size())
            return { };

        if (headerType == 1) {
            auto fullObu = data.subspan(startIndex, payloadSize + index - startIndex);
            auto obuData = data.subspan(index, payloadSize);
            return std::make_pair(fullObu, obuData);
        }

        index += payloadSize;
    } while (true);
    return { };
}

RetainPtr<CMVideoFormatDescriptionRef> computeAV1InputFormat(std::span<const uint8_t> data, int32_t width, int32_t height)
{
    auto sequenceHeaderData = getSequenceHeaderOBU(data);
    if (!sequenceHeaderData)
        return { };

    auto record = parseSequenceHeaderOBU(sequenceHeaderData->second);
    if (!record)
        return { };

    if (width && record->width != static_cast<uint32_t>(width))
        return { };
    if (height && record->height != static_cast<uint32_t>(height))
        return { };

    auto fullOBUHeader = sequenceHeaderData->first;

    constexpr size_t VPCodecConfigurationContentsSize = 4;
    size_t cfDataSize = VPCodecConfigurationContentsSize + fullOBUHeader.size();
    auto cfData = adoptCF(CFDataCreateMutable(kCFAllocatorDefault, cfDataSize));
    CFDataIncreaseLength(cfData.get(), cfDataSize);
    auto header = mutableSpan(cfData.get());

    // Build AV1 codec configuration header
    uint8_t high_bitdepth = (record->bitDepth > 8) ? 1 : 0;
    uint8_t twelve_bit = (record->bitDepth == 12) ? 1 : 0;
    uint8_t chroma_type = 3; // Default chroma type

    header[0] = 129; // marker=1, version=1
    header[1] = (static_cast<uint8_t>(record->profile) << 5) | static_cast<uint8_t>(record->level);
    header[2] = (static_cast<uint8_t>(record->tier) << 7) | (high_bitdepth << 6) | (twelve_bit << 5) | (chroma_type << 2);
    header[3] = 0;

    memcpySpan(header.subspan(4), fullOBUHeader);

    auto configurationDict = @{ @"av1C": (__bridge NSData *)cfData.get() };
    auto extensions = @{ (__bridge NSString *)PAL::kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms: configurationDict };

    CMVideoFormatDescriptionRef formatDescription = nullptr;
    // Use kCMVideoCodecType_AV1 once added to CMFormatDescription.h
    if (noErr != PAL::CMVideoFormatDescriptionCreate(kCFAllocatorDefault, 'av01', record->width, record->height, (__bridge CFDictionaryRef)extensions, &formatDescription))
        return { };

    return adoptCF(formatDescription);
}

}

#endif
