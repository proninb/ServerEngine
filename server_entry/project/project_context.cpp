#include "project_context.hpp"

#include <utility>

namespace cw::server {

project_context::project_context(project_configuration configuration)
    : project_configuration_value(std::move(configuration)) {}

} // namespace cw::server
