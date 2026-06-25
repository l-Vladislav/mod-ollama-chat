#ifndef MOD_OLLAMA_CHAT_JOURNAL_H
#define MOD_OLLAMA_CHAT_JOURNAL_H

#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <mutex>
#include <ctime>

class Player;

// ------------------------------------------------------------
// Per-day autobiographical journal for named/extended bots.
// ------------------------------------------------------------

struct DayJournal
{
    std::vector<std::string> locations;      // deduped, last-added-first-added order
    std::vector<std::string> events;
    std::vector<std::string> conversations;
};

class OllamaBotJournal
{
public:
    OllamaBotJournal() = default;

    // Returns true if botName is in the extended-memory set.
    bool IsExtendedBot(const std::string& botName) const;

    // Returns true if bot qualifies for extended memory via named-list OR guild membership.
    // Preferred gate at all call sites that have a Player* available.
    bool IsExtendedBotPlayer(Player* bot) const;

    // Record an event ("убил Hogger", "получил Меч X", etc.)
    void RecordEvent(const std::string& botName, const std::string& text);

    // Record current zone; deduplicates consecutive identical zones within the same day.
    void RecordLocation(const std::string& botName, const std::string& zone);

    // Record a conversation snippet.
    void RecordConversation(const std::string& botName, const std::string& text);

    // Build a human-readable digest of the last g_ExtendedMemoryDaysInPrompt days.
    // Today -> "Сегодня", yesterday -> "Вчера", older -> "YYYY-MM-DD".
    // Returns "" if no data.
    std::string GetJournalDigest(const std::string& botName);

    // Load from mod_ollama_chat_bot_journal (acore_characters).
    // Safe: guards on table existence via information_schema.
    void LoadFromDB();

    // Serialise all bots to DB. Calls PruneOld first.
    void SaveToDB();

    // Remove days older than g_ExtendedMemoryRetentionDays (acquires mutex).
    void PruneOld();

private:
    // Same as PruneOld() but called when m_mutex is already held.
    void PruneOldLocked();
    // Returns current date as "YYYY-MM-DD" string.
    static std::string TodayString();

    // Serialise one bot's journal map to JSON string.
    static std::string SerialiseJournal(const std::map<std::string, DayJournal>& journal);

    // Deserialise JSON string into journal map.
    static std::map<std::string, DayJournal> DeserialiseJournal(const std::string& json);

    // Inner store: botName -> (day "YYYY-MM-DD" -> DayJournal)
    std::unordered_map<std::string, std::map<std::string, DayJournal>> m_journals;
    mutable std::mutex m_mutex;
};

extern OllamaBotJournal* g_BotJournal;

#endif // MOD_OLLAMA_CHAT_JOURNAL_H
