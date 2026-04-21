#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/types/value.hpp"

#include "dlx_solver.hpp"

namespace duckdb {

// ── Argument validation ───────────────────────────────────────────────────

static int ValidatePrimary(const Value &v) {
    if (v.IsNull()) throw BinderException("num_primary must not be NULL");
    int n = v.GetValue<int32_t>();
    if (n < 1)     throw BinderException("num_primary must be >= 1");
    return n;
}

static int ValidateSecondary(const Value &v) {
    int n = v.GetValue<int32_t>();
    if (n < 0)     throw BinderException("num_secondary must be >= 0");
    return n;
}

static void ValidateRowsType(const Value &v) {
    if (v.IsNull()) throw BinderException("rows must not be NULL");
    if (v.type().id() != LogicalTypeId::LIST)
        throw BinderException("rows must be LIST(LIST(INTEGER))");
}

// ── Build solver directly from a DuckDB LIST(LIST(INTEGER)) Value.
// Templated on DLXBase so both Solver and StreamingSolver work —
// fixes the type mismatch bug where dlx_count passed a Solver to
// a function expecting StreamingSolver&.
template<typename SolverType>
static void FillSolverFromValue(SolverType &solver, const Value &rows_val) {
    for (const auto &row_val : ListValue::GetChildren(rows_val)) {
        if (row_val.IsNull()) continue;
        if (row_val.type().id() != LogicalTypeId::LIST)
            throw InvalidInputException(
                "dlx: each element of rows must be a LIST(INTEGER)");
        std::vector<int> cols;
        for (const auto &cv : ListValue::GetChildren(row_val))
            if (!cv.IsNull()) cols.push_back(cv.GetValue<int32_t>());
        solver.add_row(cols);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  dlx_solve — streaming table function
// ═══════════════════════════════════════════════════════════════════════════

struct DLXSolveBindData : public TableFunctionData {
    int     num_primary   = 0;
    int     num_secondary = 0;
    int64_t max_solutions = -1;
    Value   rows_val;

    DLXSolveBindData(int np, int ns, int64_t ms, Value rows)
        : num_primary(np), num_secondary(ns), max_solutions(ms),
          rows_val(std::move(rows)) {}
};

struct DLXSolveState : public GlobalTableFunctionState {
    dlx::StreamingSolver solver;
    int32_t solution_id     = 0;
    idx_t   sol_cursor      = 0;
    bool    has_more        = false;
    int64_t solutions_found = 0;

    explicit DLXSolveState(const DLXSolveBindData &bd)
        : solver(bd.num_primary, bd.num_secondary) {
        FillSolverFromValue(solver, bd.rows_val);
        has_more = solver.next();
        if (!has_more) solution_id = -1;
    }

    idx_t MaxThreads() const override { return 1; }
};

static unique_ptr<FunctionData>
DLXSolveBind(ClientContext &, TableFunctionBindInput &input,
             vector<LogicalType> &return_types, vector<string> &names) {
    int np = ValidatePrimary(input.inputs[0]);
    ValidateRowsType(input.inputs[1]);

    int ns = 0; int64_t ms = -1;
    auto it = input.named_parameters.find("num_secondary");
    if (it != input.named_parameters.end() && !it->second.IsNull())
        ns = ValidateSecondary(it->second);
    it = input.named_parameters.find("max_solutions");
    if (it != input.named_parameters.end() && !it->second.IsNull()) {
        ms = it->second.GetValue<int64_t>();
        if (ms < 1) throw BinderException("max_solutions must be >= 1");
    }

    return_types = {LogicalType::INTEGER, LogicalType::INTEGER};
    names        = {"solution_id", "row_index"};
    return make_uniq<DLXSolveBindData>(np, ns, ms, std::move(input.inputs[1]));
}

static unique_ptr<GlobalTableFunctionState>
DLXSolveInit(ClientContext &, TableFunctionInitInput &input) {
    auto &bd = input.bind_data->Cast<DLXSolveBindData>();
    try {
        return make_uniq<DLXSolveState>(bd);
    } catch (const std::out_of_range &e) {
        throw InvalidInputException("dlx_solve: %s", e.what());
    } catch (const std::exception &e) {
        throw InvalidInputException("dlx_solve: %s", e.what());
    }
}

static void DLXSolveScan(ClientContext &, TableFunctionInput &data_p,
                         DataChunk &output) {
    auto &gs = data_p.global_state->Cast<DLXSolveState>();
    auto &bd = data_p.bind_data->Cast<DLXSolveBindData>();

    auto *sol_ids  = FlatVector::GetData<int32_t>(output.data[0]);
    auto *row_idxs = FlatVector::GetData<int32_t>(output.data[1]);
    idx_t count    = 0;

    while (count < STANDARD_VECTOR_SIZE && gs.has_more) {
        const auto &sol = gs.solver.solution();
        while (gs.sol_cursor < sol.size() && count < STANDARD_VECTOR_SIZE) {
            sol_ids[count]  = gs.solution_id;
            row_idxs[count] = static_cast<int32_t>(sol[gs.sol_cursor++]);
            count++;
        }
        if (gs.sol_cursor == sol.size()) {
            if (bd.max_solutions >= 0 && gs.solutions_found >= bd.max_solutions) {
                gs.has_more = false;
                break;
            }
            gs.has_more = gs.solver.next();
            if (gs.has_more) {
                gs.solution_id++;
                gs.solutions_found++;
                gs.sol_cursor = 0;
            }
        }
    }
    output.SetCardinality(count);
}

// ═══════════════════════════════════════════════════════════════════════════
//  dlx_count — counts solutions without materialising them
// ═══════════════════════════════════════════════════════════════════════════

struct DLXCountBindData : public TableFunctionData {
    int     num_primary   = 0;
    int     num_secondary = 0;
    int64_t max_solutions = -1;
    Value   rows_val;

    DLXCountBindData(int np, int ns, int64_t ms, Value rows)
        : num_primary(np), num_secondary(ns), max_solutions(ms),
          rows_val(std::move(rows)) {}
};

struct DLXCountState : public GlobalTableFunctionState {
    int64_t solution_count = 0;
    bool    emitted = false;
};

static unique_ptr<FunctionData>
DLXCountBind(ClientContext &, TableFunctionBindInput &input,
             vector<LogicalType> &return_types, vector<string> &names) {
    int np = ValidatePrimary(input.inputs[0]);
    ValidateRowsType(input.inputs[1]);

    int ns = 0; int64_t ms = -1;
    auto it = input.named_parameters.find("num_secondary");
    if (it != input.named_parameters.end() && !it->second.IsNull())
        ns = ValidateSecondary(it->second);
    it = input.named_parameters.find("max_solutions");
    if (it != input.named_parameters.end() && !it->second.IsNull()) {
        ms = it->second.GetValue<int64_t>();
        if (ms < 1) throw BinderException("max_solutions must be >= 1");
    }

    return_types = {LogicalType::BIGINT};
    names        = {"solution_count"};
    return make_uniq<DLXCountBindData>(np, ns, ms, std::move(input.inputs[1]));
}

static unique_ptr<GlobalTableFunctionState>
DLXCountInit(ClientContext &, TableFunctionInitInput &input) {
    auto &bd = input.bind_data->Cast<DLXCountBindData>();
    auto  gs = make_uniq<DLXCountState>();
    try {
        dlx::Solver solver(bd.num_primary, bd.num_secondary);
        FillSolverFromValue(solver, bd.rows_val);  // now works: Solver is DLXBase
        gs->solution_count = solver.count(bd.max_solutions);
    } catch (const std::exception &e) {
        throw InvalidInputException("dlx_count: %s", e.what());
    }
    return std::move(gs);
}

static void DLXCountScan(ClientContext &, TableFunctionInput &data_p,
                         DataChunk &output) {
    auto &gs = data_p.global_state->Cast<DLXCountState>();
    if (gs.emitted) { output.SetCardinality(0); return; }
    FlatVector::GetData<int64_t>(output.data[0])[0] = gs.solution_count;
    output.SetCardinality(1);
    gs.emitted = true;
}

// ── Registration — now takes ExtensionLoader& (DuckDB >= v1.4) ───────────

void RegisterDLXFunctions(ExtensionLoader &loader) {
    TableFunction solve_func(
        "dlx_solve",
        {LogicalType::INTEGER,
         LogicalType::LIST(LogicalType::LIST(LogicalType::INTEGER))},
        DLXSolveScan, DLXSolveBind, DLXSolveInit);
    solve_func.named_parameters["num_secondary"] = LogicalType::INTEGER;
    solve_func.named_parameters["max_solutions"] = LogicalType::BIGINT;
    loader.RegisterFunction(solve_func);

    TableFunction count_func(
        "dlx_count",
        {LogicalType::INTEGER,
         LogicalType::LIST(LogicalType::LIST(LogicalType::INTEGER))},
        DLXCountScan, DLXCountBind, DLXCountInit);
    count_func.named_parameters["num_secondary"] = LogicalType::INTEGER;
    count_func.named_parameters["max_solutions"] = LogicalType::BIGINT;
    loader.RegisterFunction(count_func);
}

}  // namespace duckdb
