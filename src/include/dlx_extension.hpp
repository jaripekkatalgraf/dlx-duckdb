#pragma once

#include "duckdb.hpp"

namespace duckdb {

class DlxExtension : public Extension {
public:
    void Load(ExtensionLoader &loader) override;
    std::string Name() override { return "dlx"; }
    std::string Version() const override;
};

}  // namespace duckdb
