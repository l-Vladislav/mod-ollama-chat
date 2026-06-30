// mod-ollama-chat_tools.cpp
// Tool-calling (function-calling) support for the Ollama Chat module.
//
// Threading model:
//   SnapshotBotSelf()           - must run on the world-thread
//   GetToolRegistry()           - reads g_* config globals only, thread-safe
//   QueryOllamaChatWithTools()  - background-thread only, never touches Player*
//
// The file is picked up automatically by AzerothCore's CollectSourceFiles()
// glob, which recursively collects all *.cpp in the module source directory.

#include "mod-ollama-chat_tools.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_httpclient.h"
#include "mod-ollama-chat-utilities.h"
#include "Log.h"

// Core game headers (needed by SnapshotBotSelf on the world-thread)
#include "Common.h"
#include "Player.h"
#include "Item.h"
#include "Bag.h"
#include "ItemTemplate.h"
#include "ObjectMgr.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "Group.h"

#include <sstream>
#include <regex>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <fmt/core.h>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static std::string GetAreaNameRU(uint32_t areaId)
{
    if (areaId == 0)
        return "Unknown";
    AreaTableEntry const* entry = sAreaTableStore.LookupEntry(areaId);
    if (!entry)
        return "Unknown";
    // Prefer ruRU (locale index 8), fall back to enUS (index 0)
    std::string name;
    if (entry->area_name[LOCALE_ruRU] && entry->area_name[LOCALE_ruRU][0] != '\0')
        name = entry->area_name[LOCALE_ruRU];
    else if (entry->area_name[LOCALE_enUS] && entry->area_name[LOCALE_enUS][0] != '\0')
        name = entry->area_name[LOCALE_enUS];
    else
        name = "Unknown";
    return name;
}

// Resolve a ruRU item name from the ObjectMgr locale cache, falling back to
// the item template's Name1 field.
static std::string GetItemNameRU(uint32_t itemId, const std::string& fallback)
{
    if (itemId == 0)
        return fallback;
    if (ItemLocale const* il = sObjectMgr->GetItemLocale(itemId))
    {
        if (il->Name.size() > LOCALE_ruRU && !il->Name[LOCALE_ruRU].empty())
            return il->Name[LOCALE_ruRU];
    }
    return fallback;
}

// Simple URL-encode: replace spaces with '+', encode common special chars.
static std::string UrlEncode(const std::string& s)
{
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s)
    {
        if (c == ' ')
        {
            out += '+';
        }
        else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                 c == '.' || c == '~')
        {
            out += static_cast<char>(c);
        }
        else
        {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// Strip HTML tags and collapse whitespace.  Best-effort, not a full parser.
static std::string StripHtml(const std::string& html)
{
    // Remove <script>...</script> and <style>...</style> blocks first
    std::string s = html;
    {
        std::regex scriptRe("<(script|style)[^>]*>[\\s\\S]*?</(script|style)>",
                            std::regex_constants::icase);
        s = std::regex_replace(s, scriptRe, " ");
    }
    // Remove all remaining tags
    {
        std::regex tagRe("<[^>]+>");
        s = std::regex_replace(s, tagRe, " ");
    }
    // Decode common HTML entities
    {
        std::pair<const char*, const char*> entities[] = {
            {"&amp;",  "&"}, {"&lt;",   "<"}, {"&gt;",   ">"},
            {"&nbsp;", " "}, {"&quot;", "\""}, {"&#39;", "'"},
        };
        for (auto& e : entities)
        {
            size_t pos = 0;
            while ((pos = s.find(e.first, pos)) != std::string::npos)
            {
                s.replace(pos, strlen(e.first), e.second);
                pos += strlen(e.second);
            }
        }
    }
    // Collapse whitespace
    std::string result;
    result.reserve(s.size());
    bool lastSpace = true;
    for (char c : s)
    {
        if (c == '\r' || c == '\n' || c == '\t')
            c = ' ';
        if (c == ' ')
        {
            if (!lastSpace)
                result += ' ';
            lastSpace = true;
        }
        else
        {
            result += c;
            lastSpace = false;
        }
    }
    // Trim
    size_t start = result.find_first_not_of(' ');
    if (start == std::string::npos) return "";
    size_t end = result.find_last_not_of(' ');
    return result.substr(start, end - start + 1);
}

// Truncate a UTF-8 string to at most maxChars bytes, not cutting inside a
// multi-byte sequence.
static std::string TruncateUTF8(const std::string& s, uint32_t maxChars)
{
    if (maxChars == 0 || s.size() <= maxChars)
        return s;
    size_t i = maxChars;
    // Walk back to the start of a multi-byte sequence
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80)
        --i;
    return s.substr(0, i) + "...";
}

// ---------------------------------------------------------------------------
// SnapshotBotSelf — must be called on the world-thread
// ---------------------------------------------------------------------------
BotSelfSnapshot SnapshotBotSelf(Player* bot)
{
    BotSelfSnapshot snap;
    if (!bot)
        return snap;

    // Identity
    snap.name      = bot->GetName();
    snap.level     = bot->GetLevel();

    // Class name: use same lookup as GenerateBotPrompt via FormatPlayerClass
    static const char* classNames[] = {
        "", "Warrior", "Paladin", "Hunter", "Rogue", "Priest",
        "Death Knight", "Shaman", "Mage", "Warlock", "", "Druid"
    };
    uint8_t cls = bot->getClass();
    snap.className = (cls < 12 && classNames[cls][0]) ? classNames[cls] : "Unknown";

    // Race name
    static const char* raceNames[] = {
        "", "Human", "Orc", "Dwarf", "Night Elf", "Undead",
        "Tauren", "Gnome", "Troll", "", "Blood Elf", "Draenei"
    };
    uint8_t race = bot->getRace();
    snap.raceName = (race < 12 && raceNames[race][0]) ? raceNames[race] : "Unknown";

    snap.factionName = (bot->GetTeamId() == TEAM_ALLIANCE) ? "Alliance" : "Horde";

    // Location
    snap.mapId   = bot->GetMapId();
    snap.posX    = bot->GetPositionX();
    snap.posY    = bot->GetPositionY();
    snap.posZ    = bot->GetPositionZ();
    snap.zoneName = GetAreaNameRU(bot->GetZoneId());
    snap.areaName = GetAreaNameRU(bot->GetAreaId());

    // Vitals
    uint32_t maxHp  = bot->GetMaxHealth();
    snap.healthPct  = maxHp > 0 ? (bot->GetHealth() * 100 / maxHp) : 0;
    uint32_t maxMana = bot->GetMaxPower(POWER_MANA);
    snap.manaPct     = maxMana > 0 ? (bot->GetPower(POWER_MANA) * 100 / maxMana) : 0;

    // Currency
    uint32_t copper = bot->GetMoney();
    snap.moneyG = copper / 10000;
    snap.moneyC = copper % 100;
    snap.moneyS = (copper % 10000) / 100;

    // Target
    if (Unit* target = bot->GetSelectedUnit())
        snap.targetName = target->GetName();

    snap.inCombat  = bot->IsInCombat();
    snap.groupSize = bot->GetGroup() ? bot->GetGroup()->GetMembersCount() : 0;

    // Equipment (EQUIPMENT_SLOT_START..EQUIPMENT_SLOT_END)
    static const char* slotNames[] = {
        "Head", "Neck", "Shoulders", "Body", "Chest",
        "Waist", "Legs", "Feet", "Wrists", "Hands",
        "Finger1", "Finger2", "Trinket1", "Trinket2",
        "Back", "MainHand", "OffHand", "Ranged", "Tabard"
    };
    for (uint8_t slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item) continue;
        uint32_t itemId = item->GetEntry();
        std::string itemName = GetItemNameRU(itemId, item->GetTemplate()->Name1);
        uint8_t idx = slot - EQUIPMENT_SLOT_START;
        std::string slotName = (idx < 19) ? slotNames[idx] : "Slot";
        snap.equipped.push_back(slotName + ": " + itemName);
    }

    // Bag items
    const uint32_t kItemCap = 40;
    uint32_t itemCount = 0;
    uint32_t freeSlots = 0;

    // Backpack (bag 0)
    for (uint8_t slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
    {
        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
        {
            ++freeSlots;
            continue;
        }
        if (itemCount < kItemCap)
        {
            uint32_t id   = item->GetEntry();
            std::string nm = GetItemNameRU(id, item->GetTemplate()->Name1);
            snap.bagItems.push_back({nm, item->GetCount()});
            ++itemCount;
        }
    }

    // Equipped bags (bag slots 1-4)
    for (uint8_t bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag* bag = bot->GetBagByPos(bagSlot);
        if (!bag) continue;
        uint32_t bagSize = bag->GetBagSize();
        for (uint8_t slot2 = 0; slot2 < static_cast<uint8_t>(bagSize); ++slot2)
        {
            Item* item = bag->GetItemByPos(slot2);
            if (!item)
            {
                ++freeSlots;
                continue;
            }
            if (itemCount < kItemCap)
            {
                uint32_t id    = item->GetEntry();
                std::string nm = GetItemNameRU(id, item->GetTemplate()->Name1);
                snap.bagItems.push_back({nm, item->GetCount()});
                ++itemCount;
            }
        }
    }
    snap.freeBagSlots = freeSlots;

    return snap;
}

// ---------------------------------------------------------------------------
// Tool handler: get_self_status
// ---------------------------------------------------------------------------
static std::string HandleGetSelfStatus(const json& /*args*/, const BotSelfSnapshot& snap)
{
    std::ostringstream out;
    out << "[Статус " << snap.name << "]\n";
    out << "Уровень: " << snap.level
        << ", Класс: " << snap.className
        << ", Раса: " << snap.raceName
        << ", Фракция: " << snap.factionName << "\n";
    out << "Зона: " << snap.zoneName
        << " / Подзона: " << snap.areaName
        << " (карта " << snap.mapId
        << ", X=" << fmt::format("{:.1f}", snap.posX)
        << " Y=" << fmt::format("{:.1f}", snap.posY)
        << " Z=" << fmt::format("{:.1f}", snap.posZ) << ")\n";
    out << "Здоровье: " << snap.healthPct << "%"
        << ", Мана: " << snap.manaPct << "%\n";
    out << "Золото: " << snap.moneyG << "g "
        << snap.moneyS << "s "
        << snap.moneyC << "c\n";
    if (!snap.targetName.empty())
        out << "Цель: " << snap.targetName << "\n";
    out << "В бою: " << (snap.inCombat ? "да" : "нет") << "\n";
    out << "Группа: " << (snap.groupSize > 0
        ? (std::to_string(snap.groupSize) + " чел.") : "нет") << "\n";
    return out.str();
}

// ---------------------------------------------------------------------------
// Tool handler: get_inventory
// ---------------------------------------------------------------------------
static std::string HandleGetInventory(const json& /*args*/, const BotSelfSnapshot& snap)
{
    uint32_t maxChars = g_ToolResultMaxChars > 0 ? g_ToolResultMaxChars : 800;

    std::ostringstream out;
    out << "[Экипировка " << snap.name << "]\n";
    if (snap.equipped.empty())
    {
        out << "(ничего не одето)\n";
    }
    else
    {
        for (const auto& e : snap.equipped)
            out << e << "\n";
    }

    out << "\n[Инвентарь]\n";
    if (snap.bagItems.empty())
    {
        out << "(сумки пусты)\n";
    }
    else
    {
        for (const auto& bi : snap.bagItems)
            out << bi.name << " x" << bi.count << "\n";
    }
    out << "Свободных слотов: " << snap.freeBagSlots << "\n";

    return TruncateUTF8(out.str(), maxChars);
}

// ---------------------------------------------------------------------------
// Tool handler: wowhead_lookup
// ---------------------------------------------------------------------------
static std::string HandleWowheadLookup(const json& args, const BotSelfSnapshot& /*snap*/)
{
    uint32_t maxChars = g_ToolResultMaxChars > 0 ? g_ToolResultMaxChars : 800;

    std::string query;
    try
    {
        if (args.contains("query") && args["query"].is_string())
            query = args["query"].get<std::string>();
    }
    catch (...) {}

    if (query.empty())
        return "По WoWHead ничего не нашлось (пустой запрос).";

    std::string url = g_WowheadSearchUrl + UrlEncode(query);

    if (g_DebugEnabled)
        LOG_INFO("server.loading", "[OllamaChat/Tools] wowhead_lookup: GET {}", url);

    static OllamaHttpClient httpClient;
    std::string html;
    try
    {
        html = httpClient.Get(url);
    }
    catch (const std::exception& ex)
    {
        LOG_WARN("server.loading", "[OllamaChat/Tools] wowhead_lookup HTTP error: {}", ex.what());
        return "По WoWHead ничего не нашлось (сайт недоступен).";
    }

    if (html.empty())
        return "По WoWHead ничего не нашлось (сайт не ответил).";

    // Wowhead renders search results from JSON blobs inside <script> tags, so
    // StripHtml would discard them. Extract the "name":"..." entries directly.
    std::vector<std::string> names;
    {
        const std::string key = "\"name\":\"";
        size_t p = 0;
        while (names.size() < 12 && (p = html.find(key, p)) != std::string::npos)
        {
            p += key.size();
            std::string val;
            while (p < html.size() && html[p] != '"')
            {
                if (html[p] == '\\' && p + 1 < html.size()) { val.push_back(html[p + 1]); p += 2; }
                else { val.push_back(html[p]); ++p; }
            }
            if (p < html.size()) ++p;
            if (val.empty() || val.size() > 80)
                continue;
            bool dup = false;
            for (const auto& n : names)
                if (n == val) { dup = true; break; }
            if (!dup)
                names.push_back(val);
        }
    }

    if (!names.empty())
    {
        std::string out = "WoWHead, результаты по '" + query + "': ";
        for (size_t i = 0; i < names.size(); ++i)
        {
            if (i) out += "; ";
            out += names[i];
        }
        return TruncateUTF8(out, maxChars);
    }

    // Fallback: strip tags (best-effort) if no JSON result names were found.
    std::string text = StripHtml(html);
    if (text.empty())
        return "По WoWHead ничего не нашлось (пустой ответ).";
    return TruncateUTF8(text, maxChars);
}

// ---------------------------------------------------------------------------
// GetToolRegistry
// ---------------------------------------------------------------------------
std::vector<OllamaTool> GetToolRegistry()
{
    std::vector<OllamaTool> tools;

    if (g_EnableSelfStateTools)
    {
        // get_self_status
        {
            OllamaTool t;
            t.name        = "get_self_status";
            t.description = "Returns the bot's current in-game status: location (zone, area, map, coordinates), level, class, race, faction, HP%, mana%, gold, current target, combat status, group size.";
            t.parameters  = {
                {"type", "object"},
                {"properties", json::object()},
                {"required", json::array()}
            };
            t.handler = HandleGetSelfStatus;
            tools.push_back(std::move(t));
        }

        // get_inventory
        {
            OllamaTool t;
            t.name        = "get_inventory";
            t.description = "Returns the bot's current equipment (what is worn in each gear slot) and the contents of the bot's bags (item name and stack count), plus the number of free bag slots.";
            t.parameters  = {
                {"type", "object"},
                {"properties", json::object()},
                {"required", json::array()}
            };
            t.handler = HandleGetInventory;
            tools.push_back(std::move(t));
        }
    }

    if (g_EnableWowheadTool)
    {
        OllamaTool t;
        t.name        = "wowhead_lookup";
        t.description = "Searches WoWHead (wotlk) for information about a WoW item, NPC, spell, quest, or zone by name. Returns a text excerpt from the search results page. Use this when asked about game mechanics, items, or lore you are unsure about.";
        t.parameters  = {
            {"type", "object"},
            {"properties", {
                {"query", {
                    {"type", "string"},
                    {"description", "The item, NPC, spell, or zone name to search for on WoWHead."}
                }}
            }},
            {"required", {"query"}}
        };
        t.handler = HandleWowheadLookup;
        tools.push_back(std::move(t));
    }

    return tools;
}

// ---------------------------------------------------------------------------
// QueryOllamaChatWithTools — background-thread entry point
// ---------------------------------------------------------------------------
std::string QueryOllamaChatWithTools(const std::string& userPrompt,
                                     const BotSelfSnapshot& snap)
{
    static OllamaHttpClient httpClient;

    if (!httpClient.IsAvailable())
    {
        LOG_ERROR("server.loading", "[OllamaChat/Tools] HTTP client not available.");
        return "";
    }

    // Derive /api/chat URL from the generate URL when no override is set.
    std::string chatUrl = g_OllamaChatUrl;
    if (chatUrl.empty())
    {
        chatUrl = g_OllamaUrl;
        const std::string kGeneratePath = "/api/generate";
        const std::string kChatPath     = "/api/chat";
        size_t pos = chatUrl.rfind(kGeneratePath);
        if (pos != std::string::npos)
            chatUrl.replace(pos, kGeneratePath.size(), kChatPath);
        else
            chatUrl += kChatPath; // fallback: just append
    }

    // Build initial message list
    json messages = json::array();
    // Prepend the tool-use instruction so the model actually CALLS tools for
    // factual self-state / lookup questions instead of role-playing an answer.
    std::string systemContent;
    if (!g_ToolSystemPrompt.empty())
        systemContent = g_ToolSystemPrompt;
    if (!g_OllamaSystemPrompt.empty())
    {
        if (!systemContent.empty())
            systemContent += "\n\n";
        systemContent += g_OllamaSystemPrompt;
    }
    if (!systemContent.empty())
    {
        messages.push_back({
            {"role", "system"},
            {"content", SanitizeUTF8(systemContent)}
        });
    }
    messages.push_back({
        {"role", "user"},
        {"content", SanitizeUTF8(userPrompt)}
    });

    // Build tools array from registry
    std::vector<OllamaTool> registry = GetToolRegistry();
    json toolsJson = json::array();
    for (const auto& tool : registry)
    {
        toolsJson.push_back({
            {"type", "function"},
            {"function", {
                {"name",        tool.name},
                {"description", tool.description},
                {"parameters",  tool.parameters}
            }}
        });
    }

    // Build options object (mirrors QueryOllamaAPI logic)
    json options;
    bool hasOptions = false;
    if (g_OllamaNumPredict > 0)   { options["num_predict"] = g_OllamaNumPredict; hasOptions = true; }
    if (g_OllamaTemperature != 0.8f) { options["temperature"] = g_OllamaTemperature; hasOptions = true; }
    if (g_OllamaTopP != 0.95f)    { options["top_p"] = g_OllamaTopP; hasOptions = true; }
    if (g_OllamaRepeatPenalty != 1.1f) { options["repeat_penalty"] = g_OllamaRepeatPenalty; hasOptions = true; }
    if (g_OllamaNumCtx > 0)       { options["num_ctx"] = g_OllamaNumCtx; hasOptions = true; }
    if (g_OllamaNumThreads > 0)   { options["num_thread"] = g_OllamaNumThreads; hasOptions = true; }
    if (!g_OllamaSeed.empty())
    {
        try
        {
            options["seed"] = std::stoi(g_OllamaSeed);
            hasOptions = true;
        }
        catch (...) {}
    }

    uint32_t maxRounds = (g_MaxToolRounds > 0) ? g_MaxToolRounds : 3;

    // Tool-call loop
    for (uint32_t round = 0; round < maxRounds; ++round)
    {
        json requestData = {
            {"model",   g_OllamaModel},
            {"messages", messages},
            {"stream",  false},
            {"think",   false}
        };

        // On the last round, omit tools to force a plain text reply
        if (round < maxRounds - 1 && !toolsJson.empty())
            requestData["tools"] = toolsJson;

        if (hasOptions)
            requestData["options"] = options;

        std::string body = requestData.dump(-1, ' ', false,
                     nlohmann::json::error_handler_t::replace);

        if (g_DebugEnabled)
            LOG_INFO("server.loading", "[OllamaChat/Tools] Round {}/{} -> POST {}", round + 1, maxRounds, chatUrl);

        std::string rawResp = httpClient.Post(chatUrl, body);
        if (rawResp.empty())
        {
            LOG_ERROR("server.loading", "[OllamaChat/Tools] Empty response from {}", chatUrl);
            return "";
        }

        json resp;
        try
        {
            resp = json::parse(rawResp);
        }
        catch (const std::exception& ex)
        {
            LOG_ERROR("server.loading", "[OllamaChat/Tools] JSON parse error: {}", ex.what());
            if (g_DebugEnabled)
                LOG_INFO("server.loading", "[OllamaChat/Tools] Raw response: {}", rawResp);
            return "";
        }

        // /api/chat response has a "message" field
        if (!resp.contains("message") || !resp["message"].is_object())
        {
            LOG_ERROR("server.loading", "[OllamaChat/Tools] Response missing 'message' field.");
            if (g_DebugEnabled)
                LOG_INFO("server.loading", "[OllamaChat/Tools] Raw response: {}", rawResp);
            return "";
        }

        json assistantMsg = resp["message"];

        // Check for tool_calls
        bool hasToolCalls = assistantMsg.contains("tool_calls") &&
                            assistantMsg["tool_calls"].is_array() &&
                            !assistantMsg["tool_calls"].empty();

        if (!hasToolCalls)
        {
            // Final text reply
            std::string content;
            if (assistantMsg.contains("content") && assistantMsg["content"].is_string())
                content = assistantMsg["content"].get<std::string>();
            content = SanitizeUTF8(content);
            if (g_DebugEnabled)
                LOG_INFO("server.loading", "[OllamaChat/Tools] Final reply after {} round(s): {}", round + 1, content);
            return content;
        }

        // Append the assistant turn (with tool_calls) to the message history
        messages.push_back(assistantMsg);

        if (g_DebugEnabled)
        {
            LOG_INFO("server.loading", "[OllamaChat/Tools] Round {} model called {} tool(s).",
                round + 1, assistantMsg["tool_calls"].size());
        }

        // Execute each tool call and append tool results
        for (const auto& tc : assistantMsg["tool_calls"])
        {
            std::string toolName;
            json toolArgs;

            try
            {
                if (tc.contains("function"))
                {
                    const auto& fn = tc["function"];
                    if (fn.contains("name") && fn["name"].is_string())
                        toolName = fn["name"].get<std::string>();

                    if (fn.contains("arguments"))
                    {
                        // arguments may be a json object or a json string
                        if (fn["arguments"].is_string())
                        {
                            try { toolArgs = json::parse(fn["arguments"].get<std::string>()); }
                            catch (...) { toolArgs = json::object(); }
                        }
                        else if (fn["arguments"].is_object())
                        {
                            toolArgs = fn["arguments"];
                        }
                    }
                }
            }
            catch (const std::exception& ex)
            {
                LOG_WARN("server.loading", "[OllamaChat/Tools] Error parsing tool_call: {}", ex.what());
                toolName  = "";
                toolArgs  = json::object();
            }

            if (g_DebugEnabled)
                LOG_INFO("server.loading", "[OllamaChat/Tools] Calling tool '{}' args: {}", toolName, toolArgs.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));

            // Dispatch to registered handler
            std::string toolResult;
            bool found = false;
            for (const auto& tool : registry)
            {
                if (tool.name == toolName)
                {
                    try { toolResult = tool.handler(toolArgs, snap); }
                    catch (const std::exception& ex)
                    {
                        toolResult = std::string("Ошибка инструмента: ") + ex.what();
                    }
                    found = true;
                    break;
                }
            }
            if (!found)
                toolResult = "Неизвестный инструмент: " + toolName;

            toolResult = SanitizeUTF8(toolResult);

            if (g_DebugEnabled)
                LOG_INFO("server.loading", "[OllamaChat/Tools] Tool '{}' result ({}c): {}",
                    toolName, toolResult.size(), toolResult.substr(0, 200));

            // Append tool result message
            json toolMsg = {
                {"role",      "tool"},
                {"content",   toolResult},
                {"tool_name", toolName}    // some Ollama versions expect this
            };
            messages.push_back(std::move(toolMsg));
        }

        // Continue to next round with updated messages
    }

    // Exhausted all rounds without a plain text reply
    LOG_WARN("server.loading", "[OllamaChat/Tools] Exhausted {} tool rounds without final text.", maxRounds);
    return "";
}
