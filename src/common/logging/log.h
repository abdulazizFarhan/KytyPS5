#ifndef KYTY_COMMON_LOGGING_LOG_H_
#define KYTY_COMMON_LOGGING_LOG_H_

#include "common/common.h"
#include "common/subsystems.h"

#include <fmt/color.h>
#include <fmt/printf.h>
#include <string_view>

namespace Log {

KYTY_SUBSYSTEM_DEFINE(Log);

enum class Direction { Silent, Console, File };

// M1W6: numeric debug level. 0 = silent for verbose LOGFs, 1 =
// per-second [HEALTH] only (cheap, always on), 2 = NID + axis/button
// events, 3 = every LOGF. Defaults to 1 so release builds stay
// snappy but you always have the one-line-per-second health status.
enum class DebugLevel { Silent = 0, Health = 1, Nid = 2, Verbose = 3 };

void          SetDebugLevel(DebugLevel level);
DebugLevel    GetDebugLevel();
bool          IsAtLeast(DebugLevel level);
Direction GetDirection();
void      Write(std::string_view text);
void      Write(fmt::text_style style, std::string_view text);
void      WriteFatal(std::string_view text);
void      WriteFatal(fmt::text_style style, std::string_view text);
void      Flush();

namespace Color {

inline constexpr auto Default       = fmt::text_style {};
inline constexpr auto Red           = fmt::fg(fmt::terminal_color::red);
inline constexpr auto Green         = fmt::fg(fmt::terminal_color::green);
inline constexpr auto Yellow        = fmt::fg(fmt::terminal_color::yellow);
inline constexpr auto Magenta       = fmt::fg(fmt::terminal_color::magenta);
inline constexpr auto Cyan          = fmt::fg(fmt::terminal_color::cyan);
inline constexpr auto White         = fmt::fg(fmt::terminal_color::white);
inline constexpr auto BrightRed     = fmt::fg(fmt::terminal_color::bright_red);
inline constexpr auto BrightGreen   = fmt::fg(fmt::terminal_color::bright_green);
inline constexpr auto BrightYellow  = fmt::fg(fmt::terminal_color::bright_yellow);
inline constexpr auto BrightMagenta = fmt::fg(fmt::terminal_color::bright_magenta);
inline constexpr auto BrightWhite   = fmt::fg(fmt::terminal_color::bright_white);

} // namespace Color

} // namespace Log

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LOGF(...) ::Log::Write(::fmt::sprintf(__VA_ARGS__))
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LOGF_COLOR(style, ...) ::Log::Write((style), ::fmt::sprintf(__VA_ARGS__))

#endif /* KYTY_COMMON_LOGGING_LOG_H_ */
