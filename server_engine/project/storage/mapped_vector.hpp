#pragma once

#include "../../status.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace cw::server {

// Construction-only baseline+append storage. Baseline records stay in mmap and
// are decoded only when Builder touches them. Materialized baseline records own
// stable addresses; post-baseline records use one ordinary append vector.
template<class T>
class mapped_vector final {
public:
    using value_type = T;
    using iterator = typename std::vector<T>::iterator;
    using const_iterator = typename std::vector<T>::const_iterator;
    using reader_type = status (*)(const void*, std::size_t, T&) noexcept;

    mapped_vector() = default;
    mapped_vector(const mapped_vector&) = delete;
    mapped_vector& operator=(const mapped_vector&) = delete;
    mapped_vector(mapped_vector&&) noexcept = default;
    mapped_vector& operator=(mapped_vector&&) noexcept = default;

    void bind_baseline(
        const void* context_value,
        std::size_t count_value,
        reader_type reader_value) noexcept {

        context = context_value;
        baseline_count = count_value;
        reader = reader_value;
        read_failure = {};
        element_cache.clear();
        element_index.clear();
        range_cache.clear();
        local.clear();
    }

    [[nodiscard]] bool baseline_backed() const noexcept {
        return baseline_count != 0 || reader != nullptr;
    }

    [[nodiscard]] std::size_t baseline_size() const noexcept {
        return baseline_count;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return baseline_count + local.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return size() == 0;
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return baseline_count + local.capacity();
    }

    [[nodiscard]] std::size_t local_size() const noexcept {
        return local.size();
    }

    [[nodiscard]] std::size_t local_capacity() const noexcept {
        return local.capacity();
    }

    [[nodiscard]] std::size_t materialized_baseline_records() const noexcept {
        return element_cache.size();
    }

    [[nodiscard]] std::size_t heap_record_capacity() const noexcept {
        std::size_t result = local.capacity() + element_cache.size();
        for (const auto& range : range_cache) {
            if (range.count > (std::numeric_limits<std::size_t>::max)() - result)
                return (std::numeric_limits<std::size_t>::max)();
            result += range.count;
        }
        return result;
    }

    [[nodiscard]] status read_status() const noexcept {
        return read_failure;
    }

    // Allocation-free read boundary. Baseline records are decoded directly from
    // mmap unless a construction-time materialized/modified copy already exists.
    [[nodiscard]] status read(
        std::size_t index,
        T& output) const noexcept {

        if (index >= size()) {
            output = {};
            return {status_code::not_found};
        }

        try {
            if (index >= baseline_count) {
                output = local[index - baseline_count];
                return {};
            }

            if (const auto* existing = find_element(index); existing != nullptr) {
                output = *existing;
                return {};
            }

            output = {};
            return reader != nullptr
                ? reader(context, index, output)
                : status{status_code::invalid_state};
        }
        catch (const std::bad_alloc&) {
            output = {};
            return {status_code::not_available};
        }
        catch (const std::length_error&) {
            output = {};
            return {status_code::not_available};
        }
    }

    [[nodiscard]] const T* materialized(
        std::size_t index) const noexcept {

        if (index >= size())
            return nullptr;
        if (index >= baseline_count)
            return &local[index - baseline_count];
        return find_element(index);
    }

    void reserve(std::size_t requested) {
        const auto local_requested = requested > baseline_count
            ? requested - baseline_count
            : std::size_t{0};
        local.reserve(local_requested);
    }

    void resize(std::size_t requested) {
        if (!baseline_backed()) {
            local.resize(requested);
            return;
        }
        if (requested < baseline_count)
            throw std::length_error("mapped_vector cannot shrink baseline");
        local.resize(requested - baseline_count);
    }

    void clear() noexcept {
        local.clear();
        element_cache.clear();
        element_index.clear();
        range_cache.clear();
        if (!baseline_backed())
            baseline_count = 0;
        read_failure = {};
    }

    void assign(std::size_t count, const T& value) {
        if (baseline_backed())
            throw std::length_error("mapped_vector assign on baseline");
        local.assign(count, value);
    }

    void push_back(const T& value) {
        local.push_back(value);
    }

    void push_back(T&& value) {
        local.push_back(std::move(value));
    }

    template<class... Args>
    T& emplace_back(Args&&... args) {
        return local.emplace_back(std::forward<Args>(args)...);
    }

    iterator begin() noexcept { return local.begin(); }
    iterator end() noexcept { return local.end(); }
    const_iterator begin() const noexcept { return local.begin(); }
    const_iterator end() const noexcept { return local.end(); }

    template<class InputIterator>
    iterator insert(iterator position, InputIterator first, InputIterator last) {
        return local.insert(position, first, last);
    }

    void swap(mapped_vector& other) noexcept {
        std::swap(context, other.context);
        std::swap(baseline_count, other.baseline_count);
        std::swap(reader, other.reader);
        std::swap(read_failure, other.read_failure);
        element_cache.swap(other.element_cache);
        element_index.swap(other.element_index);
        range_cache.swap(other.range_cache);
        local.swap(other.local);
    }

    // Publication may replace a persisted acceleration index with a fully
    // rebuilt local table. This intentionally detaches only this container from
    // its mmap baseline; semantic arrays remain mapped and sparse.
    void swap(std::vector<T>& values) noexcept {
        context = nullptr;
        baseline_count = 0;
        reader = nullptr;
        read_failure = {};
        element_cache.clear();
        element_index.clear();
        range_cache.clear();
        local.swap(values);
    }

    [[nodiscard]] const T& operator[](std::size_t index) const noexcept {
        if (index >= baseline_count)
            return local[index - baseline_count];
        return materialize_element(index);
    }

    [[nodiscard]] T& operator[](std::size_t index) noexcept {
        if (index >= baseline_count)
            return local[index - baseline_count];
        invalidate_ranges_containing(index);
        return const_cast<T&>(
            static_cast<const mapped_vector&>(*this).materialize_element(index));
    }

    [[nodiscard]] std::span<const T> span(
        std::size_t begin_value,
        std::size_t count_value) const noexcept {

        if (count_value == 0)
            return {};
        if (begin_value > size() || count_value > size() - begin_value)
            return {};

        if (begin_value >= baseline_count) {
            const auto local_begin = begin_value - baseline_count;
            return {local.data() + local_begin, count_value};
        }

        if (begin_value + count_value > baseline_count)
            return {};

        for (const auto& range : range_cache) {
            if (range.begin == begin_value && range.count == count_value)
                return {range.values.get(), range.count};
        }

        try {
            range_block candidate;
            candidate.begin = begin_value;
            candidate.count = count_value;
            candidate.values = std::make_unique<T[]>(count_value);

            for (std::size_t offset = 0; offset < count_value; ++offset) {
                const auto index = begin_value + offset;
                if (const auto* existing = find_element(index); existing != nullptr) {
                    candidate.values[offset] = *existing;
                    continue;
                }

                const auto result = reader != nullptr
                    ? reader(context, index, candidate.values[offset])
                    : status{status_code::invalid_state};
                if (!result.ok()) {
                    if (read_failure.ok())
                        read_failure = result;
                    return {};
                }
            }

            range_cache.push_back(std::move(candidate));
            const auto& stored = range_cache.back();
            return {stored.values.get(), stored.count};
        }
        catch (const std::bad_alloc&) {
            if (read_failure.ok())
                read_failure = {status_code::not_available};
            return {};
        }
        catch (const std::length_error&) {
            if (read_failure.ok())
                read_failure = {status_code::not_available};
            return {};
        }
    }

    [[nodiscard]] status copy_all(std::vector<T>& output) const noexcept {
        try {
            output.clear();
            output.reserve(size());
            for (std::size_t index = 0; index < baseline_count; ++index)
                output.push_back(materialize_element(index));
            output.insert(output.end(), local.begin(), local.end());
            return read_failure.ok() ? status{} : read_failure;
        }
        catch (const std::bad_alloc&) {
            output.clear();
            return {status_code::not_available};
        }
        catch (const std::length_error&) {
            output.clear();
            return {status_code::not_available};
        }
    }

    [[nodiscard]] const T* data() const noexcept {
        return baseline_backed() ? nullptr : local.data();
    }

    [[nodiscard]] T* data() noexcept {
        return baseline_backed() ? nullptr : local.data();
    }

    [[nodiscard]] operator std::span<const T>() const noexcept {
        return baseline_backed()
            ? std::span<const T>{}
            : std::span<const T>{local};
    }

    [[nodiscard]] const std::vector<T>& local_values() const noexcept {
        return local;
    }

private:
    struct element_block final {
        std::size_t index = 0;
        std::unique_ptr<T> value;
    };

    struct index_slot final {
        std::size_t key = 0;      // baseline index + 1
        std::size_t position = 0; // element_cache index + 1
    };

    struct range_block final {
        std::size_t begin = 0;
        std::size_t count = 0;
        std::unique_ptr<T[]> values;
    };

    [[nodiscard]] static std::uint64_t mix64(std::uint64_t value) noexcept {
        value ^= value >> 30;
        value *= 0xbf58476d1ce4e5b9ULL;
        value ^= value >> 27;
        value *= 0x94d049bb133111ebULL;
        return value ^ (value >> 31);
    }

    [[nodiscard]] const T* find_element(std::size_t index) const noexcept {
        if (element_index.empty())
            return nullptr;

        const auto key = index + 1;
        const auto mask = element_index.size() - 1;
        auto position = static_cast<std::size_t>(mix64(key)) & mask;

        for (std::size_t probe = 0; probe < element_index.size(); ++probe) {
            const auto& slot = element_index[position];
            if (slot.key == 0)
                return nullptr;
            if (slot.key == key) {
                const auto cache_index = slot.position - 1;
                return cache_index < element_cache.size()
                    ? element_cache[cache_index].value.get()
                    : nullptr;
            }
            position = (position + 1) & mask;
        }
        return nullptr;
    }

    void ensure_element_index() const {
        if (!element_index.empty() &&
            (element_cache.size() + 1) * 2 < element_index.size()) {
            return;
        }

        const auto required = element_cache.size() + 1;
        if (required > (std::numeric_limits<std::size_t>::max)() / 2)
            throw std::length_error("mapped_vector cache too large");

        const auto minimum = (required * 2 < 8) ? std::size_t{8} : required * 2;
        const auto capacity = std::bit_ceil(minimum);
        std::vector<index_slot> replacement(capacity);
        const auto mask = capacity - 1;

        for (std::size_t index = 0; index < element_cache.size(); ++index) {
            const auto key = element_cache[index].index + 1;
            auto position = static_cast<std::size_t>(mix64(key)) & mask;
            while (replacement[position].key != 0)
                position = (position + 1) & mask;
            replacement[position] = index_slot{key, index + 1};
        }

        element_index.swap(replacement);
    }

    void invalidate_ranges_containing(std::size_t index) noexcept {
        for (std::size_t position = 0; position < range_cache.size();) {
            const auto& range = range_cache[position];
            if (index >= range.begin && index - range.begin < range.count) {
                range_cache.erase(range_cache.begin() +
                    static_cast<std::ptrdiff_t>(position));
                continue;
            }
            ++position;
        }
    }

    [[nodiscard]] const T& materialize_element(std::size_t index) const noexcept {
        if (const auto* existing = find_element(index))
            return *existing;

        try {
            ensure_element_index();

            auto value = std::make_unique<T>();
            const auto result = reader != nullptr
                ? reader(context, index, *value)
                : status{status_code::invalid_state};
            if (!result.ok()) {
                if (read_failure.ok())
                    read_failure = result;
                failure_value = {};
                return failure_value;
            }

            element_cache.push_back(element_block{index, std::move(value)});
            const auto cache_position = element_cache.size();
            const auto key = index + 1;
            const auto mask = element_index.size() - 1;
            auto position = static_cast<std::size_t>(mix64(key)) & mask;
            while (element_index[position].key != 0)
                position = (position + 1) & mask;
            element_index[position] = index_slot{key, cache_position};
            return *element_cache.back().value;
        }
        catch (const std::bad_alloc&) {
            if (read_failure.ok())
                read_failure = {status_code::not_available};
        }
        catch (const std::length_error&) {
            if (read_failure.ok())
                read_failure = {status_code::not_available};
        }

        failure_value = {};
        return failure_value;
    }

    const void* context = nullptr;
    std::size_t baseline_count = 0;
    reader_type reader = nullptr;
    mutable status read_failure{};
    mutable std::vector<element_block> element_cache;
    mutable std::vector<index_slot> element_index;
    mutable std::vector<range_block> range_cache;
    mutable T failure_value{};
    std::vector<T> local;
};

// Read-only sequence facade used by persistence views. It keeps existing
// size/index iteration semantics without requiring one contiguous baseline copy.
template<class T>
class mapped_vector_view final {
public:
    class iterator final {
    public:
        using difference_type = std::ptrdiff_t;
        using value_type = T;

        iterator() noexcept = default;

        [[nodiscard]] T operator*() const noexcept {
            T output{};
            if (owner != nullptr)
                (void)owner->read(position, output);
            return output;
        }

        iterator& operator++() noexcept {
            ++position;
            return *this;
        }

        [[nodiscard]] friend bool operator==(
            const iterator& left,
            const iterator& right) noexcept {
            return left.owner == right.owner && left.position == right.position;
        }

    private:
        iterator(const mapped_vector<T>* owner_value, std::size_t position_value) noexcept
            : owner(owner_value), position(position_value) {}

        const mapped_vector<T>* owner = nullptr;
        std::size_t position = 0;

        friend class mapped_vector_view;
    };

    mapped_vector_view() noexcept = default;
    explicit mapped_vector_view(const mapped_vector<T>& values) noexcept
        : owner(&values) {}

    [[nodiscard]] std::size_t size() const noexcept {
        return owner != nullptr ? owner->size() : 0;
    }

    [[nodiscard]] bool empty() const noexcept {
        return size() == 0;
    }

    [[nodiscard]] T operator[](std::size_t index) const noexcept {
        T output{};
        if (owner != nullptr)
            (void)owner->read(index, output);
        return output;
    }

    [[nodiscard]] std::span<const T> span(
        std::size_t begin_value,
        std::size_t count_value) const {
        return owner != nullptr
            ? owner->span(begin_value, count_value)
            : std::span<const T>{};
    }

    [[nodiscard]] iterator begin() const noexcept {
        return iterator{owner, 0};
    }

    [[nodiscard]] iterator end() const noexcept {
        return iterator{owner, size()};
    }

    [[nodiscard]] operator std::span<const T>() const noexcept {
        return owner != nullptr
            ? static_cast<std::span<const T>>(*owner)
            : std::span<const T>{};
    }

private:
    const mapped_vector<T>* owner = nullptr;
};


} // namespace cw::server
