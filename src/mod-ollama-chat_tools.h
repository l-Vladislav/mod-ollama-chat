#ifndef MOD_OLLAMA_CHAT_TOOLS_H
#define MOD_OLLAMA_CHAT_TOOLS_H

#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include <nlohmann/json.hpp>

// --------------------------------------------
// Snapshot of bot self-state (immutable, thread-safe).
// Populated on the world-thread in SnapshotBotSelf(), then
// read from background threads only.
// All strings are resolved at snapshot time — no Player* reads
// happen after the snapshot is taken.
// --------------------------------------------
struct BotSelfSnapshot
{
    // Identity
    std::string name;
    uint32_t    level       = 0;
    std::string className;
    std::string raceName;
    std::string factionName;

    // Location
    std::string zoneName;
    std::string areaName;
    uint32_t    mapId       = 0;
    float       posX        = 0.f;
    float       posY        = 0.f;
    float       posZ        = 0.f;

    // Vitals
    uint32_t    healthPct   = 0;    // 0-100
    uint32_t    manaPct     = 0;    // 0-100

    // Currency
    uint32_t    moneyG      = 0;    // gold
    uint32_t    moneyS      = 0;    // silver
    uint32_t    moneyC      = 0;    // copper

    // Combat / social
    std::string targetName;
    bool        inCombat    = false;
    uint32_t    groupSize   = 0;

    // Equipment: "slot_name: item_name" strings
    std::vector<std::string> equipped;

    // Inventory items: {name, count}
    struct BagItem
    {
        std::string name;
        uint32_t    count = 0;
    };
    std::vector<BagItem> bagItems;
    uint32_t freeBagSlots = 0;
};

// Forward declarations to avoid including Player.h here
class Player;

// Called on the world-thread to fill a BotSelfSnapshot from a live Player*.
BotSelfSnapshot SnapshotBotSelf(Player* bot);

// --------------------------------------------
// Tool registry entry
// --------------------------------------------
struct OllamaTool
{
    std::string name;
    std::string description;
    nlohmann::json parameters;  // JSON Schema (type: object)
    std::function<std::string(const nlohmann::json& args, const BotSelfSnapshot& snap)> handler;
};

// Returns the list of enabled tools according to current config globals.
// Thread-safe: reads config globals (set at startup), never touches game objects.
std::vector<OllamaTool> GetToolRegistry();

// --------------------------------------------
// Tool-calling chat loop: calls /api/chat with tools, runs the
// tool-call loop (up to g_MaxToolRounds), and returns the final
// assistant text content.
// Must only be called from background threads (never world-thread).
// --------------------------------------------
std::string QueryOllamaChatWithTools(const std::string& userPrompt,
                                     const BotSelfSnapshot& snap);

#endif // MOD_OLLAMA_CHAT_TOOLS_H
