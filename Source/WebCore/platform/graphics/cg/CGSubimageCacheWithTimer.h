/*
 * Copyright (C) 2012-2023 Apple Inc. All rights reserved.
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

#pragma once

#include "FloatRect.h"
#include "Timer.h"

#include <CoreGraphics/CoreGraphics.h>
#include <wtf/CheckedPtr.h>
#include <wtf/HashCountedSet.h>
#include <wtf/HashSet.h>
#include <wtf/HashTraits.h>
#include <wtf/Lock.h>
#include <wtf/RetainPtr.h>
#include <wtf/RunLoop.h>
#include <wtf/TZoneMalloc.h>

#define CACHE_SUBIMAGES 1

namespace WebCore {

#if CACHE_SUBIMAGES

class CGSubimageCacheWithTimer final : public CanMakeThreadSafeCheckedPtr<CGSubimageCacheWithTimer> {
    WTF_MAKE_TZONE_ALLOCATED(CGSubimageCacheWithTimer);
    WTF_MAKE_NONCOPYABLE(CGSubimageCacheWithTimer);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(CGSubimageCacheWithTimer);
public:
    struct CacheEntry {
        RetainPtr<CGImageRef> image;
        RetainPtr<CGImageRef> subimage;
        FloatRect rect;
        MonotonicTime lastAccessTime;
    };

    struct CacheEntryTraits : HashTraits<CacheEntry> {
        typedef HashTraits<RetainPtr<CGImageRef>> ImageTraits;

        static const bool emptyValueIsZero = true;

        static const bool hasIsEmptyValueFunction = true;
        static bool isEmptyValue(const CacheEntry& value) { return !value.image; }

        static void constructDeletedValue(CacheEntry& slot) { ImageTraits::constructDeletedValue(slot.image); }
        static bool isDeletedValue(const CacheEntry& value) { return ImageTraits::isDeletedValue(value.image); }
    };

    struct CacheHash {
        static unsigned hash(CGImageRef image, const FloatRect& rect)
        {
            return pairIntHash(PtrHash<CGImageRef>::hash(image),
                (static_cast<unsigned>(rect.x()) << 16) | static_cast<unsigned>(rect.y()));
        }
        static unsigned hash(const CacheEntry& key)
        {
            return hash(key.image.get(), key.rect);
        }
        static bool equal(const CacheEntry& a, const CacheEntry& b)
        {
            return a.image == b.image && a.rect == b.rect;
        }
        static const bool safeToCompareToEmptyOrDeleted = true;
    };

    static RetainPtr<CGImageRef> getSubimage(CGImageRef, const FloatRect&);
    static void clearImage(CGImageRef);
    static void clear();

private:
    static constexpr Seconds cachePruneDelay { 500_ms };
    static constexpr Seconds cacheEntryLifetime { 500_ms };
    static constexpr int maxCacheSize = 300;

    using CacheHashSet = HashSet<CacheEntry, CacheHash, CacheEntryTraits>;

    CGSubimageCacheWithTimer();
    void pruneCacheTimerFired();

    RetainPtr<CGImageRef> subimage(CGImageRef, const FloatRect&);
    void clearImageAndSubimages(CGImageRef);
    void prune() WTF_REQUIRES_LOCK(m_lock);
    void clearAll();

    Lock m_lock;
    HashCountedSet<CGImageRef> m_imageCounts WTF_GUARDED_BY_LOCK(m_lock);
    CacheHashSet m_cache WTF_GUARDED_BY_LOCK(m_lock);
    RunLoop::Timer m_timer WTF_GUARDED_BY_LOCK(m_lock);

    static CGSubimageCacheWithTimer& subimageCache();
    static bool subimageCacheExists();
    static CGSubimageCacheWithTimer* s_cache;
};

#endif // CACHE_SUBIMAGES

}
