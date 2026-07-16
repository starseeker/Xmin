#ifndef XMIN_NEXT_CHECKED_HPP
#define XMIN_NEXT_CHECKED_HPP

#include <limits>
#include <optional>
#include <type_traits>

namespace xmin::next {

template <typename T>
constexpr std::optional<T> checked_add(T left, T right) noexcept
{
    static_assert(std::is_integral_v<T>);
    if constexpr (std::is_unsigned_v<T>) {
        if (right > std::numeric_limits<T>::max() - left)
            return std::nullopt;
    }
    else {
        if ((right > 0 && left > std::numeric_limits<T>::max() - right) ||
            (right < 0 && left < std::numeric_limits<T>::min() - right)) {
            return std::nullopt;
        }
    }
    return static_cast<T>(left + right);
}

template <typename T>
constexpr std::optional<T> checked_subtract(T left, T right) noexcept
{
    static_assert(std::is_integral_v<T>);
    if constexpr (std::is_unsigned_v<T>) {
        if (right > left)
            return std::nullopt;
    }
    else {
        if ((right > 0 && left < std::numeric_limits<T>::min() + right) ||
            (right < 0 && left > std::numeric_limits<T>::max() + right)) {
            return std::nullopt;
        }
    }
    return static_cast<T>(left - right);
}

template <typename T>
constexpr std::optional<T> checked_multiply(T left, T right) noexcept
{
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    if (left != 0 && right > std::numeric_limits<T>::max() / left)
        return std::nullopt;
    return static_cast<T>(left * right);
}

constexpr std::optional<std::size_t> padded_to_four(std::size_t value) noexcept
{
    const auto sum = checked_add(value, std::size_t{3});
    if (!sum)
        return std::nullopt;
    return *sum & ~std::size_t{3};
}

} // namespace xmin::next

#endif
