#pragma once

#include <exception>
#include <iostream>
#include <string_view>

#define HT2MP_CHECK(expression)                                                                    \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            std::cerr << __FILE__ << ':' << __LINE__ << ": check failed: " #expression << '\n'; \
            return false;                                                                          \
        }                                                                                          \
    } while (false)

template <typename Function>
int run_test(std::string_view name, Function&& function) {
    try {
        if (!function()) {
            std::cerr << "FAILED: " << name << '\n';
            return 1;
        }
        std::cout << "PASS: " << name << '\n';
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "FAILED: " << name << ": " << exception.what() << '\n';
        return 1;
    }
}
