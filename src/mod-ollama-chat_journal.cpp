#include "mod-ollama-chat_journal.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat-utilities.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Player.h"
#include <nlohmann/json.hpp>
#include <sstream>
#include <algorithm>

// Global instance - created in OnStartup if g_EnableExtendedMemory.
OllamaBotJournal* g_BotJournal = nullptr;

// -----------------------------------------------------------------------
// Static helpers
// -----------------------------------------------------------------------

std::string OllamaBotJournal::TodayString()
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

std::string OllamaBotJournal::SerialiseJournal(const std::map<std::string, DayJournal>& journal)
{
    nlohmann::json root = nlohmann::json::object();
    for (const auto& [day, dj] : journal)
    {
        nlohmann::json dayObj;
        dayObj["locations"]     = dj.locations;
        dayObj["events"]        = dj.events;
        dayObj["conversations"] = dj.conversations;
        root[day] = dayObj;
    }
    return root.dump();
}

std::map<std::string, DayJournal> OllamaBotJournal::DeserialiseJournal(const std::string& json)
{
    std::map<std::string, DayJournal> result;
    if (json.empty())
        return result;

    try
    {
        nlohmann::json root = nlohmann::json::parse(json);
        if (!root.is_object())
            return result;

        for (auto& [day, dayObj] : root.items())
        {
            DayJournal dj;
            if (dayObj.contains("locations") && dayObj["locations"].is_array())
                dj.locations = dayObj["locations"].get<std::vector<std::string>>();
            if (dayObj.contains("events") && dayObj["events"].is_array())
                dj.events = dayObj["events"].get<std::vector<std::string>>();
            if (dayObj.contains("conversations") && dayObj["conversations"].is_array())
                dj.conversations = dayObj["conversations"].get<std::vector<std::string>>();
            result[day] = std::move(dj);
        }
    }
    catch (const std::exception& e)
    {
        LOG_WARN("server.loading", "[Ollama Journal] DeserialiseJournal parse error: {}", e.what());
    }
    return result;
}

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------

bool OllamaBotJournal::IsExtendedBot(const std::string& botName) const
{
    return g_ExtendedMemoryBotsSet.count(botName) > 0;
}

bool OllamaBotJournal::IsExtendedBotPlayer(Player* bot) const
{
    if (!g_EnableExtendedMemory || !bot)
        return false;
    if (g_ExtendedMemoryBotsSet.count(bot->GetName()) > 0)
        return true;
    if (g_ExtendedMemoryBotGuids.count(bot->GetGUID().GetCounter()) > 0)
        return true;
    if (g_ExtendedMemoryGuildId != 0 && bot->GetGuildId() == g_ExtendedMemoryGuildId)
        return true;
    return false;
}

void OllamaBotJournal::RecordEvent(const std::string& botName, const std::string& text)
{
    if (!g_EnableExtendedMemory)
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto& dj = m_journals[botName][TodayString()];
    if (g_ExtendedMemoryMaxEntriesPerDay > 0 &&
        dj.events.size() >= static_cast<size_t>(g_ExtendedMemoryMaxEntriesPerDay))
        return;
    dj.events.push_back(text);
}

void OllamaBotJournal::RecordLocation(const std::string& botName, const std::string& zone)
{
    if (!g_EnableExtendedMemory || zone.empty())
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto& dj = m_journals[botName][TodayString()];
    if (g_ExtendedMemoryMaxEntriesPerDay > 0 &&
        dj.locations.size() >= static_cast<size_t>(g_ExtendedMemoryMaxEntriesPerDay))
        return;
    // Dedup consecutive identical zones
    if (!dj.locations.empty() && dj.locations.back() == zone)
        return;
    dj.locations.push_back(zone);
}

void OllamaBotJournal::RecordConversation(const std::string& botName, const std::string& text)
{
    if (!g_EnableExtendedMemory)
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto& dj = m_journals[botName][TodayString()];
    if (g_ExtendedMemoryMaxEntriesPerDay > 0 &&
        dj.conversations.size() >= static_cast<size_t>(g_ExtendedMemoryMaxEntriesPerDay))
        return;
    dj.conversations.push_back(text);
}

std::string OllamaBotJournal::GetJournalDigest(const std::string& botName)
{
    if (!g_EnableExtendedMemory)
        return "";

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_journals.find(botName);
    if (it == m_journals.end())
        return "";

    const auto& journal = it->second;
    if (journal.empty())
        return "";

    std::string today     = TodayString();

    // Build yesterday string
    time_t nowT = time(nullptr);
    struct tm tmBuf;
#ifdef _WIN32
    localtime_s(&tmBuf, &nowT);
#else
    localtime_r(&nowT, &tmBuf);
#endif
    tmBuf.tm_mday -= 1;
    mktime(&tmBuf);
    char yestBuf[16];
    snprintf(yestBuf, sizeof(yestBuf), "%04d-%02d-%02d",
             tmBuf.tm_year + 1900, tmBuf.tm_mon + 1, tmBuf.tm_mday);
    std::string yesterday = std::string(yestBuf);

    // Collect at most g_ExtendedMemoryDaysInPrompt days in descending order
    uint32_t daysToShow = g_ExtendedMemoryDaysInPrompt > 0 ? g_ExtendedMemoryDaysInPrompt : 2;

    // journal is std::map<string,DayJournal> - keys are sorted ascending; iterate in reverse
    std::vector<std::string> dayKeys;
    dayKeys.reserve(journal.size());
    for (const auto& kv : journal)
        dayKeys.push_back(kv.first);
    // sort descending
    std::sort(dayKeys.begin(), dayKeys.end(), std::greater<std::string>());

    std::ostringstream oss;
    uint32_t count = 0;
    for (const auto& day : dayKeys)
    {
        if (count >= daysToShow)
            break;
        const DayJournal& dj = journal.at(day);

        bool hasAny = !dj.locations.empty() || !dj.events.empty() || !dj.conversations.empty();
        if (!hasAny)
        {
            ++count;
            continue;
        }

        std::string label;
        if (day == today)
            label = "Сегодня";
        else if (day == yesterday)
            label = "Вчера";
        else
            label = day;

        oss << label << ": ";

        bool needSep = false;

        if (!dj.locations.empty())
        {
            oss << "был в ";
            for (size_t i = 0; i < dj.locations.size(); ++i)
            {
                if (i > 0) oss << ", ";
                oss << dj.locations[i];
            }
            needSep = true;
        }

        if (!dj.events.empty())
        {
            if (needSep) oss << "; ";
            for (size_t i = 0; i < dj.events.size(); ++i)
            {
                if (i > 0) oss << "; ";
                oss << dj.events[i];
            }
            needSep = true;
        }

        if (!dj.conversations.empty())
        {
            if (needSep) oss << "; ";
            for (size_t i = 0; i < dj.conversations.size(); ++i)
            {
                if (i > 0) oss << "; ";
                oss << dj.conversations[i];
            }
        }

        oss << ".\n";
        ++count;
    }

    return oss.str();
}

void OllamaBotJournal::LoadFromDB()
{
    // Guard: table may not exist yet
    QueryResult tableExists = CharacterDatabase.Query(
        "SELECT 1 FROM information_schema.tables "
        "WHERE table_schema = DATABASE() "
        "AND table_name = 'mod_ollama_chat_bot_journal' LIMIT 1");
    if (!tableExists)
    {
        LOG_WARN("server.loading",
                 "[Ollama Journal] mod_ollama_chat_bot_journal table not found - "
                 "extended memory journal disabled until migration is applied.");
        return;
    }

    QueryResult result = CharacterDatabase.Query(
        "SELECT bot_name, journal_json FROM mod_ollama_chat_bot_journal");
    if (!result)
    {
        LOG_INFO("server.loading", "[Ollama Journal] Journal table empty, nothing to load.");
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_journals.clear();

    uint32_t count = 0;
    do
    {
        std::string botName   = (*result)[0].Get<std::string>();
        std::string jsonBlob  = (*result)[1].Get<std::string>();

        m_journals[botName] = DeserialiseJournal(jsonBlob);
        ++count;
    } while (result->NextRow());

    LOG_INFO("server.loading", "[Ollama Journal] Loaded journal for {} bot(s) from DB.", count);
}

// Internal prune — must be called while m_mutex is already held.
void OllamaBotJournal::PruneOldLocked()
{
    if (g_ExtendedMemoryRetentionDays == 0)
        return;

    // Compute cutoff date
    time_t nowT = time(nullptr);
    struct tm tmBuf;
#ifdef _WIN32
    localtime_s(&tmBuf, &nowT);
#else
    localtime_r(&nowT, &tmBuf);
#endif
    tmBuf.tm_mday -= static_cast<int>(g_ExtendedMemoryRetentionDays);
    mktime(&tmBuf);
    char cutBuf[16];
    snprintf(cutBuf, sizeof(cutBuf), "%04d-%02d-%02d",
             tmBuf.tm_year + 1900, tmBuf.tm_mon + 1, tmBuf.tm_mday);
    std::string cutoff = std::string(cutBuf);

    for (auto& [botName, journal] : m_journals)
    {
        for (auto it = journal.begin(); it != journal.end(); )
        {
            if (it->first < cutoff)
                it = journal.erase(it);
            else
                ++it;
        }
    }
}

// Public prune — acquires mutex then delegates to PruneOldLocked.
void OllamaBotJournal::PruneOld()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    PruneOldLocked();
}

void OllamaBotJournal::SaveToDB()
{
    // Guard: table may not exist yet
    QueryResult tableExists = CharacterDatabase.Query(
        "SELECT 1 FROM information_schema.tables "
        "WHERE table_schema = DATABASE() "
        "AND table_name = 'mod_ollama_chat_bot_journal' LIMIT 1");
    if (!tableExists)
    {
        LOG_WARN("server.loading",
                 "[Ollama Journal] mod_ollama_chat_bot_journal table not found - skipping save.");
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    PruneOldLocked(); // prune under the same lock to avoid races

    uint32_t saved = 0;
    for (const auto& [botName, journal] : m_journals)
    {
        std::string jsonBlob = SerialiseJournal(journal);

        std::string escBotName = botName;
        CharacterDatabase.EscapeString(escBotName);
        std::string escJson = jsonBlob;
        CharacterDatabase.EscapeString(escJson);

        CharacterDatabase.Execute(
            SafeFormat(
                "REPLACE INTO mod_ollama_chat_bot_journal "
                "(bot_name, journal_json, updated_at) VALUES ('{}', '{}', NOW())",
                escBotName, escJson));
        ++saved;
    }

    if (g_DebugEnabled)
        LOG_INFO("server.loading", "[Ollama Journal] Saved journal for {} bot(s) to DB.", saved);
}
