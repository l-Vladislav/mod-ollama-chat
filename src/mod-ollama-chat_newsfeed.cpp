#include "mod-ollama-chat_newsfeed.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_httpclient.h"
#include "Log.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <thread>

// Global instance — created in OnStartup when g_EnableNewsFeed is true.
OllamaNewsFeedManager* g_NewsFeedManager = nullptr;

// -----------------------------------------------------------------------
// Internal helpers: XML/RSS parsing
// -----------------------------------------------------------------------

// Convert a single decimal or hex numeric HTML entity (&#NNN; or &#xHH;) to
// the corresponding UTF-8 sequence.  Only handles the common ASCII range here
// to keep the implementation dependency-free; code points > 127 are left as-is
// (they pass through unchanged because the source RSS is already UTF-8).
static std::string DecodeNumericEntity(const std::string& entity)
{
    // entity arrives WITHOUT leading '&' and trailing ';', e.g. "#160" or "#xA0"
    if (entity.empty() || entity[0] != '#')
        return "&" + entity + ";";

    long codepoint = 0;
    if (entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X'))
    {
        try { codepoint = std::stol(entity.substr(2), nullptr, 16); }
        catch (...) { return "&" + entity + ";"; }
    }
    else
    {
        try { codepoint = std::stol(entity.substr(1)); }
        catch (...) { return "&" + entity + ";"; }
    }

    if (codepoint <= 0x7F)
    {
        return std::string(1, static_cast<char>(codepoint));
    }
    else if (codepoint <= 0x7FF)
    {
        char buf[3];
        buf[0] = static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F));
        buf[1] = static_cast<char>(0x80 | (codepoint & 0x3F));
        buf[2] = '\0';
        return std::string(buf, 2);
    }
    else if (codepoint <= 0xFFFF)
    {
        char buf[4];
        buf[0] = static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F));
        buf[1] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        buf[2] = static_cast<char>(0x80 | (codepoint & 0x3F));
        buf[3] = '\0';
        return std::string(buf, 3);
    }
    // Surrogate or out-of-range — leave as-is
    return "&" + entity + ";";
}

// Decode named and numeric HTML entities in place.
static std::string DecodeHtmlEntities(const std::string& input)
{
    std::string result;
    result.reserve(input.size());

    size_t i = 0;
    while (i < input.size())
    {
        if (input[i] != '&')
        {
            result += input[i++];
            continue;
        }

        // Look for closing ';'
        size_t semicolon = input.find(';', i + 1);
        if (semicolon == std::string::npos || semicolon - i > 10)
        {
            // Not a valid entity — keep literal '&'
            result += input[i++];
            continue;
        }

        std::string entity = input.substr(i + 1, semicolon - i - 1);

        // Named entities
        if (entity == "amp")       { result += '&'; }
        else if (entity == "lt")   { result += '<'; }
        else if (entity == "gt")   { result += '>'; }
        else if (entity == "quot") { result += '"'; }
        else if (entity == "apos" || entity == "#39") { result += '\''; }
        else if (entity == "nbsp") { result += ' '; }
        else if (!entity.empty() && entity[0] == '#')
        {
            result += DecodeNumericEntity(entity);
        }
        else
        {
            // Unknown named entity — keep as-is
            result += '&';
            result += entity;
            result += ';';
        }

        i = semicolon + 1;
    }
    return result;
}

// Strip CDATA wrapper if present: <![CDATA[...]]> -> ...
static std::string StripCDATA(const std::string& raw)
{
    const std::string cdataOpen  = "<![CDATA[";
    const std::string cdataClose = "]]>";

    size_t start = raw.find(cdataOpen);
    if (start == std::string::npos)
        return raw;

    start += cdataOpen.size();
    size_t end = raw.find(cdataClose, start);
    if (end == std::string::npos)
        return raw.substr(start);

    return raw.substr(start, end - start);
}

// Trim leading/trailing ASCII whitespace.
static std::string TrimWhitespace(const std::string& s)
{
    size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

// Lowercase a UTF-8 string (ASCII range only — sufficient for keyword matching).
static std::string ToLowerASCII(const std::string& s)
{
    std::string result = s;
    for (char& c : result)
    {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + ('a' - 'A'));
    }
    return result;
}

// Parse all <title>...</title> blocks from an RSS or Atom XML string.
// The first title block is the feed/channel title — it is skipped.
// Returns decoded, trimmed headline strings.
static std::vector<std::string> ParseTitlesFromXML(const std::string& xml)
{
    std::vector<std::string> headlines;

    // We look for occurrences of <title ...> ... </title>  (case-insensitive "title")
    // Using a simple state machine — no external XML library required.
    size_t pos = 0;
    bool   firstSkipped = false;

    while (pos < xml.size())
    {
        // Find opening tag — must be <title or <title (with attributes for Atom)
        size_t tagStart = xml.find('<', pos);
        if (tagStart == std::string::npos) break;

        // Read tag name
        size_t nameStart = tagStart + 1;
        // Skip '/' for closing tags
        if (nameStart < xml.size() && xml[nameStart] == '/')
        {
            pos = tagStart + 1;
            continue;
        }

        // Extract tag name (up to whitespace, '>', or '/')
        size_t nameEnd = nameStart;
        while (nameEnd < xml.size() && xml[nameEnd] != '>' &&
               xml[nameEnd] != '/' && xml[nameEnd] != ' ' &&
               xml[nameEnd] != '\t' && xml[nameEnd] != '\r' &&
               xml[nameEnd] != '\n')
        {
            ++nameEnd;
        }

        std::string tagName = ToLowerASCII(xml.substr(nameStart, nameEnd - nameStart));
        if (tagName != "title")
        {
            pos = tagStart + 1;
            continue;
        }

        // Find the end of the opening tag '>'
        size_t tagClose = xml.find('>', tagStart);
        if (tagClose == std::string::npos) break;

        // Self-closing tag <title/> — skip
        if (tagClose > tagStart && xml[tagClose - 1] == '/')
        {
            pos = tagClose + 1;
            continue;
        }

        size_t contentStart = tagClose + 1;

        // Find closing </title> (case-insensitive linear scan)
        size_t closingTag = std::string::npos;
        {
            const std::string closeTarget = "</title>";
            // Case-insensitive search
            size_t searchPos = contentStart;
            while (searchPos + closeTarget.size() <= xml.size())
            {
                bool match = true;
                for (size_t k = 0; k < closeTarget.size(); ++k)
                {
                    char a = xml[searchPos + k];
                    char b = closeTarget[k];
                    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + 32);
                    if (a != b) { match = false; break; }
                }
                if (match) { closingTag = searchPos; break; }
                ++searchPos;
            }
        }

        if (closingTag == std::string::npos) break;

        std::string rawContent = xml.substr(contentStart, closingTag - contentStart);
        pos = closingTag + 8; // len("</title>") == 8

        // Process content
        std::string content = TrimWhitespace(StripCDATA(TrimWhitespace(rawContent)));
        content = TrimWhitespace(DecodeHtmlEntities(content));

        if (content.empty()) continue;

        // Skip the first title (channel/feed title)
        if (!firstSkipped)
        {
            firstSkipped = true;
            continue;
        }

        headlines.push_back(content);
    }

    return headlines;
}

// -----------------------------------------------------------------------
// IsHeadlineBlocked
// -----------------------------------------------------------------------

bool IsHeadlineBlocked(const std::string& headline)
{
    if (g_NewsFeedBlockedKeywords.empty()) return false;

    std::string lower = ToLowerASCII(headline);
    for (const auto& keyword : g_NewsFeedBlockedKeywords)
    {
        if (!keyword.empty() && lower.find(keyword) != std::string::npos)
            return true;
    }
    return false;
}

// -----------------------------------------------------------------------
// OllamaNewsFeedManager
// -----------------------------------------------------------------------

// Returns today's date as "YYYY-MM-DD".
std::string OllamaNewsFeedManager::TodayString()
{
    time_t now = time(nullptr);
    struct tm tmBuf;
#ifdef _WIN32
    localtime_s(&tmBuf, &now);
#else
    localtime_r(&now, &tmBuf);
#endif
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
             tmBuf.tm_year + 1900, tmBuf.tm_mon + 1, tmBuf.tm_mday);
    return std::string(buf);
}

bool OllamaNewsFeedManager::IsStale() const
{
    std::string today = TodayString();

    std::lock_guard<std::mutex> lock(m_mutex);

    // New calendar day — always refresh
    if (m_currentDay != today)
        return true;

    // Same day but daily topics empty — retry after RefreshInterval minutes
    if (m_dailyTopics.empty())
    {
        time_t last = m_lastFetched.load();
        if (last == 0) return true;
        time_t now = time(nullptr);
        return difftime(now, last) >= static_cast<double>(g_NewsFeedRefreshInterval) * 60.0;
    }

    // Topics are set for today — not stale
    return false;
}

std::string OllamaNewsFeedManager::GetRandomHeadline()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_dailyTopics.empty()) return "";

    // Simple uniform random using rand() — sufficient, no crypto needed
    size_t idx = static_cast<size_t>(rand()) % m_dailyTopics.size();
    return m_dailyTopics[idx];
}

void OllamaNewsFeedManager::FetchNewsInternal()
{
    std::vector<NewsItem> fetched;
    std::string today = TodayString();

    try
    {
        OllamaHttpClient client;
        // RSS/Atom feeds are typically small; 30 s is enough
        client.SetTimeout(30);

        std::string xmlBody = client.Get(g_NewsFeedUrl);

        if (xmlBody.empty())
        {
            LOG_WARN("server.loading", "[Ollama Chat NewsFeed] Empty response from {}", g_NewsFeedUrl);
        }
        else
        {
            std::vector<std::string> headlines = ParseTitlesFromXML(xmlBody);

            // Truncate to configured max (pool from which we pick daily topics)
            if (g_NewsFeedMaxItems > 0 && headlines.size() > g_NewsFeedMaxItems)
                headlines.resize(g_NewsFeedMaxItems);

            for (const auto& h : headlines)
                fetched.push_back({ h });

            LOG_INFO("server.loading", "[Ollama Chat NewsFeed] Fetched {} headlines from {}",
                     fetched.size(), g_NewsFeedUrl);
        }
    }
    catch (const std::exception& ex)
    {
        LOG_ERROR("server.loading", "[Ollama Chat NewsFeed] Exception during fetch: {}", ex.what());
        // Do NOT clear existing daily topics on error — keep stale data.
        m_lastFetched.store(time(nullptr));
        m_fetchInProgress.store(false);
        return;
    }
    catch (...)
    {
        LOG_ERROR("server.loading", "[Ollama Chat NewsFeed] Unknown exception during fetch.");
        m_lastFetched.store(time(nullptr));
        m_fetchInProgress.store(false);
        return;
    }

    // Update cache + select daily topics only when we got something
    if (!fetched.empty())
    {
        // Filter blocked headlines
        std::vector<std::string> pool;
        pool.reserve(fetched.size());
        for (const auto& item : fetched)
        {
            if (!IsHeadlineBlocked(item.headline))
                pool.push_back(item.headline);
        }

        // Randomly pick g_NewsFeedDailyTopicCount unique topics
        uint32_t topicCount = g_NewsFeedDailyTopicCount > 0 ? g_NewsFeedDailyTopicCount : 5;
        topicCount = static_cast<uint32_t>(std::min(static_cast<size_t>(topicCount), pool.size()));

        // Fisher-Yates partial shuffle to select topicCount items
        for (uint32_t i = 0; i < topicCount; ++i)
        {
            size_t j = i + (static_cast<size_t>(rand()) % (pool.size() - i));
            std::swap(pool[i], pool[j]);
        }
        pool.resize(topicCount);

        std::lock_guard<std::mutex> lock(m_mutex);
        m_cache = std::move(fetched);
        m_dailyTopics = std::move(pool);
        m_currentDay  = today;

        LOG_INFO("server.loading", "[Ollama Chat NewsFeed] Selected {} daily topics for {}",
                 m_dailyTopics.size(), today);

        if (g_DebugEnabled)
        {
            for (size_t i = 0; i < m_dailyTopics.size(); ++i)
                LOG_INFO("server.loading", "[Ollama Chat NewsFeed] Topic[{}]: {}", i, m_dailyTopics[i]);
        }
    }

    m_lastFetched.store(time(nullptr));
    m_fetchInProgress.store(false);
}

void OllamaNewsFeedManager::FetchNewsAsync()
{
    // Guard: skip if a fetch is already in flight
    bool expected = false;
    if (!m_fetchInProgress.compare_exchange_strong(expected, true))
    {
        if (g_DebugEnabled)
            LOG_INFO("server.loading", "[Ollama Chat NewsFeed] Fetch already in progress, skipping.");
        return;
    }

    std::thread([this]() {
        FetchNewsInternal();
    }).detach();
}
