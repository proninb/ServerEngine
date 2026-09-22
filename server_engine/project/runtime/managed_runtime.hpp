#pragma once

#include "../project_context.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <span>
#include <vector>

namespace cw::server {

// Managed storage is accessed through these APIs, never cast to native C++
// classes. References alias field storage; value links copy on propagation.
struct managed_field_layout final {
    object_endpoint endpoint{};
    std::size_t offset = 0;
    std::uint32_t size = 0;
    std::uint32_t alignment = 1;
    TypeRef value_type{};
    construction_value initial{};
    bool reference = false;
    bool is_signed = false;
    bool floating = false;
    bool pointer = false;
    bool read_only = false;
    bool boolean = false;
    bool aggregate = false;
    // Direct children occupy a contiguous range; descendants follow separately.
    std::size_t first_child = 0;
    std::size_t child_count = 0;
    std::size_t sibling_first = 0;
    std::size_t sibling_count = 0;
};
struct managed_object_layout final {
    object_handle object{};
    std::size_t offset = 0;
    std::size_t size = 0;
    std::size_t alignment = 1;
    std::size_t first_field = 0;
    std::size_t field_count = 0;
};
struct managed_binding final {
    std::size_t target = 0;
    std::size_t source = 0;
};

class managed_runtime final {
public:
    // Transactional: failure leaves the previous Runtime untouched. This owns
    // its plan and bytes independently of the Project's mmap/source lifetimes.
    [[nodiscard]] status construct(const project_read_view& project, std::string& error) noexcept;
    [[nodiscard]] status read_integer(object_endpoint endpoint, std::int64_t& value) const noexcept;
    [[nodiscard]] status write_integer(object_endpoint endpoint, std::int64_t value) noexcept;
    [[nodiscard]] status read_real(object_endpoint endpoint, double& value) const noexcept;
    [[nodiscard]] status write_real(object_endpoint endpoint, double value) noexcept;
    // Path indexes are local member positions inside each nested record.
    [[nodiscard]] status read_integer(object_endpoint root, std::span<const member_index> path, std::int64_t& value) const noexcept;
    [[nodiscard]] status write_integer(object_endpoint root, std::span<const member_index> path, std::int64_t value) noexcept;
    [[nodiscard]] status read_real(object_endpoint root, std::span<const member_index> path, double& value) const noexcept;
    [[nodiscard]] status write_real(object_endpoint root, std::span<const member_index> path, double value) noexcept;
    [[nodiscard]] bool aliases(object_endpoint first, object_endpoint second) const noexcept;
    [[nodiscard]] const std::vector<managed_object_layout>& objects() const noexcept { return object_layouts; }
    [[nodiscard]] const std::vector<managed_field_layout>& fields() const noexcept { return field_layouts; }
    [[nodiscard]] const std::vector<managed_binding>& value_links() const noexcept { return copies; }
    [[nodiscard]] std::size_t storage_size() const noexcept { return storage.size(); }

private:
    static constexpr std::size_t missing = static_cast<std::size_t>(-1);
    [[nodiscard]] std::size_t field(object_endpoint endpoint) const noexcept;
    [[nodiscard]] std::size_t field(object_endpoint endpoint, std::span<const member_index> path) const noexcept;
    void propagate() noexcept;
    std::vector<managed_object_layout> object_layouts;
    std::vector<managed_field_layout> field_layouts;
    std::vector<std::size_t> object_slots;
    std::vector<std::size_t> resolved;
    std::vector<managed_binding> copies;
    std::vector<std::byte> storage;
};
} // namespace cw::server
