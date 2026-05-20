// include/helper/log.hpp

#pragma once
#ifdef __DEBUG

#ifndef FMT_HEADER_ONLY
#define FMT_HEADER_ONLY
#endif
#include "../3rd-party/fmt/color.h"
#include "../3rd-party/fmt/core.h"
#include "../3rd-party/fmt/format.h"

#include <chrono>
#include <concepts>
#include <source_location>
#include <stacktrace>
#include <type_traits>

namespace exodus::Log {

enum class level : uint8_t { Info, Warn, Error, Fatal };

// should we use cpo ? ... I dont think so .. im lazy AHHHHH.H.H...
class logger {

public:
  template <typename... Args>
    requires(sizeof...(Args) >= 0)
  static void output(
    level lvl,
    const std::source_location &loc,
    fmt::format_string<Args...> fmt_str,
    Args &&...args
  ) {

    fmt::print(
      fg(fmt::terminal_color::bright_black),
      "{}:{} ",
      loc.file_name(),
      loc.line()
    );

    fmt::print(fg(get_color(lvl)), "[{}] ", get_level_name(lvl));
    fmt::print(fmt_str, std::forward<Args>(args)...);
    fmt::print("\n");
  }

private:
  static std::string_view get_level_name(level lvl) {
    switch (lvl) {
    case level::Info:
      return "INFO";
    case level::Warn:
      return "WARN";
    case level::Error:
      return "ERROR";
    case level::Fatal:
      return "FATAL";
    }
    return "UNKNOWN";
  }

  static fmt::terminal_color get_color(level lvl) {
    switch (lvl) {
    case level::Info:
      return fmt::terminal_color::cyan;
    case level::Warn:
      return fmt::terminal_color::yellow;
    case level::Error:
      return fmt::terminal_color::red;
    case level::Fatal:
      return fmt::terminal_color::bright_red;
    default:
      return fmt::terminal_color::white;
    }
  }
};

class exception : public std::exception {
public:
  exception(std::string msg) : msg_(std::move(msg)) {
    trace_ += fmt::format(
      fg(fmt::terminal_color::bright_black), "--- stack trace ---\n"
    );
    capture_stack();
    trace_ +=
      fmt::format(fg(fmt::terminal_color::bright_black), "--- stack end ---\n");
  }

  const char *what() const noexcept override { return msg_.c_str(); }

  const std::string_view stacktrace() const { return trace_; }

private:
  void capture_stack() {
    auto trace = std::stacktrace::current();
    int cnt = 0;
    for (size_t i = 0; i < size(trace); ++i) {
      const auto &e = trace[i];
      if (e.description().empty())
        continue;
      trace_ += fmt::format(
        "  #{} {} at {}:{}\n",
        cnt++,
        e.description(),
        e.source_file(),
        e.source_line()
      );
    }
  }

  std::string msg_;
  std::string trace_;
};

template <typename Func>
void with_exception_handling(
  Func &&func, const std::source_location &loc = std::source_location::current()
) {
  try {
    std::forward<Func>(func)();
  } catch (const exception &e) {
    fmt::print("caught exception: {}\n", e.what());
    if (!e.stacktrace().empty()) {
      fmt::print("Trace from exception:\n{}", e.stacktrace());
    }
    logger::output(level::Fatal, loc, "Exception occurred!");
  } catch (const std::exception &e) {
    fmt::print("caught std::exception: {}\n", e.what());
  }
}

#define log_info(fmt_str, ...)                                                 \
  logger::output(                                                              \
    exodus::Log::level::Info,                                                  \
    std::source_location::current(),                                           \
    fmt_str,                                                                   \
    ##__VA_ARGS__                                                              \
  )

#define log_warn(fmt_str, ...)                                                 \
  logger::output(                                                              \
    exodus::Log::level::Warn,                                                  \
    std::source_location::current(),                                           \
    fmt_str,                                                                   \
    ##__VA_ARGS__                                                              \
  )

#define log_error(fmt_str, ...)                                                \
  logger::output(                                                              \
    exodus::Log::level::Error,                                                 \
    std::source_location::current(),                                           \
    fmt_str,                                                                   \
    ##__VA_ARGS__                                                              \
  )

#define log_fatal(fmt_str, ...)                                                \
  exodus::Log::logger::output(                                                 \
    level::Fatal, std::source_location::current(), fmt_str, ##__VA_ARGS__      \
  )

} // namespace exodus::Log

#else

namespace exodus::Log {

template <typename Format, typename... Args>
inline void log_info(Format, Args &&...) noexcept {}

template <typename Format, typename... Args>
inline void log_warn(Format, Args &&...) noexcept {}

template <typename Format, typename... Args>
inline void log_error(Format, Args &&...) noexcept {}

template <typename Format, typename... Args>
inline void log_fatal(Format, Args &&...) noexcept {}

template <typename Func>
inline void with_exception_handling(Func &&func) { // NOLINT
  func();
}

} // namespace exodus::Log

#endif
