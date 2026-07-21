#include "graphics/presentation/window.h"

#include <atomic>

#include "SDL.h"
#include "SDL_error.h"
#include "SDL_events.h"
#include "SDL_gamecontroller.h"
#include "SDL_hints.h"
#include "SDL_joystick.h"
#include "SDL_keyboard.h"
#include "SDL_keycode.h"
#include "SDL_mouse.h"
#include "SDL_pixels.h"
#include "SDL_rwops.h"
#include "SDL_stdinc.h"
#include "SDL_surface.h"
#include "SDL_thread.h"
#include "SDL_touch.h"
#include "SDL_video.h"
#include "SDL_vulkan.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/subsystems.h"
#include "common/systemInfo.h"
#include "common/threads.h"
#include "common/timer.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/utils.h"
#include "graphics/host_gpu/vma.h"
#include "graphics/presentation/renderDoc.h"
#include "graphics/presentation/videoOut.h"
#include "graphics/presentation/window/windowInternal.h"
#include "libs/controller.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <vulkan/vk_enum_string_helper.h>
#include <vulkan/vk_platform.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_SIMD
#include "stb_image.h"

#include <fmt/format.h>

// IWYU pragma: no_include <intrin.h>

#define KYTY_ENABLE_DEBUG_PRINTF
#define KYTY_DBG_INPUT

namespace Libs::Graphics {

constexpr float FPS_UPDATE_TIME        = 1.0f;
constexpr int   KEYBOARD_CONTROLLER_ID = -1000;

// M1W3: when set, the window thread also prints a once-per-second FPS
// summary to stdout. Independent of the in-window title-bar FPS
// (which is always shown when the window is focused). Useful for
// headless measurement runs and CI logs.
static std::atomic<bool> g_print_fps_to_stdout {false};
void SetFpsStdoutEnabled(bool enabled) { g_print_fps_to_stdout.store(enabled); }

// M1W7+: scripted input — a thread-safe queue of timed input events
// (key, button, mouse, stick, release-all, wait, label, goto) that the
// emulator replays into the controller state-machine in the same way
// real SDL input does. Let me run headless scenario tests like "open
// the Options menu in Worms" without a human at the keyboard.
//
// Format is plain text — one event per line:
//
//   <time_s> <type> <arg...> [down|up]
//
//   time          |   absolute game time in seconds (when the event fires)
//   type = key    |   SDL keyboard (Up Down Left Right Return Space Escape W A S D ...)
//   type = btn    |   SDL controller button (Cross Circle Square Triangle L1 R1 L2 R2
//                 |     Options Touchpad Up Down Left Right)
//   type = mouse  |   mouse button (LMB RMB MMB X1 X2)
//   type = stick  |   analog stick (LeftX LeftY RightX RightY TriggerLeft TriggerRight) value
//   type = rel    |   release all buttons + center sticks
//   type = wait   |   arg = seconds to sleep before the next event (relative)
//   type = goto   |   arg = label name; jump the timeline cursor to the named
//                 |     label's time, optionally re-armed by appending "+N"
//                 |     (e.g. "goto NAVIGATE 3" repeats 3 times total)
//
//   :LABEL on its own line defines a label (goto targets)
//   Lines beginning with '#' or empty lines are ignored.
//
// Examples:
//   0.0  rel                       ; clean slate
//   8.0  btn Cross   down          ; press X
//   8.2  btn Cross   up            ; release X
//   :NAVIGATE                      ; label
//   9.0  btn Down    down          ; press Down (returns to this label from the goto)
//   9.12 btn Down    up            ; release Down
//   goto NAVIGATE 3               ; loop the above back 3 times
//   20.0 rel                       ; cleanup
enum class ScriptedEventType : uint8_t { Key, Btn, Mouse, Stick, ReleaseAll, Wait, Goto };

struct ScriptedEvent {
	double             time;
	ScriptedEventType type;
	int                arg1   = 0;        // axis value / repeat count / unused
	int                arg2   = 0;        // unused (slots into label match)
	std::string        label;             // for Goto targets (LABEL strings)
	bool               down   = true;
};

static struct {
	std::vector<ScriptedEvent> events;
	size_t                     next_idx   = 0;
	std::atomic<bool>          active     {false};
	std::string                name       = "(none)";
	// M1W6+: when the human uses a real controller/keyboard while a
	// script is replaying, set this timestamp (game time seconds) and
	// skip scripted events until it decays past the timeout. Without
	// this the script and the human race to drive the game state and
	// nothing useful happens.
	double                     human_activity_until = 0.0;
	// M1W7+: cumulative target clock. Wait/Goto advance this clock
	// instead of inserting single events into the queue (which would
	// overflow for long scripts) and let ScriptedInputAdvance catch
	// up. Tracks the script's current "scheduled time".
	double                     timeline_now    = 0.0;
	// M1W7+: pending Goto target (label name + repeat counter).
	// Non-empty means: when ScriptedInputAdvance reaches the next
	// event-time boundary AND the linear index is at the goto
	// event, jump rather than firing. Implemented as a flag the
	// advance loop consumes.
	bool                       goto_pending    = false;
	std::string                goto_target;
	int                        goto_remaining  = 0;
} g_scripted;

// forward decl — body is below struct WindowGame so we can poke the
// current_time_seconds field of the active WindowGame. Forward
// declaration lets the SDL dispatcher call this safely.
static void ScriptedInputNotifyHumanActivity();

// Map a script "btn" name to an SDL_CONTROLLER_BUTTON_* constant.
[[maybe_unused]] static int ScriptedButtonId(const std::string& name) {
	if (name == "Cross" || name == "A")   return SDL_CONTROLLER_BUTTON_A;
	if (name == "Circle" || name == "B")  return SDL_CONTROLLER_BUTTON_B;
	if (name == "Square" || name == "X")  return SDL_CONTROLLER_BUTTON_X;
	if (name == "Triangle" || name == "Y") return SDL_CONTROLLER_BUTTON_Y;
	if (name == "L1")   return SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
	if (name == "R1")   return SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
	if (name == "L3")   return SDL_CONTROLLER_BUTTON_LEFTSTICK;
	if (name == "R3")   return SDL_CONTROLLER_BUTTON_RIGHTSTICK;
	if (name == "Options") return SDL_CONTROLLER_BUTTON_START;
	if (name == "Touchpad") return SDL_CONTROLLER_BUTTON_BACK;
	if (name == "Up")    return SDL_CONTROLLER_BUTTON_DPAD_UP;
	if (name == "Down")  return SDL_CONTROLLER_BUTTON_DPAD_DOWN;
	if (name == "Left")  return SDL_CONTROLLER_BUTTON_DPAD_LEFT;
	if (name == "Right") return SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
	return -1;
}

// Map a script "key" name to an SDLK_* constant.
static int ScriptedKeyCode(const std::string& name) {
	if (name == "Up")   return SDLK_UP;
	if (name == "Down") return SDLK_DOWN;
	if (name == "Left") return SDLK_LEFT;
	if (name == "Right") return SDLK_RIGHT;
	if (name == "Return") return SDLK_RETURN;
	if (name == "Space")  return SDLK_SPACE;
	if (name == "Escape") return SDLK_ESCAPE;
	if (name == "W") return SDLK_w;
	if (name == "A") return SDLK_a;
	if (name == "S") return SDLK_s;
	if (name == "D") return SDLK_d;
	if (name == "Tab") return SDLK_TAB;
	if (name == "Backspace") return SDLK_BACKSPACE;
	return -1;
}

// Map a script "mouse" name to a button id (1=LMB 2=MID 3=RMB X1=4 X2=5).
static int ScriptedMouseButton(const std::string& name) {
	if (name == "LMB") return SDL_BUTTON_LEFT;
	if (name == "MMB") return SDL_BUTTON_MIDDLE;
	if (name == "RMB") return SDL_BUTTON_RIGHT;
	if (name == "X1")  return SDL_BUTTON_X1;
	if (name == "X2")  return SDL_BUTTON_X2;
	return 0;
}

// Map a script "stick" name to a Controller::Axis.
static int ScriptedAxisId(const std::string& name) {
	if (name == "LeftX")   return static_cast<int>(Controller::Axis::LeftX);
	if (name == "LeftY")   return static_cast<int>(Controller::Axis::LeftY);
	if (name == "RightX")  return static_cast<int>(Controller::Axis::RightX);
	if (name == "RightY")  return static_cast<int>(Controller::Axis::RightY);
	if (name == "TriggerLeft")  return static_cast<int>(Controller::Axis::TriggerLeft);
	if (name == "TriggerRight") return static_cast<int>(Controller::Axis::TriggerRight);
	return -1;
}

bool LoadScriptedInput(const std::filesystem::path& path) {
	g_scripted.events.clear();
	g_scripted.next_idx = 0;
	g_scripted.active.store(false);

	Common::File f(path, Common::File::Mode::Read);
	if (f.IsInvalid()) {
		LOGF("ScriptedInput: failed to open script '%s'\n", path.string().c_str());
		return false;
	}
		auto buf = f.ReadWholeBuffer();
		f.Close();
		std::string content(reinterpret_cast<const char*>(buf.GetData()), buf.Size());

		// M1W7+: a :LABEL records the EVENT INDEX it sits at, not a
		// timeline_now. `goto NAME` rewinds the linear cursor (next_idx)
		// to that index. Timeline_now continues advancing (set by wait/goto),
		// but the per-event e.time carries the absolute game-time stamp the
		// ScriptedInputAdvance loop compares against the running clock.
		std::unordered_map<std::string, size_t> label_to_event_idx;

		// M1W7+: secondary pass for sorting + label resolution.
		// First pass: collect events in source order so labels know their
		// starting index, then sort.
		std::vector<ScriptedEvent> raw_events;

		int line_num = 0;
		size_t pos   = 0;
		while (pos <= content.size()) {
			size_t eol = content.find('\n', pos);
			std::string line;
			if (eol == std::string::npos) {
				line = content.substr(pos);
				pos  = content.size() + 1;
			} else {
				line = content.substr(pos, eol - pos);
				pos  = eol + 1;
			}
			++line_num;
			// strip trailing \r / whitespace
			while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
				line.pop_back();
			}
			if (line.empty() || line[0] == '#') {
				continue;
			}
			// :LABEL — record the index of the NEXT event so a later
			// `goto NAME` rewinds the cursor here. If the label sits at
			// the very end (no more events), the goto to it becomes a
			// no-op and we drop the entry.
			if (line[0] == ':') {
				std::string name = line.substr(1);
				if (!name.empty()) {
					label_to_event_idx[name] = raw_events.size();
				}
				continue;
			}
			std::istringstream iss(line);
			double              t_s = 0.0;
			std::string         type_str, arg1_str, arg2_str, down_str;
			iss >> t_s >> type_str >> arg1_str >> arg2_str >> down_str;

			ScriptedEvent e {};
			e.time = g_scripted.timeline_now + t_s;

			const bool arg2_is_down_spec = (arg2_str == "down" || arg2_str == "up");
			const std::string& down_spec = arg2_is_down_spec ? arg2_str : down_str;

			if (type_str == "key") {
				e.type  = ScriptedEventType::Key;
				e.arg1  = ScriptedKeyCode(arg1_str);
				e.down  = (down_spec.empty() || down_spec == "down");
			} else if (type_str == "btn") {
				e.type  = ScriptedEventType::Btn;
				e.arg1  = ScriptedButtonId(arg1_str);
				e.down  = (down_spec.empty() || down_spec == "down");
			} else if (type_str == "mouse") {
				e.type  = ScriptedEventType::Mouse;
				e.arg1  = ScriptedMouseButton(arg1_str);
				e.down  = (down_spec.empty() || down_spec == "down");
			} else if (type_str == "stick") {
				e.type  = ScriptedEventType::Stick;
				e.arg1  = ScriptedAxisId(arg1_str);
				e.arg2  = std::atoi(arg2_str.c_str());
			} else if (type_str == "rel") {
				e.type  = ScriptedEventType::ReleaseAll;
			} else if (type_str == "wait") {
				e.type = ScriptedEventType::Wait;
				e.arg1 = static_cast<int>(t_s);
				e.time = g_scripted.timeline_now;
				raw_events.push_back(e);
				g_scripted.timeline_now += t_s;
				continue;
			} else if (type_str == "goto") {
				e.type  = ScriptedEventType::Goto;
				e.label = arg1_str;
				auto it = label_to_event_idx.find(e.label);
				if (it == label_to_event_idx.end()) {
					LOGF("ScriptedInput: unknown label '%s' on line %d, skipping\n",
					     e.label.c_str(), line_num);
					continue;
				}
				e.arg2 = static_cast<int>(it->second);     // target event index
				e.arg1 = (!down_str.empty() && down_str != "0") ? std::atoi(down_str.c_str()) : 0;
				e.time = g_scripted.timeline_now;          // not used for ordering
				raw_events.push_back(e);
				continue;
			} else {
				LOGF("ScriptedInput: unknown type '%s' on line %d, skipping\n", type_str.c_str(),
				     line_num);
				continue;
			}
			raw_events.push_back(e);
		}
	// M1W7+: store in source order (not sorted) so label positions are
	// stable. Control-flow events (Wait/Goto) and labeled blocks need
	// their original positions; the Advance function uses next_idx (not
	// event time) as its primary cursor, with the time check as a guard.
	g_scripted.events = std::move(raw_events);

	g_scripted.name   = path.filename().string();
	g_scripted.active.store(!g_scripted.events.empty());
	LOGF("ScriptedInput: loaded %zu events from '%s', active=%d\n", g_scripted.events.size(),
	     path.string().c_str(), g_scripted.active.load());
	return true;
}

// Public: kick off replay (CLI calls this from main() before RunWindowMain).
void StartScriptedInputFromCli(const std::string& path) {
	if (!LoadScriptedInput(std::filesystem::path(path))) {
		g_scripted.active.store(false);
	}
}

// Dispatcher forward decl (defined after the SDL-style event structs +
// the actual GameEvent* dispatcher functions in this file).
static void DispatchScriptedEvent(const ScriptedEvent& e);

// Per-frame tick: fire any script events whose time has passed.
//
// M1W7+: handles Wait and Goto control flow.
//   Wait: bump the timeline cursor by N seconds.
//   Goto: rewind the linear event cursor (next_idx) to the label's
//     recorded event index. If loop counter > 1, set up a single
//     rewind that fires after the next dispatched event (so the
//     goto itself counts as the original execution).
static void ScriptedInputAdvance(double t_now) {
	if (!g_scripted.active.load()) {
		return;
	}
	// M1W6+: back off when the human is actively pressing buttons.
	if (t_now < g_scripted.human_activity_until) {
		if (Log::IsAtLeast(Log::DebugLevel::Nid)) {
			LOGF("ScriptedInput: paused (human active until %.3fs, now %.3fs)\n",
			     g_scripted.human_activity_until, t_now);
		}
		return;
	}
	while (g_scripted.next_idx < g_scripted.events.size()) {
		const auto& e = g_scripted.events[g_scripted.next_idx];
		if (e.type == ScriptedEventType::Wait) {
			// Wait bumps the timeline clock — useful for letting
			// the script pause without scheduling real events.
			g_scripted.timeline_now += e.arg1;
			if (Log::IsAtLeast(Log::DebugLevel::Nid)) {
				LOGF("ScriptedInput: wait %.3fs -> timeline now=%.3fs\n", e.arg1,
				     g_scripted.timeline_now);
			}
			++g_scripted.next_idx;
			continue;
		}
		if (e.type == ScriptedEventType::Goto) {
			// Goto rewinds next_idx to the label's recorded event index
			// (arg2). arg1 = total loop budget; first iteration counts
			// as one, so we decrement AFTER consuming an event from
			// the rewound range.
			const size_t target = static_cast<size_t>(e.arg2);
			if (Log::IsAtLeast(Log::DebugLevel::Nid)) {
				LOGF("ScriptedInput: goto '%s' -> event idx=%zu (loop budget=%d)\n",
				     e.label.c_str(), target, e.arg1);
			}
			if (e.arg1 > 0) {
				g_scripted.goto_remaining = e.arg1;
				g_scripted.goto_target    = e.label;
			}
			g_scripted.next_idx = target;
			continue;
		}
		if (e.time > t_now) {
			break;
		}
		// Real-world event past due — dispatch.
		++g_scripted.next_idx;
		if (Log::IsAtLeast(Log::DebugLevel::Nid) && e.type == ScriptedEventType::Btn) {
			Libs::Controller::ControllerHealth h {};
			Libs::Controller::ControllerGetHealth(&h);
			LOGF("ScriptedInput: pre-dispatch active_id=%d connected=%d\n", h.active_id,
			     h.connected ? 1 : 0);
		}
		DispatchScriptedEvent(e);
		if (Log::IsAtLeast(Log::DebugLevel::Nid)) {
			LOGF("ScriptedInput: t=%.3fs %s fired (idx=%zu/%zu)\n", e.time,
			     g_scripted.name.c_str(), g_scripted.next_idx, g_scripted.events.size());
		}
		// M1W7+: pending goto-loop. After dispatching one event from
		// the rewound range, if we still owe loops, seek next_idx back
		// to the goto event so the range fires again.
		if (g_scripted.goto_remaining > 0) {
			--g_scripted.goto_remaining;
			if (g_scripted.goto_remaining > 0) {
				// Seek back to the goto event (already consumed — but
				// we'll re-iterate the loop body, which is the labeled
				// range AFTER the goto).
				for (size_t i = 0; i < g_scripted.events.size(); ++i) {
					if (g_scripted.events[i].type == ScriptedEventType::Goto &&
					    g_scripted.events[i].label == g_scripted.goto_target) {
						g_scripted.next_idx = i + 1;
						break;
					}
				}
			}
		}
	}
}


struct EventKeyboard {
	bool     down;
	bool     up;
	bool     pressed;
	bool     released;
	bool     repeat;
	int      scan_code;
	int      key_code;
	uint16_t mod;
	double   timestamp_seconds;
};

static uint32_t KeyboardKeyToPadButton(int key_code) {
	switch (key_code) {
		case SDLK_w: return Controller::PAD_BUTTON_UP;
		case SDLK_a: return Controller::PAD_BUTTON_LEFT;
		case SDLK_s: return Controller::PAD_BUTTON_DOWN;
		case SDLK_d: return Controller::PAD_BUTTON_RIGHT;
		case SDLK_j: return Controller::PAD_BUTTON_CROSS;
		case SDLK_i: return Controller::PAD_BUTTON_TRIANGLE;
		case SDLK_k: return Controller::PAD_BUTTON_SQUARE;
		case SDLK_l: return Controller::PAD_BUTTON_CIRCLE;
		case SDLK_q: return Controller::PAD_BUTTON_L1;
		case SDLK_e: return Controller::PAD_BUTTON_R1;
		case SDLK_RETURN:
		case SDLK_RETURN2: return Controller::PAD_BUTTON_OPTIONS;
		case SDLK_BACKSPACE:
		case SDLK_TAB: return Controller::PAD_BUTTON_TOUCH_PAD;
		default: return 0;
	}
}

static uint32_t ControllerButtonToPadButton(int button) {
	switch (button) {
		case SDL_CONTROLLER_BUTTON_A: return Controller::PAD_BUTTON_CROSS;
		case SDL_CONTROLLER_BUTTON_B: return Controller::PAD_BUTTON_CIRCLE;
		case SDL_CONTROLLER_BUTTON_X: return Controller::PAD_BUTTON_SQUARE;
		case SDL_CONTROLLER_BUTTON_Y: return Controller::PAD_BUTTON_TRIANGLE;
		case SDL_CONTROLLER_BUTTON_BACK: return Controller::PAD_BUTTON_TOUCH_PAD;
		case SDL_CONTROLLER_BUTTON_START: return Controller::PAD_BUTTON_OPTIONS;
		case SDL_CONTROLLER_BUTTON_LEFTSTICK: return Controller::PAD_BUTTON_L3;
		case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return Controller::PAD_BUTTON_R3;
		case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return Controller::PAD_BUTTON_L1;
		case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return Controller::PAD_BUTTON_R1;
		case SDL_CONTROLLER_BUTTON_DPAD_UP: return Controller::PAD_BUTTON_UP;
		case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return Controller::PAD_BUTTON_DOWN;
		case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return Controller::PAD_BUTTON_LEFT;
		case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return Controller::PAD_BUTTON_RIGHT;
		default: return 0;
	}
}

static Controller::Axis ControllerAxisFromSdl(int axis_id) {
	switch (axis_id) {
		case SDL_CONTROLLER_AXIS_LEFTX: return Controller::Axis::LeftX;
		case SDL_CONTROLLER_AXIS_LEFTY: return Controller::Axis::LeftY;
		case SDL_CONTROLLER_AXIS_RIGHTX: return Controller::Axis::RightX;
		case SDL_CONTROLLER_AXIS_RIGHTY: return Controller::Axis::RightY;
		case SDL_CONTROLLER_AXIS_TRIGGERLEFT: return Controller::Axis::TriggerLeft;
		case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return Controller::Axis::TriggerRight;
		default: return Controller::Axis::AxisMax;
	}
}

static bool ControllerAxisIsTrigger(int axis_id) {
	return axis_id == SDL_CONTROLLER_AXIS_TRIGGERLEFT ||
	       axis_id == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
}

static int ControllerAxisValueFromSdl(int axis_id, int axis_value) {
	return ControllerAxisIsTrigger(axis_id)
	           ? Controller::controller_get_axis(0, SDL_JOYSTICK_AXIS_MAX, axis_value)
	           : Controller::controller_get_axis(SDL_JOYSTICK_AXIS_MIN, SDL_JOYSTICK_AXIS_MAX,
	                                             axis_value);
}

struct EventMouse {
	bool   down;
	bool   up;
	bool   left;
	bool   middle;
	bool   right;
	bool   x1;
	bool   x2;
	bool   touch;
	bool   pressed;
	bool   released;
	int    num_of_clicks;
	bool   wheel;
	int    x;
	int    y;
	bool   motion;
	int    motion_x;
	int    motion_y;
	double timestamp_seconds;
};

struct EventFinger {
	bool   down;
	bool   up;
	bool   motion;
	int    touch_id;
	int    finger_id;
	float  x;
	float  y;
	float  dx;
	float  dy;
	float  pressure;
	double timestamp_seconds;
};

struct EventController {
	int    id;
	int    button;
	int    axis_id;
	int    axis_value;
	bool   down;
	bool   up;
	bool   added;
	bool   removed;
	bool   remapped;
	bool   axis;
	bool   pressed;
	bool   released;
	double timestamp_seconds;
};

enum class DisplayOrientation {
	Unknown,   /* The display orientation can't be determined */
	Landscape, /* The display is in landscape mode, with the right side up, relative to portrait
	              mode */
	LandscapeFlipped, /* The display is in landscape mode, with the left side up, relative to
	                     portrait mode */
	Portrait,         /* The display is in portrait mode */
	PortraitFlipped,  /* The display is in portrait mode, upside down */

	DisplayEventOrientation = 0xF0
};

struct EventDisplay {
	DisplayOrientation orientation;
};

constexpr uint32_t KYTY_SDL_BUTTON_LMASK  = SDL_BUTTON_LMASK;  // NOLINT(hicpp-signed-bitwise)
constexpr uint32_t KYTY_SDL_BUTTON_MMASK  = SDL_BUTTON_MMASK;  // NOLINT(hicpp-signed-bitwise)
constexpr uint32_t KYTY_SDL_BUTTON_RMASK  = SDL_BUTTON_RMASK;  // NOLINT(hicpp-signed-bitwise)
constexpr uint32_t KYTY_SDL_BUTTON_X1MASK = SDL_BUTTON_X1MASK; // NOLINT(hicpp-signed-bitwise)
constexpr uint32_t KYTY_SDL_BUTTON_X2MASK = SDL_BUTTON_X2MASK; // NOLINT(hicpp-signed-bitwise)

struct WindowGame {
	void* private_data = nullptr;
	void* event        = nullptr;

	bool     m_game_need_exit        = {false};
	bool     m_game_is_paused        = {false};
	uint32_t m_screen_width          = {0};
	uint32_t m_screen_height         = {0};
	double   m_current_time_seconds  = {0.0};
	double   m_previous_time_seconds = {0.0};
	int      m_update_num            = {0};
	int      m_frame_num             = {0};
	double   m_update_time_seconds   = {0.0};
	double   m_current_fps           = {0.0};
	int      m_max_updates_per_frame = {4};
	double   m_update_fixed_time     = 1.0 / 60.0;
	int      m_fps_frames_num        = {0};
	double   m_fps_start_time        = {0};
};

// M1W6+: body of the human-activity forward decl above. Set when
// real SDL events arrive; ScriptedInputAdvance uses this to back
// off so the script doesn't fight the live user during scenario
// test debugging. Lives here (after the WindowGame definition)
// because it touches g_window_ctx->game->m_current_time_seconds.
static void ScriptedInputNotifyHumanActivity() {
	if (!g_scripted.active.load()) {
		return;
	}
	if (g_window_ctx != nullptr && g_window_ctx->game != nullptr) {
		g_scripted.human_activity_until = g_window_ctx->game->m_current_time_seconds + 1.5;
	}
}

struct WindowGamePrivate {
	WindowGamePrivate() = default;

	Common::Mutex   mutex;
	int             skip_frames = 0;
	GraphicContext* ctx         = nullptr;
};

WindowContext* g_window_ctx = nullptr;

constexpr const char* KYTY_SDL_WINDOW_CAPTION = "Game";
constexpr uint32_t    KYTY_SDL_WINDOW_FLAGS =
    (static_cast<uint32_t>(SDL_WINDOW_HIDDEN) | static_cast<uint32_t>(SDL_WINDOW_VULKAN));
constexpr int KYTY_SDL_WINDOWPOS_CENTERED = SDL_WINDOWPOS_CENTERED; /*NOLINT(hicpp-signed-bitwise)*/

static void CalcFrameTime(WindowGame* game, double game_time_s) {
	game->m_previous_time_seconds = game->m_current_time_seconds;
	game->m_current_time_seconds  = game_time_s;

	game->m_frame_num++;
	game->m_fps_frames_num++;

	const auto fps_time = game->m_current_time_seconds - game->m_fps_start_time;
	if (fps_time > FPS_UPDATE_TIME) {
		game->m_current_fps    = static_cast<double>(game->m_fps_frames_num) / fps_time;
		game->m_fps_frames_num = 0;
		game->m_fps_start_time = game->m_current_time_seconds;
	}
}

static bool Init(WindowGame* /*game*/) {
	return true;
}
static bool Update(WindowGame* /*game*/) {
	return true;
}
static bool Render(WindowGame* /*game*/) {
	return true;
}
static bool Close(WindowGame* /*game*/) {
	return true;
}
static void SetPause(WindowGame* game, bool flag) {
	LOGF("Pause: %s\n", flag ? "true" : "false");

	game->m_game_is_paused = flag;
}

static bool RenderAndUpdate(WindowGame* game) {
	static double lag = 0.0;

	// M1W6+: per-frame scripted-input tick. Fires queued key/btn/mouse
	// events whose scheduled time has passed. Cheap when no script is
	// loaded (atomic load + return).
	ScriptedInputAdvance(game->m_current_time_seconds);

	lag += game->m_current_time_seconds - game->m_previous_time_seconds;

	int num = 0;

	bool ok = true;

	while (lag >= game->m_update_fixed_time) {
		if (num < game->m_max_updates_per_frame) {
			ok = ok && Update(game);

			game->m_update_num++;
			num++;
			game->m_update_time_seconds = game->m_update_num * game->m_update_fixed_time;
		}

		lag -= game->m_update_fixed_time;
	}

	ok = ok && Render(game);

	return ok;
}

bool GameInit(WindowGame* game, const Common::Timer& timer, void* data) {
	EXIT_IF(game == nullptr);
	EXIT_IF(data == nullptr);
	EXIT_IF(game->private_data || game->event);

	auto* ctx = static_cast<GraphicContext*>(data);

	EXIT_IF(ctx->screen_width == 0 || ctx->screen_height == 0);

	auto* pdata = new WindowGamePrivate;
	pdata->ctx  = ctx;

	game->private_data = pdata;
	game->event        = new SDL_Event;

	game->m_screen_width  = ctx->screen_width;
	game->m_screen_height = ctx->screen_height;

	CalcFrameTime(game, timer.GetTimeS());

	return Init(game);
}

bool GameClose(WindowGame* game) {
	EXIT_IF(!game);

	EXIT_IF(!game->private_data || !game->event);

	delete (static_cast<WindowGamePrivate*>(game->private_data));
	delete (static_cast<SDL_Event*>(game->event));

	return Close(game);
}

void GameShowWindow(WindowGame* game, const Common::Timer& timer) {
	EXIT_IF(!game);

	auto* p = static_cast<WindowGamePrivate*>(game->private_data);

	EXIT_IF(!p);

	p->mutex.Lock();
	{
		if (p->skip_frames > 0) {
			p->skip_frames--;
			LOGF("skip frame %d\n", p->skip_frames);
		} else {
			VideoOut::VideoOutBeginVblank();
			if (VideoOut::VideoOutFlipWindow(0)) {
				CalcFrameTime(game, timer.GetTimeS());
			}
			VideoOut::VideoOutEndVblank();
		}
	}
	p->mutex.Unlock();
}

void GameEventQuit(WindowGame* game) {
	LOGF("Event: quit\n");

	game->m_game_need_exit = true;
}

void GameEventTerminate(WindowGame* game) {
	LOGF("Event: terminate\n");

	game->m_game_need_exit = true;
}

void GameEventKeyboard(WindowGame* game, const EventKeyboard* key) {
#ifdef KYTY_DBG_INPUT
	LOGF("Key: time = %.04f, %s%s, %s%s, %s, scan = %d, key = %d, mod = %04" PRIx16 "\n",
	     key->timestamp_seconds, (key->down ? "down" : ""), (key->up ? "up" : ""),
	     (key->pressed ? "pressed" : ""), (key->released ? "released" : ""),
	     (key->repeat ? "repeat" : ""), key->scan_code, key->key_code, key->mod);
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS || KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	if (key->down) {
		switch (key->key_code) {
			case SDLK_ESCAPE: game->m_game_need_exit = true; break;
			case SDLK_SPACE: SetPause(game, !game->m_game_is_paused); break;
			case SDLK_F1:
				if (!key->repeat) {
					RenderDocRequestCapture();
				}
				break;
			default: break;
		}
	}

	const auto button = KeyboardKeyToPadButton(key->key_code);
	if (button != 0 && (key->down || key->up) && !key->repeat) {
		static bool keyboard_connected = false;
		if (!keyboard_connected) {
			Controller::ControllerConnect(KEYBOARD_CONTROLLER_ID);
			keyboard_connected = true;
		}
		ScriptedInputNotifyHumanActivity();
		Controller::ControllerButton(KEYBOARD_CONTROLLER_ID, button, key->down);
	}
#endif
}

void GameEventMouse([[maybe_unused]] WindowGame* game, [[maybe_unused]] const EventMouse* mb) {
	// M1W5: route mouse motion to the right analog stick so M&K users
	// can aim a camera (Worms reads right_stick_x/y). Mouse buttons map
	// to Cross (LMB), Circle (RMB), Square (MMB). Sensitivity 1 unit per
	// SDL pixel; SDL's xrel/yrel are already delta values.
	if (mb->motion) {
		const int sens = 8; // amplify small deltas so the stick reaches the edge
		int rx = mb->motion_x * sens;
		int ry = mb->motion_y * sens;
		if (rx > 127) rx = 127;
		if (rx < -128) rx = -128;
		if (ry > 127) ry = 127;
		if (ry < -128) ry = -128;
		Controller::Axis rx_a = Controller::Axis::RightX;
		Controller::Axis ry_a = Controller::Axis::RightY;
		Controller::ControllerAxis(KEYBOARD_CONTROLLER_ID, rx_a, 128 + rx);
		Controller::ControllerAxis(KEYBOARD_CONTROLLER_ID, ry_a, 128 + ry);
	}
	if (mb->down || mb->up) {
		uint32_t btn = 0;
		if (mb->left) btn = Controller::PAD_BUTTON_CROSS;
		else if (mb->right) btn = Controller::PAD_BUTTON_CIRCLE;
		else if (mb->middle) btn = Controller::PAD_BUTTON_SQUARE;
		else if (mb->x1) btn = Controller::PAD_BUTTON_TRIANGLE;
		else if (mb->x2) btn = Controller::PAD_BUTTON_R1;
		if (btn != 0) {
			ScriptedInputNotifyHumanActivity();
			Controller::ControllerButton(KEYBOARD_CONTROLLER_ID, btn, mb->down);
		}
	}
#ifdef KYTY_DBG_INPUT
	if (mb->wheel) {
		LOGF("Mouse wheel: time = %.04f, %s[%d, %d]\n", mb->timestamp_seconds,
		     (mb->touch ? "touch, " : ""), mb->x, mb->y);
	} else if (mb->motion) {
		LOGF("Mouse motion: time = %.04f, %s%s%s%s%s%s, [%d, %d], (%d, %d)\n",
		     mb->timestamp_seconds, (mb->left ? "left" : ""), (mb->middle ? "middle" : ""),
		     (mb->right ? "right" : ""), (mb->x1 ? "x1" : ""), (mb->x2 ? "x2" : ""),
		     (mb->touch ? "_touch" : ""), mb->x, mb->y, mb->motion_x, mb->motion_y);
	} else {
		LOGF("Mouse click: time = %.04f, %d, %s%s%s%s%s%s, %s%s, %s%s, [%d, %d]\n",
		     mb->timestamp_seconds, mb->num_of_clicks, (mb->left ? "left" : ""),
		     (mb->middle ? "middle" : ""), (mb->right ? "right" : ""), (mb->x1 ? "x1" : ""),
		     (mb->x2 ? "x2" : ""), (mb->touch ? "_touch" : ""), (mb->down ? "down" : ""),
		     (mb->up ? "up" : ""), (mb->pressed ? "pressed" : ""), (mb->released ? "released" : ""),
		     mb->x, mb->y);
	}
#endif
}

void GameEventFinger([[maybe_unused]] WindowGame* game, [[maybe_unused]] const EventFinger* f) {
#ifdef KYTY_DBG_INPUT
	if (f->motion) {
		LOGF("Finger motion: time = %.04f, %d, %d, (x,y) = [%f, %f], (dx,dy) = [%f, %f], pressure "
		     "= %f\n",
		     f->timestamp_seconds, f->touch_id, f->finger_id, f->x, f->y, f->dx, f->dy,
		     f->pressure);
	} else {
		LOGF("Finger press: time = %.04f, %d, %d, %s%s, (x,y) = [%f, %f], (dx,dy) = [%f, %f], "
		     "pressure = %f\n",
		     f->timestamp_seconds, f->touch_id, f->finger_id, (f->down ? "down" : ""),
		     (f->up ? "up" : ""), f->x, f->y, f->dx, f->dy, f->pressure);
	}
#endif
}

void GameEventController([[maybe_unused]] WindowGame*            game,
                         [[maybe_unused]] const EventController* f) {
	EXIT_NOT_IMPLEMENTED(f->remapped);

#ifdef KYTY_DBG_INPUT
	if (f->added || f->removed) {
		LOGF("Controller %s: %d, time = %.04f\n", (f->added ? "added" : "removed"), f->id,
		     f->timestamp_seconds);
	} else if (f->axis) {
		LOGF("Controller axis: %d, axis = %d, value = %d, time = %.04f\n", f->id, f->axis_id,
		     f->axis_value, f->timestamp_seconds);
	} else {
		LOGF("Controller button: "
		     "%d, %s%s, %s%s, button = %d, time = %.04f\n",
		     f->id, (f->down ? "down" : ""), (f->up ? "up" : ""), (f->pressed ? "pressed" : ""),
		     (f->released ? "released" : ""), f->button, f->timestamp_seconds);
	}
#endif

	if (f->added) {
		auto* pad = SDL_GameControllerOpen(f->id);
		EXIT_NOT_IMPLEMENTED(pad == nullptr);
		int id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad));
		Controller::ControllerConnect(id);
	}

	if (f->removed) {
		Controller::ControllerDisconnect(f->id);
		SDL_GameControllerClose(SDL_GameControllerFromInstanceID(f->id));
	}

	if (f->down || f->up) {
		const auto button = ControllerButtonToPadButton(f->button);
		if (button != 0) {
			// M1W6+: a real SDL button event arrived — this is the
			// human taking over. Flag scripted input to back off so
			// the replay doesn't fight the live user. Decays off
			// after a short timeout so the script can resume if the
			// human goes quiet.
			ScriptedInputNotifyHumanActivity();
			Controller::ControllerButton(f->id, button, f->down);
		}
	}

	if (f->axis) {
		const auto axis = ControllerAxisFromSdl(f->axis_id);
		if (axis != Controller::Axis::AxisMax) {
			ScriptedInputNotifyHumanActivity();
			Controller::ControllerAxis(f->id, axis,
			                           ControllerAxisValueFromSdl(f->axis_id, f->axis_value));
		}
	}
}

// M1W6+: dispatcher for scripted input (headless scenario tests).
// Routes around the SDL<GameControllerButton>→ControllerButton mapping
// because the active gamepad id depends on whatever SDL connected
// (real Xbox controller, virtual joystick, etc). Instead we ask the
// controller subsystem for the currently active id and push the input
// straight into the state machine — the game sees an identical event
// stream regardless of the physical controller source.
static void DispatchScriptedEvent(const ScriptedEvent& e) {
	auto* game = g_window_ctx->game;

	Libs::Controller::ControllerHealth ch {};
	Libs::Controller::ControllerGetHealth(&ch);
	const int target_id = (ch.connected && ch.active_id >= 0) ? ch.active_id : KEYBOARD_CONTROLLER_ID;

	switch (e.type) {
		case ScriptedEventType::Key: {
			EventKeyboard key {};
			key.down              = e.down;
			key.up                = !e.down;
			key.pressed           = e.down;
			key.released          = !e.down;
			key.repeat            = false;
			key.scan_code         = 0;
			key.key_code          = e.arg1;
			key.mod               = 0;
			key.timestamp_seconds = e.time;
			Libs::Graphics::GameEventKeyboard(game, &key);
			break;
		}
		case ScriptedEventType::Btn: {
			// SDL→Pad translation is the same the dispatch functions
			// use, so we just inline it here rather than synthesizing
			// an EventController and risk SDL opening a non-existent
			// joystick.
			const uint32_t btn = ControllerButtonToPadButton(e.arg1);
			if (btn != 0) {
				Controller::ControllerButton(target_id, btn, e.down);
				if (Log::IsAtLeast(Log::DebugLevel::Nid)) {
					// Read back state immediately to confirm the press
					// actually landed. If not, the active_id mismatch
					// problem is biting and we need to fix the dispatch.
					Libs::Controller::ControllerHealth h {};
					Libs::Controller::ControllerGetHealth(&h);
					LOGF("ScriptedInput: pressed pad_btn=0x%08x down=%d target_id=%d "
					     "active=%d last_buttons=0x%08x\n",
					     btn, e.down ? 1 : 0, target_id, h.active_id, h.last_buttons);
				}
			}
			break;
		}
		case ScriptedEventType::Mouse: {
			EventMouse m {};
			m.down              = e.down;
			m.up                = !e.down;
			m.pressed           = e.down;
			m.released          = !e.down;
			m.left              = (e.arg1 == SDL_BUTTON_LEFT);
			m.middle            = (e.arg1 == SDL_BUTTON_MIDDLE);
			m.right             = (e.arg1 == SDL_BUTTON_RIGHT);
			m.x1                = (e.arg1 == SDL_BUTTON_X1);
			m.x2                = (e.arg1 == SDL_BUTTON_X2);
			m.timestamp_seconds = e.time;
			Libs::Graphics::GameEventMouse(game, &m);
			break;
		}
		case ScriptedEventType::Stick:
			Controller::ControllerAxis(target_id, static_cast<Controller::Axis>(e.arg1), e.arg2);
			break;
		case ScriptedEventType::ReleaseAll: {
			// Send "up" for every button we know about, plus center sticks.
			const int all_btns[] = {SDL_CONTROLLER_BUTTON_A,     SDL_CONTROLLER_BUTTON_B,
			                        SDL_CONTROLLER_BUTTON_X,     SDL_CONTROLLER_BUTTON_Y,
			                        SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_BACK,
			                        SDL_CONTROLLER_BUTTON_LEFTSTICK,
			                        SDL_CONTROLLER_BUTTON_RIGHTSTICK,
			                        SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
			                        SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
			                        SDL_CONTROLLER_BUTTON_DPAD_UP,    SDL_CONTROLLER_BUTTON_DPAD_DOWN,
			                        SDL_CONTROLLER_BUTTON_DPAD_LEFT,  SDL_CONTROLLER_BUTTON_DPAD_RIGHT};
			for (int b: all_btns) {
				const uint32_t btn = ControllerButtonToPadButton(b);
				if (btn != 0) {
					Controller::ControllerButton(target_id, btn, false);
				}
			}
			Controller::ControllerAxis(target_id, Controller::Axis::LeftX, 128);
			Controller::ControllerAxis(target_id, Controller::Axis::LeftY, 128);
			Controller::ControllerAxis(target_id, Controller::Axis::RightX, 128);
			Controller::ControllerAxis(target_id, Controller::Axis::RightY, 128);
			break;
		}
		case ScriptedEventType::Wait:
		case ScriptedEventType::Goto:
			// Control-flow events are handled in ScriptedInputAdvance,
			// not here. Anything that reaches DispatchScriptedEvent
			// means the timeline is malformed; silently skip.
			break;
	}
}

void GameEventDisplay([[maybe_unused]] WindowGame* game, [[maybe_unused]] const EventDisplay* d) {
	auto* p   = static_cast<WindowGamePrivate*>(game->private_data);
	auto* ctx = static_cast<GraphicContext*>(p->ctx);

	p->mutex.Lock();
	game->m_screen_width  = ctx->screen_width;
	game->m_screen_height = ctx->screen_height;
	p->mutex.Unlock();
}

void GameEventLowMemory(WindowGame* /*game*/) {
	LOGF("Event: low_memory\n");
}

void GameEventWillEnterBackground(WindowGame* game) {
	LOGF("Event: will_enter_background\n");

	SetPause(game, true);
}

void GameEventDidEnterBackground(WindowGame* /*game*/) {
	LOGF("Event: did_enter_background\n");
}

void GameEventWillEnterForeground(WindowGame* /*game*/) {
	LOGF("Event: will_enter_foreground\n");
}

void GameEventDidEnterForeground(WindowGame* game) {
	LOGF("Event: did_enter_foreground\n");

	SetPause(game, false);
}

void GameEventResize(WindowGame* game, uint32_t new_width, uint32_t new_height) {
	EXIT_IF(new_width == 0 || new_height == 0);
	EXIT_IF(!game);

	auto* p = static_cast<WindowGamePrivate*>(game->private_data);
	EXIT_IF(p == nullptr);

	auto* ctx = static_cast<GraphicContext*>(p->ctx);
	EXIT_IF(ctx == nullptr);

	p->mutex.Lock();
	{
		p->skip_frames++;
		ctx->screen_width  = new_width;
		ctx->screen_height = new_height;

		game->m_screen_width  = ctx->screen_width;
		game->m_screen_height = ctx->screen_height;
	}
	p->mutex.Unlock();
}

static void ProcessWindowEvent(WindowGame* game, SDL_WindowEvent window) {
	switch (window.event) {
		case SDL_WINDOWEVENT_SHOWN: LOGF("Window %" PRIu32 " shown\n", window.windowID); break;

		case SDL_WINDOWEVENT_HIDDEN: LOGF("Window %" PRIu32 " hidden\n", window.windowID); break;

		case SDL_WINDOWEVENT_EXPOSED: LOGF("Window %" PRIu32 " exposed\n", window.windowID); break;

		case SDL_WINDOWEVENT_MOVED:
			LOGF("Window %" PRIu32 " moved to %" PRId32 ",%" PRId32 "\n", window.windowID,
			     window.data1, window.data2);
			break;

		case SDL_WINDOWEVENT_RESIZED:
			LOGF("Window %" PRIu32 " resized to %" PRId32 "x%" PRId32 "\n", window.windowID,
			     window.data1, window.data2);

			LOGF("m: %d\n", static_cast<int>(SDL_ThreadID()));
			GameEventResize(game, window.data1, window.data2);

			break;

		case SDL_WINDOWEVENT_SIZE_CHANGED:
			LOGF("Window %" PRIu32 " size changed to %" PRId32 "x%" PRId32 "\n", window.windowID,
			     window.data1, window.data2);

			LOGF("m: %d\n", static_cast<int>(SDL_ThreadID()));
			GameEventResize(game, window.data1, window.data2);

			break;

		case SDL_WINDOWEVENT_MINIMIZED:
			LOGF("Window %" PRIu32 " minimized\n", window.windowID);
			break;
		case SDL_WINDOWEVENT_MAXIMIZED:
			LOGF("Window %" PRIu32 " maximized\n", window.windowID);
			break;
		case SDL_WINDOWEVENT_RESTORED:
			LOGF("Window %" PRIu32 " restored\n", window.windowID);
			break;
		case SDL_WINDOWEVENT_ENTER:
			LOGF("Mouse entered window %" PRIu32 "\n", window.windowID);
			break;
		case SDL_WINDOWEVENT_LEAVE: LOGF("Mouse left window %" PRIu32 "\n", window.windowID); break;
		case SDL_WINDOWEVENT_FOCUS_GAINED:
			LOGF("Window %" PRIu32 " gained keyboard focus\n", window.windowID);
			break;
		case SDL_WINDOWEVENT_FOCUS_LOST:
			LOGF("Window %" PRIu32 " lost keyboard focus\n", window.windowID);
			break;
		case SDL_WINDOWEVENT_CLOSE: LOGF("Window %" PRIu32 " closed\n", window.windowID); break;
		default:
			LOGF("Window %" PRIu32 " got unknown event %" PRIu8 "\n", window.windowID,
			     window.event);
			break;
	}
}

static void ProcessDisplayEvent(WindowGame* game, SDL_DisplayEvent display) {
	bool sdl = false;

	switch (display.event) {
		case SDL_DISPLAYEVENT_ORIENTATION: sdl = true; [[fallthrough]];
		case static_cast<Uint8>(DisplayOrientation::DisplayEventOrientation): {
			LOGF("Display %" PRIu32 "[%s] changed orientation to %d - ", display.display,
			     sdl ? "SDL" : "Kyty", static_cast<int>(display.data1));

			EventDisplay d {};
			d.orientation = DisplayOrientation::Unknown;

			switch (display.data1) {
				case SDL_ORIENTATION_UNKNOWN: LOGF("UNKNOWN\n"); break;
				case SDL_ORIENTATION_LANDSCAPE:
					LOGF("LANDSCAPE\n");
					d.orientation = DisplayOrientation::Landscape;
					break;
				case SDL_ORIENTATION_LANDSCAPE_FLIPPED:
					LOGF("LANDSCAPE_FLIPPED\n");
					d.orientation = DisplayOrientation::LandscapeFlipped;
					break;
				case SDL_ORIENTATION_PORTRAIT:
					LOGF("PORTRAIT\n");
					d.orientation = DisplayOrientation::Portrait;
					break;
				case SDL_ORIENTATION_PORTRAIT_FLIPPED:
					d.orientation = DisplayOrientation::PortraitFlipped;
					LOGF("PORTRAIT_FLIPPED\n");
					break;
				default: LOGF("???\n");
			}

			if (!sdl) {
				GameEventDisplay(game, &d);
			}

			break;
		}
		default:
			LOGF("Display %" PRIu32 " got unknown event 0x%" PRIx8 "\n", display.display,
			     display.event);
			break;
	}
}

int GamePollEvent(WindowGame* game) {
	EXIT_IF(!game);

	auto* event = static_cast<SDL_Event*>(game->event);

	EXIT_IF(!event);

	return SDL_PollEvent(event);
}

int GameWaitEvent(WindowGame* game) {
	EXIT_IF(!game);

	auto* event = static_cast<SDL_Event*>(game->event);

	EXIT_IF(!event);

	return SDL_WaitEvent(event);
}

void GameProcessEvent(WindowGame* game, double time_s) {
	EXIT_IF(!game);

	auto* event = static_cast<SDL_Event*>(game->event);

	EXIT_IF(!event);

	EXIT_IF(SDL_GetEventState(SDL_DISPLAYEVENT) != SDL_ENABLE);

	switch (event->type) {
		case SDL_QUIT: GameEventQuit(game); break;

		case SDL_APP_TERMINATING: GameEventTerminate(game); break;

		case SDL_APP_LOWMEMORY: GameEventLowMemory(game); break;

		case SDL_APP_WILLENTERBACKGROUND: GameEventWillEnterBackground(game); break;

		case SDL_APP_DIDENTERBACKGROUND: GameEventDidEnterBackground(game); break;

		case SDL_APP_WILLENTERFOREGROUND: GameEventWillEnterForeground(game); break;

		case SDL_APP_DIDENTERFOREGROUND: GameEventDidEnterForeground(game); break;

		case SDL_KEYDOWN:
		case SDL_KEYUP: {
			EventKeyboard key {};

			key.down              = (event->type == SDL_KEYDOWN);
			key.up                = (event->type == SDL_KEYUP);
			key.pressed           = (event->key.state == SDL_PRESSED);
			key.released          = (event->key.state == SDL_RELEASED);
			key.repeat            = (event->key.repeat != 0u);
			key.scan_code         = event->key.keysym.scancode;
			key.key_code          = event->key.keysym.sym;
			key.mod               = event->key.keysym.mod;
			key.timestamp_seconds = time_s;

			GameEventKeyboard(game, &key);

			break;
		}

		case SDL_WINDOWEVENT: ProcessWindowEvent(game, event->window); break;

		case SDL_DISPLAYEVENT: ProcessDisplayEvent(game, event->display); break;

		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP: {
			EventMouse mb {};

			mb.down              = (event->button.type == SDL_MOUSEBUTTONDOWN);
			mb.up                = (event->button.type == SDL_MOUSEBUTTONUP);
			mb.left              = (event->button.button == SDL_BUTTON_LEFT);
			mb.middle            = (event->button.button == SDL_BUTTON_MIDDLE);
			mb.right             = (event->button.button == SDL_BUTTON_RIGHT);
			mb.x1                = (event->button.button == SDL_BUTTON_X1);
			mb.x2                = (event->button.button == SDL_BUTTON_X2);
			mb.touch             = (event->button.which == SDL_TOUCH_MOUSEID);
			mb.pressed           = (event->button.state == SDL_PRESSED);
			mb.released          = (event->button.state == SDL_RELEASED);
			mb.num_of_clicks     = event->button.clicks;
			mb.wheel             = false;
			mb.x                 = event->button.x;
			mb.y                 = event->button.y;
			mb.motion            = false;
			mb.motion_x          = 0;
			mb.motion_y          = 0;
			mb.timestamp_seconds = time_s;

			GameEventMouse(game, &mb);

			break;
		}

		case SDL_MOUSEWHEEL: {
			EventMouse mb {};

			mb.down              = false;
			mb.up                = false;
			mb.left              = false;
			mb.middle            = false;
			mb.right             = false;
			mb.x1                = false;
			mb.x2                = false;
			mb.touch             = (event->wheel.which == SDL_TOUCH_MOUSEID);
			mb.pressed           = false;
			mb.released          = false;
			mb.num_of_clicks     = 0;
			mb.wheel             = true;
			mb.x                 = event->wheel.x;
			mb.y                 = event->wheel.y;
			mb.motion            = false;
			mb.motion_x          = 0;
			mb.motion_y          = 0;
			mb.timestamp_seconds = time_s;

			GameEventMouse(game, &mb);

			break;
		}

		case SDL_MOUSEMOTION: {
			EventMouse mb {};

			mb.down              = false;
			mb.up                = false;
			mb.left              = ((event->motion.state & KYTY_SDL_BUTTON_LMASK) != 0u);
			mb.middle            = ((event->motion.state & KYTY_SDL_BUTTON_MMASK) != 0u);
			mb.right             = ((event->motion.state & KYTY_SDL_BUTTON_RMASK) != 0u);
			mb.x1                = ((event->motion.state & KYTY_SDL_BUTTON_X1MASK) != 0u);
			mb.x2                = ((event->motion.state & KYTY_SDL_BUTTON_X2MASK) != 0u);
			mb.touch             = (event->motion.which == SDL_TOUCH_MOUSEID);
			mb.pressed           = false;
			mb.released          = false;
			mb.num_of_clicks     = 0;
			mb.wheel             = false;
			mb.x                 = event->motion.x;
			mb.y                 = event->motion.y;
			mb.motion            = true;
			mb.motion_x          = event->motion.xrel;
			mb.motion_y          = event->motion.yrel;
			mb.timestamp_seconds = time_s;

			GameEventMouse(game, &mb);

			break;
		}

		case SDL_FINGERMOTION:
		case SDL_FINGERDOWN:
		case SDL_FINGERUP: {
			EventFinger f {};

			f.down              = (event->tfinger.type == SDL_FINGERDOWN);
			f.up                = (event->tfinger.type == SDL_FINGERUP);
			f.motion            = (event->tfinger.type == SDL_FINGERMOTION);
			f.finger_id         = static_cast<int>(event->tfinger.fingerId);
			f.touch_id          = static_cast<int>(event->tfinger.touchId);
			f.x                 = event->tfinger.x;
			f.y                 = event->tfinger.y;
			f.dx                = event->tfinger.dx;
			f.dy                = event->tfinger.dy;
			f.pressure          = event->tfinger.pressure;
			f.timestamp_seconds = time_s;

			GameEventFinger(game, &f);

			break;
		}

		case SDL_CONTROLLERAXISMOTION: {
			EventController c {};

			c.id                = event->caxis.which;
			c.button            = SDL_CONTROLLER_BUTTON_INVALID;
			c.axis_id           = event->caxis.axis;
			c.axis_value        = event->caxis.value;
			c.down              = false;
			c.up                = false;
			c.added             = false;
			c.removed           = false;
			c.remapped          = false;
			c.axis              = true;
			c.pressed           = false;
			c.released          = false;
			c.timestamp_seconds = time_s;

			GameEventController(game, &c);

			break;
		}

		case SDL_CONTROLLERBUTTONDOWN:
		case SDL_CONTROLLERBUTTONUP: {
			EventController c {};

			c.id                = event->cbutton.which;
			c.button            = event->cbutton.button;
			c.axis_id           = SDL_CONTROLLER_AXIS_INVALID;
			c.axis_value        = 0;
			c.down              = (event->cbutton.type == SDL_CONTROLLERBUTTONDOWN);
			c.up                = (event->cbutton.type == SDL_CONTROLLERBUTTONUP);
			c.added             = false;
			c.removed           = false;
			c.remapped          = false;
			c.axis              = false;
			c.pressed           = (event->cbutton.state == SDL_PRESSED);
			c.released          = (event->cbutton.state == SDL_RELEASED);
			c.timestamp_seconds = time_s;

			GameEventController(game, &c);

			break;
		}

		case SDL_CONTROLLERDEVICEADDED:
		case SDL_CONTROLLERDEVICEREMOVED:
		case SDL_CONTROLLERDEVICEREMAPPED: {
			EventController c {};

			c.id                = event->cdevice.which;
			c.button            = SDL_CONTROLLER_BUTTON_INVALID;
			c.axis_id           = SDL_CONTROLLER_AXIS_INVALID;
			c.axis_value        = 0;
			c.down              = false;
			c.up                = false;
			c.added             = (event->cdevice.type == SDL_CONTROLLERDEVICEADDED);
			c.removed           = (event->cdevice.type == SDL_CONTROLLERDEVICEREMOVED);
			c.remapped          = (event->cdevice.type == SDL_CONTROLLERDEVICEREMAPPED);
			c.axis              = false;
			c.pressed           = false;
			c.released          = false;
			c.timestamp_seconds = time_s;

			GameEventController(game, &c);

			break;
		}
	}
}

void GameMainLoop(WindowGame* game, void* data) {
	bool need_exit = false;

	Common::Timer timer;
	timer.Start();

	if (!GameInit(game, timer, data)) {
		need_exit = true;
	}

	for (;;) {
		if (need_exit) {
			break;
		}

		if (GamePollEvent(game) != 0) {
			GameProcessEvent(game, timer.GetTimeS());
			continue;
		}

		if (game->m_game_is_paused) {
			if (!timer.IsPaused()) {
				timer.Pause();
			}

			GameWaitEvent(game);

			GameProcessEvent(game, timer.GetTimeS());
			need_exit = game->m_game_need_exit;
			continue;
		}

		need_exit = game->m_game_need_exit;

		if (game->m_game_is_paused) {
			if (!timer.IsPaused()) {
				timer.Pause();
			}
		} else {
			if (timer.IsPaused()) {
				timer.Resume();
			}

			if (!need_exit) {
				need_exit = !RenderAndUpdate(game);
			}

			if (!need_exit) {
				GameShowWindow(game, timer);
			}
		}
	}

	GameClose(game);
}

static void WindowCreate(WindowContext* ctx) {
	EXIT_IF(ctx == nullptr);
	EXIT_IF(ctx->window != nullptr);
	EXIT_IF(ctx->graphic_ctx.screen_width == 0);
	EXIT_IF(ctx->graphic_ctx.screen_height == 0);

	int width  = static_cast<int>(ctx->graphic_ctx.screen_width);
	int height = static_cast<int>(ctx->graphic_ctx.screen_height);

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	SDL_SetHint(SDL_HINT_WINDOWS_DPI_SCALING, "0");
#endif

	if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0) {
		EXIT("%s\n", SDL_GetError());
	}

	LOGF("WindowCreate(): width = %d, height = %d\n", width, height);

	ctx->window =
	    SDL_CreateWindow(KYTY_SDL_WINDOW_CAPTION, KYTY_SDL_WINDOWPOS_CENTERED,
	                     KYTY_SDL_WINDOWPOS_CENTERED, width, height, KYTY_SDL_WINDOW_FLAGS);

	ctx->window_hidden = true;

	if (ctx->window == nullptr) {
		EXIT("%s\n", SDL_GetError());
	}

	SDL_SetWindowResizable(ctx->window, SDL_FALSE);
}

void WindowInit(uint32_t width, uint32_t height) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	EXIT_IF(g_window_ctx != nullptr);

	g_window_ctx = new WindowContext;

	g_window_ctx->graphic_ctx.screen_width  = width;
	g_window_ctx->graphic_ctx.screen_height = height;
}

void WindowWaitForGraphicInitialized() {
	EXIT_IF(g_window_ctx == nullptr);

	Common::LockGuard lock(g_window_ctx->mutex);

	while (!g_window_ctx->graphic_initialized) {
		g_window_ctx->graphic_initialized_condvar.Wait(&g_window_ctx->mutex);
	}
}

void WindowRun() {
	EXIT_IF(g_window_ctx == nullptr);

	KYTY_PROFILER_THREAD("Thread_Window");

	WindowGame game;

	g_window_ctx->mutex.Lock();
	{
		EXIT_IF(g_window_ctx->graphic_initialized);

		WindowCreate(g_window_ctx);
		VulkanCreate(g_window_ctx);

		// M1W5: auto-connect the keyboard as controller 0 so M&K users
		// don't have to press a key first. Games like Worms read
		// PadGetHandle(1000, 0, 0) at startup and reject the run if no
		// controller is connected — this gives them something to read.
		// The real user_id=1000 is the standard "user 1" the game expects.
		static bool kb_connected = false;
		if (!kb_connected) {
			Controller::ControllerConnect(KEYBOARD_CONTROLLER_ID);
			kb_connected = true;
		}

		g_window_ctx->game = &game;

		g_window_ctx->graphic_initialized = true;
		g_window_ctx->graphic_initialized_condvar.Signal();
	}
	g_window_ctx->mutex.Unlock();

	GraphicsRenderCreateContext();

	GameMainLoop(&game, &g_window_ctx->graphic_ctx);

	// TODO: replace std::_Exit shutdown with full Vulkan teardown, then destroy
	// the VMA allocator immediately before vkDestroyDevice.
	Common::SubsystemsListSingleton::Instance()->ShutdownAll();
	std::_Exit(0);
}

VkSurfaceCapabilitiesKHR* VulkanGetSurfaceCapabilities() {
	EXIT_IF(g_window_ctx == nullptr);

	Common::LockGuard lock(g_window_ctx->mutex);

	return &g_window_ctx->surface_capabilities->capabilities;
}

GraphicContext* WindowGetGraphicContext() {
	EXIT_IF(g_window_ctx == nullptr);

	Common::LockGuard lock(g_window_ctx->mutex);

	return &g_window_ctx->graphic_ctx;
}

static int WindowIconRead(void* user, char* data, int size) {
	auto*    src        = static_cast<Common::File*>(user);
	uint32_t bytes_read = 0;
	src->Read(data, static_cast<uint32_t>(size), &bytes_read);
	return static_cast<int>(bytes_read);
}

static void WindowIconSkip(void* user, int n) {
	auto*          src      = static_cast<Common::File*>(user);
	const uint64_t position = src->Tell();

	if (n >= 0) {
		src->Seek(position + static_cast<uint64_t>(n));
	} else {
		const uint64_t distance = static_cast<uint64_t>(-static_cast<int64_t>(n));
		EXIT_IF(distance > position);
		src->Seek(position - distance);
	}
}

static int WindowIconEof(void* user) {
	auto* src = static_cast<Common::File*>(user);
	return src->IsEOF() ? 1 : 0;
}

struct WindowIcon {
	SDL_Surface* surface = nullptr;
	void*        pixels  = nullptr;

	~WindowIcon() {
		SDL_FreeSurface(surface);
		stbi_image_free(pixels);
	}
};

static void WindowLoadPngIcon(const std::string& path, WindowIcon* icon) {
	Common::File f;
	if (!f.Open(path, Common::File::Mode::Read)) {
		EXIT("Can't open icon file %s\n", path.c_str());
	}

	int width  = 0;
	int height = 0;

	stbi_io_callbacks cb {};
	cb.read = WindowIconRead;
	cb.skip = WindowIconSkip;
	cb.eof  = WindowIconEof;

	icon->pixels = stbi_load_from_callbacks(&cb, &f, &width, &height, nullptr, 4);
	f.Close();

	EXIT_IF(icon->pixels == nullptr);

	icon->surface = SDL_CreateRGBSurfaceWithFormatFrom(icon->pixels, width, height, 32, width * 4,
	                                                   SDL_PIXELFORMAT_RGBA32);
	EXIT_NOT_IMPLEMENTED(icon->surface == nullptr);
}

void WindowUpdateIcon() {
	EXIT_IF(g_window_ctx == nullptr);

	static WindowIcon icon;
	static bool       icon_loaded = false;

	if (!icon_loaded) {
		std::string icon_path;
		if (Loader::SystemContentGetIconPath(&icon_path)) {
			WindowLoadPngIcon(icon_path, &icon);
		}
		icon_loaded = true;
	}

	if (icon.surface != nullptr) {
		SDL_SetWindowIcon(g_window_ctx->window, icon.surface);
	}
}

void WindowUpdateTitle() {
	EXIT_IF(g_window_ctx == nullptr);
	EXIT_IF(g_window_ctx->game == nullptr);

	static char title[128];
	static char title_id[12];
	static char app_ver[12];
	static bool has_title = Loader::SystemContentParamSfoGetString("TITLE", title, sizeof(title));
	static bool has_title_id =
	    Loader::SystemContentParamSfoGetString("TITLE_ID", title_id, sizeof(title_id));
	static bool has_app_ver =
	    Loader::SystemContentParamSfoGetString("APP_VER", app_ver, sizeof(app_ver));

	auto fps = fmt::format("{}{}{}{}{}{}[{}] [{}], frame: {}, fps: {:f}", (has_title ? title : ""),
	                       (has_title ? ", " : ""), (has_title_id ? title_id : ""),
	                       (has_title_id ? ", " : ""), (has_app_ver ? app_ver : ""),
	                       (has_app_ver ? " " : ""), g_window_ctx->device_name,
	                       g_window_ctx->processor_name, g_window_ctx->game->m_frame_num,
	                       g_window_ctx->game->m_current_fps);

	SDL_SetWindowTitle(g_window_ctx->window, fps.c_str());

	// M1W6: per-second health log. Gated by Log::IsAtLeast(Health) —
	// defaults to true (level 1) so it's always on in release. Set
	// --debug-level 0 to silence it; --debug-level 2/3 enables the
	// verbose per-call LOGFs in controller.cpp as well.
	{
		static double last_health_t = 0.0;
		const double  t_now_health  = g_window_ctx->game->m_current_time_seconds;
		if (t_now_health - last_health_t >= FPS_UPDATE_TIME &&
		    Log::IsAtLeast(Log::DebugLevel::Health)) {
			Libs::Controller::ControllerHealth h {};
			Libs::Controller::ControllerGetHealth(&h);
			const char* conn = h.connected ? "yes" : "no";
			LOGF("[HEALTH] t=%.1fs frame=%u fps=%.1f ctrl=%s(active_id=%d n=%d) "
			     "pad_reads=%" PRIu64 "(rej=%" PRIu64
			     ") btn=0x%08x sticks=L(%d,%d) R(%d,%d)\n",
			     t_now_health, g_window_ctx->game->m_frame_num,
			     g_window_ctx->game->m_current_fps, conn, h.active_id, h.connected_count,
			     h.pad_read_count, h.pad_read_reject, h.last_buttons,
			     h.last_left_x, h.last_left_y, h.last_right_x, h.last_right_y);
			last_health_t = t_now_health;
		}
	}

	// M1W3: optional stdout FPS summary for headless measurement runs.
	// Prints once per FPS_UPDATE_TIME window to avoid log spam.
	if (g_print_fps_to_stdout.load()) {
		static double last_print = 0.0;
		static double t_start   = 0.0;
		const double t_now = g_window_ctx->game->m_current_time_seconds;
		if (t_start == 0.0) {
			t_start = t_now;
		}
		if (t_now - last_print >= FPS_UPDATE_TIME) {
			::printf("[FPS] %.2f frames/sec (frame=%u, t=%.2fs)\n",
			         g_window_ctx->game->m_current_fps,
			         g_window_ctx->game->m_frame_num,
			         t_now - t_start);
			last_print = t_now;
		}
	}
}

} // namespace Libs::Graphics
