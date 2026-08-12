#pragma once

#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace spatch::graphics::detail {

enum class CallbackFailure {
    allocation,
    standard,
    unknown,
};

// ReShade's convenience helper allocates first and then publishes the pointer
// through set_private_data. If publication throws while its internal map grows,
// that raw allocation is leaked. Keep ownership until publication succeeds.
template <typename State, typename Object, typename... Args>
State* CreatePrivateData(Object* object, Args&&... args) {
    if (object == nullptr) {
        return nullptr;
    }
    if (State* const existing = object->template get_private_data<State>()) {
        return existing;
    }

    auto state = std::make_unique<State>(std::forward<Args>(args)...);
    State* const published = state.get();
    object->set_private_data(
        reinterpret_cast<const std::uint8_t*>(&__uuidof(State)),
        static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(published)));
    state.release();
    return published;
}

template <auto Callback, auto Reporter, typename Signature = decltype(Callback)>
struct ReShadeCallbackBoundary;

template <auto Callback, auto Reporter, typename Result, typename... Args>
struct ReShadeCallbackBoundary<
    Callback,
    Reporter,
    Result (*)(Args...)> {
    static_assert(noexcept(Reporter(CallbackFailure::unknown, nullptr)));

    static Result Invoke(Args... args) noexcept {
        try {
            if constexpr (std::is_void_v<Result>) {
                Callback(std::forward<Args>(args)...);
                return;
            } else {
                return Callback(std::forward<Args>(args)...);
            }
        } catch (const std::bad_alloc& exception) {
            Reporter(CallbackFailure::allocation, exception.what());
        } catch (const std::exception& exception) {
            Reporter(CallbackFailure::standard, exception.what());
        } catch (...) {
            Reporter(CallbackFailure::unknown, nullptr);
        }

        if constexpr (!std::is_void_v<Result>) {
            return Result{};
        }
    }
};

template <auto Callback, auto Reporter, typename Result, typename... Args>
struct ReShadeCallbackBoundary<
    Callback,
    Reporter,
    Result (*)(Args...) noexcept> {
    static_assert(noexcept(Reporter(CallbackFailure::unknown, nullptr)));

    static Result Invoke(Args... args) noexcept {
        try {
            if constexpr (std::is_void_v<Result>) {
                Callback(std::forward<Args>(args)...);
                return;
            } else {
                return Callback(std::forward<Args>(args)...);
            }
        } catch (const std::bad_alloc& exception) {
            Reporter(CallbackFailure::allocation, exception.what());
        } catch (const std::exception& exception) {
            Reporter(CallbackFailure::standard, exception.what());
        } catch (...) {
            Reporter(CallbackFailure::unknown, nullptr);
        }

        if constexpr (!std::is_void_v<Result>) {
            return Result{};
        }
    }
};

}  // namespace spatch::graphics::detail
