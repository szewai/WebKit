/*
 * Copyright (C) 1999 Lars Knoll (knoll@kde.org)
 * Copyright (C) 2003-2025 Apple Inc. All rights reserved.
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

#include "MatchResult.h"
#include "StyleScopeRuleSets.h"
#include <wtf/Forward.h>

namespace WebCore {

class StyleRulePage;

namespace Style {

class PageRuleCollector {
public:
    PageRuleCollector(ScopeRuleSets& ruleSets, WritingMode rootWritingMode)
        : m_ruleSets(ruleSets)
        , m_rootWritingMode(rootWritingMode)
        , m_result(MatchResult::create())
    { }

    void matchAllPageRules(int pageIndex);
    const MatchResult& matchResult() const { return m_result; }

private:
    bool isLeftPage(int pageIndex) const;
    bool isRightPage(int pageIndex) const { return !isLeftPage(pageIndex); }
    bool isFirstPage(int pageIndex) const;
    String pageName(int pageIndex) const;

    void matchPageRules(RuleSet* rules, bool isLeftPage, bool isFirstPage, const String& pageName);
    void matchPageRulesForList(Vector<StyleRulePage*>& matchedRules, const Vector<StyleRulePage*>& rules, bool isLeftPage, bool isFirstPage, const String& pageName);

    ScopeRuleSets& m_ruleSets;
    WritingMode m_rootWritingMode;

    const Ref<MatchResult> m_result;
};

} // namespace Style
} // namespace WebCore
