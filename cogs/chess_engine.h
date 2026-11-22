#pragma once
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace bronx::chess {

enum Piece : int8_t {
    EMPTY = 0,
    WP = 1, WN = 2, WB = 3, WR = 4, WQ = 5, WK = 6,
    BP = -1, BN = -2, BB = -3, BR = -4, BQ = -5, BK = -6
};

inline bool is_white(Piece p) { return p > 0; }
inline bool is_black(Piece p) { return p < 0; }
inline bool is_same_color(Piece p, bool white_turn) {
    return white_turn ? (p > 0) : (p < 0);
}
inline bool is_enemy(Piece p, bool white_turn) {
    return white_turn ? (p < 0) : (p > 0);
}
inline int piece_abs(Piece p) { return p < 0 ? -p : p; }

inline std::string piece_unicode(Piece p) {
    switch (p) {
        case WP: return "♙";
        case WN: return "♘";
        case WB: return "♗";
        case WR: return "♖";
        case WQ: return "♕";
        case WK: return "♔";
        case BP: return "♟";
        case BN: return "♞";
        case BB: return "♝";
        case BR: return "♜";
        case BQ: return "♛";
        case BK: return "♚";
        default: return "·";
    }
}

inline char piece_letter(int abs_p) {
    switch (abs_p) {
        case 2: return 'N';
        case 3: return 'B';
        case 4: return 'R';
        case 5: return 'Q';
        case 6: return 'K';
        default: return 'P';
    }
}

struct Move {
    int from = -1;       // 0..63 (row*8 + col, row 0 = rank 8, row 7 = rank 1)
    int to = -1;
    int promo = 0;       // 0 or 2(N),3(B),4(R),5(Q)
    bool is_ep = false;
    bool is_castle = false;
};

enum class GameStatus {
    Ongoing,
    Check,
    Checkmate,
    Stalemate,
    Draw50Move,
    DrawInsufficient
};

class ChessBoard {
public:
    std::array<Piece, 64> sq{};
    bool white_turn = true;
    bool wk = true, wq = true, bk = true, bq = true;
    int ep_sq = -1;
    int halfmove = 0;
    int fullmove = 1;
    std::vector<Piece> captured_by_white;
    std::vector<Piece> captured_by_black;
    std::vector<std::string> history_san;
    std::string last_move_san;

    ChessBoard() { reset(); }

    void reset() {
        sq = {
            BR, BN, BB, BQ, BK, BB, BN, BR,
            BP, BP, BP, BP, BP, BP, BP, BP,
            EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY,
            EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY,
            EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY,
            EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY,
            WP, WP, WP, WP, WP, WP, WP, WP,
            WR, WN, WB, WQ, WK, WB, WN, WR
        };
        white_turn = true;
        wk = wq = bk = bq = true;
        ep_sq = -1;
        halfmove = 0;
        fullmove = 1;
        captured_by_white.clear();
        captured_by_black.clear();
        history_san.clear();
        last_move_san.clear();
    }

    static int sq_index(int r, int c) { return r * 8 + c; }
    static std::string sq_name(int idx) {
        if (idx < 0 || idx >= 64) return "";
        char file = 'a' + (idx % 8);
        char rank = '8' - (idx / 8);
        return std::string(1, file) + std::string(1, rank);
    }
    static int parse_sq(char file, char rank) {
        if (file < 'a' || file > 'h' || rank < '1' || rank > '8') return -1;
        int c = file - 'a';
        int r = '8' - rank;
        return r * 8 + c;
    }

    int find_king(bool white) const {
        Piece k = white ? WK : BK;
        for (int i = 0; i < 64; ++i) {
            if (sq[i] == k) return i;
        }
        return -1;
    }

    bool is_attacked(int idx, bool by_white) const {
        int r = idx / 8;
        int c = idx % 8;

        // Pawn attacks
        int pr = by_white ? r + 1 : r - 1;
        Piece ep = by_white ? WP : BP;
        if (pr >= 0 && pr < 8) {
            if (c - 1 >= 0 && sq[pr * 8 + c - 1] == ep) return true;
            if (c + 1 < 8 && sq[pr * 8 + c + 1] == ep) return true;
        }

        // Knight attacks
        static constexpr int kr[8] = {-2,-2,-1,-1,1,1,2,2};
        static constexpr int kc[8] = {-1,1,-2,2,-2,2,-1,1};
        Piece en = by_white ? WN : BN;
        for (int i = 0; i < 8; ++i) {
            int nr = r + kr[i], nc = c + kc[i];
            if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8 && sq[nr * 8 + nc] == en) return true;
        }

        // King attacks
        Piece ek = by_white ? WK : BK;
        for (int dr = -1; dr <= 1; ++dr) {
            for (int dc = -1; dc <= 1; ++dc) {
                if (dr == 0 && dc == 0) continue;
                int nr = r + dr, nc = c + dc;
                if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8 && sq[nr * 8 + nc] == ek) return true;
            }
        }

        // Rook / Queen orthogonal
        static constexpr int orow[4] = {-1, 1, 0, 0};
        static constexpr int ocol[4] = {0, 0, -1, 1};
        Piece er = by_white ? WR : BR;
        Piece eq = by_white ? WQ : BQ;
        for (int d = 0; d < 4; ++d) {
            int nr = r + orow[d], nc = c + ocol[d];
            while (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                Piece p = sq[nr * 8 + nc];
                if (p != EMPTY) {
                    if (p == er || p == eq) return true;
                    break;
                }
                nr += orow[d];
                nc += ocol[d];
            }
        }

        // Bishop / Queen diagonal
        static constexpr int drow[4] = {-1, -1, 1, 1};
        static constexpr int dcol[4] = {-1, 1, -1, 1};
        Piece eb = by_white ? WB : BB;
        for (int d = 0; d < 4; ++d) {
            int nr = r + drow[d], nc = c + dcol[d];
            while (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                Piece p = sq[nr * 8 + nc];
                if (p != EMPTY) {
                    if (p == eb || p == eq) return true;
                    break;
                }
                nr += drow[d];
                nc += dcol[d];
            }
        }

        return false;
    }

    bool in_check(bool white) const {
        int k = find_king(white);
        if (k < 0) return false;
        return is_attacked(k, !white);
    }

    std::vector<Move> generate_pseudo_moves() const {
        std::vector<Move> moves;
        moves.reserve(48);
        for (int i = 0; i < 64; ++i) {
            Piece p = sq[i];
            if (p == EMPTY || !is_same_color(p, white_turn)) continue;
            int r = i / 8, c = i % 8;
            int ap = piece_abs(p);

            if (ap == 1) { // Pawn
                int dir = white_turn ? -1 : 1;
                int start_row = white_turn ? 6 : 1;
                int promo_row = white_turn ? 0 : 7;
                int nr = r + dir;
                if (nr >= 0 && nr < 8) {
                    int fwd = nr * 8 + c;
                    if (sq[fwd] == EMPTY) {
                        if (nr == promo_row) {
                            for (int pr : {5, 4, 3, 2}) moves.push_back({i, fwd, pr, false, false});
                        } else {
                            moves.push_back({i, fwd, 0, false, false});
                            if (r == start_row) {
                                int fwd2 = (r + 2 * dir) * 8 + c;
                                if (sq[fwd2] == EMPTY) moves.push_back({i, fwd2, 0, false, false});
                            }
                        }
                    }
                    for (int dc : {-1, 1}) {
                        int nc = c + dc;
                        if (nc < 0 || nc >= 8) continue;
                        int cap = nr * 8 + nc;
                        if (is_enemy(sq[cap], white_turn)) {
                            if (nr == promo_row) {
                                for (int pr : {5, 4, 3, 2}) moves.push_back({i, cap, pr, false, false});
                            } else {
                                moves.push_back({i, cap, 0, false, false});
                            }
                        } else if (cap == ep_sq) {
                            moves.push_back({i, cap, 0, true, false});
                        }
                    }
                }
            } else if (ap == 2) { // Knight
                static constexpr int kr[8] = {-2,-2,-1,-1,1,1,2,2};
                static constexpr int kc[8] = {-1,1,-2,2,-2,2,-1,1};
                for (int d = 0; d < 8; ++d) {
                    int nr = r + kr[d], nc = c + kc[d];
                    if (nr < 0 || nr >= 8 || nc < 0 || nc >= 8) continue;
                    int to = nr * 8 + nc;
                    if (!is_same_color(sq[to], white_turn)) moves.push_back({i, to, 0, false, false});
                }
            } else if (ap == 3 || ap == 4 || ap == 5) { // Bishop, Rook, Queen
                static constexpr int dr[8] = {-1,-1,1,1,-1,1,0,0};
                static constexpr int dc[8] = {-1,1,-1,1,0,0,-1,1};
                int start = (ap == 4) ? 4 : 0;
                int end = (ap == 3) ? 4 : 8;
                for (int d = start; d < end; ++d) {
                    int nr = r + dr[d], nc = c + dc[d];
                    while (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                        int to = nr * 8 + nc;
                        if (sq[to] == EMPTY) {
                            moves.push_back({i, to, 0, false, false});
                        } else {
                            if (is_enemy(sq[to], white_turn)) moves.push_back({i, to, 0, false, false});
                            break;
                        }
                        nr += dr[d];
                        nc += dc[d];
                    }
                }
            } else if (ap == 6) { // King
                for (int dr = -1; dr <= 1; ++dr) {
                    for (int dc = -1; dc <= 1; ++dc) {
                        if (dr == 0 && dc == 0) continue;
                        int nr = r + dr, nc = c + dc;
                        if (nr < 0 || nr >= 8 || nc < 0 || nc >= 8) continue;
                        int to = nr * 8 + nc;
                        if (!is_same_color(sq[to], white_turn)) moves.push_back({i, to, 0, false, false});
                    }
                }
                // Castling
                if (white_turn && i == 60 && !in_check(true)) {
                    if (wk && sq[61] == EMPTY && sq[62] == EMPTY && sq[63] == WR &&
                        !is_attacked(61, false) && !is_attacked(62, false)) {
                        moves.push_back({60, 62, 0, false, true});
                    }
                    if (wq && sq[59] == EMPTY && sq[58] == EMPTY && sq[57] == EMPTY && sq[56] == WR &&
                        !is_attacked(59, false) && !is_attacked(58, false)) {
                        moves.push_back({60, 58, 0, false, true});
                    }
                } else if (!white_turn && i == 4 && !in_check(false)) {
                    if (bk && sq[5] == EMPTY && sq[6] == EMPTY && sq[7] == BR &&
                        !is_attacked(5, true) && !is_attacked(6, true)) {
                        moves.push_back({4, 6, 0, false, true});
                    }
                    if (bq && sq[3] == EMPTY && sq[2] == EMPTY && sq[1] == EMPTY && sq[0] == BR &&
                        !is_attacked(3, true) && !is_attacked(2, true)) {
                        moves.push_back({4, 2, 0, false, true});
                    }
                }
            }
        }
        return moves;
    }

    ChessBoard apply_move_copy(const Move& m) const {
        ChessBoard next = *this;
        next.apply_move_raw(m);
        return next;
    }

    void apply_move_raw(const Move& m) {
        Piece moving = sq[m.from];
        Piece captured = sq[m.to];
        int ap = piece_abs(moving);

        if (m.is_ep) {
            int cap_sq = white_turn ? (m.to + 8) : (m.to - 8);
            captured = sq[cap_sq];
            sq[cap_sq] = EMPTY;
        }

        if (captured != EMPTY) {
            if (white_turn) captured_by_white.push_back(captured);
            else captured_by_black.push_back(captured);
        }

        sq[m.to] = moving;
        sq[m.from] = EMPTY;

        if (m.promo != 0) {
            sq[m.to] = static_cast<Piece>(white_turn ? m.promo : -m.promo);
        }

        if (m.is_castle) {
            if (m.to == 62) { sq[61] = WR; sq[63] = EMPTY; }
            else if (m.to == 58) { sq[59] = WR; sq[56] = EMPTY; }
            else if (m.to == 6) { sq[5] = BR; sq[7] = EMPTY; }
            else if (m.to == 2) { sq[3] = BR; sq[0] = EMPTY; }
        }

        // Update castling rights
        if (moving == WK) { wk = wq = false; }
        if (moving == BK) { bk = bq = false; }
        if (m.from == 63 || m.to == 63) wk = false;
        if (m.from == 56 || m.to == 56) wq = false;
        if (m.from == 7 || m.to == 7) bk = false;
        if (m.from == 0 || m.to == 0) bq = false;

        // En passant target
        if (ap == 1 && std::abs(m.to - m.from) == 16) {
            ep_sq = (m.from + m.to) / 2;
        } else {
            ep_sq = -1;
        }

        if (ap == 1 || captured != EMPTY) halfmove = 0;
        else halfmove++;

        if (!white_turn) fullmove++;
        white_turn = !white_turn;
    }

    std::vector<Move> generate_legal_moves() const {
        auto pseudo = generate_pseudo_moves();
        std::vector<Move> legal;
        legal.reserve(pseudo.size());
        for (const auto& m : pseudo) {
            ChessBoard after = apply_move_copy(m);
            if (!after.in_check(!after.white_turn)) {
                legal.push_back(m);
            }
        }
        return legal;
    }

    bool has_insufficient_material() const {
        int wp = 0, wn = 0, wb = 0, wr = 0, wq = 0;
        int bp = 0, bn = 0, bb = 0, br = 0, bq = 0;
        for (Piece p : sq) {
            switch (p) {
                case WP: wp++; break; case WN: wn++; break; case WB: wb++; break; case WR: wr++; break; case WQ: wq++; break;
                case BP: bp++; break; case BN: bn++; break; case BB: bb++; break; case BR: br++; break; case BQ: bq++; break;
                default: break;
            }
        }
        if (wp || bp || wr || br || wq || bq) return false;
        int w_minor = wn + wb;
        int b_minor = bn + bb;
        if (w_minor <= 1 && b_minor <= 1) return true;
        return false;
    }

    GameStatus status() const {
        if (halfmove >= 100) return GameStatus::Draw50Move;
        if (has_insufficient_material()) return GameStatus::DrawInsufficient;
        auto legal = generate_legal_moves();
        bool chk = in_check(white_turn);
        if (legal.empty()) {
            return chk ? GameStatus::Checkmate : GameStatus::Stalemate;
        }
        return chk ? GameStatus::Check : GameStatus::Ongoing;
    }

    std::string move_to_san(const Move& m, const std::vector<Move>& all_legal) const {
        if (m.is_castle) {
            std::string s = (m.to % 8 == 6) ? "O-O" : "O-O-O";
            ChessBoard after = apply_move_copy(m);
            auto st = after.status();
            if (st == GameStatus::Checkmate) s += "#";
            else if (st == GameStatus::Check) s += "+";
            return s;
        }

        Piece p = sq[m.from];
        int ap = piece_abs(p);
        bool is_cap = (sq[m.to] != EMPTY) || m.is_ep;
        std::string san;

        if (ap == 1) {
            if (is_cap) {
                san += static_cast<char>('a' + (m.from % 8));
                san += 'x';
            }
            san += sq_name(m.to);
            if (m.promo != 0) {
                san += '=';
                san += piece_letter(m.promo);
            }
        } else {
            san += piece_letter(ap);
            bool same_file = false, same_rank = false, need_disambig = false;
            for (const auto& other : all_legal) {
                if (other.from == m.from) continue;
                if (other.to == m.to && piece_abs(sq[other.from]) == ap) {
                    need_disambig = true;
                    if ((other.from % 8) == (m.from % 8)) same_file = true;
                    if ((other.from / 8) == (m.from / 8)) same_rank = true;
                }
            }
            if (need_disambig) {
                if (!same_file) san += static_cast<char>('a' + (m.from % 8));
                else if (!same_rank) san += static_cast<char>('8' - (m.from / 8));
                else san += sq_name(m.from);
            }
            if (is_cap) san += 'x';
            san += sq_name(m.to);
        }

        ChessBoard after = apply_move_copy(m);
        auto st = after.status();
        if (st == GameStatus::Checkmate) san += "#";
        else if (st == GameStatus::Check) san += "+";
        return san;
    }

    static bool is_move_syntax(const std::string& raw) {
        std::string s;
        for (char c : raw) {
            if (!std::isspace(static_cast<unsigned char>(c))) s += c;
        }
        if (s.size() < 2 || s.size() > 8) return false;
        std::string low = s;
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        if (low == "o-o" || low == "o-o-o" || low == "0-0" || low == "0-0-0") return true;
        // Coordinate: e2e4, e2-e4, e7e8q
        if (s.size() >= 4 && s[0] >= 'a' && s[0] <= 'h' && s[1] >= '1' && s[1] <= '8') {
            size_t idx = (s[2] == '-' || s[2] == ' ') ? 3 : 2;
            if (idx + 1 < s.size() && s[idx] >= 'a' && s[idx] <= 'h' && s[idx + 1] >= '1' && s[idx + 1] <= '8') {
                return true;
            }
        }
        // SAN: e4, exd5, Nf3, Qxe4+, Nbd2, e8=Q
        char f = s[0];
        if ((f >= 'a' && f <= 'h') || f == 'K' || f == 'Q' || f == 'R' || f == 'B' || f == 'N') {
            bool has_digit = false;
            for (char c : s) if (c >= '1' && c <= '8') has_digit = true;
            if (has_digit) return true;
        }
        return false;
    }

    bool parse_and_make_move(const std::string& input, std::string& out_san, std::string& out_err) {
        std::string s;
        for (char c : input) {
            if (!std::isspace(static_cast<unsigned char>(c))) s += c;
        }
        if (s.empty()) {
            out_err = "Empty move.";
            return false;
        }

        auto legal = generate_legal_moves();

        // Strip trailing + or #
        while (!s.empty() && (s.back() == '+' || s.back() == '#')) s.pop_back();

        // Castling
        std::string low = s;
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        if (low == "o-o" || low == "0-0") {
            for (const auto& m : legal) {
                if (m.is_castle && (m.to % 8 == 6)) {
                    out_san = move_to_san(m, legal);
                    apply_move_raw(m);
                    last_move_san = out_san;
                    history_san.push_back(out_san);
                    return true;
                }
            }
            out_err = "Kingside castling is not legal right now.";
            return false;
        }
        if (low == "o-o-o" || low == "0-0-0") {
            for (const auto& m : legal) {
                if (m.is_castle && (m.to % 8 == 2)) {
                    out_san = move_to_san(m, legal);
                    apply_move_raw(m);
                    last_move_san = out_san;
                    history_san.push_back(out_san);
                    return true;
                }
            }
            out_err = "Queenside castling is not legal right now.";
            return false;
        }

        // 1. Try coordinate notation: e2e4, e2-e4, e7e8q, e7e8=Q
        {
            std::string clean;
            for (char c : low) {
                if (c != '-' && c != '=') clean += c;
            }
            if (clean.size() >= 4 && clean[0] >= 'a' && clean[0] <= 'h' && clean[1] >= '1' && clean[1] <= '8' &&
                clean[2] >= 'a' && clean[2] <= 'h' && clean[3] >= '1' && clean[3] <= '8') {
                int from = parse_sq(clean[0], clean[1]);
                int to = parse_sq(clean[2], clean[3]);
                int promo = 0;
                if (clean.size() >= 5) {
                    char pc = clean[4];
                    if (pc == 'q') promo = 5;
                    else if (pc == 'r') promo = 4;
                    else if (pc == 'b') promo = 3;
                    else if (pc == 'n') promo = 2;
                }
                for (const auto& m : legal) {
                    if (m.from == from && m.to == to) {
                        if (m.promo == 0 || m.promo == promo || (promo == 0 && m.promo == 5)) {
                            out_san = move_to_san(m, legal);
                            apply_move_raw(m);
                            last_move_san = out_san;
                            history_san.push_back(out_san);
                            return true;
                        }
                    }
                }
            }
        }

        // 2. Try SAN match against all legal moves (case-insensitive for pawn moves, piece letter uppercase)
        for (const auto& m : legal) {
            std::string san = move_to_san(m, legal);
            std::string san_clean = san;
            while (!san_clean.empty() && (san_clean.back() == '+' || san_clean.back() == '#')) san_clean.pop_back();
            if (san_clean == s) {
                out_san = san;
                apply_move_raw(m);
                last_move_san = out_san;
                history_san.push_back(out_san);
                return true;
            }
            // Also allow omitting 'x' or lowercase piece letter if unambiguous, or omitting '=Q'
            std::string san_low = san_clean;
            std::transform(san_low.begin(), san_low.end(), san_low.begin(), ::tolower);
            if (san_low == low && piece_abs(sq[m.from]) == 1) {
                out_san = san;
                apply_move_raw(m);
                last_move_san = out_san;
                history_san.push_back(out_san);
                return true;
            }
        }

        // 3. Fallback: SAN without '=' on promotion (e.g. e8Q) or without 'x'
        for (const auto& m : legal) {
            std::string san = move_to_san(m, legal);
            std::string san_nox;
            for (char c : san) {
                if (c != '+' && c != '#' && c != '=' && c != 'x') san_nox += c;
            }
            std::string s_nox;
            for (char c : s) {
                if (c != '=' && c != 'x') s_nox += c;
            }
            if (san_nox == s_nox) {
                out_san = san;
                apply_move_raw(m);
                last_move_san = out_san;
                history_san.push_back(out_san);
                return true;
            }
        }

        out_err = "Illegal move `" + input + "`.";
        return false;
    }

    int evaluate() const {
        static constexpr int val[7] = {0, 100, 320, 330, 500, 900, 20000};
        static constexpr int pst_p[64] = {
             0,  0,  0,  0,  0,  0,  0,  0,
            50, 50, 50, 50, 50, 50, 50, 50,
            10, 10, 20, 30, 30, 20, 10, 10,
             5,  5, 10, 25, 25, 10,  5,  5,
             0,  0,  0, 20, 20,  0,  0,  0,
             5, -5,-10,  0,  0,-10, -5,  5,
             5, 10, 10,-20,-20, 10, 10,  5,
             0,  0,  0,  0,  0,  0,  0,  0
        };
        static constexpr int pst_n[64] = {
            -50,-40,-30,-30,-30,-30,-40,-50,
            -40,-20,  0,  0,  0,  0,-20,-40,
            -30,  0, 10, 15, 15, 10,  0,-30,
            -30,  5, 15, 20, 20, 15,  5,-30,
            -30,  0, 15, 20, 20, 15,  0,-30,
            -30,  5, 10, 15, 15, 10,  5,-30,
            -40,-20,  0,  5,  5,  0,-20,-40,
            -50,-40,-30,-30,-30,-30,-40,-50
        };
        int score = 0;
        for (int i = 0; i < 64; ++i) {
            Piece p = sq[i];
            if (p == EMPTY) continue;
            int ap = piece_abs(p);
            int s = val[ap];
            int r = i / 8, c = i % 8;
            int idx = (p > 0) ? i : ((7 - r) * 8 + c);
            if (ap == 1) s += pst_p[idx];
            else if (ap == 2) s += pst_n[idx];
            else if (ap == 3 || ap == 4 || ap == 5) {
                // Slight center bonus
                int cr = std::abs(3 - r) + std::abs(3 - c);
                s += (7 - cr) * 2;
            }
            score += (p > 0) ? s : -s;
        }
        return white_turn ? score : -score;
    }

    int negamax(int depth, int alpha, int beta) const {
        auto legal = generate_legal_moves();
        if (legal.empty()) {
            if (in_check(white_turn)) return -100000 - depth;
            return 0;
        }
        if (depth == 0) return evaluate();

        // Order captures first
        std::sort(legal.begin(), legal.end(), [this](const Move& a, const Move& b) {
            int va = (sq[a.to] != EMPTY) ? piece_abs(sq[a.to]) * 10 - piece_abs(sq[a.from]) : 0;
            int vb = (sq[b.to] != EMPTY) ? piece_abs(sq[b.to]) * 10 - piece_abs(sq[b.from]) : 0;
            return va > vb;
        });

        int best = -200000;
        for (const auto& m : legal) {
            ChessBoard child = apply_move_copy(m);
            int val = -child.negamax(depth - 1, -beta, -alpha);
            if (val > best) best = val;
            if (val > alpha) alpha = val;
            if (alpha >= beta) break;
        }
        return best;
    }

    bool pick_ai_move(int depth, std::string& out_san) {
        auto legal = generate_legal_moves();
        if (legal.empty()) return false;
        Move best_move = legal[0];
        int best_val = -200000;
        for (const auto& m : legal) {
            ChessBoard child = apply_move_copy(m);
            int val = -child.negamax(depth - 1, -200000, 200000);
            if (val > best_val) {
                best_val = val;
                best_move = m;
            }
        }
        out_san = move_to_san(best_move, legal);
        apply_move_raw(best_move);
        last_move_san = out_san;
        history_san.push_back(out_san);
        return true;
    }

    std::string render_board(bool flip = false) const {
        std::ostringstream ss;
        ss << "```\n";
        for (int step = 0; step < 8; ++step) {
            int r = flip ? (7 - step) : step;
            ss << (8 - r) << " | ";
            for (int cstep = 0; cstep < 8; ++cstep) {
                int c = flip ? (7 - cstep) : cstep;
                ss << piece_unicode(sq[r * 8 + c]);
                if (cstep < 7) ss << " ";
            }
            ss << "\n";
        }
        ss << "  +----------------\n    ";
        if (!flip) ss << "a b c d e f g h";
        else ss << "h g f e d c b a";
        ss << "\n```";
        return ss.str();
    }

    std::string render_captured(const std::vector<Piece>& caps) const {
        if (caps.empty()) return "None";
        std::string out;
        for (Piece p : caps) out += piece_unicode(p);
        return out;
    }
};

} // namespace bronx::chess
