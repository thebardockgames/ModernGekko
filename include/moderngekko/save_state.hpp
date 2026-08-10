#pragma once

#include <thread>

namespace moderngekko
{
// Phase 10: opt-in via MODERNGEKKO_LOAD_STATE=<path>. Loads a real Dolphin
// save state (Core::State::LoadAs) right after Run() successfully boots the
// module, before the platform's MainLoop starts -- drops straight into
// whatever live gameplay a human previously captured with the F10 hotkey
// below, skipping the entire boot/menu/navigate-to-content sequence on every
// future run. Must be called after BootManager::BootCore() succeeds (HW::Init
// has run, so Core::State::Init has already registered the savestate
// subsystem) and before MainLoop. Strictly opt-in: does nothing if the env
// var is unset, so existing cold-boot behavior is unregressed.
//
// This is intentionally simpler than TAS movie record/playback (Core/Movie.h):
// we don't need bit-identical deterministic input replay, just "resume real
// gameplay from a known-good point" -- the game's own AI/animation/combat
// logic keeps running forward from the loaded state with no input needed at
// all, so there's no RTC/hardware-seed/frame-pacing determinism to worry
// about.
void MaybeLoadStateOnBoot();

// Phase 10: opt-in via MODERNGEKKO_SAVE_STATE_HOTKEY=<path>. Starts a
// background poller (mirroring gx_vertex_dump.cpp's F9 capture-arm hotkey)
// that watches for F10 (edge-triggered) and calls Core::State::SaveAs(path)
// each time it's pressed, so a human playing interactively once can capture
// "I'm in real combat right now" and reuse that exact moment forever via
// MaybeLoadStateOnBoot() above. Returns a default-constructed (non-joinable)
// jthread if the env var is unset; otherwise returns a running jthread that
// stops and joins automatically when destroyed (RAII, same lifetime as the
// title-bar-FPS thread in Runtime::Run()). Windows-only (GetAsyncKeyState),
// like the existing F9 hotkey.
std::jthread MaybeStartSaveStateHotkeyThread();
}
