#pragma once
#include "stoatpp/bot_module.h"
#include "stoatpp/cluster.h"
#include "../database.h"
#include "../emojis.h"
#include "helpers.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bronx::cogs {

class GamblingModule : public stoatpp::bot_module {
private:
    static constexpr int64_t MAX_BET = 2000000000LL;

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

    static std::string fmt_mult(double m) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.2fx", m);
        return buf;
    }

    static int64_t parse_bet(const std::string& raw, int64_t wallet) {
        if (raw.empty()) return -1;
        std::string s = raw;
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        if (s == "all" || s == "max") return std::min(wallet, MAX_BET);
        if (s == "half") return std::min(wallet / 2, MAX_BET);

        double mult = 1.0;
        if (s.back() == 'k') { mult = 1e3; s.pop_back(); }
        else if (s.back() == 'm') { mult = 1e6; s.pop_back(); }
        else if (s.back() == 'b') { mult = 1e9; s.pop_back(); }
        else if (s.back() == '%') {
            s.pop_back();
            try {
                double pct = std::stod(s);
                if (pct <= 0.0 || pct > 100.0) return -1;
                return std::min(static_cast<int64_t>(wallet * (pct / 100.0)), MAX_BET);
            } catch (...) { return -1; }
        }
        try {
            double v = std::stod(s) * mult;
            if (v < 1.0) return -1;
            return static_cast<int64_t>(v);
        } catch (...) {
            return -1;
        }
    }

    static void finalize_gamble(stoatpp::cluster& bot, const std::string& channel_id, const std::string& user_id, int64_t bet, int64_t net_delta) {
        if (!g_db) return;
        g_db->record_gambling_result(user_id, bet, net_delta);
        if (net_delta < 0) {
            int64_t contrib = std::max<int64_t>(1, (-net_delta) / 100); // 1% of losses feed progressive jackpot
            g_db->contribute_to_jackpot(contrib);
        } else if (net_delta > 0) {
            static thread_local std::mt19937 rng(std::random_device{}());
            std::uniform_real_distribution<double> dist(0.0, 1.0);
            if (dist(rng) < 0.0001) { // 0.01% (1 in 10,000) jackpot win chance
                int64_t jp = g_db->try_win_jackpot(user_id);
                if (jp > 0) {
                    stoatpp::models::MessagePayload payload;
                    stoatpp::models::Embed embed;
                    embed.set_title("🎰 Progressive Jackpot Winner!");
                    embed.set_colour("#F1C40F");
                    embed.set_description("# 🎰💰 PROGRESSIVE JACKPOT WON! 💰🎰\n\n"
                                          "<@" + user_id + "> just hit the **Progressive Jackpot**!\n\n"
                                          "🏆 **Won: " + fmt_coins(jp) + "**\n\n"
                                          "*The jackpot pool has reset to $1,000,000. Every gambling loss contributes 1%!*");
                    payload.add_embed(embed);
                    bot.send_message(channel_id, payload);
                }
            }
        }
    }

    // ── 1. CRASH STATE ────────────────────────────────────────────────────────
    struct CrashGame {
        std::string user_id;
        std::string channel_id;
        std::string message_id;
        int64_t bet = 0;
        double current_mult = 1.00;
        double crash_point = 1.00;
        double auto_cashout = 0.0;
        bool cashed_out = false;
        bool crashed = false;
        bool active = true;
        int tick = 0;
    };
    std::unordered_map<std::string, CrashGame> crash_games_;
    std::mutex crash_mutex_;

    static double generate_crash_point() {
        static thread_local std::mt19937 rng(std::random_device{}());
        std::uniform_real_distribution<double> dis(0.0, 1.0);
        double r = dis(rng);
        if (r < 0.04) return 1.00; // 4% instant bust house edge
        return std::max(1.01, 1.0 + (-std::log(1.0 - r) * 1.5));
    }

    static double next_crash_mult(double m) {
        return 1.0 + (m - 1.0) * 1.08 + 0.025;
    }

    static stoatpp::models::MessagePayload build_crash_payload(const CrashGame& g) {
        stoatpp::models::MessagePayload payload;
        payload.content = " ";
        stoatpp::models::Embed embed;

        if (g.cashed_out) {
            int64_t payout = static_cast<int64_t>(g.bet * g.current_mult);
            int64_t profit = payout - g.bet;
            embed.set_title(bronx::emj_yes + " Cashed out at " + fmt_mult(g.current_mult) + "!");
            embed.set_colour("#2ECC71");
            embed.set_description("👤 **Player:** <@" + g.user_id + ">\n"
                                  "**Bet:** " + fmt_coins(g.bet) + "\n"
                                  "**Cash-out:** `" + fmt_mult(g.current_mult) + "`\n"
                                  "**Payout:** **" + fmt_coins(payout) + "** (+" + fmt_coins(profit) + ")\n\n"
                                  "*Actual crash point was " + fmt_mult(g.crash_point) + "*");
        } else if (g.crashed) {
            embed.set_title("💥 CRASHED at " + fmt_mult(g.crash_point) + "!");
            embed.set_colour("#ED4245");
            embed.set_description("👤 **Player:** <@" + g.user_id + ">\n"
                                  "**Bet:** " + fmt_coins(g.bet) + "\n"
                                  "**Lost:** " + fmt_coins(g.bet) + "\n\n"
                                  "*Better luck next time!*");
        } else {
            int bars = std::min(10, static_cast<int>((g.current_mult - 1.0) * 5.0));
            std::string bar = "`[";
            for (int i = 0; i < 10; ++i) bar += (i < bars) ? "▓" : "░";
            bar += "]`";
            int64_t if_cashout = static_cast<int64_t>(g.bet * g.current_mult);

            embed.set_title("🟢 Crash — Running");
            embed.set_colour("#5865F2");
            std::string desc = "👤 **Player:** <@" + g.user_id + ">\n"
                               "## 🚀 " + fmt_mult(g.current_mult) + "\n" + bar + "\n"
                               "**Bet:** " + fmt_coins(g.bet) + "\n"
                               "**Value Now:** **" + fmt_coins(if_cashout) + "**\n";
            if (g.auto_cashout > 1.0) {
                desc += "**Auto Cash-Out:** `" + fmt_mult(g.auto_cashout) + "`\n";
            }
            desc += "\n💰 *React with 💰 or type `cashout` (`c`) before it crashes!*";
            embed.set_description(desc);
        }
        payload.add_embed(embed);
        return payload;
    }

    // ── 2. MINESWEEPER STATE ──────────────────────────────────────────────────
    struct MinesweeperGame {
        std::string user_id;
        std::string channel_id;
        std::string message_id;
        int64_t initial_bet = 0;
        int rows = 5;
        int cols = 5;
        int total_mines = 4;
        int safe_cells_revealed = 0;
        std::set<int> mine_positions;
        std::set<int> revealed_positions;
        bool active = true;
        bool game_over = false;
        bool won = false;
        std::chrono::steady_clock::time_point created_at;
    };
    std::unordered_map<std::string, MinesweeperGame> mines_games_;
    std::mutex mines_mutex_;

    static double calc_mines_multiplier(int total_cells, int num_mines, int revealed) {
        int safe_cells = total_cells - num_mines;
        if (safe_cells <= 0 || revealed <= 0) return 1.0;
        if (revealed > safe_cells) revealed = safe_cells;
        double mult = 1.0;
        for (int i = 0; i < revealed; ++i) {
            mult *= static_cast<double>(total_cells - i) / static_cast<double>(safe_cells - i);
        }
        return mult * 0.97; // 3% house edge
    }

    static stoatpp::models::MessagePayload build_mines_payload(const MinesweeperGame& game, bool show_all = false) {
        int total_cells = game.rows * game.cols;
        double mult = calc_mines_multiplier(total_cells, game.total_mines, game.safe_cells_revealed);
        int64_t payout = static_cast<int64_t>(game.initial_bet * mult);

        std::ostringstream board;
        board << "```\n";
        for (int r = 0; r < game.rows; ++r) {
            for (int c = 0; c < game.cols; ++c) {
                int pos = r * game.cols + c;
                if (game.revealed_positions.count(pos)) {
                    board << (game.mine_positions.count(pos) ? " 💥 " : " 💎 ");
                } else if (show_all && game.mine_positions.count(pos)) {
                    board << " 💣 ";
                } else {
                    char cell_buf[8];
                    snprintf(cell_buf, sizeof(cell_buf), "[%02d]", pos + 1);
                    board << cell_buf;
                }
            }
            board << "\n";
        }
        board << "```";

        stoatpp::models::MessagePayload payload;
        payload.content = " ";
        stoatpp::models::Embed embed;
        if (game.game_over) {
            if (game.won) {
                embed.set_title("💎 Minesweeper — Cashed Out (" + fmt_mult(mult) + ")!");
                embed.set_colour("#2ECC71");
            } else {
                embed.set_title("💥 Minesweeper — BOOM!");
                embed.set_colour("#ED4245");
            }
        } else {
            embed.set_title("💣 Minesweeper — " + std::to_string(game.total_mines) + " Mines");
            embed.set_colour("#5865F2");
        }

        std::string desc = "👤 **Player:** <@" + game.user_id + ">\n"
                           "**Bet:** " + fmt_coins(game.initial_bet) + " | **Multiplier:** `" + fmt_mult(mult) + "`\n"
                           "**Current Payout:** **" + fmt_coins(payout) + "** (" + std::to_string(game.safe_cells_revealed) + "/" +
                           std::to_string(total_cells - game.total_mines) + " safe tiles)\n\n" +
                           board.str();
        if (game.active && !game.game_over) {
            desc += "\n💡 *Type tile numbers `1`–`25` (e.g. `5` or `1 7 13`) to reveal, or type `cashout` (`c`) / react 💰 to cash out!*";
        }
        embed.set_description(desc);
        payload.add_embed(embed);
        return payload;
    }

    // ── 3. FROGGER STATE ──────────────────────────────────────────────────────
    struct FroggerGame {
        std::string user_id;
        std::string channel_id;
        std::string message_id;
        int64_t initial_bet = 0;
        int logs_hopped = 0;
        int difficulty = 1; // 1=easy, 2=medium, 3=hard
        int frog_lane = 1;  // 0=left, 1=middle, 2=right
        bool active = true;
        bool cashed_out = false;
        bool crashed = false;
        std::vector<std::array<bool, 3>> upcoming_logs;
    };
    std::unordered_map<std::string, FroggerGame> frogger_games_;
    std::mutex frogger_mutex_;

    static std::array<bool, 3> make_frogger_row(int difficulty) {
        static thread_local std::mt19937 gen(std::random_device{}());
        std::uniform_int_distribution<int> dis(1, 100);
        int crack_chance = (difficulty == 1) ? 20 : (difficulty == 2) ? 35 : 45;
        std::array<bool, 3> row = {
            dis(gen) > crack_chance,
            dis(gen) > crack_chance,
            dis(gen) > crack_chance
        };
        if (!row[0] && !row[1] && !row[2]) {
            row[gen() % 3] = true;
        }
        return row;
    }

    static double frogger_mult(int difficulty, int logs_hopped) {
        double per_log = (difficulty == 1) ? 0.15 : (difficulty == 2) ? 0.25 : 0.40;
        return 1.0 + logs_hopped * per_log;
    }

    static stoatpp::models::MessagePayload build_frogger_payload(const FroggerGame& g) {
        double mult = frogger_mult(g.difficulty, g.logs_hopped);
        double per_log = (g.difficulty == 1) ? 0.15 : (g.difficulty == 2) ? 0.25 : 0.40;
        int64_t payout = static_cast<int64_t>(g.initial_bet * mult);
        std::string diff_name = (g.difficulty == 1) ? "Easy" : (g.difficulty == 2) ? "Medium" : "Hard";

        stoatpp::models::MessagePayload payload;
        payload.content = " ";
        stoatpp::models::Embed embed;

        std::string desc = "👤 **Player:** <@" + g.user_id + ">\n"
                           "**Difficulty:** " + diff_name + " | **Bet:** " + fmt_coins(g.initial_bet) + "\n\n";

        for (int i = 4; i >= 0; --i) {
            desc += "│ ";
            for (int j = 0; j < 3; ++j) {
                if (i == 0 && j == g.frog_lane) {
                    desc += (g.crashed ? "💦 │ " : "🐸 │ ");
                } else {
                    desc += "🪵 │ ";
                }
            }
            double row_m = 1.0 + (g.logs_hopped + i) * per_log;
            desc += "`" + fmt_mult(row_m) + "`\n";
        }

        if (g.cashed_out) {
            embed.set_title("🐸 Frogger — Cashed Out at " + fmt_mult(mult) + "!");
            embed.set_colour("#2ECC71");
            desc += "\n" + bronx::emj_yes + " **Payout:** **" + fmt_coins(payout) + "** (+" + fmt_coins(payout - g.initial_bet) + ")";
        } else if (g.crashed) {
            embed.set_title("💦 Frogger — Log Cracked!");
            embed.set_colour("#ED4245");
            desc += "\n💥 The log snapped after **" + std::to_string(g.logs_hopped) + "** hops! You lost **" + fmt_coins(g.initial_bet) + "**.";
        } else {
            embed.set_title("🐸 Frogger | Hops: " + std::to_string(g.logs_hopped) + " | " + fmt_mult(mult));
            embed.set_colour("#57F287");
            desc += "\n💰 **Current Payout:** **" + fmt_coins(payout) + "**\n"
                    "React ⬅️ / ⬆️ / ➡️ (or type `left` / `middle` / `right`) to hop, or 💰 (`cashout`) to take your winnings!";
        }
        embed.set_description(desc);
        payload.add_embed(embed);
        return payload;
    }

    // ── 4. VIDEO POKER STATE ──────────────────────────────────────────────────
    struct PokerCard {
        int rank; // 2..14 (14=Ace)
        int suit; // 0=♠, 1=♥, 2=♦, 3=♣
        std::string str() const {
            static const char* rnames[] = {"", "", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K", "A"};
            static const char* snames[] = {"♠", "♥", "♦", "♣"};
            return std::string("`[") + rnames[rank] + snames[suit] + "]`";
        }
    };

    struct VideoPokerSession {
        std::string user_id;
        std::string channel_id;
        std::string message_id;
        int64_t bet = 0;
        std::vector<PokerCard> hand;
        std::vector<PokerCard> deck;
        bool active = true;
    };
    std::unordered_map<std::string, VideoPokerSession> poker_sessions_;
    std::mutex poker_mutex_;

    static std::pair<std::string, double> evaluate_video_poker(const std::vector<PokerCard>& hand) {
        std::vector<int> ranks;
        std::map<int, int> counts;
        bool flush = true;
        for (size_t i = 0; i < hand.size(); ++i) {
            ranks.push_back(hand[i].rank);
            counts[hand[i].rank]++;
            if (i > 0 && hand[i].suit != hand[0].suit) flush = false;
        }
        std::sort(ranks.begin(), ranks.end());

        bool straight = false;
        if (counts.size() == 5) {
            if (ranks[4] - ranks[0] == 4) straight = true;
            else if (ranks == std::vector<int>{2, 3, 4, 5, 14}) straight = true;
        }

        if (flush && straight) {
            if (ranks[0] == 10 && ranks[4] == 14) return {"👑 ROYAL FLUSH", 250.0};
            return {"🌟 Straight Flush", 50.0};
        }
        std::vector<int> freq;
        for (const auto& kv : counts) freq.push_back(kv.second);
        std::sort(freq.rbegin(), freq.rend());

        if (freq[0] == 4) return {"🔥 Four of a Kind", 25.0};
        if (freq[0] == 3 && freq[1] == 2) return {"🏠 Full House", 9.0};
        if (flush) return {"♠️ Flush", 6.0};
        if (straight) return {"📏 Straight", 4.0};
        if (freq[0] == 3) return {"🎲 Three of a Kind", 3.0};
        if (freq[0] == 2 && freq[1] == 2) return {"✌️ Two Pair", 2.0};
        for (const auto& kv : counts) {
            if (kv.second == 2 && kv.first >= 11) return {"🃏 Jacks or Better", 1.5};
        }
        return {"High Card", 0.0};
    }

    // ── 5. TIC-TAC-TOE STATE ──────────────────────────────────────────────────
    struct TicTacToeSession {
        std::string p1_id;
        std::string p2_id; // "bot" for AI
        std::string channel_id;
        std::string message_id;
        int64_t bet = 0;
        std::array<int, 9> board = {0, 0, 0, 0, 0, 0, 0, 0, 0}; // 0=empty, 1=X, 2=O, 3=wildcard
        int turn = 1; // 1=p1, 2=p2
        bool vs_bot = false;
        bool accepted = false;
        bool active = true;
    };
    std::unordered_map<std::string, TicTacToeSession> ttt_sessions_;
    std::mutex ttt_mutex_;

    static int check_ttt_winner(const std::array<int, 9>& b) {
        static const int lines[8][3] = {
            {0,1,2},{3,4,5},{6,7,8},
            {0,3,6},{1,4,7},{2,5,8},
            {0,4,8},{2,4,6}
        };
        for (const auto& ln : lines) {
            int v0 = b[ln[0]], v1 = b[ln[1]], v2 = b[ln[2]];
            if (v0 == 0 || v1 == 0 || v2 == 0) continue;
            int owner = 0;
            bool conflict = false;
            for (int v : {v0, v1, v2}) {
                if (v == 3) continue; // wildcard matches both
                if (owner == 0) owner = v;
                else if (owner != v) conflict = true;
            }
            if (!conflict && owner != 0) return owner;
        }
        for (int c : b) if (c == 0) return 0;
        return -1; // draw
    }

    static std::string render_ttt_board(const std::array<int, 9>& b) {
        static const char* nums[] = {"1️⃣","2️⃣","3️⃣","4️⃣","5️⃣","6️⃣","7️⃣","8️⃣","9️⃣"};
        std::string out;
        for (int i = 0; i < 9; ++i) {
            if (b[i] == 1) out += "❌";
            else if (b[i] == 2) out += "⭕";
            else if (b[i] == 3) out += "⚡";
            else out += nums[i];
            out += (i % 3 == 2) ? "\n" : " ";
        }
        return out;
    }

    // ── 6. TRIVIA & QUICKREACT STATE ──────────────────────────────────────────
    struct TriviaSession {
        std::string user_id;
        std::string channel_id;
        std::string message_id;
        int correct_idx = 0; // 0..3
        int64_t reward = 500;
        bool active = true;
    };
    std::unordered_map<std::string, TriviaSession> trivia_sessions_;
    std::mutex trivia_mutex_;

    struct QuickReactSession {
        std::string channel_id;
        std::string message_id;
        std::string target_emoji = "⚡";
        int64_t reward = 750;
        bool ready = false;
        bool claimed = false;
    };
    std::unordered_map<std::string, QuickReactSession> qr_sessions_;
    std::mutex qr_mutex_;

    void handle_crash_cashout(stoatpp::cluster& bot, const std::string& uid) {
        CrashGame copy;
        {
            std::lock_guard<std::mutex> lk(crash_mutex_);
            auto it = crash_games_.find(uid);
            if (it == crash_games_.end() || !it->second.active || it->second.crashed || it->second.cashed_out) return;
            it->second.cashed_out = true;
            it->second.active = false;
            copy = it->second;
            crash_games_.erase(it);
        }
        int64_t payout = static_cast<int64_t>(copy.bet * copy.current_mult);
        if (g_db) g_db->update_balances(uid, payout, 0);
        finalize_gamble(bot, copy.channel_id, uid, copy.bet, payout - copy.bet);
        if (!copy.message_id.empty()) {
            bot.edit_message(copy.channel_id, copy.message_id, build_crash_payload(copy));
        }
    }

    void handle_mines_cashout(stoatpp::cluster& bot, const std::string& uid) {
        MinesweeperGame copy;
        {
            std::lock_guard<std::mutex> lk(mines_mutex_);
            auto it = mines_games_.find(uid);
            if (it == mines_games_.end() || !it->second.active || it->second.game_over || it->second.safe_cells_revealed <= 0) return;
            it->second.active = false;
            it->second.game_over = true;
            it->second.won = true;
            copy = it->second;
            mines_games_.erase(it);
        }
        double mult = calc_mines_multiplier(copy.rows * copy.cols, copy.total_mines, copy.safe_cells_revealed);
        int64_t payout = static_cast<int64_t>(copy.initial_bet * mult);
        if (g_db) g_db->update_balances(uid, payout, 0);
        finalize_gamble(bot, copy.channel_id, uid, copy.initial_bet, payout - copy.initial_bet);
        if (!copy.message_id.empty()) {
            bot.edit_message(copy.channel_id, copy.message_id, build_mines_payload(copy, true));
        }
    }

    void handle_frogger_action(stoatpp::cluster& bot, const std::string& uid, int action) {
        // action: 0=left, 1=middle, 2=right, 3=cashout
        FroggerGame copy;
        bool finished = false;
        {
            std::lock_guard<std::mutex> lk(frogger_mutex_);
            auto it = frogger_games_.find(uid);
            if (it == frogger_games_.end() || !it->second.active) return;
            FroggerGame& g = it->second;

            if (action == 3) {
                if (g.logs_hopped <= 0) return;
                g.active = false;
                g.cashed_out = true;
                copy = g;
                frogger_games_.erase(it);
                finished = true;
            } else if (action >= 0 && action <= 2) {
                g.frog_lane = action;
                bool safe = g.upcoming_logs.front()[action];
                if (!safe) {
                    g.active = false;
                    g.crashed = true;
                    copy = g;
                    frogger_games_.erase(it);
                    finished = true;
                } else {
                    g.logs_hopped++;
                    g.upcoming_logs.erase(g.upcoming_logs.begin());
                    g.upcoming_logs.push_back(make_frogger_row(g.difficulty));
                    copy = g;
                }
            } else {
                return;
            }
        }

        if (finished) {
            if (copy.cashed_out) {
                double mult = frogger_mult(copy.difficulty, copy.logs_hopped);
                int64_t payout = static_cast<int64_t>(copy.initial_bet * mult);
                if (g_db) g_db->update_balances(uid, payout, 0);
                finalize_gamble(bot, copy.channel_id, uid, copy.initial_bet, payout - copy.initial_bet);
            } else {
                finalize_gamble(bot, copy.channel_id, uid, copy.initial_bet, -copy.initial_bet);
            }
        }
        if (!copy.message_id.empty()) {
            bot.edit_message(copy.channel_id, copy.message_id, build_frogger_payload(copy));
        }
    }

public:
    void register_handlers(stoatpp::cluster& bot) override {
        // ── 1. .crash <bet> [auto_cashout_mult] ──────────────────────────────
        bot.register_command({
            "crash",
            {"cr"},
            "bet on a rising multiplier — cash out before it crashes!",
            "<bet> [auto_cashout_mult]",
            {"<bet>", "[auto_cashout]"},
            [this](stoatpp::cluster& bot, const stoatpp::events::Message& msg, const std::vector<std::string>& args) {
                if (!g_db) return;
                if (args.empty()) {
                    bot.send_reply(msg, "**🚀 Crash**\nA multiplier starts at **1.00x** and climbs every second. React 💰 or type `cashout` (`c`) before it crashes!\nUsage: `.crash <bet> [auto_cashout_mult]`", true);
                    return;
                }
                {
                    std::lock_guard<std::mutex> lk(crash_mutex_);
                    if (crash_games_.count(msg.author.id)) {
                        bot.send_reply(msg, bronx::emj_no + " You already have an active Crash game! Type `cashout` or react 💰.", true);
                        return;
                    }
                }
                auto u = g_db->get_economy_user(msg.author.id);
                if (!u) return;
                int64_t bet = parse_bet(args[0], u->wallet);
                if (bet < 100) {
                    bot.send_reply(msg, bronx::emj_no + " Minimum Crash bet is **$100**.", true);
                    return;
                }
                if (bet > MAX_BET || bet > u->wallet) {
                    bot.send_reply(msg, bronx::emj_no + " You don't have enough coins in your wallet (max bet $2,000,000,000).", true);
                    return;
                }
                double auto_co = 0.0;
                if (args.size() > 1) {
                    std::string s = args[1];
                    if (!s.empty() && (s.back() == 'x' || s.back() == 'X')) s.pop_back();
                    try { auto_co = std::stod(s); } catch (...) {}
                }

                g_db->update_balances(msg.author.id, -bet, 0);

                CrashGame g;
                g.user_id = msg.author.id;
                g.channel_id = msg.channel_id;
                g.bet = bet;
                g.crash_point = generate_crash_point();
                g.auto_cashout = auto_co;

                {
                    std::lock_guard<std::mutex> lk(crash_mutex_);
                    crash_games_[msg.author.id] = g;
                }

                auto payload = build_crash_payload(g);
                payload.set_interactions({"💰"});
                std::string uid = msg.author.id;
                bot.send_message(msg.channel_id, payload, [this, &bot, uid](stoatpp::models::Message sent, bool ok) {
                    if (!ok) {
                        std::lock_guard<std::mutex> lk(crash_mutex_);
                        auto it = crash_games_.find(uid);
                        if (it != crash_games_.end()) {
                            if (g_db) g_db->update_balances(uid, it->second.bet, 0);
                            crash_games_.erase(it);
                        }
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> lk(crash_mutex_);
                        auto it = crash_games_.find(uid);
                        if (it != crash_games_.end()) it->second.message_id = sent.id;
                    }
                    std::thread([this, &bot, uid]() {
                        while (true) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                            CrashGame copy;
                            bool did_crash = false;
                            bool did_auto_cashout = false;
                            {
                                std::lock_guard<std::mutex> lk(crash_mutex_);
                                auto it = crash_games_.find(uid);
                                if (it == crash_games_.end() || !it->second.active) return;
                                CrashGame& cg = it->second;
                                cg.tick++;
                                cg.current_mult = next_crash_mult(cg.current_mult);

                                if (cg.auto_cashout > 1.0 && cg.current_mult >= cg.auto_cashout && cg.auto_cashout < cg.crash_point) {
                                    cg.current_mult = cg.auto_cashout;
                                    cg.cashed_out = true;
                                    cg.active = false;
                                    copy = cg;
                                    crash_games_.erase(it);
                                    did_auto_cashout = true;
                                } else if (cg.current_mult >= cg.crash_point || cg.tick > 120) {
                                    cg.current_mult = cg.crash_point;
                                    cg.crashed = true;
                                    cg.active = false;
                                    copy = cg;
                                    crash_games_.erase(it);
                                    did_crash = true;
                                } else {
                                    copy = cg;
                                }
                            }
                            if (did_auto_cashout) {
                                int64_t payout = static_cast<int64_t>(copy.bet * copy.current_mult);
                                if (g_db) g_db->update_balances(uid, payout, 0);
                                finalize_gamble(bot, copy.channel_id, uid, copy.bet, payout - copy.bet);
                                bot.edit_message(copy.channel_id, copy.message_id, build_crash_payload(copy));
                                return;
                            }
                            if (did_crash) {
                                finalize_gamble(bot, copy.channel_id, uid, copy.bet, -copy.bet);
                                bot.edit_message(copy.channel_id, copy.message_id, build_crash_payload(copy));
                                return;
                            }
                            if (copy.tick % 2 == 0 && !copy.message_id.empty()) {
                                bot.edit_message(copy.channel_id, copy.message_id, build_crash_payload(copy));
                            }
                        }
                    }).detach();
                });
            },
            "Games"
        });

        // ── 2. .minesweeper <bet> [difficulty|mines] ─────────────────────────
        bot.register_command({
            "minesweeper",
            {"mines", "ms"},
            "navigate a 5x5 minefield for a rising cumulative multiplier.",
            "<bet> [easy|medium|hard|impossible|1-24]",
            {"<bet>", "[difficulty]"},
            [this](stoatpp::cluster& bot, const stoatpp::events::Message& msg, const std::vector<std::string>& args) {
                if (!g_db) return;
                if (args.empty()) {
                    bot.send_reply(msg, "**💣 Minesweeper**\nUsage: `.mines <bet> [easy|medium|hard|impossible|1-24]`\nReveal safe tiles (`1`–`25`) to boost your multiplier and type `cashout` (`c`) or react 💰 to claim your payout!", true);
                    return;
                }
                {
                    std::lock_guard<std::mutex> lk(mines_mutex_);
                    auto it = mines_games_.find(msg.author.id);
                    if (it != mines_games_.end()) {
                        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - it->second.created_at).count();
                        if (elapsed < 300 && it->second.active) {
                            bot.send_reply(msg, bronx::emj_no + " You already have an active Minesweeper game! Type tile numbers (`1`–`25`) or `cashout`.", true);
                            return;
                        }
                        mines_games_.erase(it);
                    }
                }
                auto u = g_db->get_economy_user(msg.author.id);
                if (!u) return;
                int64_t bet = parse_bet(args[0], u->wallet);
                if (bet < 100) {
                    bot.send_reply(msg, bronx::emj_no + " Minimum Minesweeper bet is **$100**.", true);
                    return;
                }
                if (bet > MAX_BET || bet > u->wallet) {
                    bot.send_reply(msg, bronx::emj_no + " You don't have enough coins in your wallet.", true);
                    return;
                }

                int num_mines = 4;
                if (args.size() > 1) {
                    std::string d = args[1];
                    std::transform(d.begin(), d.end(), d.begin(), ::tolower);
                    if (d == "easy" || d == "e") num_mines = 3;
                    else if (d == "medium" || d == "med" || d == "m") num_mines = 6;
                    else if (d == "hard" || d == "h") num_mines = 9;
                    else if (d == "impossible" || d == "imp" || d == "i") num_mines = 13;
                    else {
                        try { num_mines = std::clamp(std::stoi(d), 1, 24); } catch (...) {}
                    }
                }

                g_db->update_balances(msg.author.id, -bet, 0);

                MinesweeperGame game;
                game.user_id = msg.author.id;
                game.channel_id = msg.channel_id;
                game.initial_bet = bet;
                game.total_mines = num_mines;
                game.created_at = std::chrono::steady_clock::now();

                std::mt19937 gen(std::random_device{}());
                std::uniform_int_distribution<int> dis(0, 24);
                while (static_cast<int>(game.mine_positions.size()) < num_mines) {
                    game.mine_positions.insert(dis(gen));
                }

                auto payload = build_mines_payload(game);
                payload.set_interactions({"💰"});
                std::string uid = msg.author.id;
                {
                    std::lock_guard<std::mutex> lk(mines_mutex_);
                    mines_games_[uid] = game;
                }
                bot.send_message(msg.channel_id, payload, [this, uid](stoatpp::models::Message sent, bool ok) {
                    if (ok) {
                        std::lock_guard<std::mutex> lk(mines_mutex_);
                        auto it = mines_games_.find(uid);
                        if (it != mines_games_.end()) it->second.message_id = sent.id;
                    }
                });
            },
            "Games"
        });

        // ── 3. .dice <bet> [target] ──────────────────────────────────────────
        bot.register_command({
            "dice",
            {"roll", "craps"},
            "roll two dice and bet on combinations or sums.",
            "<bet> [high|low|7|doubles|2-12]",
            {"<bet>", "[target]"},
            [](stoatpp::cluster& bot, const stoatpp::events::Message& msg, const std::vector<std::string>& args) {
                if (!g_db) return;
                if (args.empty()) {
                    bot.send_reply(msg, "**🎲 Dice Roll**\nUsage: `.dice <bet> [high|low|7|doubles|2-12]`\nDefault payouts: Snake Eyes (2) / Boxcars (12) = **4x**, Lucky 7 or 11 = **1.5x**, Doubles = **2x**!", true);
                    return;
                }
                auto u = g_db->get_economy_user(msg.author.id);
                if (!u) return;
                int64_t bet = parse_bet(args[0], u->wallet);
                if (bet < 50) {
                    bot.send_reply(msg, bronx::emj_no + " Minimum dice bet is **$50**.", true);
                    return;
                }
                if (bet > MAX_BET || bet > u->wallet) {
                    bot.send_reply(msg, bronx::emj_no + " You don't have enough coins in your wallet.", true);
                    return;
                }

                static thread_local std::mt19937 gen(std::random_device{}());
                std::uniform_int_distribution<int> dis(1, 6);
                int die1 = dis(gen);
                int die2 = dis(gen);
                int total = die1 + die2;

                int64_t net_winnings = -bet;
                std::string result_text;

                if (args.size() > 1) {
                    std::string target = args[1];
                    std::transform(target.begin(), target.end(), target.begin(), ::tolower);
                    if (target == "high" || target == "h") {
                        if (total >= 8) { net_winnings = static_cast<int64_t>(bet * 1.25); result_text = "**HIGH ROLL (" + std::to_string(total) + ")!** 📈"; }
                        else result_text = "Needed 8–12 for High...";
                    } else if (target == "low" || target == "l") {
                        if (total <= 6) { net_winnings = static_cast<int64_t>(bet * 1.25); result_text = "**LOW ROLL (" + std::to_string(total) + ")!** 📉"; }
                        else result_text = "Needed 2–6 for Low...";
                    } else if (target == "doubles" || target == "double" || target == "d") {
                        if (die1 == die2) { net_winnings = bet * 4; result_text = "**DOUBLES HIT!** 🎲🎲"; }
                        else result_text = "Missed doubles...";
                    } else {
                        int exact = 0;
                        try { exact = std::stoi(target); } catch (...) {}
                        if (exact >= 2 && exact <= 12) {
                            static const int ways[] = {0,0,1,2,3,4,5,6,5,4,3,2,1};
                            double mult = (36.0 / ways[exact]) * 0.95;
                            if (total == exact) {
                                net_winnings = static_cast<int64_t>(bet * (mult - 1.0));
                                result_text = "**EXACT SUM " + std::to_string(exact) + " (" + fmt_mult(mult) + ")!** 🎯";
                            } else {
                                result_text = "Needed exact sum **" + std::to_string(exact) + "**...";
                            }
                        }
                    }
                }

                if (result_text.empty()) {
                    if (total == 2 || total == 12) {
                        net_winnings = bet * 4;
                        result_text = "**SNAKE EYES / BOXCARS!** 🐍";
                    } else if (total == 7 || total == 11) {
                        net_winnings = static_cast<int64_t>(bet * 1.5);
                        result_text = "**LUCKY " + std::to_string(total) + "!** 🍀";
                    } else if (die1 == die2) {
                        net_winnings = bet * 2;
                        result_text = "**DOUBLES!** 🎲🎲";
                    } else {
                        net_winnings = -bet;
                        result_text = "No special combination...";
                    }
                }

                g_db->update_balances(msg.author.id, net_winnings, 0);
                finalize_gamble(bot, msg.channel_id, msg.author.id, bet, net_winnings);

                stoatpp::models::MessagePayload payload;
                stoatpp::models::Embed embed;
                embed.set_title("🎲 Dice Roll");
                embed.set_colour(net_winnings > 0 ? "#2ECC71" : "#ED4245");
                std::string desc = "👤 **Player:** <@" + msg.author.id + ">\n"
                                   "You rolled: **" + std::to_string(die1) + "** + **" + std::to_string(die2) + "** = **" + std::to_string(total) + "**\n\n" +
                                   result_text + "\n\n" +
                                   (net_winnings > 0 ? ("**Won:** **" + fmt_coins(net_winnings) + "**") : ("**Lost:** " + fmt_coins(-net_winnings)));
                embed.set_description(desc);
                payload.add_embed(embed);
                bot.send_message(msg.channel_id, payload);
            },
            "Games"
        });

        // ── 4. .frogger [easy|medium|hard] <bet> ─────────────────────────────
        bot.register_command({
            "frogger",
            {"frog"},
            "hop across logs without hitting a cracked log!",
            "[easy|medium|hard] <bet>",
            {"[difficulty]", "<bet>"},
            [this](stoatpp::cluster& bot, const stoatpp::events::Message& msg, const std::vector<std::string>& args) {
                if (!g_db) return;
                if (args.empty()) {
                    bot.send_reply(msg, "**🐸 Frogger**\nUsage: `.frogger <easy|medium|hard> <bet>` (or `.frogger <bet>`)\nHop across 3 lanes (⬅️, ⬆️, ➡️) and cash out (💰) before a log cracks!", true);
                    return;
                }
                {
                    std::lock_guard<std::mutex> lk(frogger_mutex_);
                    if (frogger_games_.count(msg.author.id)) {
                        bot.send_reply(msg, bronx::emj_no + " You already have an active Frogger game! React ⬅️/⬆️/➡️/💰 or type `left`/`middle`/`right`/`cashout`.", true);
                        return;
                    }
                }

                int difficulty = 1;
                std::string bet_arg = args[0];
                if (args.size() >= 2) {
                    std::string d = args[0];
                    std::transform(d.begin(), d.end(), d.begin(), ::tolower);
                    if (d == "medium" || d == "med" || d == "m") difficulty = 2;
                    else if (d == "hard" || d == "h") difficulty = 3;
                    bet_arg = args[1];
                }

                auto u = g_db->get_economy_user(msg.author.id);
                if (!u) return;
                int64_t bet = parse_bet(bet_arg, u->wallet);
                if (bet < 100) {
                    bot.send_reply(msg, bronx::emj_no + " Minimum Frogger bet is **$100**.", true);
                    return;
                }
                if (bet > MAX_BET || bet > u->wallet) {
                    bot.send_reply(msg, bronx::emj_no + " You don't have enough coins in your wallet.", true);
                    return;
                }

                g_db->update_balances(msg.author.id, -bet, 0);

                FroggerGame g;
                g.user_id = msg.author.id;
                g.channel_id = msg.channel_id;
                g.initial_bet = bet;
                g.difficulty = difficulty;
                for (int i = 0; i < 5; ++i) g.upcoming_logs.push_back(make_frogger_row(difficulty));

                auto payload = build_frogger_payload(g);
                payload.set_interactions({"⬅️", "⬆️", "➡️", "💰"});
                std::string uid = msg.author.id;
                {
                    std::lock_guard<std::mutex> lk(frogger_mutex_);
                    frogger_games_[uid] = g;
                }
                bot.send_message(msg.channel_id, payload, [this, uid](stoatpp::models::Message sent, bool ok) {
                    if (ok) {
                        std::lock_guard<std::mutex> lk(frogger_mutex_);
                        auto it = frogger_games_.find(uid);
                        if (it != frogger_games_.end()) it->second.message_id = sent.id;
                    }
                });
            },
            "Games"
        });

        // ── 5. .poker <bet> (5-Card Draw / Jacks-or-Better Video Poker) ──────
        bot.register_command({
            "poker",
            {"videopoker", "vp", "drawpoker"},
            "play 5-card draw Jacks-or-Better video poker (up to 250x payout).",
            "<bet>",
            {"<bet>"},
            [this](stoatpp::cluster& bot, const stoatpp::events::Message& msg, const std::vector<std::string>& args) {
                if (!g_db) return;
                if (args.empty()) {
                    bot.send_reply(msg, "**🃏 Video Poker (Jacks or Better)**\nUsage: `.poker <bet>`\nPayouts: Royal Flush **250x** | Straight Flush **50x** | Four of a Kind **25x** | Full House **9x** | Flush **6x** | Straight **4x** | Three of a Kind **3x** | Two Pair **2x** | Jacks or Better **1.5x**", true);
                    return;
                }
                {
                    std::lock_guard<std::mutex> lk(poker_mutex_);
                    if (poker_sessions_.count(msg.author.id)) {
                        bot.send_reply(msg, bronx::emj_no + " Finish your current poker hand first! Type card positions `1`–`5` to hold (e.g. `1 3 5`), `all`, or `none`.", true);
                        return;
                    }
                }
                auto u = g_db->get_economy_user(msg.author.id);
                if (!u) return;
                int64_t bet = parse_bet(args[0], u->wallet);
                if (bet < 100) {
                    bot.send_reply(msg, bronx::emj_no + " Minimum Poker bet is **$100**.", true);
                    return;
                }
                if (bet > MAX_BET || bet > u->wallet) {
                    bot.send_reply(msg, bronx::emj_no + " You don't have enough coins in your wallet.", true);
                    return;
                }

                g_db->update_balances(msg.author.id, -bet, 0);

                std::vector<PokerCard> deck;
                for (int s = 0; s < 4; ++s) {
                    for (int r = 2; r <= 14; ++r) deck.push_back({r, s});
                }
                std::mt19937 rng(std::random_device{}());
                std::shuffle(deck.begin(), deck.end(), rng);

                VideoPokerSession sess;
                sess.user_id = msg.author.id;
                sess.channel_id = msg.channel_id;
                sess.bet = bet;
                for (int i = 0; i < 5; ++i) {
                    sess.hand.push_back(deck.back());
                    deck.pop_back();
                }
                sess.deck = std::move(deck);

                std::string cards_line;
                for (int i = 0; i < 5; ++i) {
                    cards_line += "**" + std::to_string(i + 1) + ":** " + sess.hand[i].str() + "  ";
                }
                auto [cur_name, cur_mult] = evaluate_video_poker(sess.hand);

                stoatpp::models::MessagePayload payload;
                stoatpp::models::Embed embed;
                embed.set_title("🃏 Video Poker — Deal");
                embed.set_colour("#5865F2");
                embed.set_description("👤 **Player:** <@" + msg.author.id + "> | **Bet:** " + fmt_coins(bet) + "\n\n" +
                                      cards_line + "\n\n"
                                      "**Current Hand:** " + cur_name + " (`" + fmt_mult(cur_mult) + "`)\n\n"
                                      "💡 *Type which card positions `1`–`5` to **HOLD** (e.g. `1 4`, `all`, or `none` to redraw all 5)!*");
                payload.add_embed(embed);

                std::lock_guard<std::mutex> lk(poker_mutex_);
                poker_sessions_[msg.author.id] = std::move(sess);
                bot.send_message(msg.channel_id, payload);
            },
            "Games"
        });

        // ── 6. .jackpot ──────────────────────────────────────────────────────
        bot.register_command({
            "jackpot",
            {"jp", "progressive"},
            "view the progressive jackpot pool and recent winners.",
            "",
            {},
            [](stoatpp::cluster& bot, const stoatpp::events::Message& msg, const std::vector<std::string>&) {
                if (!g_db) return;
                auto jp = g_db->get_jackpot_info();
                auto hist = g_db->get_jackpot_history(5);

                std::string desc = "# 🎰 Progressive Jackpot\n\n"
                                   "💰 **Current Pool: " + fmt_coins(jp.pool) + "**\n\n"
                                   "**How it works:**\n"
                                   "• 1% of all gambling losses feed the progressive pool\n"
                                   "• Every gambling win has a **0.01%** (1 in 10,000) chance to trigger the entire jackpot\n"
                                   "• The pool grows across all servers until someone hits it!\n\n"
                                   "📊 **Stats**\n"
                                   "• Times Won: **" + std::to_string(jp.times_won) + "**\n"
                                   "• Total Paid Out: **" + fmt_coins(jp.total_won_all_time) + "**\n";
                if (!jp.last_winner_id.empty()) {
                    desc += "• Last Winner: <@" + jp.last_winner_id + "> (" + fmt_coins(jp.last_won_amount) + ")\n";
                }
                if (!hist.empty()) {
                    desc += "\n📜 **Recent Winners**\n";
                    for (const auto& h : hist) {
                        desc += "• <@" + h.user_id + "> — **" + fmt_coins(h.amount) + "**\n";
                    }
                }

                stoatpp::models::MessagePayload payload;
                stoatpp::models::Embed embed;
                embed.set_title("🎰 Progressive Jackpot");
                embed.set_colour("#F1C40F");
                embed.set_description(desc);
                payload.add_embed(embed);
                bot.send_message(msg.channel_id, payload);
            },
            "Economy"
        });

        // ── 7. .lottery [count|max|info|draw] ────────────────────────────────
        bot.register_command({
            "lottery",
            {"lotto"},
            "buy lottery tickets into the progressive $30M+ lottery pool.",
            "[count|max|info|draw]",
            {"[count]"},
            [](stoatpp::cluster& bot, const stoatpp::events::Message& msg, const std::vector<std::string>& args) {
                if (!g_db) return;
                if (args.empty() || args[0] == "info" || args[0] == "pool" || args[0] == "status") {
                    int64_t pool = g_db->get_lottery_pool();
                    int64_t users = g_db->get_lottery_participant_count();
                    int64_t total_t = g_db->get_total_lottery_tickets();
                    int64_t my_t = g_db->get_user_lottery_tickets(msg.author.id);
                    double my_pct = (total_t > 0) ? (100.0 * my_t / total_t) : 0.0;

                    std::ostringstream oss;
                    oss << "🎟️ **Current Lottery Pool:** **" << fmt_coins(pool) << "**\n"
                        << "👥 **Participants:** **" << users << "**\n"
                        << "🎫 **Total Tickets Sold:** **" << total_t << "**\n"
                        << "👤 **Your Tickets:** **" << my_t << "** (" << std::fixed << std::setprecision(2) << my_pct << "% win chance)\n"
                        << "💵 **Ticket Cost:** **$300 – $1,000** each (30% feeds the pool)\n\n"
                        << "Buy tickets with `.lottery <count|max>`!";

                    stoatpp::models::MessagePayload payload;
                    stoatpp::models::Embed embed;
                    embed.set_title("🎟️ Bronx Progressive Lottery");
                    embed.set_colour("#F1C40F");
                    embed.set_description(oss.str());
                    payload.add_embed(embed);
                    bot.send_message(msg.channel_id, payload);
                    return;
                }

                if (args[0] == "draw") {
                    if (!bot.is_owner(msg.author.id)) {
                        bot.send_reply(msg, bronx::emj_no + " Only the bot owner can manually trigger a lottery draw.", true);
                        return;
                    }
                    auto [winner_id, prize] = g_db->draw_lottery_winner();
                    if (winner_id.empty()) {
                        bot.send_reply(msg, bronx::emj_warning + " No lottery tickets have been purchased yet!", true);
                        return;
                    }
                    stoatpp::models::MessagePayload payload;
                    stoatpp::models::Embed embed;
                    embed.set_title("🎉 Lottery Draw Winner!");
                    embed.set_colour("#2ECC71");
                    embed.set_description("🏆 <@" + winner_id + "> won the **" + fmt_coins(prize) + "** Bronx Lottery jackpot!\n\n*The lottery pool has reset to $30,000,000.*");
                    payload.add_embed(embed);
                    bot.send_message(msg.channel_id, payload);
                    return;
                }

                auto u = g_db->get_economy_user(msg.author.id);
                if (!u) return;

                int64_t count = 1;
                if (args[0] == "max" || args[0] == "all") {
                    count = 50000;
                } else {
                    try { count = std::clamp<int64_t>(std::stoll(args[0]), 1, 50000); } catch (...) {
                        bot.send_reply(msg, bronx::emj_no + " Invalid ticket count. Usage: `.lottery <count|max|info>`", true);
                        return;
                    }
                }

                int64_t wallet = u->wallet;
                std::mt19937 gen(std::random_device{}());
                std::uniform_int_distribution<int64_t> cost_dis(300, 1000);

                int64_t bought = 0;
                int64_t spent = 0;
                int64_t pool_added = 0;
                for (int64_t i = 0; i < count; ++i) {
                    int64_t c = cost_dis(gen);
                    if (wallet < c) break;
                    wallet -= c;
                    spent += c;
                    pool_added += (c * 30) / 100;
                    bought++;
                }

                if (bought == 0) {
                    bot.send_reply(msg, bronx::emj_no + " You can't afford any lottery tickets ($300–$1,000 each).", true);
                    return;
                }

                g_db->update_balances(msg.author.id, -spent, 0);
                g_db->add_lottery_tickets(msg.author.id, bought);
                g_db->add_to_lottery_pool(pool_added);
                int64_t new_pool = g_db->get_lottery_pool();

                stoatpp::models::MessagePayload payload;
                stoatpp::models::Embed embed;
                embed.set_title("🎟️ Lottery Tickets Purchased");
                embed.set_colour("#2ECC71");
                embed.set_description("👤 **Buyer:** <@" + msg.author.id + ">\n"
                                      "Purchased **" + std::to_string(bought) + "** ticket(s) for **" + fmt_coins(spent) + "**.\n"
                                      "Added **" + fmt_coins(pool_added) + "** (30%) to the lottery pool.\n\n"
                                      "💰 **Current Pool:** **" + fmt_coins(new_pool) + "**");
                payload.add_embed(embed);
                bot.send_message(msg.channel_id, payload);
            },
            "Economy"
        });

        // ── 8. .gamblestats [@user] ──────────────────────────────────────────
        bot.register_command({
            "gamblestats",
            {"gstats", "casinostats"},
            "view gambling statistics, net profit, and biggest win.",
            "[@user]",
            {"[@user]"},
            [](stoatpp::cluster& bot, const stoatpp::events::Message& msg, const std::vector<std::string>& args) {
                if (!g_db) return;
                std::string target_id = msg.author.id;
                if (!args.empty()) {
                    std::string cleaned = helpers::clean_user_id(args[0]);
                    if (!cleaned.empty()) target_id = cleaned;
                }
                auto st = g_db->get_gambling_stats(target_id);
                double win_rate = (st.games_played > 0) ? (100.0 * st.games_won / st.games_played) : 0.0;
                int64_t net = st.total_profit - st.total_losses;

                auto cached_u = bot.get_user(target_id);
                std::string title = "🎰 Gambling Stats" + (cached_u ? (" — " + cached_u->username) : "");

                std::ostringstream oss;
                oss << "👤 **Gambler:** <@" << target_id << ">\n\n"
                    << "🎲 **Games Played:** **" << st.games_played << "** (" << st.games_won << "W / " << st.games_lost << "L — "
                    << std::fixed << std::setprecision(1) << win_rate << "%)\n"
                    << "💵 **Total Wagered:** **" << fmt_coins(st.total_wagered) << "**\n"
                    << "📈 **Total Won:** **" << fmt_coins(st.total_profit) << "**\n"
                    << "📉 **Total Lost:** **" << fmt_coins(st.total_losses) << "**\n"
                    << "💰 **Net Profit:** **" << fmt_coins(net) << "**\n"
                    << "🏆 **Biggest Single Win:** **" << fmt_coins(st.biggest_win) << "**";

                stoatpp::models::MessagePayload payload;
                stoatpp::models::Embed embed;
                embed.set_title(title);
                embed.set_colour(net >= 0 ? "#2ECC71" : "#ED4245");
                embed.set_description(oss.str());
                payload.add_embed(embed);
                bot.send_message(msg.channel_id, payload);
            },
            "Games"
        });

        // ── 9. .tictactoe <@user|bot> [bet] ──────────────────────────────────
        bot.register_command({
            "tictactoe",
            {"ttt"},
            "play interactive Tic-Tac-Toe against another user or the bot AI.",
            "<@user|bot> [bet]",
            {"<@user|bot>", "[bet]"},
            [this](stoatpp::cluster& bot, const stoatpp::events::Message& msg, const std::vector<std::string>& args) {
                if (args.empty()) {
                    bot.send_reply(msg, "**❌⭕ Tic-Tac-Toe**\nUsage: `.tictactoe <@user|bot> [bet]`\nMake moves by typing `1`–`9` in chat!", true);
                    return;
                }
                std::string arg0 = args[0];
                std::transform(arg0.begin(), arg0.end(), arg0.begin(), ::tolower);
                bool vs_bot = (arg0 == "bot" || arg0 == "ai" || arg0 == "cpu");
                std::string p2_id = vs_bot ? "bot" : helpers::clean_user_id(args[0]);
                if (!vs_bot && (p2_id.empty() || p2_id == msg.author.id)) {
                    bot.send_reply(msg, bronx::emj_no + " Mention a valid opponent or use `.tictactoe bot [bet]`.", true);
                    return;
                }

                int64_t bet = 0;
                if (args.size() > 1 && g_db) {
                    auto u1 = g_db->get_economy_user(msg.author.id);
                    if (!u1) return;
                    bet = parse_bet(args[1], u1->wallet);
                    if (bet < 0 || bet > u1->wallet) {
                        bot.send_reply(msg, bronx::emj_no + " Invalid bet or insufficient wallet balance.", true);
                        return;
                    }
                    if (!vs_bot && bet > 0) {
                        auto u2 = g_db->get_economy_user(p2_id);
                        if (!u2 || u2->wallet < bet) {
                            bot.send_reply(msg, bronx::emj_no + " Your opponent doesn't have enough coins in their wallet for that wager.", true);
                            return;
                        }
                    }
                }

                TicTacToeSession sess;
                sess.p1_id = msg.author.id;
                sess.p2_id = p2_id;
                sess.channel_id = msg.channel_id;
                sess.bet = bet;
                sess.vs_bot = vs_bot;
                sess.accepted = true;
                if (bet > 0 && g_db) {
                    g_db->update_balances(msg.author.id, -bet, 0);
                    if (!vs_bot) g_db->update_balances(p2_id, -bet, 0);
                }

                stoatpp::models::MessagePayload payload;
                stoatpp::models::Embed embed;
                embed.set_title("❌⭕ Tic-Tac-Toe");
                embed.set_colour("#5865F2");
                std::string p2_label = vs_bot ? "🤖 **Bronx AI**" : ("<@" + p2_id + ">");
                embed.set_description("❌ <@" + sess.p1_id + "> vs ⭕ " + p2_label +
                                      (bet > 0 ? ("\n💰 **Wager:** " + fmt_coins(bet)) : "") + "\n\n" +
                                      render_ttt_board(sess.board) +
                                      "\n👉 **Turn:** <@" + sess.p1_id + "> (type `1`–`9` to place ❌)");
                payload.add_embed(embed);

                std::lock_guard<std::mutex> lk(ttt_mutex_);
                ttt_sessions_[msg.channel_id] = sess;
                bot.send_message(msg.channel_id, payload);
            },
            "Games"
        });

        // ── 10. .trivia ──────────────────────────────────────────────────────
        bot.register_command({
            "trivia",
            {"quiz"},
            "answer a multiple-choice trivia question to win coins.",
            "",
            {},
            [this](stoatpp::cluster& bot, const stoatpp::events::Message& msg, const std::vector<std::string>&) {
                struct QA { const char* q; const char* a[4]; int correct; };
                static const QA bank[] = {
                    {"Which planet in our solar system has the most moons?", {"Jupiter", "Saturn", "Uranus", "Neptune"}, 1},
                    {"What is the chemical symbol for Gold?", {"Ag", "Au", "Gd", "Fe"}, 1},
                    {"In computer science, what does HTTP stand for?", {"HyperText Transfer Protocol", "High Transmission Text Program", "Hyperlink Terminal Transport Process", "Host Transfer Text Protocol"}, 0},
                    {"Which country gifted the Statue of Liberty to the United States?", {"United Kingdom", "Spain", "France", "Italy"}, 2},
                    {"What is the hardest natural substance on Earth?", {"Graphene", "Titanium", "Quartz", "Diamond"}, 3},
                    {"Who painted 'The Starry Night'?", {"Claude Monet", "Vincent van Gogh", "Pablo Picasso", "Leonardo da Vinci"}, 1},
                    {"How many bits are in a standard IPv6 address?", {"32", "64", "128", "256"}, 2},
                    {"Which element has the atomic number 1?", {"Helium", "Hydrogen", "Lithium", "Carbon"}, 1},
                    {"What year did the Apollo 11 moon landing take place?", {"1965", "1969", "1971", "1973"}, 1},
                    {"Which programming language was created by Bjarne Stroustrup?", {"C", "C++", "Rust", "Java"}, 1},
                    {"What is the capital city of Australia?", {"Sydney", "Melbourne", "Canberra", "Perth"}, 2},
                    {"Which ocean is the deepest on Earth?", {"Atlantic Ocean", "Indian Ocean", "Arctic Ocean", "Pacific Ocean"}, 3}
                };
                static thread_local std::mt19937 rng(std::random_device{}());
                const auto& item = bank[rng() % (sizeof(bank) / sizeof(bank[0]))];

                TriviaSession ts;
                ts.user_id = msg.author.id;
                ts.channel_id = msg.channel_id;
                ts.correct_idx = item.correct;
                ts.reward = 500 + (rng() % 501);

                std::string desc = "**" + std::string(item.q) + "**\n\n"
                                   "🇦 **" + item.a[0] + "**\n"
                                   "🇧 **" + item.a[1] + "**\n"
                                   "🇨 **" + item.a[2] + "**\n"
                                   "🇩 **" + item.a[3] + "**\n\n"
                                   "💰 **Reward:** " + fmt_coins(ts.reward) + " — React 🇦/🇧/🇨/🇩 or type `A`/`B`/`C`/`D`!";

                stoatpp::models::MessagePayload payload;
                stoatpp::models::Embed embed;
                embed.set_title("🧠 Bronx Trivia");
                embed.set_colour("#5865F2");
                embed.set_description(desc);
                payload.add_embed(embed);
                payload.set_interactions({"🇦", "🇧", "🇨", "🇩"});

                {
                    std::lock_guard<std::mutex> lk(trivia_mutex_);
                    trivia_sessions_[msg.channel_id] = ts;
                }
                std::string ch_id = msg.channel_id;
                bot.send_message(ch_id, payload, [this, ch_id](stoatpp::models::Message sent, bool ok) {
                    if (ok) {
                        std::lock_guard<std::mutex> lk(trivia_mutex_);
                        auto it = trivia_sessions_.find(ch_id);
                        if (it != trivia_sessions_.end()) it->second.message_id = sent.id;
                    }
                });
            },
            "Games"
        });

        // ── 11. .quickreact ──────────────────────────────────────────────────
        bot.register_command({
            "quickreact",
            {"react", "qr"},
            "test your reflexes — be the first to react when the signal appears!",
            "",
            {},
            [this](stoatpp::cluster& bot, const stoatpp::events::Message& msg, const std::vector<std::string>&) {
                std::string ch_id = msg.channel_id;
                {
                    std::lock_guard<std::mutex> lk(qr_mutex_);
                    if (qr_sessions_.count(ch_id)) {
                        bot.send_reply(msg, bronx::emj_no + " A QuickReact round is already running in this channel!", true);
                        return;
                    }
                    QuickReactSession qs;
                    qs.channel_id = ch_id;
                    qr_sessions_[ch_id] = qs;
                }

                stoatpp::models::MessagePayload payload;
                stoatpp::models::Embed embed;
                embed.set_title("⚡ QuickReact — Wait for the signal...");
                embed.set_colour("#FEE75C");
                embed.set_description("Get ready... DO NOT react yet!");
                payload.add_embed(embed);

                bot.send_message(ch_id, payload, [this, &bot, ch_id](stoatpp::models::Message sent, bool ok) {
                    if (!ok) {
                        std::lock_guard<std::mutex> lk(qr_mutex_);
                        qr_sessions_.erase(ch_id);
                        return;
                    }
                    std::thread([this, &bot, ch_id, sent]() {
                        std::mt19937 rng(std::random_device{}());
                        int delay_ms = 2000 + (rng() % 3000);
                        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                        {
                            std::lock_guard<std::mutex> lk(qr_mutex_);
                            auto it = qr_sessions_.find(ch_id);
                            if (it == qr_sessions_.end()) return;
                            it->second.message_id = sent.id;
                            it->second.ready = true;
                        }
                        stoatpp::models::MessagePayload go_payload;
                        stoatpp::models::Embed go_embed;
                        go_embed.set_title("⚡ QUICKREACT — GO! GO! GO!");
                        go_embed.set_colour("#57F287");
                        go_embed.set_description("# ⚡ REACT WITH ⚡ OR TYPE `go` NOW!\nFirst person wins **$750**!");
                        go_payload.add_embed(go_embed);
                        go_payload.set_interactions({"⚡"});
                        bot.edit_message(ch_id, sent.id, go_payload);
                    }).detach();
                });
            },
            "Games"
        });

        // ── REACTION LISTENER FOR INTERACTIVE GAMBLING GAMES ─────────────────
        bot.on_message_react([this, &bot](const stoatpp::events::MessageReact& e) {
            if (e.user_id == bot.current_user().id) return;

            if (e.emoji_id == "💰") {
                handle_crash_cashout(bot, e.user_id);
                handle_mines_cashout(bot, e.user_id);
                handle_frogger_action(bot, e.user_id, 3);
            } else if (e.emoji_id == "⬅️" || e.emoji_id == "⬅") {
                handle_frogger_action(bot, e.user_id, 0);
            } else if (e.emoji_id == "⬆️" || e.emoji_id == "⬆") {
                handle_frogger_action(bot, e.user_id, 1);
            } else if (e.emoji_id == "➡️" || e.emoji_id == "➡") {
                handle_frogger_action(bot, e.user_id, 2);
            } else if (e.emoji_id == "⚡") {
                QuickReactSession won_qr;
                bool claimed = false;
                {
                    std::lock_guard<std::mutex> lk(qr_mutex_);
                    auto it = qr_sessions_.find(e.channel_id);
                    if (it != qr_sessions_.end() && it->second.ready && !it->second.claimed && it->second.message_id == e.id) {
                        it->second.claimed = true;
                        won_qr = it->second;
                        qr_sessions_.erase(it);
                        claimed = true;
                    }
                }
                if (claimed) {
                    if (g_db) g_db->update_balances(e.user_id, won_qr.reward, 0);
                    stoatpp::models::MessagePayload p;
                    stoatpp::models::Embed emb;
                    emb.set_title("⚡ QuickReact Winner!");
                    emb.set_colour("#2ECC71");
                    emb.set_description("🏆 <@" + e.user_id + "> reacted first and won **" + fmt_coins(won_qr.reward) + "**!");
                    p.add_embed(emb);
                    bot.edit_message(e.channel_id, e.id, p);
                }
            } else if (e.emoji_id == "🇦" || e.emoji_id == "🇧" || e.emoji_id == "🇨" || e.emoji_id == "🇩") {
                int picked = (e.emoji_id == "🇦") ? 0 : (e.emoji_id == "🇧") ? 1 : (e.emoji_id == "🇨") ? 2 : 3;
                TriviaSession ts;
                bool matched = false;
                {
                    std::lock_guard<std::mutex> lk(trivia_mutex_);
                    auto it = trivia_sessions_.find(e.channel_id);
                    if (it != trivia_sessions_.end() && it->second.active && it->second.message_id == e.id) {
                        ts = it->second;
                        trivia_sessions_.erase(it);
                        matched = true;
                    }
                }
                if (matched) {
                    static const char* letters[] = {"A", "B", "C", "D"};
                    if (picked == ts.correct_idx) {
                        if (g_db) g_db->update_balances(e.user_id, ts.reward, 0);
                        bot.send_message(e.channel_id, bronx::emj_yes + " <@" + e.user_id + "> got it right (**" + letters[ts.correct_idx] + "**) and won **" + fmt_coins(ts.reward) + "**!");
                    } else {
                        bot.send_message(e.channel_id, bronx::emj_no + " <@" + e.user_id + "> answered **" + letters[picked] + "**, but the correct answer was **" + letters[ts.correct_idx] + "**!");
                    }
                }
            }
        });

        // ── MESSAGE LISTENER FOR CHAT-BASED MOVES (cashout, tile numbers, poker hold, ttt) ──
        bot.on_message([this, &bot](const stoatpp::events::Message& msg) {
            if (msg.author.bot || msg.content.empty()) return;
            std::string trimmed = msg.content;
            trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
            if (trimmed.empty()) return;
            trimmed.erase(trimmed.find_last_not_of(" \t\r\n") + 1);
            std::string lower = trimmed;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

            // 1. Cashout keyword for Crash, Minesweeper, Frogger
            if (lower == "cashout" || lower == "c" || lower == "cash out" || lower == "stop") {
                handle_crash_cashout(bot, msg.author.id);
                handle_mines_cashout(bot, msg.author.id);
                handle_frogger_action(bot, msg.author.id, 3);
                return;
            }

            // 2. Frogger lane keywords
            {
                bool has_frog = false;
                {
                    std::lock_guard<std::mutex> lk(frogger_mutex_);
                    auto it = frogger_games_.find(msg.author.id);
                    if (it != frogger_games_.end() && it->second.active && it->second.channel_id == msg.channel_id) has_frog = true;
                }
                if (has_frog) {
                    if (lower == "left" || lower == "l" || lower == "1") { handle_frogger_action(bot, msg.author.id, 0); return; }
                    if (lower == "middle" || lower == "mid" || lower == "m" || lower == "up" || lower == "2") { handle_frogger_action(bot, msg.author.id, 1); return; }
                    if (lower == "right" || lower == "r" || lower == "3") { handle_frogger_action(bot, msg.author.id, 2); return; }
                }
            }

            // 3. Minesweeper tile reveal (e.g. "5" or "1 7 13")
            {
                bool has_mines = false;
                {
                    std::lock_guard<std::mutex> lk(mines_mutex_);
                    auto it = mines_games_.find(msg.author.id);
                    if (it != mines_games_.end() && it->second.active && it->second.channel_id == msg.channel_id) has_mines = true;
                }
                if (has_mines && std::isdigit(static_cast<unsigned char>(lower[0]))) {
                    std::stringstream ss(lower);
                    std::string tok;
                    std::vector<int> picks;
                    bool valid_picks = true;
                    while (ss >> tok) {
                        try {
                            int cell = std::stoi(tok);
                            if (cell >= 1 && cell <= 25) picks.push_back(cell - 1);
                            else { valid_picks = false; break; }
                        } catch (...) { valid_picks = false; break; }
                    }
                    if (valid_picks && !picks.empty()) {
                        MinesweeperGame copy;
                        bool boom = false;
                        bool cleared_all = false;
                        {
                            std::lock_guard<std::mutex> lk(mines_mutex_);
                            auto it = mines_games_.find(msg.author.id);
                            if (it == mines_games_.end() || !it->second.active) return;
                            MinesweeperGame& g = it->second;
                            for (int pos : picks) {
                                if (g.revealed_positions.count(pos)) continue;
                                g.revealed_positions.insert(pos);
                                if (g.mine_positions.count(pos)) {
                                    g.active = false;
                                    g.game_over = true;
                                    g.won = false;
                                    boom = true;
                                    break;
                                } else {
                                    g.safe_cells_revealed++;
                                    if (g.safe_cells_revealed >= (g.rows * g.cols - g.total_mines)) {
                                        g.active = false;
                                        g.game_over = true;
                                        g.won = true;
                                        cleared_all = true;
                                        break;
                                    }
                                }
                            }
                            copy = g;
                            if (boom || cleared_all) mines_games_.erase(it);
                        }
                        if (boom) {
                            finalize_gamble(bot, copy.channel_id, msg.author.id, copy.initial_bet, -copy.initial_bet);
                        } else if (cleared_all) {
                            double mult = calc_mines_multiplier(copy.rows * copy.cols, copy.total_mines, copy.safe_cells_revealed);
                            int64_t payout = static_cast<int64_t>(copy.initial_bet * mult);
                            if (g_db) g_db->update_balances(msg.author.id, payout, 0);
                            finalize_gamble(bot, copy.channel_id, msg.author.id, copy.initial_bet, payout - copy.initial_bet);
                        }
                        if (!copy.message_id.empty()) {
                            bot.edit_message(copy.channel_id, copy.message_id, build_mines_payload(copy, boom || cleared_all));
                        } else {
                            bot.send_message(copy.channel_id, build_mines_payload(copy, boom || cleared_all));
                        }
                        return;
                    }
                }
            }

            // 4. Video Poker hold selection ("1 3 5", "all", "none", "draw")
            {
                VideoPokerSession sess;
                bool has_poker = false;
                {
                    std::lock_guard<std::mutex> lk(poker_mutex_);
                    auto it = poker_sessions_.find(msg.author.id);
                    if (it != poker_sessions_.end() && it->second.active && it->second.channel_id == msg.channel_id) {
                        sess = it->second;
                        has_poker = true;
                    }
                }
                if (has_poker) {
                    std::set<int> hold_idx;
                    bool valid_cmd = false;
                    if (lower == "all" || lower == "stand") {
                        hold_idx = {0, 1, 2, 3, 4};
                        valid_cmd = true;
                    } else if (lower == "none" || lower == "draw" || lower == "0") {
                        valid_cmd = true;
                    } else if (std::isdigit(static_cast<unsigned char>(lower[0]))) {
                        std::stringstream ss(lower);
                        std::string tok;
                        valid_cmd = true;
                        while (ss >> tok) {
                            try {
                                int idx = std::stoi(tok);
                                if (idx >= 1 && idx <= 5) hold_idx.insert(idx - 1);
                                else { valid_cmd = false; break; }
                            } catch (...) { valid_cmd = false; break; }
                        }
                    }
                    if (valid_cmd) {
                        {
                            std::lock_guard<std::mutex> lk(poker_mutex_);
                            poker_sessions_.erase(msg.author.id);
                        }
                        for (int i = 0; i < 5; ++i) {
                            if (!hold_idx.count(i) && !sess.deck.empty()) {
                                sess.hand[i] = sess.deck.back();
                                sess.deck.pop_back();
                            }
                        }
                        auto [hand_title, mult] = evaluate_video_poker(sess.hand);
                        int64_t payout = static_cast<int64_t>(sess.bet * mult);
                        int64_t net = payout - sess.bet;
                        if (payout > 0 && g_db) g_db->update_balances(msg.author.id, payout, 0);
                        finalize_gamble(bot, msg.channel_id, msg.author.id, sess.bet, net);

                        std::string cards_line;
                        for (int i = 0; i < 5; ++i) {
                            cards_line += "**" + std::to_string(i + 1) + ":** " + sess.hand[i].str() + (hold_idx.count(i) ? " *(held)*  " : "  ");
                        }

                        stoatpp::models::MessagePayload payload;
                        stoatpp::models::Embed embed;
                        embed.set_title("🃏 Video Poker — " + hand_title + "!");
                        embed.set_colour(payout > 0 ? "#2ECC71" : "#ED4245");
                        embed.set_description("👤 **Player:** <@" + msg.author.id + ">\n\n" +
                                              cards_line + "\n\n"
                                              "**Result:** **" + hand_title + "** (`" + fmt_mult(mult) + "`)\n" +
                                              (payout > 0 ? ("**Payout:** **" + fmt_coins(payout) + "** (+" + fmt_coins(net) + ")")
                                                          : ("**Lost:** " + fmt_coins(sess.bet))));
                        payload.add_embed(embed);
                        bot.send_message(msg.channel_id, payload);
                        return;
                    }
                }
            }

            // 5. Tic-Tac-Toe move ("1".."9")
            if (lower.size() == 1 && lower[0] >= '1' && lower[0] <= '9') {
                int cell = lower[0] - '1';
                TicTacToeSession copy;
                bool moved = false;
                int winner = 0;
                {
                    std::lock_guard<std::mutex> lk(ttt_mutex_);
                    auto it = ttt_sessions_.find(msg.channel_id);
                    if (it != ttt_sessions_.end() && it->second.active) {
                        TicTacToeSession& s = it->second;
                        std::string expected_uid = (s.turn == 1) ? s.p1_id : s.p2_id;
                        if (msg.author.id == expected_uid && s.board[cell] == 0) {
                            s.board[cell] = s.turn;
                            moved = true;
                            winner = check_ttt_winner(s.board);
                            if (winner == 0) {
                                if (s.vs_bot) {
                                    // Bot AI move: win if possible, block if needed, else center/random
                                    int bot_move = -1;
                                    for (int target_p : {2, 1}) {
                                        for (int c = 0; c < 9; ++c) {
                                            if (s.board[c] == 0) {
                                                s.board[c] = target_p;
                                                if (check_ttt_winner(s.board) == target_p) bot_move = c;
                                                s.board[c] = 0;
                                                if (bot_move != -1) break;
                                            }
                                        }
                                        if (bot_move != -1) break;
                                    }
                                    if (bot_move == -1 && s.board[4] == 0) bot_move = 4;
                                    if (bot_move == -1) {
                                        std::vector<int> avail;
                                        for (int c = 0; c < 9; ++c) if (s.board[c] == 0) avail.push_back(c);
                                        if (!avail.empty()) bot_move = avail[std::rand() % avail.size()];
                                    }
                                    if (bot_move != -1) s.board[bot_move] = 2;
                                    winner = check_ttt_winner(s.board);
                                } else {
                                    s.turn = (s.turn == 1) ? 2 : 1;
                                }
                            }
                            copy = s;
                            if (winner != 0) ttt_sessions_.erase(it);
                        }
                    }
                }
                if (moved) {
                    stoatpp::models::MessagePayload payload;
                    stoatpp::models::Embed embed;
                    embed.set_title("❌⭕ Tic-Tac-Toe");
                    std::string p2_label = copy.vs_bot ? "🤖 **Bronx AI**" : ("<@" + copy.p2_id + ">");
                    std::string status;
                    if (winner == 1) {
                        embed.set_colour("#2ECC71");
                        int64_t prize = copy.vs_bot ? (copy.bet * 2) : (copy.bet * 2);
                        if (copy.bet > 0 && g_db) g_db->update_balances(copy.p1_id, prize, 0);
                        status = "🏆 <@" + copy.p1_id + "> (❌) wins!" + (copy.bet > 0 ? (" (+" + fmt_coins(copy.bet) + ")") : "");
                    } else if (winner == 2) {
                        embed.set_colour(copy.vs_bot ? "#ED4245" : "#2ECC71");
                        if (!copy.vs_bot && copy.bet > 0 && g_db) g_db->update_balances(copy.p2_id, copy.bet * 2, 0);
                        status = "🏆 " + p2_label + " (⭕) wins!" + (copy.bet > 0 ? (" (" + fmt_coins(copy.bet) + ")") : "");
                    } else if (winner == -1) {
                        embed.set_colour("#FEE75C");
                        if (copy.bet > 0 && g_db) {
                            g_db->update_balances(copy.p1_id, copy.bet, 0);
                            if (!copy.vs_bot) g_db->update_balances(copy.p2_id, copy.bet, 0);
                        }
                        status = "🤝 It's a draw! Wagers returned.";
                    } else {
                        embed.set_colour("#5865F2");
                        std::string next_u = (copy.turn == 1) ? ("<@" + copy.p1_id + "> (❌)") : (p2_label + " (⭕)");
                        status = "👉 **Turn:** " + next_u + " (type `1`–`9`)";
                    }
                    embed.set_description("❌ <@" + copy.p1_id + "> vs ⭕ " + p2_label + "\n\n" +
                                          render_ttt_board(copy.board) + "\n" + status);
                    payload.add_embed(embed);
                    bot.send_message(msg.channel_id, payload);
                    return;
                }
            }

            // 6. Trivia text answer ("a", "b", "c", "d")
            if (lower == "a" || lower == "b" || lower == "c" || lower == "d") {
                int picked = lower[0] - 'a';
                TriviaSession ts;
                bool matched = false;
                {
                    std::lock_guard<std::mutex> lk(trivia_mutex_);
                    auto it = trivia_sessions_.find(msg.channel_id);
                    if (it != trivia_sessions_.end() && it->second.active) {
                        ts = it->second;
                        trivia_sessions_.erase(it);
                        matched = true;
                    }
                }
                if (matched) {
                    static const char* letters[] = {"A", "B", "C", "D"};
                    if (picked == ts.correct_idx) {
                        if (g_db) g_db->update_balances(msg.author.id, ts.reward, 0);
                        bot.send_message(msg.channel_id, bronx::emj_yes + " <@" + msg.author.id + "> got it right (**" + letters[ts.correct_idx] + "**) and won **" + fmt_coins(ts.reward) + "**!");
                    } else {
                        bot.send_message(msg.channel_id, bronx::emj_no + " <@" + msg.author.id + "> answered **" + letters[picked] + "**, but the correct answer was **" + letters[ts.correct_idx] + "**!");
                    }
                    return;
                }
            }

            // 7. QuickReact text fallback ("go")
            if (lower == "go" || lower == "⚡") {
                QuickReactSession won_qr;
                bool claimed = false;
                {
                    std::lock_guard<std::mutex> lk(qr_mutex_);
                    auto it = qr_sessions_.find(msg.channel_id);
                    if (it != qr_sessions_.end() && it->second.ready && !it->second.claimed) {
                        it->second.claimed = true;
                        won_qr = it->second;
                        qr_sessions_.erase(it);
                        claimed = true;
                    }
                }
                if (claimed) {
                    if (g_db) g_db->update_balances(msg.author.id, won_qr.reward, 0);
                    bot.send_message(msg.channel_id, "⚡🏆 <@" + msg.author.id + "> was the fastest and won **" + fmt_coins(won_qr.reward) + "**!");
                    return;
                }
            }
        });
    }
};

} // namespace bronx::cogs
