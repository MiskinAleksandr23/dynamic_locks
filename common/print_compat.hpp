#pragma once

#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace print_compat {

struct FormatSpec {
  int width = 0;
  int precision = -1;
  bool left_align = false;
};

inline FormatSpec ParseSpec(std::string_view spec) {
  FormatSpec result;
  if (spec.empty()) {
    return result;
  }

  size_t pos = 0;
  if (spec[pos] == '<') {
    result.left_align = true;
    ++pos;
  } else if (spec[pos] == '>') {
    ++pos;
  }

  while (pos < spec.size() && spec[pos] >= '0' && spec[pos] <= '9') {
    result.width = result.width * 10 + static_cast<int>(spec[pos] - '0');
    ++pos;
  }

  if (pos < spec.size() && spec[pos] == '.') {
    ++pos;
    result.precision = 0;
    while (pos < spec.size() && spec[pos] >= '0' && spec[pos] <= '9') {
      result.precision =
          result.precision * 10 + static_cast<int>(spec[pos] - '0');
      ++pos;
    }
  }

  return result;
}

template <typename T> std::string FormatValue(const T &value, FormatSpec spec) {
  std::ostringstream out;
  if (spec.left_align) {
    out << std::left;
  }
  if (spec.width > 0) {
    out << std::setw(spec.width);
  }
  if (spec.precision >= 0) {
    out << std::fixed << std::setprecision(spec.precision);
  }
  out << value;
  return out.str();
}

inline void AppendFormat(std::ostringstream &out, std::string_view fmt) {
  out << fmt;
}

template <typename T, typename... Rest>
void AppendFormat(std::ostringstream &out, std::string_view fmt, T &&value,
                  Rest &&...rest) {
  const size_t open = fmt.find('{');
  if (open == std::string_view::npos) {
    out << fmt;
    return;
  }

  const size_t close = fmt.find('}', open);
  if (close == std::string_view::npos) {
    out << fmt;
    return;
  }

  out << fmt.substr(0, open);
  std::string_view spec;
  if (open + 1 < close && fmt[open + 1] == ':') {
    spec = fmt.substr(open + 2, close - open - 2);
  }
  out << FormatValue(std::forward<T>(value), ParseSpec(spec));
  AppendFormat(out, fmt.substr(close + 1), std::forward<Rest>(rest)...);
}

template <typename... Args>
void Print(std::string_view fmt, Args &&...args) {
  std::ostringstream out;
  AppendFormat(out, fmt, std::forward<Args>(args)...);
  std::cout << out.str();
}

template <typename... Args>
void Println(std::string_view fmt, Args &&...args) {
  Print(fmt, std::forward<Args>(args)...);
  std::cout << '\n';
}

} // namespace print_compat

namespace std {

template <typename... Args>
void print(std::string_view fmt, Args &&...args) {
  print_compat::Print(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void println(std::string_view fmt, Args &&...args) {
  print_compat::Println(fmt, std::forward<Args>(args)...);
}

inline void println() { std::cout << '\n'; }

} // namespace std
