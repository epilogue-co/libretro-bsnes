// libretro-hashtrace: generic, core-agnostic cross-platform determinism tracer.
//
// dlopens ANY libretro core, runs it headless (CPU video — declines SET_HW_RENDER, no GPU), and
// writes one per-frame determinism-hash line ("<frame> <hex>") so the same content traced on
// different host-FPU environments (linux/gcc, windows/clang64, macos arm64, macos Rosetta x86_64)
// can be diffed for byte identity. This is the cross-core successor to ares-libretro-bench's
// --hash-trace: it hashes the SAME state production netplay compares (RetroCoreNetplayAdapter::
// gameStateHash) — the renderer-agnostic logic hash when the core exports one (ares), else an XXH3
// stream over the canonical memory regions SYSTEM_RAM + SAVE_RAM (bsnes, mGBA, any core). Per-core
// determinism option pins come from the single shared catalog (CoreDeterminismOptions.h) so the CI
// gate runs the exact option set the in-process determinism harness pins.
//
// Usage:
//   libretro-hashtrace <rom> --core <path> --out <file> [--frames N] [--set key=value]...
//   --core   path to the libretro core shared library (required)
//   --out    output trace file (required)
//   --frames emulated frames to trace (default 1800)
//   --set    override/add a core option (repeatable); applied ON TOP of the pinned catalog
//            (e.g. ares needs --set ares_n64_homebrew_mode=enabled to boot the n64-systemtest ROM,
//            overriding the catalog default of disabled).

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#if defined(_WIN32)
  #define NOMINMAX
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <dlfcn.h>
#endif

#include "libretro.h"
#include "CoreDeterminismOptions.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

namespace {

#if defined(_WIN32)
using LibHandle = HMODULE;
LibHandle libOpen(const char* p) { return LoadLibraryA(p); }
void* libSym(LibHandle l, const char* n) { return reinterpret_cast<void*>(GetProcAddress(l, n)); }
#else
using LibHandle = void*;
LibHandle libOpen(const char* p) { return dlopen(p, RTLD_NOW | RTLD_LOCAL); }
void* libSym(LibHandle l, const char* n) { return dlsym(l, n); }
#endif

// Core entry points resolved from the shared library. The netplay/memory exports are optional
// (nullable) — the hash path probes for them.
struct CoreApi {
  LibHandle dl = nullptr;
  void (*set_environment)(retro_environment_t) = nullptr;
  void (*set_video_refresh)(retro_video_refresh_t) = nullptr;
  void (*set_audio_sample)(retro_audio_sample_t) = nullptr;
  void (*set_audio_sample_batch)(retro_audio_sample_batch_t) = nullptr;
  void (*set_input_poll)(retro_input_poll_t) = nullptr;
  void (*set_input_state)(retro_input_state_t) = nullptr;
  void (*init)() = nullptr;
  void (*deinit)() = nullptr;
  bool (*load_game)(const retro_game_info*) = nullptr;
  void (*unload_game)() = nullptr;
  void (*run)() = nullptr;
  void* (*get_memory_data)(unsigned) = nullptr;     // optional
  size_t (*get_memory_size)(unsigned) = nullptr;    // optional
  uint64_t (*netplay_logic_hash)() = nullptr;       // optional (ares fork only)
};

struct Host {
  CoreApi core;
  std::map<std::string, std::string> options;  // catalog pins + --set overrides
  std::string saveDir;
};
Host g;

void logPrintf(retro_log_level level, const char* fmt, ...) {
  (void)level;
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
}

bool envCallback(unsigned cmd, void* data) {
  switch (cmd) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
      static_cast<retro_log_callback*>(data)->log = logPrintf;
      return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
      return true;  // accept any; we never read pixels
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
      *static_cast<bool*>(data) = true;
      return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
      *static_cast<unsigned*>(data) = 2;
      return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
      auto* var = static_cast<retro_variable*>(data);
      if (!var || !var->key) return false;
      const auto it = g.options.find(var->key);
      if (it == g.options.end()) return false;  // unpinned → core uses its (deterministic) default
      var->value = it->second.c_str();
      return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
      *static_cast<bool*>(data) = false;
      return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
      *static_cast<const char**>(data) = g.saveDir.c_str();
      return true;
    case RETRO_ENVIRONMENT_SET_HW_RENDER:
      return false;  // decline → core falls back to CPU video; no GPU needed
    // Accept-and-ignore: option/variable declarations (we serve GET_VARIABLE from the catalog,
    // never the core's declared defaults) and the harmless metadata setters.
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
      return true;
    default:
      return false;
  }
}

void videoRefresh(const void*, unsigned, unsigned, size_t) {}
void audioSample(int16_t, int16_t) {}
size_t audioSampleBatch(const int16_t*, size_t frames) { return frames; }
void inputPoll() {}
int16_t inputState(unsigned, unsigned, unsigned, unsigned) { return 0; }  // zero input = deterministic

bool loadCore(const std::string& path) {
  g.core.dl = libOpen(path.c_str());
  if (!g.core.dl) {
    std::fprintf(stderr, "[hashtrace] cannot load core %s\n", path.c_str());
    return false;
  }
  auto sym = [&](const char* n) { return libSym(g.core.dl, n); };
  g.core.set_environment = reinterpret_cast<decltype(g.core.set_environment)>(sym("retro_set_environment"));
  g.core.set_video_refresh = reinterpret_cast<decltype(g.core.set_video_refresh)>(sym("retro_set_video_refresh"));
  g.core.set_audio_sample = reinterpret_cast<decltype(g.core.set_audio_sample)>(sym("retro_set_audio_sample"));
  g.core.set_audio_sample_batch =
      reinterpret_cast<decltype(g.core.set_audio_sample_batch)>(sym("retro_set_audio_sample_batch"));
  g.core.set_input_poll = reinterpret_cast<decltype(g.core.set_input_poll)>(sym("retro_set_input_poll"));
  g.core.set_input_state = reinterpret_cast<decltype(g.core.set_input_state)>(sym("retro_set_input_state"));
  g.core.init = reinterpret_cast<decltype(g.core.init)>(sym("retro_init"));
  g.core.deinit = reinterpret_cast<decltype(g.core.deinit)>(sym("retro_deinit"));
  g.core.load_game = reinterpret_cast<decltype(g.core.load_game)>(sym("retro_load_game"));
  g.core.unload_game = reinterpret_cast<decltype(g.core.unload_game)>(sym("retro_unload_game"));
  g.core.run = reinterpret_cast<decltype(g.core.run)>(sym("retro_run"));
  g.core.get_memory_data = reinterpret_cast<decltype(g.core.get_memory_data)>(sym("retro_get_memory_data"));
  g.core.get_memory_size = reinterpret_cast<decltype(g.core.get_memory_size)>(sym("retro_get_memory_size"));
  g.core.netplay_logic_hash = reinterpret_cast<decltype(g.core.netplay_logic_hash)>(sym("retro_netplay_logic_hash"));
  if (!g.core.set_environment || !g.core.init || !g.core.load_game || !g.core.run) {
    std::fprintf(stderr, "[hashtrace] core missing required retro_* entry points\n");
    return false;
  }
  return true;
}

// Per-frame determinism hash, matching RetroCoreNetplayAdapter::gameStateHash exactly: prefer the
// core's renderer-agnostic logic hash; else a single XXH3 stream over SYSTEM_RAM then SAVE_RAM
// (skip zero-size, 16 MiB per-region cap). Hashes guest-memory bytes → host-endian-independent.
constexpr size_t kMaxRegionBytes = 16u * 1024 * 1024;

uint64_t frameHash(bool& anyRegionOut) {
  anyRegionOut = true;
  if (g.core.netplay_logic_hash) return g.core.netplay_logic_hash();
  anyRegionOut = false;
  if (!g.core.get_memory_data || !g.core.get_memory_size) return 0;
  XXH3_state_t* st = XXH3_createState();
  if (!st) return 0;
  XXH3_64bits_reset(st);
  for (unsigned id : {RETRO_MEMORY_SYSTEM_RAM, RETRO_MEMORY_SAVE_RAM}) {
    const size_t n = g.core.get_memory_size(id);
    if (n == 0 || n > kMaxRegionBytes) continue;
    void* d = g.core.get_memory_data(id);
    if (!d) continue;
    XXH3_64bits_update(st, d, n);
    anyRegionOut = true;
  }
  const uint64_t h = XXH3_64bits_digest(st);
  XXH3_freeState(st);
  return h;
}

}  // namespace

int main(int argc, char** argv) {
  std::string romPath, corePath, outPath;
  long frames = 1800;
  std::vector<std::pair<std::string, std::string>> setOverrides;

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) { std::fprintf(stderr, "[hashtrace] %s needs a value\n", a.c_str()); std::exit(2); }
      return argv[++i];
    };
    if (a == "--core") corePath = next();
    else if (a == "--out") outPath = next();
    else if (a == "--frames") frames = std::atol(next());
    else if (a == "--set") {
      std::string kv = next();
      const auto eq = kv.find('=');
      if (eq == std::string::npos) { std::fprintf(stderr, "[hashtrace] --set expects key=value\n"); return 2; }
      setOverrides.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
    } else if (!a.empty() && a[0] != '-') romPath = a;
    else { std::fprintf(stderr, "[hashtrace] unknown option %s\n", a.c_str()); return 2; }
  }
  if (romPath.empty() || corePath.empty() || outPath.empty()) {
    std::fprintf(stderr, "usage: libretro-hashtrace <rom> --core <path> --out <file> "
                         "[--frames N] [--set key=value]...\n");
    return 2;
  }
  if (frames <= 0) { std::fprintf(stderr, "[hashtrace] --frames must be > 0\n"); return 2; }

  // Per-core determinism pins from the shared catalog, then caller --set overrides on top.
  g.options = netplay::coredet::optionsFor(corePath);
  for (const auto& [k, v] : setOverrides) g.options[k] = v;

  std::error_code ec;
  auto dir = std::filesystem::temp_directory_path(ec) / "libretro-hashtrace-save";
  std::filesystem::create_directories(dir, ec);
  g.saveDir = ec ? std::string(".") : dir.string();

  if (!loadCore(corePath)) return 1;

  g.core.set_environment(envCallback);
  g.core.set_video_refresh(videoRefresh);
  g.core.set_audio_sample(audioSample);
  g.core.set_audio_sample_batch(audioSampleBatch);
  g.core.set_input_poll(inputPoll);
  g.core.set_input_state(inputState);
  g.core.init();

  retro_game_info game = {};
  game.path = romPath.c_str();
  if (!g.core.load_game(&game)) {
    std::fprintf(stderr, "[hashtrace] retro_load_game failed (rom=%s core=%s)\n", romPath.c_str(), corePath.c_str());
    return 1;
  }

  // Refuse to emit a meaningless all-zero trace: the core must expose either the logic-hash export
  // or at least one canonical memory region, or the gate would compare zeros and read as "identical."
  {
    bool any = false;
    (void)frameHash(any);
    if (!any) {
      std::fprintf(stderr, "[hashtrace] core exposes neither retro_netplay_logic_hash nor a canonical "
                           "memory region (SYSTEM_RAM/SAVE_RAM); cannot trace determinism\n");
      g.core.unload_game();
      g.core.deinit();
      return 1;
    }
  }

  // Arm the core's deterministic codegen once before the loop if it has the logic-hash export
  // (mirrors ares-libretro-bench's --arm-netplay-determinism: the first logic_hash call switches the
  // CPU/RSP JITs to cross-machine-deterministic mode). No-op for cores without the export (bsnes).
  if (g.core.netplay_logic_hash) g.core.netplay_logic_hash();

  // Binary mode: text mode on Windows translates '\n' -> "\r\n", which would make the trace differ
  // by one byte per line from the LF traces the other platforms write — a false cross-platform
  // divergence. The trace must be byte-identical across hosts, so write raw LF everywhere.
  FILE* out = std::fopen(outPath.c_str(), "wb");
  if (!out) { std::fprintf(stderr, "[hashtrace] cannot open %s\n", outPath.c_str()); return 1; }

  for (long f = 0; f < frames; f++) {
    g.core.run();
    bool any = false;
    const uint64_t h = frameHash(any);
    std::fprintf(out, "%ld %016llx\n", f, static_cast<unsigned long long>(h));
  }

  std::fclose(out);
  g.core.unload_game();
  g.core.deinit();
  std::fprintf(stderr, "[hashtrace] wrote %ld frames to %s (core=%s rom=%s)\n",
               frames, outPath.c_str(), corePath.c_str(), romPath.c_str());
  return 0;
}
