#pragma once

#include <cctype>
#include <map>
#include <string>

// Per-core determinism option pins for the hash-trace tool. Trimmed to bsnes for this repo (the
// gate only ever traces bsnes); the canonical multi-core catalog lives in Playback. THE critical pin
// is bsnes_entropy=None: by default bsnes randomises power-on memory/registers, producing a different
// state hash every run. The rest lock features that drift between builds (run-ahead, fast/cubic DSP,
// overclock, coprocessor sync). Keep in sync with Playback's CoreDeterminismOptions.h::bsnesOptions().

namespace netplay::coredet {

using CoreOptions = std::map<std::string, std::string>;

[[nodiscard]] inline CoreOptions bsnesOptions() {
  return {
      {"bsnes_entropy", "None"},
      {"bsnes_run_ahead_frames", "OFF"},
      {"bsnes_hotfixes", "disabled"},
      {"bsnes_ppu_fast", "ON"},
      {"bsnes_ppu_deinterlace", "OFF"},
      {"bsnes_ppu_no_sprite_limit", "OFF"},
      {"bsnes_ppu_no_vram_blocking", "OFF"},
      {"bsnes_dsp_fast", "OFF"},
      {"bsnes_dsp_cubic", "OFF"},
      {"bsnes_dsp_echo_shadow", "OFF"},
      {"bsnes_blur_emulation", "OFF"},
      {"bsnes_video_luminance", "100%"},
      {"bsnes_video_saturation", "100%"},
      {"bsnes_video_gamma", "100%"},
      {"bsnes_aspect_ratio", "Auto"},
      {"bsnes_cpu_fastmath", "OFF"},
      {"bsnes_cpu_overclock", "100%"},
      {"bsnes_coprocessor_delayed_sync", "ON"},
      {"bsnes_coprocessor_prefer_hle", "ON"},
  };
}

// The tool calls this with the core path; return the bsnes pins for a bsnes core, else empty.
[[nodiscard]] inline CoreOptions optionsFor(const std::string& corePath) {
  std::string lower;
  lower.reserve(corePath.size());
  for (char c : corePath) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  if (lower.find("bsnes") != std::string::npos) return bsnesOptions();
  return {};
}

} // namespace netplay::coredet
