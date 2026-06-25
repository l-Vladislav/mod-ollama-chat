#ifndef MOD_OLLAMA_CHAT_NEWSFEED_H
#define MOD_OLLAMA_CHAT_NEWSFEED_H

#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <ctime>

struct NewsItem
{
    std::string headline;
};

class OllamaNewsFeedManager
{
public:
    OllamaNewsFeedManager() = default;

    // Starts a detached thread to fetch headlines from g_NewsFeedUrl.
    // Returns immediately; guards against concurrent fetches via m_fetchInProgress.
    void FetchNewsAsync();

    // Returns a random headline from the daily topics (up to g_NewsFeedDailyTopicCount).
    // Returns "" if no daily topics are selected yet.
    std::string GetRandomHeadline();

    // Returns true when the feed needs to be refreshed:
    //   - New calendar day has started (m_currentDay != today), OR
    //   - Daily topics are empty AND at least g_NewsFeedRefreshInterval minutes have
    //     passed since the last fetch attempt (retry guard).
    bool IsStale() const;

private:
    void FetchNewsInternal();

    // Returns the current date as "YYYY-MM-DD".
    static std::string TodayString();

    // Full parsed-headline buffer (up to g_NewsFeedMaxItems).
    // Used only as an intermediate buffer inside FetchNewsInternal.
    std::vector<NewsItem> m_cache;

    // The ~5 randomly selected "topics of the day".
    std::vector<std::string> m_dailyTopics;

    // The calendar date for which m_dailyTopics was selected ("YYYY-MM-DD").
    std::string m_currentDay;

    mutable std::mutex    m_mutex;
    std::atomic<time_t>   m_lastFetched{0};
    std::atomic<bool>     m_fetchInProgress{false};
};

// Helper: returns true when any g_NewsFeedBlockedKeywords token appears (case-insensitive)
// in the given headline.
bool IsHeadlineBlocked(const std::string& headline);

extern OllamaNewsFeedManager* g_NewsFeedManager;

#endif // MOD_OLLAMA_CHAT_NEWSFEED_H
