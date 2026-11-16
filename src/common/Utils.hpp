#pragma once
#include <iostream>
#include <string>

class TextUtilities {
public:
    static void print(const std::string& s) {
        std::cout << s << std::flush;
    }
};


inline std::wstring Widen(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], n);
    return out;
}