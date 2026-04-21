# dlx — Dancing Links DuckDB Extension

Exact cover solver using Knuth's Algorithm DLX, exposed as DuckDB table functions.

## Building

```bash
# 1. Clone this repo alongside the extension-template submodules
git clone --recurse-submodules https://github.com/<you>/dlx-duckdb.git
cd dlx-duckdb

# 2. Optional but recommended: install ccache + ninja for fast rebuilds
#    brew install ccache ninja   (macOS)
#    apt install ccache ninja-build  (Ubuntu)

# 3. Build
GEN=ninja make

# 4. Test
make test

# 5. Start a DuckDB shell with the extension pre-loaded
./build/release/duckdb
```

## Functions

### `dlx_solve(num_primary, rows [, num_secondary := 0] [, max_solutions := -1])`

**Returns** `TABLE(solution_id INTEGER, row_index INTEGER)`

Solves the exact cover problem defined by:
- `num_primary INTEGER` — number of primary columns (must be covered **exactly once**)
- `rows LIST(LIST(INTEGER))` — sparse row representation; each inner list is the set of column indices (0-based) that this row covers
- `num_secondary := INTEGER` — optional secondary columns appended after the primary ones (covered **at most once**)
- `max_solutions := BIGINT` — stop after this many solutions

Each output row identifies one (solution, selected-input-row) pair. Join on `solution_id` to reconstruct full solutions.

### `dlx_count(num_primary, rows [, num_secondary := 0] [, max_solutions := -1])`

**Returns** `TABLE(solution_count BIGINT)`

Counts solutions without materialising them (faster for large problems).

---

## Examples

### Knuth's paper example

```sql
SELECT solution_id, row_index
FROM dlx_solve(7, [
    [2,4,5],      -- row 0
    [0,3,6],      -- row 1
    [1,2,3,4,5],  -- row 2
    [0,3],        -- row 3
    [1,2,5,6],    -- row 4
    [3,4,6]       -- row 5
]);
-- solution_id │ row_index
-- ────────────┼──────────
--           0 │         0
--           0 │         3
--           0 │         5
```

### 4-Queens (generalized exact cover with secondary columns)

```sql
-- 8 primary (4 ranks + 4 files), 12 secondary (diagonals)
SELECT solution_count FROM dlx_count(8, [...], num_secondary := 12);
-- 2
```

### Count solutions then enumerate only the first 10

```sql
-- Count first
SELECT solution_count FROM dlx_count(7, rows_expr);

-- Then enumerate with a cap
SELECT * FROM dlx_solve(7, rows_expr, max_solutions := 10);
```

### Reconstruct solutions with original row data

```sql
WITH problem(row_idx, cols) AS (
    VALUES (0, [2,4,5]), (1, [0,3,6]), (2, [1,2,3,4,5]),
           (3, [0,3]),   (4, [1,2,5,6]), (5, [3,4,6])
),
solutions AS (
    SELECT * FROM dlx_solve(7, list(cols ORDER BY row_idx) OVER ())
)
SELECT s.solution_id, p.row_idx, p.cols
FROM solutions s
JOIN problem p ON p.row_idx = s.row_index
ORDER BY s.solution_id, p.row_idx;
```

### Sudoku (sketch)

A standard 9×9 Sudoku encodes to an exact cover with 324 primary columns
(81 cell + 81 row-digit + 81 col-digit + 81 box-digit constraints) and 729
rows (one per cell×digit).  Build the row list with a helper macro and call:

```sql
SELECT row_index FROM dlx_solve(324, sudoku_rows(puzzle_string));
```

---

## Algorithm notes

- **MRV heuristic** (`choose_column`): always branches on the primary column with fewest remaining rows, minimising the search tree.
- **Generalised exact cover**: secondary columns share no link with the root's horizontal list. They are covered when a selected row contains them, preventing double-coverage, but they are never *required* to be covered.
- **Memory**: `O(total nodes)` = `O(sum of row lengths)`. All nodes are stored in a contiguous `std::vector` for cache locality.
- **Solutions collected upfront** in `InitGlobal`, then streamed in `STANDARD_VECTOR_SIZE` chunks via the scan. This keeps the scan trivial and thread-safe.

## File structure

```
src/
  dlx_solver.hpp       # Header-only Dancing Links implementation (no deps)
  dlx_function.cpp     # DuckDB table functions (dlx_solve, dlx_count)
  dlx_extension.cpp    # Extension entry point
  include/
    dlx_extension.hpp
test/sql/
  dlx.test             # sqllogictest suite
CMakeLists.txt
extension_config.cmake
```
