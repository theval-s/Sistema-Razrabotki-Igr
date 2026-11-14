#pragma once
#include "UiRuntimeAccess.hpp"
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/DataVariable.h>
#include <set>

namespace engine::ui_detail {
bool Identifier(std::string_view name);
bool Utf8(std::string_view text);
class ModelStorage final : public UiModelStorage, public std::enable_shared_from_this<ModelStorage> {
public:
    ModelStorage(UiValueSchema schema, StateValue values);
    ~ModelStorage() override;
    const UiValueSchema& Schema() const noexcept override { return schema_; }
    bool Read(std::span<const UiModelPathPart>, StateValue&, std::string&) const override;
    bool Write(std::span<const UiModelPathPart>, const StateValue&, std::string&) override;
    bool ArraySize(std::span<const UiModelPathPart>, std::size_t&, std::string&) const override;
    std::vector<std::string> TakeDirtyVariables() override;
    bool Bind(Rml::DataModelConstructor&, std::string&);
private:
    struct Node;
    struct Definition;
    Node* Path(const UiModelPath&, const UiValueSchema&);
    const StateValue* Resolve(std::span<const UiModelPathPart>, const UiValueSchema** = nullptr) const;
    UiValueSchema schema_;
    StateValue values_;
    std::set<std::string> dirty_;
    std::map<UiModelPath, std::unique_ptr<Node>> nodes_;
    std::map<const UiValueSchema*, std::unique_ptr<Definition>> definitions_;
};
}
