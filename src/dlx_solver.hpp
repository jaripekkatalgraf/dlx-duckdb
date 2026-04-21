#pragma once

#include <vector>
#include <functional>
#include <stdexcept>
#include <string>

// ============================================================
// Dancing Links — Algorithm DLX  (Knuth, arxiv:cs/0011047)
//
// Two classes:
//
//   Solver          — classic recursive, callback-per-solution.
//                     Simple; good for dlx_count.
//
//   StreamingSolver — explicit stack; call next() to advance
//                     one solution at a time.  The DLX linked
//                     lists ARE the continuation — no extra
//                     memory needed to pause/resume.
//                     Used by dlx_solve for true streaming output
//                     and natural LIMIT early-exit.
// ============================================================

namespace dlx {

// ── Shared node layout ───────────────────────────────────────────────────

struct Node {
    int L, R, U, D;  // neighbour indices
    int C;            // column header index
    int row_id;       // owning input row (-1 for headers)
};

// ── DLXBase: linked-list construction shared by both solvers ─────────────

class DLXBase {
protected:
    int num_primary_;
    int num_secondary_;
    int num_rows_ = 0;

    std::vector<Node> nodes_;
    std::vector<int>  col_size_;

    explicit DLXBase(int num_primary, int num_secondary = 0)
        : num_primary_(num_primary), num_secondary_(num_secondary) {

        if (num_primary < 0 || num_secondary < 0)
            throw std::invalid_argument("Column counts must be non-negative");

        int total = num_primary + num_secondary;
        nodes_.resize(total + 1);
        col_size_.assign(total + 1, 0);

        // Root (index 0): its horizontal list covers only primary columns.
        nodes_[0] = { (num_primary > 0 ? num_primary : 0),
                      (num_primary > 0 ? 1           : 0),
                      0, 0, 0, -1 };

        // Primary column headers 1..num_primary (circular through root)
        for (int i = 1; i <= num_primary; i++) {
            nodes_[i] = { i-1,
                          (i < num_primary ? i+1 : 0),
                          i, i, i, -1 };
        }

        // Secondary column headers: their own closed circular list,
        // NOT linked through root.  choose_column() never picks them,
        // but cover() still removes them when a selected row includes them
        // (enforcing "at most once" for generalised exact cover).
        if (num_secondary > 0) {
            int first = num_primary + 1, last = total;
            for (int i = first; i <= last; i++) {
                nodes_[i] = { (i == first ? last  : i-1),
                              (i == last  ? first : i+1),
                              i, i, i, -1 };
            }
        }
    }

public:
    void add_row(const std::vector<int>& col_indices) {
        if (col_indices.empty()) return;
        int total = num_primary_ + num_secondary_;
        int row_id = num_rows_++;
        int first = -1, prev = -1;

        for (int col : col_indices) {
            if (col < 0 || col >= total)
                throw std::out_of_range(
                    "Column index " + std::to_string(col) +
                    " out of range [0, " + std::to_string(total-1) + "]");

            int ch = col + 1;
            int n  = (int)nodes_.size();
            nodes_.push_back({});
            Node& nd = nodes_.back();
            nd.C = ch; nd.row_id = row_id;

            int up = nodes_[ch].U;
            nd.U = up; nd.D = ch;
            nodes_[up].D = n; nodes_[ch].U = n;
            col_size_[ch]++;

            if (first == -1) { first = n; nd.L = n; nd.R = n; }
            else {
                nd.L = prev; nd.R = nodes_[prev].R;
                nodes_[nodes_[prev].R].L = n;
                nodes_[prev].R = n;
            }
            prev = n;
        }
    }

    int num_rows() const { return num_rows_; }

protected:
    int choose_column() const {
        int best = nodes_[0].R, best_s = col_size_[best];
        for (int c = nodes_[best].R; c != 0; c = nodes_[c].R)
            if (col_size_[c] < best_s) { best = c; best_s = col_size_[c]; }
        return best;
    }

    void cover(int c) {
        nodes_[nodes_[c].R].L = nodes_[c].L;
        nodes_[nodes_[c].L].R = nodes_[c].R;
        for (int i = nodes_[c].D; i != c; i = nodes_[i].D)
            for (int j = nodes_[i].R; j != i; j = nodes_[j].R) {
                nodes_[nodes_[j].D].U = nodes_[j].U;
                nodes_[nodes_[j].U].D = nodes_[j].D;
                col_size_[nodes_[j].C]--;
            }
    }

    void uncover(int c) {
        for (int i = nodes_[c].U; i != c; i = nodes_[i].U)
            for (int j = nodes_[i].L; j != i; j = nodes_[j].L) {
                col_size_[nodes_[j].C]++;
                nodes_[nodes_[j].D].U = j;
                nodes_[nodes_[j].U].D = j;
            }
        nodes_[nodes_[c].R].L = c;
        nodes_[nodes_[c].L].R = c;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
//  Solver  —  recursive, callback-based
//  Used by dlx_count: we never need to buffer solutions, just count.
// ═══════════════════════════════════════════════════════════════════════════

class Solver : public DLXBase {
public:
    explicit Solver(int num_primary, int num_secondary = 0)
        : DLXBase(num_primary, num_secondary) {}

    void solve(std::function<void(const std::vector<int>&)> cb,
               int64_t max_solutions = -1) {
        sol_.clear();
        found_ = 0; max_ = max_solutions; cb_ = std::move(cb);
        search();
    }

    int64_t count(int64_t max_solutions = -1) {
        int64_t n = 0;
        solve([&](const std::vector<int>&){ n++; }, max_solutions);
        return n;
    }

private:
    std::vector<int> sol_;
    std::function<void(const std::vector<int>&)> cb_;
    int64_t found_ = 0, max_ = -1;

    void search() {
        if (max_ >= 0 && found_ >= max_) return;
        if (nodes_[0].R == 0) { cb_(sol_); found_++; return; }
        int c = choose_column();
        if (col_size_[c] == 0) return;
        cover(c);
        for (int r = nodes_[c].D; r != c; r = nodes_[r].D) {
            sol_.push_back(nodes_[r].row_id);
            for (int j = nodes_[r].R; j != r; j = nodes_[j].R) cover(nodes_[j].C);
            search();
            for (int j = nodes_[r].L; j != r; j = nodes_[j].L) uncover(nodes_[j].C);
            sol_.pop_back();
        }
        uncover(c);
    }
};

// ═══════════════════════════════════════════════════════════════════════════
//  StreamingSolver  —  explicit stack, one solution at a time
//
//  Usage:
//      StreamingSolver s(7);
//      s.add_row(...); ...
//      while (s.next()) {
//          for (int row_idx : s.solution()) { ... }
//      }
//
//  How it works
//  ────────────
//  The recursive DLX search pauses after every solution:
//
//      search(k):
//        if done → yield solution        ← pause/resume point
//        c = choose(); cover(c)
//        for r = c.down; r != c; r = r.down:
//          sol.push(r); cover others in r
//          search(k+1)
//          uncover others in r; sol.pop()
//        uncover(c)
//
//  We convert this to an explicit stack of Frames, one per level:
//
//      Frame { col, row }
//        col  — the column header chosen at this level
//        row  — the row node currently under exploration
//               (= col means "not started yet / just exhausted")
//
//  The DLX linked lists encode ALL cover/uncover state, so they
//  ARE the continuation.  Between next() calls, the linked list
//  sits exactly as the recursion would have left it.
//
//  Complexity
//  ──────────
//  Memory:   O(depth)  for the stack   (depth ≤ num_primary)
//            O(depth)  for sol_        (partial solution)
//  Vs materialising everything upfront: O(num_solutions × sol_size)
//
//  Early exit (LIMIT)
//  ──────────────────
//  DuckDB stops calling Scan() once LIMIT rows are satisfied.
//  Because we only advance the solver when Scan() asks for more,
//  we stop searching mid-tree — no wasted backtracking.
//
//      SELECT * FROM dlx_solve(...) LIMIT 1;
//      -- stops after the very first solution is found
// ═══════════════════════════════════════════════════════════════════════════

class StreamingSolver : public DLXBase {
public:
    explicit StreamingSolver(int num_primary, int num_secondary = 0)
        : DLXBase(num_primary, num_secondary) {}

    // Advance to the next solution.
    // Returns true  → solution() contains the selected row indices.
    // Returns false → search is exhausted, no more solutions.
    //
    // The state machine runs in a tight loop; each iteration is
    // one "step" of the would-be recursive call stack:
    bool next() {
        if (exhausted_) return false;

        // ── First call: bootstrap ─────────────────────────────────────────
        if (!started_) {
            started_ = true;
            if (nodes_[0].R == 0) {    // zero primary columns
                exhausted_ = true;
                return true;           // one empty solution
            }
            int c = choose_column();
            if (col_size_[c] == 0) { exhausted_ = true; return false; }
            cover(c);
            stack_.push_back({c, c}); // row == col → haven't tried any row yet
        }

        // ── Driver loop ───────────────────────────────────────────────────
        //
        // Invariant:  stack_.back() = {col, row}
        //   row == col  → entering this level fresh (no row applied yet)
        //   row != col  → returning from a deeper search; this row was
        //                 applied and its sub-search is now finished
        //
        while (!stack_.empty()) {
            Frame& f = stack_.back();

            // Step 1 – UNDO the row we applied in the previous iteration.
            //   (Skipped when row == col, i.e. fresh entry.)
            if (f.row != f.col) {
                // Reverse-iterate leftward to undo covers done rightward
                for (int j = nodes_[f.row].L; j != f.row; j = nodes_[j].L)
                    uncover(nodes_[j].C);
                sol_.pop_back();
            }

            // Step 2 – Advance cursor to the next candidate row.
            f.row = nodes_[f.row].D;

            // Step 3 – Exhausted all rows for this column → backtrack.
            if (f.row == f.col) {
                uncover(f.col);
                stack_.pop_back();
                continue;
            }

            // Step 4 – Apply this row: cover every other column it touches.
            sol_.push_back(nodes_[f.row].row_id);
            for (int j = nodes_[f.row].R; j != f.row; j = nodes_[j].R)
                cover(nodes_[j].C);

            // Step 5 – All primary columns covered?  We have a solution.
            //   Return true now.  On the next call to next(), Step 1 will
            //   undo this row and searching will continue naturally.
            if (nodes_[0].R == 0)
                return true;

            // Step 6 – Go deeper, unless it's a dead-end.
            int c_next = choose_column();
            if (col_size_[c_next] == 0)
                continue;   // dead end: Step 1 will undo on the next iter

            cover(c_next);
            stack_.push_back({c_next, c_next});
        }

        exhausted_ = true;
        return false;
    }

    // The current solution.  Stable between calls to next().
    // Row indices are 0-based, matching add_row() call order.
    const std::vector<int>& solution() const { return sol_; }

    bool exhausted() const { return exhausted_; }

private:
    struct Frame {
        int col;  // column header chosen at this search level
        int row;  // current row node (= col → not yet tried any row)
    };

    std::vector<Frame> stack_;
    std::vector<int>   sol_;
    bool started_   = false;
    bool exhausted_ = false;
};

}  // namespace dlx
