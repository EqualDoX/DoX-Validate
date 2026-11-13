#include "rules/module.h"

namespace eqx {

// One factory per module; each lives in src/rules/<module>/.
std::unique_ptr<RuleModule> make_document_module();
std::unique_ptr<RuleModule> make_content_module();
std::unique_ptr<RuleModule> make_fonts_module();
std::unique_ptr<RuleModule> make_containment_module();
std::unique_ptr<RuleModule> make_structure_module();
std::unique_ptr<RuleModule> make_tables_module();
std::unique_ptr<RuleModule> make_pac_structure_module();
std::unique_ptr<RuleModule> make_pac_quality_module();
std::unique_ptr<RuleModule> make_annotations_module();

std::vector<std::unique_ptr<RuleModule>> create_modules() {
    std::vector<std::unique_ptr<RuleModule>> m;
    m.push_back(make_document_module());
    m.push_back(make_content_module());
    m.push_back(make_fonts_module());
    m.push_back(make_containment_module());
    m.push_back(make_structure_module());
    m.push_back(make_tables_module());
    m.push_back(make_pac_structure_module());
    m.push_back(make_pac_quality_module());
    m.push_back(make_annotations_module());
    return m;
}

}  // namespace eqx
