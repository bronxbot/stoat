#pragma once
#include "stoatpp/bot_module.h"
#include "stoatpp/cluster.h"
#include "../database.h"
#include "../emojis.h"
#include "helpers.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace bronx::cogs {

class RpgModule : public stoatpp::bot_module {
private:
    static std::string fmt_coins(int64_t val) {
        bool neg = val < 0;
        uint64_t u = neg ? static_cast<uint64_t>(-val) : static_cast<uint64_t>(val);
        std::string s = std::to_string(u);
        int n = static_cast<int>(s.length()) - 3;
        while (n > 0) {
            s.insert(n, ",");
            n -= 3;
        }
        return (neg ? "-$" : "$") + s;
    }

    static std::string get_est_date_str(int offset_days = 0) {
        auto now = std::chrono::system_clock::now();
        time_t tnow = std::chrono::system_clock::to_time_t(now);
        time_t est = tnow - 5 * 3600 + (offset_days * 86400);
        tm est_tm = *gmtime(&est);
        char buf[16];
        strftime(buf, sizeof(buf), "%Y-%m-%d", &est_tm);
        return std::string(buf);
    }

    // ── 1. PICKAXE & ORE CATALOG ─────────────────────────────────────────────
    struct PickaxeDef {
        std::string id;
        std::string name;
        std::string emoji;
        int level;
        int64_t cost;
        int req_prestige;
        int cooldown_sec;
        int min_yield;
        int max_yield;
        double value_mult;
        std::string description;
    };

    static const std::vector<PickaxeDef>& get_pickaxes() {
        static const std::vector<PickaxeDef> catalog = {
            {"hands",      "Bare Hands",        "👐", 0,  0,          0, 25, 1, 2, 1.00, "Digging with your bare hands"},
            {"bronze",     "Bronze Pickaxe",    "⛏️", 1,  25000,      0, 22, 1, 3, 1.15, "Unlocks Uncommon ores (Silver, Gold, Lapis, Amethyst)"},
            {"silver",     "Silver Pickaxe",    "⛏️", 2,  150000,     0, 20, 2, 3, 1.35, "Unlocks Rare ores (Platinum, Titanium, Ruby, Sapphire, Emerald)"},
            {"gold",       "Gold Pickaxe",      "⛏️", 3,  1000000,    0, 18, 2, 4, 1.65, "Unlocks Epic ores (Diamond, Alexandrite, Iridium, Mithril, Dragon Stone)"},
            {"diamond",    "Diamond Pickaxe",   "💎", 4,  10000000,   0, 15, 2, 5, 2.10, "Unlocks Legendary ores (Philosopher's Stone, Void Crystal, Celestial Ore)"},
            {"mythic",     "Mythic Pickaxe",    "🔥", 5,  75000000,   0, 12, 3, 6, 2.75, "Unlocks World Core Shard, Stardust Ore, and Eternity Gem"},
            {"void",       "Void Pickaxe",      "🕳️", 7,  250000000,  1, 10, 3, 7, 3.75, "Prestige 1+ — Unlocks Molten Core, Phoenix Ember, Infernal Ruby"},
            {"shadow",     "Shadow Pickaxe",    "🌑", 8,  750000000,  2, 9,  4, 7, 5.00, "Prestige 2+ — Unlocks Shadow Ore, Void Shard, Dark Matter"},
            {"nebula",     "Nebula Pickaxe",    "🌌", 9,  2500000000LL, 3, 8, 4, 8, 6.50, "Prestige 3+ — Unlocks Nebula Crystal, Astral Shard, Cosmic Topaz"},
            {"stellar",    "Stellar Pickaxe",   "🌠", 10, 8000000000LL, 4, 7, 5, 9, 8.50, "Prestige 4+ — Unlocks Stellar Diamond, Gravity Gem, Solar Topaz"},
            {"divine",     "Divine Pickaxe",    "✨", 11, 25000000000LL, 5, 6, 5, 10, 12.0, "Prestige 5+ — Unlocks Eternal Ore, Immortal Gem, Divine Crystal"},
            {"infinity",   "Infinity Pickaxe",  "♾️", 16, 100000000000LL, 8, 5, 6, 12, 20.0, "Prestige 8+ — Unlocks Quantum, Temporal, Infinity & Antigravity Crystals"}
        };
        return catalog;
    }

    static const PickaxeDef* find_pickaxe(const std::string& q) {
        std::string s = q;
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        if (s.rfind("pickaxe_", 0) == 0) s = s.substr(8);
        for (const auto& p : get_pickaxes()) {
            std::string nl = p.name;
            std::transform(nl.begin(), nl.end(), nl.begin(), ::tolower);
            if (p.id == s || nl == s || nl.find(s) != std::string::npos) return &p;
        }
        return nullptr;
    }

    struct OreDef {
        std::string name;
        std::string emoji;
        int weight;
        int64_t min_val;
        int64_t max_val;
        int min_pick_lvl;
        std::string effect_name;
        double effect_chance;
    };

    static const std::vector<OreDef>& get_ores() {
        static const std::vector<OreDef> ores = {
            // Level 0 (Common)
            {"stone",               "🪨", 250, 15,       60,       0, "",            0.00},
            {"coal",                "⬛", 220, 30,       105,      0, "",            0.00},
            {"copper ore",          "🟤", 200, 45,       150,      0, "Flat Surge",  0.08},
            {"tin ore",             "⬜", 190, 35,       120,      0, "",            0.00},
            {"quartz",              "💎", 160, 75,       210,      0, "Flat Surge",  0.09},
            {"iron ore",            "🔩", 150, 90,       270,      0, "Flat Surge",  0.10},
            {"obsidian shard",      "🖤", 140, 105,      300,      0, "Critical 2x", 0.10},
            {"pyrite",              "🌟", 155, 85,       240,      0, "Jackpot 4x",  0.12},
            // Level 1 (Uncommon)
            {"silver ore",          "🥈", 95,  240,      750,      1, "Flat Surge",  0.10},
            {"gold ore",            "🥇", 75,  360,      1140,     1, "Log Boost",   0.12},
            {"cobalt ore",          "🔵", 80,  300,      960,      1, "Flat Surge",  0.11},
            {"lapis lazuli",        "💙", 70,  390,      1200,     1, "Cascading",   0.12},
            {"topaz",               "🟡", 65,  450,      1350,     1, "Wacky 3x",    0.13},
            {"amethyst",            "🟣", 55,  540,      1650,     1, "Exponential", 0.12},
            // Level 2 (Rare)
            {"platinum ore",        "⚪", 42,  1050,     2700,     2, "Cascading",   0.14},
            {"titanium ore",        "🛡️", 38,  1200,     3000,     2, "Log Boost",   0.12},
            {"ruby",                "❤️", 30,  1500,     4200,     2, "Critical 2x", 0.16},
            {"sapphire",            "💙", 28,  1650,     4500,     2, "Wacky 3x",    0.15},
            {"emerald",             "💚", 25,  1800,     5400,     2, "Exponential", 0.14},
            {"meteorite shard",     "☄️", 22,  1950,     5700,     2, "Jackpot 4x",  0.20},
            // Level 3 (Epic)
            {"diamond",             "💎", 15,  3600,     12000,    3, "Critical 2x", 0.20},
            {"alexandrite",         "💜", 12,  4500,     15000,    3, "Wacky 3x",    0.18},
            {"iridium ore",         "🌌", 8,   6000,     21000,    3, "Exponential", 0.18},
            {"mithril ore",         "🔮", 6,   15000,    36000,    3, "Gear Surge",  0.15},
            {"dragon stone",        "🐉", 5,   18000,    54000,    3, "Jackpot 4x",  0.22},
            // Level 4-5 (Legendary)
            {"philosopher's stone", "🔴", 3,   25000,    75000,    4, "Exponential", 0.25},
            {"void crystal",        "🕳️", 2,   40000,    125000,   4, "Wacky 3x",    0.22},
            {"celestial ore",       "🌠", 1,   75000,    250000,   4, "Cascading",   0.30},
            {"adamantite",          "🛡️", 2,   60000,    175000,   4, "Critical 2x", 0.24},
            {"world core shard",    "🌍", 1,   100000,   300000,   5, "Gear Surge",  0.26},
            {"stardust ore",        "✨", 1,   125000,   400000,   5, "Exponential", 0.30},
            {"eternity gem",        "♾️", 1,   225000,   625000,   5, "Wacky 3x",    0.35},
            // Prestige Tiers (7..16)
            {"molten core",         "🔥", 3,   300000,   900000,   7, "Exponential", 0.25},
            {"phoenix ember",       "🕊️", 2,   400000,   1200000,  7, "Wacky 3x",    0.25},
            {"shadow ore",          "🌑", 3,   700000,   2000000,  8, "Wacky 3x",    0.25},
            {"dark matter",         "🔮", 2,   900000,   2500000,  8, "Exponential", 0.27},
            {"nebula crystal",      "🌌", 2,   1500000,  4500000,  9, "Cascading",   0.28},
            {"stellar diamond",     "🌠", 2,   3000000,  9000000,  10,"Exponential", 0.30},
            {"eternal ore",         "💫", 2,   6000000,  18000000, 11,"Jackpot 4x",  0.35},
            {"quantum ore",         "📦", 1,   15000000, 45000000, 16,"Jackpot 4x",  0.40},
            {"infinity ore",        "♾️", 1,   30000000, 90000000, 16,"Exponential", 0.45},
            {"antigravity crystal", "🍏", 1,   50000000, 150000000,16,"Wacky 3x",    0.50}
        };
        return ores;
    }

    // ── 2. PET SPECIES CATALOG ───────────────────────────────────────────────
    struct PetSpeciesDef {
        std::string id;
        std::string name;
        std::string emoji;
        std::string rarity;
        std::string bonus_type;
        double base_bonus;
        double bonus_per_level;
        int64_t adopt_cost;
        int max_level;
        std::string description;
    };

    static const std::vector<PetSpeciesDef>& get_pet_species() {
        static const std::vector<PetSpeciesDef> species = {
            {"cat",      "Cat",      "🐱", "Common",    "luck_bonus",          3.0,  0.5, 50000,     20, "A curious cat that brings general luck"},
            {"dog",      "Dog",      "🐶", "Common",    "xp_bonus",            3.0,  0.5, 50000,     20, "A loyal dog that boosts XP gain"},
            {"hamster",  "Hamster",  "🐹", "Common",    "work_bonus",          5.0,  1.0, 50000,     20, "A hardworking hamster that boosts work & daily earnings"},
            {"parrot",   "Parrot",   "🦜", "Uncommon",  "fish_value_bonus",    5.0,  0.8, 200000,    25, "A colorful parrot that appraises fish for higher value"},
            {"rabbit",   "Rabbit",   "🐇", "Uncommon",  "daily_bonus",         5.0,  1.0, 200000,    25, "A lucky rabbit that boosts daily & challenge rewards"},
            {"fox",      "Fox",      "🦊", "Uncommon",  "rob_protection",      5.0,  0.8, 200000,    25, "A cunning fox that guards your wallet from thieves"},
            {"owl",      "Owl",      "🦉", "Rare",      "ore_value_bonus",     8.0,  1.0, 1000000,   30, "A wise owl that spots rich ore deposits"},
            {"dolphin",  "Dolphin",  "🐬", "Rare",      "rare_fish_bonus",     5.0,  0.8, 1000000,   30, "A playful dolphin that attracts rare fish"},
            {"wolf",     "Wolf",     "🐺", "Rare",      "gambling_luck_bonus", 3.0,  0.5, 1000000,   30, "A fierce wolf that sharpens your casino edge"},
            {"phoenix",  "Phoenix",  "🔥", "Epic",      "all_value_bonus",     5.0,  0.8, 10000000,  40, "A mythical phoenix that boosts all fish & ore values"},
            {"unicorn",  "Unicorn",  "🦄", "Epic",      "legendary_bonus",     3.0,  0.5, 10000000,  40, "A magical unicorn that attracts legendary drops"},
            {"dragon",   "Dragon",   "🐉", "Legendary", "all_earnings_bonus",  8.0,  1.0, 100000000, 50, "An ancient dragon that boosts all coin earnings"},
            {"void_cat", "Void Cat", "🐈‍⬛", "Prestige",  "prestige_bonus",      10.0, 1.5, 500000000, 50, "A dimensional void cat (requires Prestige 1+)"}
        };
        return species;
    }

    static const PetSpeciesDef* find_pet_species(const std::string& q) {
        std::string s = q;
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        for (const auto& sp : get_pet_species()) {
            std::string nl = sp.name;
            std::transform(nl.begin(), nl.end(), nl.begin(), ::tolower);
            if (sp.id == s || nl == s) return &sp;
        }
        return nullptr;
    }

    // ── 3. SKILL TREE CATALOG ────────────────────────────────────────────────
    struct SkillNodeDef {
        std::string id;
        std::string name;
        std::string branch; // angler, prospector, gambler
        int tier;
        int max_rank;
        double bonus_per_rank;
        std::string emoji;
        std::string description;
    };

    static const std::vector<SkillNodeDef>& get_skill_nodes() {
        static const std::vector<SkillNodeDef> nodes = {
            // Angler
            {"angler_speed",    "Quick Hands",     "angler",     1, 5, 3.0, "🎣", "+3% faster fishing cooldown per rank"},
            {"angler_value",    "Appraiser's Eye", "angler",     1, 5, 2.0, "💰", "+2% fish sell value per rank"},
            {"angler_rare",     "Lucky Lure",      "angler",     2, 5, 1.5, "✨", "+1.5% rare fish chance per rank"},
            {"angler_double",   "Double Hook",     "angler",     3, 5, 2.0, "🪝", "+2% double catch chance per rank"},
            {"angler_epic",     "Master Angler",   "angler",     4, 3, 2.0, "🌟", "+2% epic+ fish chance per rank"},
            {"angler_legend",   "Legendary Lure",  "angler",     5, 2, 1.0, "👑", "+1% legendary+ fish chance per rank"},
            // Prospector
            {"miner_yield",     "Deep Veins",      "prospector", 1, 5, 3.0, "⛏️", "+3% bonus ore yield chance per rank"},
            {"miner_value",     "Gem Cutter",      "prospector", 1, 5, 2.0, "💎", "+2% ore sell value per rank"},
            {"miner_rare",      "Keen Nose",       "prospector", 2, 5, 1.5, "👃", "+1.5% rare+ ore weight per rank"},
            {"miner_double",    "Twin Pickaxe",    "prospector", 3, 5, 2.0, "⚒️", "+2% double ore strike chance per rank"},
            {"miner_celestial", "Celestial Sense", "prospector", 4, 3, 1.5, "🌠", "+1.5% celestial ore weight per rank"},
            {"miner_void",      "Void Walker",     "prospector", 5, 2, 1.0, "🕳️", "+1% void/prestige ore weight per rank"},
            // Gambler
            {"gambler_luck",    "Beginner's Luck", "gambler",    1, 5, 1.0, "🍀", "+1% gambling luck bonus per rank"},
            {"gambler_payout",  "High Roller",     "gambler",    1, 5, 2.0, "💸", "+2% gambling payout bonus per rank"},
            {"gambler_streak",  "Hot Hand",        "gambler",    2, 5, 3.0, "🔥", "+3% win streak bonus per rank"},
            {"gambler_shield",  "Safety Net",      "gambler",    3, 5, 2.0, "🛡️", "+2% loss rebate per rank"},
            {"gambler_jackpot", "Jackpot Hunter",  "gambler",    4, 3, 0.5, "🎰", "+0.5% progressive jackpot trigger boost per rank"},
            {"gambler_crit",    "Critical Hit",    "gambler",    5, 2, 1.0, "💥", "+1% critical payout chance per rank"}
        };
        return nodes;
    }

    static int calc_total_skill_points(int prestige) {
        int total = 0;
        for (int p = 1; p <= prestige; ++p) {
            if (p <= 5) total += 2;
            else if (p <= 10) total += 3;
            else total += 4;
        }
        return total;
    }

    // ── 4. MASTERY TIERS ─────────────────────────────────────────────────────
    struct MasteryTierDef {
        const char* name;
        const char* emoji;
        int req;
        double bonus;
    };

    static const std::vector<MasteryTierDef>& get_mastery_tiers() {
        static const std::vector<MasteryTierDef> tiers = {
            {"Novice",      "⬜", 1,    0.00},
            {"Apprentice",  "🟢", 10,   0.01},
            {"Journeyman",  "🔵", 25,   0.02},
            {"Expert",      "🟣", 50,   0.03},
            {"Master",      "🟡", 100,  0.05},
            {"Grandmaster", "🔴", 250,  0.07},
            {"Legend",      "💎", 500,  0.10},
            {"Mythic",      "⭐", 1000, 0.15}
        };
        return tiers;
    }

    static const MasteryTierDef& get_mastery_tier(int64_t count) {
        const auto& tiers = get_mastery_tiers();
        const MasteryTierDef* best = &tiers[0];
        for (const auto& t : tiers) {
            if (count >= t.req) best = &t;
        }
        return *best;
    }

    // ── 5. CRAFTING RECIPES ──────────────────────────────────────────────────
    struct CraftRecipeDef {
        std::string id;
        std::string name;
        std::string emoji;
        std::string category;
        std::vector<std::pair<std::string, int64_t>> ore_reqs;
        int64_t coin_cost;
        int req_prestige;
        std::string reward_type; // "pickaxe", "item"
        std::string reward_id;
        int reward_qty;
        std::string description;
    };

    static const std::vector<CraftRecipeDef>& get_craft_recipes() {
        static const std::vector<CraftRecipeDef> recipes = {
            {"meteor_pickaxe",   "Meteor Pickaxe",   "☄️", "Mining",  {{"meteorite shard", 5}, {"titanium ore", 10}}, 250000,   0, "pickaxe", "gold",             1, "Forges a Gold-tier Pickaxe from Meteorite Shards & Titanium"},
            {"diamond_forge",    "Diamond Pickaxe",  "💎", "Mining",  {{"diamond", 8}, {"iridium ore", 4}},           1500000,  0, "pickaxe", "diamond",          1, "Forges a Diamond Pickaxe from raw Diamonds & Iridium"},
            {"mythic_forge",     "Mythic Pickaxe",   "🔥", "Mining",  {{"dragon stone", 5}, {"mithril ore", 6}},      15000000, 0, "pickaxe", "mythic",           1, "Forges a Mythic Pickaxe from Dragon Stone & Mithril"},
            {"void_pickaxe",     "Void Pickaxe",     "🕳️", "Mining",  {{"void crystal", 3}, {"celestial ore", 2}},    50000000, 1, "pickaxe", "void",             1, "Prestige 1+ — Forges the Void Pickaxe from Void Crystals & Celestial Ore"},
            {"lucky_charm",      "Lucky Charm",      "🍀", "Utility", {{"gold ore", 5}, {"lapis lazuli", 5}},         50000,    0, "item",    "vip_pass",         1, "Crafts a VIP Luck Pass from Gold Ore & Lapis Lazuli"},
            {"insurance_policy", "Padlock Shield",   "🛡️", "Utility", {{"iron ore", 10}, {"silver ore", 5}},          25000,    0, "item",    "padlock",          2, "Crafts 2x Padlocks from Iron & Silver Ore"},
            {"detector_kit",     "Item Detector",    "🔍", "Utility", {{"copper ore", 10}, {"quartz", 8}},            15000,    0, "item",    "item_detector",    2, "Crafts 2x Item Detectors from Copper & Quartz"},
            {" philosopher_core","Bank Expansion",   "🏦", "Prestige",{{"philosopher's stone", 2}, {"emerald", 5}},   500000,   1, "item",    "bank_note",        3, "Transmutes Philosopher's Stones into +$1,500,000 Bank Capacity"}
        };
        return recipes;
    }

    // ── 6. TITLES CATALOG ────────────────────────────────────────────────────
    struct TitleDef {
        std::string id;
        std::string display;
        std::string desc;
        int64_t price;
        int limit; // 0 = unlimited
    };

    static const std::vector<TitleDef>& get_titles_catalog() {
        static const std::vector<TitleDef> titles = {
            {"hustler",     "💰 Hustler",        "Always on the grind",                          800000,      0},
            {"shark",       "🦈 Shark",          "The loan shark of the server",                 2000000,     0},
            {"gambler",     "🎲 High Roller",    "Wins big, loses bigger",                       5000000,     0},
            {"divine",      "✨ **Divine**",     "Touched by something greater",                 15000000,    0},
            {"fisherking",  "👑 Fisher King",    "Reigns over the fishing leaderboard",          20000000,    0},
            {"angler",      "🎣 Master Angler",  "For those who live by the rod",                50000000,    0},
            {"prospector",  "⛏️ Deep Prospector","Master of the underground veins",              50000000,    0},
            {"kingpin",     "🕴️ Kingpin",        "Rules the casino scene with an iron fist",     150000000,   0},
            {"legend",      "**[Legend]**",      "A true Bronx legend",                          250000000,   0},
            {"billionaire", "💎 Billionaire",    "Baller status, flex with pride",               1000000000LL,0},
            {"og",          "🔥 OG",             "Early supporter and all-around legend (100x)", 2500000000LL,100},
            {"chosen",      "⚡ The Chosen",     "Only 5 copies exist across the bot",           500000000LL, 5},
            {"goat",        "🐐 GOAT",           "Greatest of All Time",                         50000000000LL,0}
        };
        return titles;
    }

    static const TitleDef* find_title_def(const std::string& q) {
        std::string s = q;
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        if (s.rfind("title_", 0) == 0) s = s.substr(6);
        for (const auto& t : get_titles_catalog()) {
            if (t.id == s) return &t;
        }
        return nullptr;
    }

    // ── 7. ACHIEVEMENTS CATALOG ──────────────────────────────────────────────
    struct AchievementDef {
        std::string id;
        std::string name;
        std::string emoji;
        std::string stat_type; // "ores_mined", "ore_sold", "gamble_wins", "gamble_profit", "streak"
        int64_t threshold;
        int64_t reward_coins;
        std::string reward_title_id;
        std::string desc;
    };

    static const std::vector<AchievementDef>& get_achievements_catalog() {
        static const std::vector<AchievementDef> achs = {
            {"miner_100",     "Novice Prospector",  "⛏️", "ores_mined",    100,        100000,     "",           "Mine 100 total ores"},
            {"miner_1000",    "Deep Delver",        "💎", "ores_mined",    1000,       1500000,    "prospector", "Mine 1,000 total ores"},
            {"ore_sold_10m",  "Mineral Merchant",   "🪙", "ore_sold",      10000000,   2000000,    "",           "Sell $10,000,000 worth of ores"},
            {"gamble_w_50",   "Lucky Beginner",     "🍀", "gamble_wins",   50,         250000,     "",           "Win 50 gambling games"},
            {"gamble_w_500",  "Casino Veteran",     "🎰", "gamble_wins",   500,        5000000,    "gambler",    "Win 500 gambling games"},
            {"gamble_p_10m",  "High Roller Profit", "💸", "gamble_profit", 10000000,   2500000,    "",           "Win $10,000,000 total gambling profit"},
            {"gamble_p_100m", "Casino Kingpin",     "🕴️", "gamble_profit", 100000000,  15000000,   "kingpin",    "Win $100,000,000 total gambling profit"},
            {"streak_7",      "One Week Strong",    "🔥", "streak",        7,          250000,     "",           "Reach a 7-day daily login streak"},
            {"streak_30",     "Monthly Master",     "👑", "streak",        30,         5000000,    "legend",     "Reach a 30-day daily login streak"}
        };
        return achs;
    }

    static std::vector<Database::UserDailyChallengeEntry> ensure_today_challenges(const std::string& user_id, const std::string& date_str) {
        auto list = g_db->get_user_daily_challenges(user_id, date_str);
        if (!list.empty()) return list;

        std::seed_seq seed(user_id.begin(), user_id.end());
        std::mt19937 rng(seed);
        int64_t mine_target = 10 + (rng() % 21);
        int64_t ore_val_target = 25000 + (rng() % 75001);
        int64_t daily_claim_target = 1;

        std::vector<Database::UserDailyChallengeEntry> fresh = {
            {0, "mine_ores",  mine_target,        0, mine_target * 4000,   false},
            {1, "sell_ores",  ore_val_target,     0, ore_val_target / 2,   false},
            {2, "claim_daily",daily_claim_target, 0, 50000,                false}
        };
        for (const auto& e : fresh) g_db->save_user_daily_challenge(user_id, date_str, e);
        return fresh;
    }

public:
    void register_handlers(stoatpp::cluster& bot) override {
        // ── 1. .mine (dig) ───────────────────────────────────────────────────
        bot.register_command({
            "mine",
            {"dig"},
            "mine ores and gemstones using your equipped pickaxe.",
            "",
            {},
            [](stoatpp::cluster& bot, const stoatpp::events::Message& msg, const std::vector<std::string>&) {
                if (!g_db) return;
                auto prof = g_db->get_user_mining(msg.author.id);
                const PickaxeDef* pick = find_pickaxe(prof.pickaxe_id);
                if (!pick) pick = &get_pickaxes()[0];

                int64_t now_s = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                int cd = pick->cooldown_sec;
                if (now_s - prof.last_mine < cd) {
                    int64_t rem = cd - (now_s - prof.last_mine);
                    bot.send_reply(msg, "⏳ Your pickaxe is cooling down! Try again in **" + std::to_string(rem) + "s**.", true);
                    return;
                }

                auto eco = g_db->get_economy_user(msg.author.id);
                int prestige = eco ? eco->prestige : 0;

                // Skill tree & pet bonuses
                auto skills = g_db->get_user_skills(msg.author.id);
                int yield_rank = skills["miner_yield"];
                int val_rank = skills["miner_value"];
                int rare_rank = skills["miner_rare"];
                int double_rank = skills["miner_double"];

                double pet_val_bonus = 0.0;
                for (const auto& pt : g_db->get_user_companion_pets(msg.author.id)) {
                    if (pt.equipped) {
                        if (const auto* sp = find_pet_species(pt.species_id)) {
                            if (sp->bonus_type == "ore_value_bonus" || sp->bonus_type == "all_value_bonus" || sp->bonus_type == "all_earnings_bonus") {
                                pet_val_bonus += (sp->base_bonus + (pt.level - 1) * sp->bonus_per_level) / 100.0;
                            }
                        }
                    }
                }

                // Filter eligible ores
                std::vector<const OreDef*> pool;
                std::vector<double> weights;
                for (const auto& o : get_ores()) {
                    if (o.min_pick_lvl <= pick->level) {
                        double w = static_cast<double>(o.weight);
                        if (o.min_pick_lvl >= 2) {
                            w *= (1.0 + rare_rank * 0.05 + prestige * 0.04);
                        }
                        pool.push_back(&o);
                        weights.push_back(w);
                    }
                }

                static thread_local std::mt19937 rng(std::random_device{}());
                std::uniform_int_distribution<int> qty_dist(pick->min_yield, pick->max_yield);
                int count = qty_dist(rng);
                std::uniform_int_distribution<int> pct_dist(1, 100);
                if (pct_dist(rng) <= yield_rank * 3) count++;
                if (pct_dist(rng) <= double_rank * 2) count *= 2;

                std::discrete_distribution<size_t> ore_dist(weights.begin(), weights.end());
                std::uniform_real_distribution<double> u01(0.0, 1.0);

                std::ostringstream lines;
                int64_t total_haul_val = 0;

                for (int i = 0; i < count; ++i) {
                    const OreDef* chosen = pool[ore_dist(rng)];
                    std::uniform_int_distribution<int64_t> vdist(chosen->min_val, chosen->max_val);
                    double raw_v = static_cast<double>(vdist(rng)) * pick->value_mult;
                    raw_v *= (1.0 + prestige * 0.25 + val_rank * 0.02 + pet_val_bonus);

                    // Mastery bonus for this ore species
                    int64_t prev_mastery = g_db->get_species_mastery(msg.author.id, "ore", chosen->name);
                    raw_v *= (1.0 + get_mastery_tier(prev_mastery).bonus);

                    std::string effect_tag;
                    if (!chosen->effect_name.empty() && u01(rng) < chosen->effect_chance) {
                        if (chosen->effect_name == "Jackpot 4x") { raw_v *= 4.0; effect_tag = " *(💥 Jackpot 4x!)*"; }
                        else if (chosen->effect_name == "Wacky 3x") { raw_v *= 3.0; effect_tag = " *(🤪 Wacky 3x!)*"; }
                        else if (chosen->effect_name == "Critical 2x") { raw_v *= 2.0; effect_tag = " *(⚡ Critical 2x!)*"; }
                        else if (chosen->effect_name == "Exponential") { raw_v *= 2.5; effect_tag = " *(🚀 Exponential 2.5x!)*"; }
                        else { raw_v *= 1.5; effect_tag = " *(✨ " + chosen->effect_name + "!)*"; }
                    }

                    int64_t final_v = std::max<int64_t>(1, static_cast<int64_t>(raw_v));
                    total_haul_val += final_v;
                    g_db->add_mined_ore(msg.author.id, chosen->name, 1, final_v);
                    g_db->increment_mastery(msg.author.id, "ore", chosen->name, 1);

                    lines << "• " << chosen->emoji << " **" << chosen->name << "** — " << fmt_coins(final_v) << effect_tag << "\n";
                }

                prof.last_mine = now_s;
                prof.total_ores_mined += count;
                g_db->save_user_mining(prof);
                g_db->add_equipped_pet_xp(msg.author.id, count * 8);

                std::string today = get_est_date_str(0);
                ensure_today_challenges(msg.author.id, today);
                g_db->increment_daily_challenge_progress(msg.author.id, today, "mine_ores", count);

                stoatpp::models::MessagePayload payload;
                stoatpp::models::Embed embed;
                embed.set_title(pick->emoji + " Mining Haul — " + pick->name);
                embed.set_colour("#5865F2");
                embed.set_description("👤 **Miner:** <@" + msg.author.id + ">\n\n" +
                                      lines.str() +
                                      "\n💎 **Estimated Haul Value:** **" + fmt_coins(total_haul_val) + "**\n"
                                      "*Sell ores with `.sellore` or view bag with `.minv` | Next swing in " + std::to_string(cd) + "s*");
                payload.add_embed(embed);
                bot.send_message(msg.channel_id, payload);
            },
            "Economy"
        });

        // ── 2. .minv [@user] (oreinv, ores) ──────────────────────────────────
        bot.register_command({
            "minv",
            {"oreinv", "ores"},
            "view your mined ore inventory and equipped pickaxe.",
            "[@user]",
            {"[@user]"},
            [](stoatpp::cluster& bot, const stoatpp::events::Message& msg, const std::vector<std::string>& args) {
                if (!g_db) return;
                std::string uid = msg.author.id;
                if (!args.empty()) {
                    std::string c = helpers::clean_user_id(args[0]);
                    if (!c.empty()) uid = c;
                }
                auto prof = g_db->get_user_mining(uid);
                const PickaxeDef* pick = find_pickaxe(prof.pickaxe_id);
                if (!pick) pick = &get_pickaxes()[0];
                auto ores = g_db->get_user_ores(uid);

                int64_t total_qty = 0, total_val = 0;
                std::ostringstream oss;
                oss << "👤 **Miner:** <@" << uid << ">\n"
                    << "⛏️ **Equipped Pickaxe:** " << pick->emoji << " **" << pick->name << "** (Lv." << pick->level << ", `" << pick->value_mult << "x`)\n"
                    << "📊 **Lifetime Ores Mined:** **" << prof.total_ores_mined << "** | **Sold:** **" << fmt_coins(prof.total_ore_value_sold) << "**\n\n";

                if (ores.empty()) {
                    oss << "*Your ore bag is empty! Use `.mine` to start digging.*";
                } else {
                    for (size_t i = 0; i < ores.size() && i < 20; ++i) {
                        total_qty += ores[i].quantity;
                        total_val += ores[i].total_value;
                        oss << "• **" << ores[i].ore_name << "** ×" << ores[i].quantity << " — " << fmt_coins(ores[i].total_value) << "\n";
                    }
                    oss << "\n💰 **Total Bag Value (" << total_qty << " ores):** **" << fmt_coins(total_val) << "**";
                }

                stoatpp::models::MessagePayload payload;
                stoatpp::models::Embed embed;
                embed.set_title("⛏️ Mining Inventory");
                embed.set_colour("#5865F2");
                embed.set_description(oss.str());
                payload.add_embed(embed);
                bot.send_message(msg.channel_id, payload);
            },
            "Economy"
        });

        // ── 3. .sellore [all|ore_name] ───────────────────────────────────────
        bot.register_command({
            "sellore",
            {"sello"},
            "sell your mined ores for coins.",
            "[all|ore_name]",
            {"[ore_name]"},
            [](stoatpp::cluster& bot, const stoatpp::events::Message& msg, const std::vector<std::string>& args) {
                if (!g_db) return;
                std::string filter;
                if (!args.empty() && args[0] != "all") {
                    for (size_t i = 0; i < args.size(); ++i) filter += (i ? " " : "") + args[i];
                }
                int64_t earned = g_db->sell_user_ores(msg.author.id, filter);
                if (earned <= 0) {
                    bot.send_reply(msg, bronx::emj_no + " No matching ores in your bag to sell! Use `.mine` first.", true);
                    return;
                }
                g_db->update_balances(msg.author.id, earned, 0);

                std::string today = get_est_date_str(0);
                ensure_today_challenges(msg.author.id, today);
                g_db->increment_daily_challenge_progress(msg.author.id, today, "sell_ores", earned);

                bot.send_reply(msg, bronx::emj_yes + " Sold your " + (filter.empty() ? "mined ores" : ("**" + filter + "**")) +
                                    " for **" + fmt_coins(earned) + "**!", true);
            },
            "Economy"
        });

        // ── 4. .pickaxe [shop|buy|equip] [id] ────────────────────────────────
        bot.register_command({
            "pickaxe",
            {"pickaxes", "pick"},
            "browse, buy, or equip mining pickaxes.",
            "[shop|buy|equip] [pickaxe_id]",
            {"[subcommand]", "[id]"},
            [](stoatpp::cluster& bot, const stoatpp::events::Message& msg, const std::vector<std::string>& args) {
                if (!g_db) return;
                std::string sub = args.empty() ? "shop" : args[0];
                std::transform(sub.begin(), sub.end(), sub.begin(), ::tolower);

                auto prof = g_db->get_user_mining(msg.author.id);
                auto unlocked = g_db->get_unlocked_pickaxes(msg.author.id);
                std::set<std::string> owned_set(unlocked.begin(), unlocked.end());

                if (sub == "shop" || sub == "list") {
                    std::ostringstream oss;
                    for (const auto& p : get_pickaxes()) {
                        bool own = owned_set.count(p.id) > 0;
                        bool eq = (prof.pickaxe_id == p.id);
                        oss << p.emoji << " **" << p.name << "** (`" << p.id << "`) — "
                            << (p.cost == 0 ? "Free" : fmt_coins(p.cost));
                        if (p.req_prestige > 0) oss << " *(P" << p.req_prestige << "+)*";
                        if (eq) oss << " **[EQUIPPED]**";
                        else if (own) oss << " *[OWNED]*";
                        oss << "\n  ↳ `" << p.value_mult << "x` value | " << p.min_yield << "–" << p.max_yield << " ores | " << p.cooldown_sec << "s CD — " << p.description << "\n";
                    }
                    oss << "\n💡 Use `.pickaxe buy <id>` or `.pickaxe equip <id>`!";

                    stoatpp::models::MessagePayload payload;
                    stoatpp::models::Embed embed;
                    embed.set_title("⛏️ Bronx Pickaxe Shop");
                    embed.set_colour("#5865F2");
                    embed.set_description(oss.str());
                    payload.add_embed(embed);
                    bot.send_message(msg.channel_id, payload);
                    return;
                }

                if ((sub == "buy" || sub == "equip") && args.size() < 2) {
                    bot.send_reply(msg, "Usage: `.pickaxe " + sub + " <pickaxe_id>`", true);
                    return;
                }

                std::string target_id = (sub == "buy" || sub == "equip") ? args[1] : args[0];
                const PickaxeDef* p = find_pickaxe(target_id);
                if (!p) {
                    bot.send_reply(msg, bronx::emj_no + " Unknown pickaxe `" + target_id + "`. Check `.pickaxe shop`.", true);
                    return;
                }

                if (sub == "equip" || owned_set.count(p->id)) {
                    if (!owned_set.count(p->id)) {
                        bot.send_reply(msg, bronx::emj_no + " You don't own the **" + p->name + "** yet! Buy it with `.pickaxe buy " + p->id + "`.", true);
                        return;
                    }
                    prof.pickaxe_id = p->id;
                    prof.pickaxe_level = p->level;
                    g_db->save_user_mining(prof);
                    bot.send_reply(msg, bronx::emj_yes + " Equipped " + p->emoji + " **" + p->name + "**!", true);
                    return;
                }

                auto eco = g_db->get_economy_user(msg.author.id);
                if (!eco) return;
                if (eco->prestige < p->req_prestige) {
                    bot.send_reply(msg, bronx::emj_no + " **" + p->name + "** requires **Prestige " + std::to_string(p->req_prestige) + "+**!", true);
                    return;
                }
                if (eco->wallet < p->cost) {
                    bot.send_reply(msg, bronx::emj_no + " You need **" + fmt_coins(p->cost) + "** in your wallet to buy **" + p->name + "**.", true);
                    return;
                }

                g_db->update_balances(msg.author.id, -p->cost, 0);
                g_db->unlock_pickaxe(msg.author.id, p->id);
                prof.pickaxe_id = p->id;
                prof.pickaxe_level = p->level;
                g_db->save_user_mining(prof);

                bot.send_reply(msg, "🎉 Purchased and equipped " + p->emoji + " **" + p->name + "** for **" + fmt_coins(p->cost) + "**!", true);
            },
            "Economy"
        });

        // ── 5. .craft [recipe_id] & .forge ───────────────────────────────────
        bot.register_command({
            "craft",
            {"recipes", "recipe", "forge"},
            "craft gear, pickaxes, and utility items from mined ores.",
            "[recipe_id]",
            {"[recipe_id]"},
            [](stoatpp::cluster& bot, const stoatpp::events::Message& msg, const std::vector<std::string>& args) {
                if (!g_db) return;
                if (args.empty() || args[0] == "list" || args[0] == "recipes") {
                    std::ostringstream oss;
                    for (const auto& r : get_craft_recipes()) {
                        oss << r.emoji << " **" << r.name << "** (`" << r.id << "`) — *" << r.category << "*\n  ↳ **Requires:** ";
                        for (size_t i = 0; i < r.ore_reqs.size(); ++i) {
                            oss << r.ore_reqs[i].second << "x `" << r.ore_reqs[i].first << "`" << (i + 1 < r.ore_reqs.size() ? ", " : "");
                        }
                        if (r.coin_cost > 0) oss << " + " << fmt_coins(r.coin_cost);
                        if (r.req_prestige > 0) oss << " *(P" << r.req_prestige << "+)*";
                        oss << "\n  ↳ " << r.description << "\n";
                    }
                    oss << "\n💡 Craft an item with `.craft <recipe_id>`!";

                    stoatpp::models::MessagePayload payload;
                    stoatpp::models::Embed embed;
                    embed.set_title("⚒️ Bronx Crafting & Forge");
                    embed.set_colour("#E67E22");
                    embed.set_description(oss.str());
                    payload.add_embed(embed);
                    bot.send_message(msg.channel_id, payload);
                    return;
                }

                std::string q = args[0];
                std::transform(q.begin(), q.end(), q.begin(), ::tolower);
                const CraftRecipeDef* rec = nullptr;
                for (const auto& r : get_craft_recipes()) {
                    if (r.id == q) { rec = &r; break; }
                }
                if (!rec) {
                    bot.send_reply(msg, bronx::emj_no + " Unknown recipe `" + q + "`. Check `.craft` for all recipes.", true);
                    return;
                }

                auto eco = g_db->get_economy_user(msg.author.id);
                if (!eco) return;
                if (eco->prestige < rec->req_prestige) {
                    bot.send_reply(msg, bronx::emj_no + " This recipe requires **Prestige " + std::to_string(rec->req_prestige) + "+**.", true);
                    return;
                }
                if (eco->wallet < rec->coin_cost) {
                    bot.send_reply(msg, bronx::emj_no + " You need **" + fmt_coins(rec->coin_cost) + "** in your wallet to craft this.", true);
                    return;
                }

                auto user_ores = g_db->get_user_ores(msg.author.id);
                std::map<std::string, int64_t> ore_counts;
                for (const auto& o : user_ores) ore_counts[o.ore_name] = o.quantity;

                for (const auto& [oname, req_q] : rec->ore_reqs) {
                    if (ore_counts[oname] < req_q) {
                        bot.send_reply(msg, bronx::emj_no + " Missing ingredient: you need **" + std::to_string(req_q) + "x " + oname + "** (you have " + std::to_string(ore_counts[oname]) + ").", true);
                        return;
                    }
                }

                for (const auto& [oname, req_q] : rec->ore_reqs) {
                    g_db->consume_user_ore(msg.author.id, oname, req_q);
                }
                if (rec->coin_cost > 0) g_db->update_balances(msg.author.id, -rec->coin_cost, 0);

                if (rec->reward_type == "pickaxe") {
                    g_db->unlock_pickaxe(msg.author.id, rec->reward_id);
                    auto prof = g_db->get_user_mining(msg.author.id);
                    if (const auto* pk = find_pickaxe(rec->reward_id)) {
                        prof.pickaxe_id = pk->id;
                        prof.pickaxe_level = pk->level;
                        g_db->save_user_mining(prof);
                    }
                } else if (rec->reward_id == "bank_note") {
                    g_db->prestige_user(msg.author.id, eco->bank_limit + 1500000);
                    g_db->update_balances(msg.author.id, eco->wallet - rec->coin_cost, eco->bank);
                } else {
                    g_db->add_user_economy_item(msg.author.id, rec->reward_id, rec->reward_qty, 1, 0);
                }

                bot.send_reply(msg, "⚒️" + bronx::emj_yes + " Crafted " + rec->emoji + " **" + rec->name + "**!", true);
            },
            "Economy"
        });

        // ── 6. .pet (pets) ───────────────────────────────────────────────────
        bot.register_command({
            "pet",
            {"pets"},
            "adopt, feed, equip, and level up companion pets for passive bonuses.",
            "[shop|adopt|feed|equip|rename|release] [args...]",
            {"[subcommand]", "[args...]"},
            [](stoatpp::cluster& bot, const stoatpp::events::Message& msg, const std::vector<std::string>& args) {
                if (!g_db) return;
                std::string sub = args.empty() ? "list" : args[0];
                std::transform(sub.begin(), sub.end(), sub.begin(), ::tolower);

                if (sub == "shop") {
                    std::ostringstream oss;
                    for (const auto& sp : get_pet_species()) {
                        oss << sp.emoji << " **" << sp.name << "** (`" << sp.id << "`) — *" << sp.rarity << "* — **" << fmt_coins(sp.adopt_cost) << "**\n"
                            << "  ↳ `+" << sp.base_bonus << "%` (+" << sp.bonus_per_level << "%/lv, max Lv." << sp.max_level << ") — " << sp.description << "\n";
                    }
                    oss << "\n💡 Adopt a companion with `.pet adopt <species_id> [nickname]`!";
                    stoatpp::models::MessagePayload payload;
                    stoatpp::models::Embed embed;
                    embed.set_title("🐾 Bronx Pet Adoption Center");
                    embed.set_colour("#57F287");
                    embed.set_description(oss.str());
                    payload.add_embed(embed);
                    bot.send_message(msg.channel_id, payload);
                    return;
                }

                if (sub == "adopt" || sub == "buy") {
                    if (args.size() < 2) {
                        bot.send_reply(msg, "Usage: `.pet adopt <species_id> [nickname]` (see `.pet shop`)", true);
                        return;
                    }
                    const auto* sp = find_pet_species(args[1]);
                    if (!sp) {
                        bot.send_reply(msg, bronx::emj_no + " Unknown pet species `" + args[1] + "`. Check `.pet shop`.", true);
                        return;
                    }
                    auto eco = g_db->get_economy_user(msg.author.id);
                    if (!eco) return;
                    if (sp->rarity == "Prestige" && eco->prestige < 1) {
                        bot.send_reply(msg, bronx::emj_no + " **" + sp->name + "** requires **Prestige 1+**!", true);
                        return;
                    }
                    if (eco->wallet < sp->adopt_cost) {
                        bot.send_reply(msg, bronx::emj_no + " You need **" + fmt_coins(sp->adopt_cost) + "** in your wallet to adopt a **" + sp->name + "**.", true);
                        return;
                    }
                    std::string nick = (args.size() >= 3) ? args[2] : sp->name;
                    g_db->update_balances(msg.author.id, -sp->adopt_cost, 0);
                    g_db->adopt_user_pet(msg.author.id, sp->id, nick);
                    bot.send_reply(msg, "🐾🎉 You adopted " + sp->emoji + " **" + nick + "** (" + sp->name + ") for **" + fmt_coins(sp->adopt_cost) + "**!", true);
                    return;
                }

                auto my_pets = g_db->get_user_companion_pets(msg.author.id);
                if (sub == "feed" || sub == "equip" || sub == "release" || sub == "rename") {
                    if (args.size() < 2) {
                        bot.send_reply(msg, "Usage: `.pet " + sub + " <pet_id>`", true);
                        return;
                    }
                    int64_t pid = 0;
                    try { pid = std::stoll(args[1]); } catch (...) {}
                    if (sub == "feed") {
                        auto eco = g_db->get_economy_user(msg.author.id);
                        if (!eco || eco->wallet < 2500) {
                            bot.send_reply(msg, bronx::emj_no + " Feeding a pet costs **$2,500**.", true);
                            return;
                        }
                        if (g_db->feed_user_pet(msg.author.id, pid)) {
                            g_db->update_balances(msg.author.id, -2500, 0);
                            bot.send_reply(msg, "🍖 Fed pet **#" + std::to_string(pid) + "** to 100% fullness!", true);
                        } else {
                            bot.send_reply(msg, bronx::emj_no + " Pet not found.", true);
                        }
                        return;
                    }
                    if (sub == "equip") {
                        if (g_db->equip_user_pet(msg.author.id, pid)) {
                            bot.send_reply(msg, bronx::emj_yes + " Equipped pet **#" + std::to_string(pid) + "**!", true);
                        } else {
                            bot.send_reply(msg, bronx::emj_no + " Pet not found.", true);
                        }
                        return;
                    }
                    if (sub == "rename") {
                        if (args.size() < 3) {
                            bot.send_reply(msg, "Usage: `.pet rename <pet_id> <new_nickname>`", true);
                            return;
                        }
                        if (g_db->rename_user_pet(msg.author.id, pid, args[2])) {
                            bot.send_reply(msg, bronx::emj_yes + " Renamed pet **#" + std::to_string(pid) + "** to **" + args[2] + "**!", true);
                        } else {
                            bot.send_reply(msg, bronx::emj_no + " Pet not found.", true);
                        }
                        return;
                    }
                    if (sub == "release") {
                        if (g_db->release_user_pet(msg.author.id, pid)) {
                            bot.send_reply(msg, "👋 Released pet **#" + std::to_string(pid) + "**.", true);
                        } else {
                            bot.send_reply(msg, bronx::emj_no + " Pet not found.", true);
                        }
                        return;
                    }
                }

                std::ostringstream oss;
                oss << "👤 **Owner:** <@" << msg.author.id << ">\n\n";
                if (my_pets.empty()) {
                    oss << "*You don't own any pets yet! Browse `.pet shop` and adopt one with `.pet adopt <species>`.*";
                } else {
                    for (const auto& pt : my_pets) {
                        const auto* sp = find_pet_species(pt.species_id);
                        std::string em = sp ? sp->emoji : "🐾";
                        double bonus = sp ? (sp->base_bonus + (pt.level - 1) * sp->bonus_per_level) : 0.0;
                        oss << "• `#" << pt.pet_id << "` " << em << " **" << pt.nickname << "** (Lv." << pt.level
                            << " — " << pt.xp << "/" << (pt.level * 50) << " XP) — `+" << std::fixed << std::setprecision(1) << bonus << "%`";
                        if (pt.equipped) oss << " **[EQUIPPED]**";
                        oss << "\n";
                    }
                    oss << "\n💡 Commands: `.pet shop`, `.pet equip <id>`, `.pet feed <id>`, `.pet rename <id> <name>`";
                }

                stoatpp::models::MessagePayload payload;
                stoatpp::models::Embed embed;
                embed.set_title("🐾 Your Companion Pets");
                embed.set_colour("#57F287");
                embed.set_description(oss.str());
                payload.add_embed(embed);
                bot.send_message(msg.channel_id, payload);
            },
            "Economy"
        });

        // ── 7. .skills (skilltree, st) ───────────────────────────────────────
        bot.register_command({
            "skills",
            {"skilltree", "st"},
            "invest Prestige Points into Angler, Prospector, and Gambler skill trees.",
            "[invest|respec|info] [skill_id]",
            {"[subcommand]", "[skill_id]"},
            [](stoatpp::cluster& bot, const stoatpp::events::Message& msg, const std::vector<std::string>& args) {
                if (!g_db) return;
                auto eco = g_db->get_economy_user(msg.author.id);
                if (!eco) return;

                int total_pp = calc_total_skill_points(eco->prestige);
                auto ranks = g_db->get_user_skills(msg.author.id);
                int spent_pp = 0;
                for (const auto& [k, v] : ranks) spent_pp += v;
                int avail_pp = std::max(0, total_pp - spent_pp);

                if (!args.empty()) {
                    std::string sub = args[0];
                    std::transform(sub.begin(), sub.end(), sub.begin(), ::tolower);

                    if (sub == "respec" || sub == "reset") {
                        int64_t cost = 500000;
                        if (eco->wallet < cost) {
                            bot.send_reply(msg, bronx::emj_no + " Respeccing your skill tree costs **$500,000**.", true);
                            return;
                        }
                        g_db->update_balances(msg.author.id, -cost, 0);
                        g_db->clear_user_skills(msg.author.id);
                        bot.send_reply(msg, "🔄" + bronx::emj_yes + " All skill points have been refunded!", true);
                        return;
                    }

                    if (sub == "invest" || sub == "upgrade" || sub == "buy") {
                        if (args.size() < 2) {
                            bot.send_reply(msg, "Usage: `.skills invest <skill_id>`", true);
                            return;
                        }
                        if (avail_pp <= 0) {
                            bot.send_reply(msg, bronx::emj_no + " You have **0** Prestige Points available! Prestige with `.prestige` to earn more.", true);
                            return;
                        }
                        std::string sid = args[1];
                        std::transform(sid.begin(), sid.end(), sid.begin(), ::tolower);
                        const SkillNodeDef* target = nullptr;
                        for (const auto& n : get_skill_nodes()) {
                            if (n.id == sid) { target = &n; break; }
                        }
                        if (!target) {
                            bot.send_reply(msg, bronx::emj_no + " Unknown skill `" + sid + "`. View all skill IDs with `.skills`.", true);
                            return;
                        }
                        int cur_r = ranks[target->id];
                        if (cur_r >= target->max_rank) {
                            bot.send_reply(msg, bronx::emj_no + " **" + target->name + "** is already at max rank (" + std::to_string(target->max_rank) + "/" + std::to_string(target->max_rank) + ")!", true);
                            return;
                        }
                        g_db->set_user_skill_rank(msg.author.id, target->id, cur_r + 1);
                        bot.send_reply(msg, "🌟" + bronx::emj_yes + " Upgraded " + target->emoji + " **" + target->name + "** to rank **" + std::to_string(cur_r + 1) + "/" + std::to_string(target->max_rank) + "**!", true);
                        return;
                    }
                }

                std::ostringstream oss;
                oss << "👤 **Player:** <@" << msg.author.id << "> | **Prestige:** **" << eco->prestige << "**\n"
                    << "✨ **Prestige Points:** **" << avail_pp << " available** (" << spent_pp << "/" << total_pp << " spent)\n\n";

                for (const char* br : {"angler", "prospector", "gambler"}) {
                    std::string btitle = (std::string(br) == "angler") ? "🎣 Angler Branch"
                                       : (std::string(br) == "prospector") ? "⛏️ Prospector Branch" : "🎰 Gambler Branch";
                    oss << "**" << btitle << "**\n";
                    for (const auto& n : get_skill_nodes()) {
                        if (n.branch != br) continue;
                        int r = ranks[n.id];
                        oss << "• " << n.emoji << " **" << n.name << "** (`" << n.id << "`) — **[" << r << "/" << n.max_rank << "]** — " << n.description << "\n";
                    }
                    oss << "\n";
                }
                oss << "💡 Use `.skills invest <skill_id>` or `.skills respec`!";

                stoatpp::models::MessagePayload payload;
                stoatpp::models::Embed embed;
                embed.set_title("🌳 Prestige Skill Trees");
                embed.set_colour("#9B59B6");
                embed.set_description(oss.str());
                payload.add_embed(embed);
                bot.send_message(msg.channel_id, payload);
            },
            "Economy"
        });

        // ── 8. .mastery [@user] [fish|ore] ───────────────────────────────────
        bot.register_command({
            "mastery",
            {"mst"},
            "view your species mastery progress and permanent value bonuses.",
            "[fish|ore]",
            {"[category]"},
            [](stoatpp::cluster& bot, const stoatpp::events::Message& msg, const std::vector<std::string>& args) {
                if (!g_db) return;
                std::string cat = (!args.empty() && (args[0] == "fish" || args[0] == "fishing")) ? "fish" : "ore";
                auto counts = g_db->get_category_mastery(msg.author.id, cat);

                std::vector<std::pair<std::string, int64_t>> sorted(counts.begin(), counts.end());
                std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });

                std::ostringstream oss;
                oss << "👤 **Player:** <@" << msg.author.id << "> | **Category:** **" << (cat == "fish" ? "🎣 Fish" : "⛏️ Ores") << "**\n"
                    << "*Tiers: Novice (1) ➔ Apprentice (10, +1%) ➔ Journeyman (25, +2%) ➔ Expert (50, +3%) ➔ Master (100, +5%) ➔ Grandmaster (250, +7%) ➔ Legend (500, +10%) ➔ Mythic (1000, +15%)*\n\n";

                if (sorted.empty()) {
                    oss << "*No mastery progress recorded in this category yet! Use `.mine` or `.fish`.*";
                } else {
                    for (size_t i = 0; i < sorted.size() && i < 18; ++i) {
                        const auto& t = get_mastery_tier(sorted[i].second);
                        oss << "• " << t.emoji << " **" << sorted[i].first << "** — **" << sorted[i].second << "** (" << t.name
                            << ", `+" << static_cast<int>(t.bonus * 100) << "%` value)\n";
                    }
                }

                stoatpp::models::MessagePayload payload;
                stoatpp::models::Embed embed;
                embed.set_title("🏅 Bronx Species Mastery");
                embed.set_colour("#F1C40F");
                embed.set_description(oss.str());
                payload.add_embed(embed);
                bot.send_message(msg.channel_id, payload);
            },
            "Economy"
        });

        // ── 9. .daily & .streak ──────────────────────────────────────────────
        bot.register_command({
            "daily",
            {},
            "claim your daily coin reward and build your login streak!",
            "",
            {},
            [](stoatpp::cluster& bot, const stoatpp::events::Message& msg, const std::vector<std::string>&) {
                if (!g_db) return;
                std::string today = get_est_date_str(0);
                std::string yesterday = get_est_date_str(-1);
                auto st = g_db->get_user_streak(msg.author.id);

                if (st.last_claim_date == today) {
                    bot.send_reply(msg, bronx::emj_no + " You already claimed your daily reward today! Come back tomorrow (current streak: **" + std::to_string(st.current_streak) + " days**).", true);
                    return;
                }

                st.current_streak = (st.last_claim_date == yesterday) ? (st.current_streak + 1) : 1;
                st.longest_streak = std::max(st.longest_streak, st.current_streak);
                st.last_claim_date = today;
                st.total_claims++;

                int64_t base_reward = 25000 + std::min(st.current_streak, 100) * 2500;
                int64_t milestone_bonus = 0;
                if (st.current_streak == 3) milestone_bonus = 5000;
                else if (st.current_streak == 7) milestone_bonus = 50000;
                else if (st.current_streak == 14) milestone_bonus = 200000;
                else if (st.current_streak == 21) milestone_bonus = 500000;
                else if (st.current_streak == 30) milestone_bonus = 1000000;
                else if (st.current_streak == 60) milestone_bonus = 5000000;
                else if (st.current_streak == 90) milestone_bonus = 10000000;
                else if (st.current_streak == 180) milestone_bonus = 50000000;
                else if (st.current_streak == 365) milestone_bonus = 250000000;

                st.total_streak_bonus += milestone_bonus;
                g_db->save_user_streak(msg.author.id, st);
                g_db->update_balances(msg.author.id, base_reward + milestone_bonus, 0);

                ensure_today_challenges(msg.author.id, today);
                g_db->increment_daily_challenge_progress(msg.author.id, today, "claim_daily", 1);

                std::string desc = "👤 **Player:** <@" + msg.author.id + ">\n"
                                   "💵 **Daily Reward:** **" + fmt_coins(base_reward) + "**\n"
                                   "🔥 **Current Streak:** **" + std::to_string(st.current_streak) + " day(s)** (Longest: " + std::to_string(st.longest_streak) + ")\n";
                if (milestone_bonus > 0) {
                    desc += "\n🎉 **STREAK MILESTONE BONUS:** **+" + fmt_coins(milestone_bonus) + "**!";
                }

                stoatpp::models::MessagePayload payload;
                stoatpp::models::Embed embed;
                embed.set_title("📅 Daily Reward Claimed!");
                embed.set_colour("#2ECC71");
                embed.set_description(desc);
                payload.add_embed(embed);
                bot.send_message(msg.channel_id, payload);
            },
            "Economy"
        });

        bot.register_command({
            "streak",
            {"streaks"},
            "view your daily login streak and upcoming milestone bonuses.",
            "",
            {},
            [](stoatpp::cluster& bot, const stoatpp::events::Message& msg, const std::vector<std::string>&) {
                if (!g_db) return;
                auto st = g_db->get_user_streak(msg.author.id);
                std::ostringstream oss;
                oss << "👤 **Player:** <@" << msg.author.id << ">\n"
                    << "🔥 **Current Streak:** **" << st.current_streak << " days**\n"
                    << "🏆 **Longest Streak:** **" << st.longest_streak << " days**\n"
                    << "📅 **Total Daily Claims:** **" << st.total_claims << "**\n"
                    << "💰 **Total Milestone Bonuses Earned:** **" << fmt_coins(st.total_streak_bonus) << "**\n\n"
                    << "**Milestones:** Day 3 ($5K) • Day 7 ($50K) • Day 14 ($200K) • Day 21 ($500K) • Day 30 ($1M) • Day 60 ($5M) • Day 90 ($10M) • Day 180 ($50M) • Day 365 ($250M)";

                stoatpp::models::MessagePayload payload;
                stoatpp::models::Embed embed;
                embed.set_title("🔥 Daily Login Streak");
                embed.set_colour("#E67E22");
                embed.set_description(oss.str());
                payload.add_embed(embed);
                bot.send_message(msg.channel_id, payload);
            },
            "Economy"
        });

        // ── 10. .challenges [claim] (dailychallenges, dc) ────────────────────
        bot.register_command({
            "challenges",
            {"dailychallenges", "dc"},
            "view and claim your 3 daily challenges.",
            "[claim]",
            {"[claim]"},
            [](stoatpp::cluster& bot, const stoatpp::events::Message& msg, const std::vector<std::string>& args) {
                if (!g_db) return;
                std::string today = get_est_date_str(0);
                auto list = ensure_today_challenges(msg.author.id, today);

                if (!args.empty() && (args[0] == "claim" || args[0] == "c")) {
                    int64_t claimed_coins = 0;
                    int claimed_cnt = 0;
                    for (auto& ch : list) {
                        if (!ch.claimed && ch.progress_val >= ch.target_val) {
                            ch.claimed = true;
                            g_db->save_user_daily_challenge(msg.author.id, today, ch);
                            claimed_coins += ch.reward_coins;
                            claimed_cnt++;
                        }
                    }
                    if (claimed_cnt == 0) {
                        bot.send_reply(msg, bronx::emj_no + " No completed unclaimed daily challenges yet!", true);
                        return;
                    }
                    g_db->update_balances(msg.author.id, claimed_coins, 0);
                    bot.send_reply(msg, "🎉" + bronx::emj_yes + " Claimed **" + std::to_string(claimed_cnt) + "** daily challenge(s) for **" + fmt_coins(claimed_coins) + "**!", true);
                    return;
                }

                std::ostringstream oss;
                oss << "👤 **Player:** <@" << msg.author.id << "> | **Date:** `" << today << "`\n\n";
                for (const auto& ch : list) {
                    std::string label = (ch.challenge_id == "mine_ores") ? ("⛏️ Mine " + std::to_string(ch.target_val) + " Ores")
                                      : (ch.challenge_id == "sell_ores") ? ("💎 Sell " + fmt_coins(ch.target_val) + " Worth of Ores")
                                                                         : "📅 Claim Your `.daily` Reward";
                    std::string status = ch.claimed ? "✅ **CLAIMED**"
                                       : (ch.progress_val >= ch.target_val) ? "🎁 **READY TO CLAIM** (`.challenges claim`)"
                                       : ("`" + std::to_string(ch.progress_val) + "/" + std::to_string(ch.target_val) + "`");
                    oss << "• **" << label << "** — Reward: **" << fmt_coins(ch.reward_coins) << "** — " << status << "\n";
                }

                stoatpp::models::MessagePayload payload;
                stoatpp::models::Embed embed;
                embed.set_title("🎯 Daily Challenges");
                embed.set_colour("#5865F2");
                embed.set_description(oss.str());
                payload.add_embed(embed);
                bot.send_message(msg.channel_id, payload);
            },
            "Economy"
        });

        // ── 11. .titles [shop|buy|equip] [id] ────────────────────────────────
        bot.register_command({
            "titles",
            {"title"},
            "browse, purchase, and equip cosmetic profile titles.",
            "[shop|buy|equip] [title_id]",
            {"[subcommand]", "[title_id]"},
            [](stoatpp::cluster& bot, const stoatpp::events::Message& msg, const std::vector<std::string>& args) {
                if (!g_db) return;
                std::string sub = args.empty() ? "shop" : args[0];
                std::transform(sub.begin(), sub.end(), sub.begin(), ::tolower);

                auto owned = g_db->get_user_titles(msg.author.id);
                std::set<std::string> owned_ids;
                std::string equipped_id;
                for (const auto& [tid, eq] : owned) {
                    owned_ids.insert(tid);
                    if (eq) equipped_id = tid;
                }

                if (sub == "shop" || sub == "list") {
                    std::ostringstream oss;
                    for (const auto& t : get_titles_catalog()) {
                        oss << "• " << t.display << " (`" << t.id << "`) — **" << fmt_coins(t.price) << "**";
                        if (t.limit > 0) {
                            int sold = g_db->count_title_owners(t.id);
                            oss << " *[" << sold << "/" << t.limit << " claimed]*";
                        }
                        if (equipped_id == t.id) oss << " **[EQUIPPED]**";
                        else if (owned_ids.count(t.id)) oss << " *[OWNED]*";
                        oss << "\n  ↳ " << t.desc << "\n";
                    }
                    oss << "\n💡 Buy with `.titles buy <id>` or equip with `.titles equip <id>`!";

                    stoatpp::models::MessagePayload payload;
                    stoatpp::models::Embed embed;
                    embed.set_title("🏷️ Bronx Title Shop");
                    embed.set_colour("#F1C40F");
                    embed.set_description(oss.str());
                    payload.add_embed(embed);
                    bot.send_message(msg.channel_id, payload);
                    return;
                }

                if ((sub == "buy" || sub == "equip") && args.size() < 2) {
                    bot.send_reply(msg, "Usage: `.titles " + sub + " <title_id>`", true);
                    return;
                }
                std::string target_id = (sub == "buy" || sub == "equip") ? args[1] : args[0];
                const TitleDef* td = find_title_def(target_id);
                if (!td) {
                    bot.send_reply(msg, bronx::emj_no + " Unknown title `" + target_id + "`. Check `.titles shop`.", true);
                    return;
                }

                if (sub == "equip" || owned_ids.count(td->id)) {
                    if (!owned_ids.count(td->id)) {
                        bot.send_reply(msg, bronx::emj_no + " You don't own **" + td->display + "** yet!", true);
                        return;
                    }
                    g_db->equip_user_title(msg.author.id, td->id);
                    bot.send_reply(msg, bronx::emj_yes + " Equipped title " + td->display + "!", true);
                    return;
                }

                if (td->limit > 0 && g_db->count_title_owners(td->id) >= td->limit) {
                    bot.send_reply(msg, bronx::emj_no + " **" + td->display + "** is SOLD OUT (" + std::to_string(td->limit) + "/" + std::to_string(td->limit) + ")!", true);
                    return;
                }
                auto eco = g_db->get_economy_user(msg.author.id);
                if (!eco || eco->wallet < td->price) {
                    bot.send_reply(msg, bronx::emj_no + " You need **" + fmt_coins(td->price) + "** in your wallet to buy " + td->display + ".", true);
                    return;
                }
                g_db->update_balances(msg.author.id, -td->price, 0);
                g_db->grant_user_title(msg.author.id, td->id);
                g_db->equip_user_title(msg.author.id, td->id);
                bot.send_reply(msg, "🎉 Purchased and equipped title " + td->display + " for **" + fmt_coins(td->price) + "**!", true);
            },
            "Economy"
        });

        // ── 12. .achievements [claim] (ach) ──────────────────────────────────
        bot.register_command({
            "achievements",
            {"ach"},
            "view and claim milestone achievements for coins and exclusive titles.",
            "[claim]",
            {"[claim]"},
            [](stoatpp::cluster& bot, const stoatpp::events::Message& msg, const std::vector<std::string>& args) {
                if (!g_db) return;
                auto mprof = g_db->get_user_mining(msg.author.id);
                auto gstats = g_db->get_gambling_stats(msg.author.id);
                auto streak = g_db->get_user_streak(msg.author.id);
                auto claimed_vec = g_db->get_claimed_achievements(msg.author.id);
                std::set<std::string> claimed_set(claimed_vec.begin(), claimed_vec.end());

                auto get_stat = [&](const std::string& st) -> int64_t {
                    if (st == "ores_mined") return mprof.total_ores_mined;
                    if (st == "ore_sold") return mprof.total_ore_value_sold;
                    if (st == "gamble_wins") return gstats.games_won;
                    if (st == "gamble_profit") return gstats.total_profit;
                    if (st == "streak") return streak.longest_streak;
                    return 0;
                };

                if (!args.empty() && (args[0] == "claim" || args[0] == "c")) {
                    int64_t total_coins = 0;
                    int count = 0;
                    for (const auto& a : get_achievements_catalog()) {
                        if (!claimed_set.count(a.id) && get_stat(a.stat_type) >= a.threshold) {
                            g_db->claim_achievement(msg.author.id, a.id);
                            total_coins += a.reward_coins;
                            if (!a.reward_title_id.empty()) g_db->grant_user_title(msg.author.id, a.reward_title_id);
                            count++;
                        }
                    }
                    if (count == 0) {
                        bot.send_reply(msg, bronx::emj_no + " No newly completed achievements to claim right now!", true);
                        return;
                    }
                    g_db->update_balances(msg.author.id, total_coins, 0);
                    bot.send_reply(msg, "🏆" + bronx::emj_yes + " Claimed **" + std::to_string(count) + "** achievement(s) for **" + fmt_coins(total_coins) + "**!", true);
                    return;
                }

                std::ostringstream oss;
                oss << "👤 **Player:** <@" << msg.author.id << ">\n\n";
                for (const auto& a : get_achievements_catalog()) {
                    int64_t cur = get_stat(a.stat_type);
                    bool done = cur >= a.threshold;
                    bool is_claimed = claimed_set.count(a.id) > 0;
                    oss << "• " << a.emoji << " **" << a.name << "** — " << a.desc << " (" << fmt_coins(a.reward_coins) << ") — "
                        << (is_claimed ? "✅ **CLAIMED**" : (done ? "🎁 **READY (`.ach claim`)**" : ("`" + std::to_string(cur) + "/" + std::to_string(a.threshold) + "`")))
                        << "\n";
                }

                stoatpp::models::MessagePayload payload;
                stoatpp::models::Embed embed;
                embed.set_title("🏆 Bronx Achievements");
                embed.set_colour("#F1C40F");
                embed.set_description(oss.str());
                payload.add_embed(embed);
                bot.send_message(msg.channel_id, payload);
            },
            "Economy"
        });
    }
};

} // namespace bronx::cogs
