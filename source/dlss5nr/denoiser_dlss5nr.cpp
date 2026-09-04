/* SPDX-FileCopyrightText: 2026 Blender DLSS5 NR contributors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "integrator/denoiser_dlss5nr.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "device/device.h"
#include "session/buffers.h"
#include "util/log.h"
#include "util/math.h"

CCL_NAMESPACE_BEGIN

DLSS5NRDenoiser::DLSS5NRDenoiser(Device *denoiser_device, const DenoiseParams &params)
    : Denoiser(denoiser_device, params)
{
  DCHECK_EQ(params.type, DENOISER_DLSS5NR);
}

DLSS5NRDenoiser::~DLSS5NRDenoiser()
{
  unload_runtime();
}

uint DLSS5NRDenoiser::get_device_type_mask() const
{
  return DEVICE_MASK_OPTIX | DEVICE_MASK_CUDA;
}

bool DLSS5NRDenoiser::is_device_supported(const DeviceInfo &device)
{
#ifdef _WIN32
  return device.type == DEVICE_OPTIX || device.type == DEVICE_CUDA;
#else
  (void)device;
  return false;
#endif
}

bool DLSS5NRDenoiser::ensure_runtime()
{
#ifdef _WIN32
  if (initialized_) {
    return true;
  }
  if (failed_) {
    return false;
  }

  /* An explicit path is loaded as given. Without one, fall back to the bridge
   * sitting next to blender.exe, which is where package_windows.ps1 puts it.
   * That fallback has to be resolved to an absolute path rather than passed as
   * the bare name "dlss5nr_bridge.dll": a bare name goes through the default
   * search order, which includes the process working directory, so anyone able
   * to write a file there could have Blender load their DLL into its own
   * process instead of ours. */
  const char *bridge_path = std::getenv("CYCLES_DLSS5NR_BRIDGE");
  if (bridge_path && bridge_path[0]) {
    bridge_module_ = LoadLibraryA(bridge_path);
  }
  else {
    wchar_t executable[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    /* A result equal to the buffer size means the path was truncated. */
    if (length > 0 && length < MAX_PATH) {
      wchar_t *separator = wcsrchr(executable, L'\\');
      if (separator) {
        separator[1] = L'\0';
        const std::wstring beside_blender = std::wstring(executable) + L"dlss5nr_bridge.dll";
        bridge_module_ = LoadLibraryW(beside_blender.c_str());
      }
    }
  }
  if (!bridge_module_) {
    set_error(
        "DLSS 5 NR: dlss5nr_bridge.dll was not found next to blender.exe. "
        "Set CYCLES_DLSS5NR_BRIDGE to its full path.");
    failed_ = true;
    return false;
  }

  init_ = reinterpret_cast<InitFn>(GetProcAddress(bridge_module_, "dlss5nr_init"));
  process_ = reinterpret_cast<ProcessFn>(GetProcAddress(bridge_module_, "dlss5nr_process"));
  shutdown_ = reinterpret_cast<ShutdownFn>(GetProcAddress(bridge_module_, "dlss5nr_shutdown"));
  /* Optional: a bridge built before the guided path simply will not have it,
   * and the colour only call still works. */
  process_guided_ = reinterpret_cast<ProcessGuidedFn>(
      GetProcAddress(bridge_module_, "dlss5nr_process_guided"));
  if (!init_ || !process_ || !shutdown_) {
    set_error("DLSS 5 NR: bridge has missing or incompatible exports.");
    unload_runtime();
    failed_ = true;
    return false;
  }

  const char *runtime_path = std::getenv("CYCLES_DLSS5NR_RUNTIME");
  if (!runtime_path || !runtime_path[0]) {
    set_error("DLSS 5 NR: set CYCLES_DLSS5NR_RUNTIME to directory containing nvngx_dlssnr.dll.");
    failed_ = true;
    return false;
  }

  const int required = MultiByteToWideChar(CP_UTF8, 0, runtime_path, -1, nullptr, 0);
  std::vector<wchar_t> runtime_wide(std::max(required, 1));
  if (!required || !MultiByteToWideChar(
                       CP_UTF8, 0, runtime_path, -1, runtime_wide.data(), runtime_wide.size()))
  {
    set_error("DLSS 5 NR: CYCLES_DLSS5NR_RUNTIME is not valid UTF-8.");
    failed_ = true;
    return false;
  }

  char error[1024] = {};
  if (!init_(0, runtime_wide.data(), error, sizeof(error))) {
    set_error(string("DLSS 5 NR initialization failed: ") + error);
    failed_ = true;
    return false;
  }
  initialized_ = true;
  LOG_INFO << "DLSS 5 NR experimental bridge initialized";
  return true;
#else
  set_error("DLSS 5 NR is only supported on Windows x64.");
  return false;
#endif
}

void DLSS5NRDenoiser::unload_runtime()
{
#ifdef _WIN32
  if (initialized_ && shutdown_) {
    shutdown_();
  }
  initialized_ = false;
  init_ = nullptr;
  process_ = nullptr;
  shutdown_ = nullptr;

  /* The bridge is deliberately left mapped for the process lifetime, exactly
   * like the NGX modules it loads. Calling FreeLibrary here unmapped it while
   * NGX and the caller shim still held pointers into it, and Blender then died
   * with EXCEPTION_ACCESS_VIOLATION during shutdown, after the render had
   * already been written. The crash handler could not even write its own log,
   * which is what unmapping code out from under a live callback looks like.
   *
   * This runs whenever the denoiser is destroyed, which includes every change
   * of render settings, so leaking one module handle is much the better trade.
   * dlss5nr_shutdown above already releases the D3D12 and NGX resources, and
   * a later ensure_runtime simply loads the module again and takes a fresh
   * reference. */
#endif
}

bool DLSS5NRDenoiser::denoise_buffer(const BufferParams &buffer_params,
                                     RenderBuffers *render_buffers,
                                     const int num_samples,
                                     bool /*allow_inplace_modification*/)
{
  if (!ensure_runtime()) {
    return false;
  }
  if (buffer_params.width <= 0 || buffer_params.height <= 0 || num_samples <= 0) {
    return true;
  }

  const int noisy_offset = buffer_params.get_pass_offset(PASS_COMBINED, PassMode::NOISY);
  const int output_offset = buffer_params.get_pass_offset(PASS_COMBINED, PassMode::DENOISED);
  if (noisy_offset == PASS_UNUSED || output_offset == PASS_UNUSED) {
    set_error("DLSS 5 NR: Combined noisy or denoised pass is missing.");
    return false;
  }

  /* Render buffers hold accumulated sums, so passes are divided by the sample
   * count going in and multiplied back going out, exactly as the built-in
   * filter_color_preprocess / filter_color_postprocess kernels do. With
   * adaptive sampling the count is per pixel, not the scene-wide num_samples. */
  const int sample_count_offset = buffer_params.get_pass_offset(PASS_SAMPLE_COUNT);

  /* Guides are opportunistic. Cycles only carries these passes when the view
   * layer asks for them, so a render without the Z and Vector passes gets the
   * colour only path and an evaluate that resets every frame. With both present
   * the model has frame to frame correspondence and can accumulate, which is
   * what Ray Reconstruction is built around. */
  const int depth_offset = buffer_params.get_pass_offset(PASS_DEPTH);
  const int motion_offset = buffer_params.get_pass_offset(PASS_MOTION);
  const int motion_weight_offset = buffer_params.get_pass_offset(PASS_MOTION_WEIGHT);
  const bool use_guides = process_guided_ != nullptr && depth_offset != PASS_UNUSED &&
                          motion_offset != PASS_UNUSED;
  if (guides_logged_ != int(use_guides)) {
    guides_logged_ = int(use_guides);
    LOG_INFO << "DLSS 5 NR guides "
             << (use_guides ? "active: depth and motion, temporal accumulation on" :
                              "absent: colour only, resetting every frame. Enable the "
                              "Z and Vector passes for temporal accumulation");
  }

  render_buffers->copy_from_device();
  float *pixels = render_buffers->buffer.data();
  const size_t pixel_count = size_t(buffer_params.width) * size_t(buffer_params.height);
  std::vector<float> input(pixel_count * 3);
  std::vector<float> output(pixel_count * 3);
  std::vector<float> pixel_scale(pixel_count, float(num_samples));
  std::vector<float> depth(use_guides ? pixel_count : 0);
  std::vector<float> motion(use_guides ? pixel_count * 2 : 0);

  for (int y = 0; y < buffer_params.height; ++y) {
    for (int x = 0; x < buffer_params.width; ++x) {
      const size_t image_index = size_t(y) * buffer_params.width + x;
      const size_t buffer_index = size_t(buffer_params.offset + y * buffer_params.stride + x) *
                                  buffer_params.pass_stride;
      if (sample_count_offset != PASS_UNUSED) {
        pixel_scale[image_index] = float(
            __float_as_uint(pixels[buffer_index + sample_count_offset]));
      }
      const float inv_scale = pixel_scale[image_index] > 0.0f ?
                                  1.0f / pixel_scale[image_index] :
                                  0.0f;
      for (int channel = 0; channel < 3; ++channel) {
        input[image_index * 3 + channel] =
            std::max(0.0f, pixels[buffer_index + noisy_offset + channel] * inv_scale);
      }
      if (use_guides) {
        /* Depth is not accumulated. The kernel writes it with
         * film_overwrite_pass_float at sample 0 only, and PASS_DEPTH has
         * use_filter false so Cycles' own accessor does not divide it by the
         * sample count either. Scaling it here shrank it by exactly that
         * count: a scene 21 units deep arrived as 0 to 1.35 at 16 samples,
         * and at 1024 samples it would have been indistinguishable from an
         * empty buffer. */
        depth[image_index] = pixels[buffer_index + depth_offset];
        /* The motion pass is accumulated against its own weight rather than the
         * sample count, which is what divide_type on PASS_MOTION means. Only the
         * first two components are used: they are the vector back to where this
         * pixel was in the previous frame, which is the correspondence the model
         * needs. The last two point at the next frame and are for vector blur. */
        float motion_scale = inv_scale;
        if (motion_weight_offset != PASS_UNUSED) {
          const float weight = pixels[buffer_index + motion_weight_offset];
          motion_scale = weight > 0.0f ? 1.0f / weight : 0.0f;
        }
        motion[image_index * 2 + 0] = pixels[buffer_index + motion_offset + 0] * motion_scale;
        motion[image_index * 2 + 1] = pixels[buffer_index + motion_offset + 1] * motion_scale;
      }
    }
  }

    if (use_guides && !guide_stats_logged_) {
    guide_stats_logged_ = true;
    float dmin = depth.empty() ? 0.0f : depth[0];
    float dmax = dmin;
    for (const float d : depth) {
      dmin = std::min(dmin, d);
      dmax = std::max(dmax, d);
    }
    float mmax = 0.0f;
    double msum = 0.0;
    for (const float m : motion) {
      mmax = std::max(mmax, std::fabs(m));
      msum += std::fabs(m);
    }
    LOG_INFO << "DLSS 5 NR guide stats: depth " << dmin << " to " << dmax
             << ", motion mean |v| " << (motion.empty() ? 0.0 : msum / motion.size())
             << " max " << mmax;
  }

char error[1024] = {};
  const bool reset = width_ != buffer_params.width || height_ != buffer_params.height;
  const int ok = use_guides ? process_guided_(input.data(),
                                             output.data(),
                                             depth.data(),
                                             motion.data(),
                                             buffer_params.width,
                                             buffer_params.height,
                                             params_.dlss5nr_style,
                                             0,
                                             params_.dlss5nr_intensity,
                                             params_.dlss5nr_tone,
                                             params_.dlss5nr_structure,
                                             params_.dlss5nr_skin,
                                             params_.dlss5nr_auto_mask ? 1 : 0,
                                             reset ? 1 : 0,
                                             error,
                                             sizeof(error)) :
                              process_(input.data(),
                output.data(),
                buffer_params.width,
                buffer_params.height,
                params_.dlss5nr_style,
                /* Preset is passed at its default because every value across
                 * its whole 0..3 range produced bit identical output, so either
                 * the parameter name is wrong or the runtime ignores it. */
                0, /* preset */
                params_.dlss5nr_intensity,
                params_.dlss5nr_tone,
                params_.dlss5nr_structure,
                /* Negative leaves the model default alone, and skin only does
                 * anything when the auto mask is on. */
                params_.dlss5nr_skin,
                params_.dlss5nr_auto_mask ? 1 : 0,
                reset ? 1 : 0,
                error,
                sizeof(error));
  if (!ok) {
    set_error(string("DLSS 5 NR evaluation failed: ") + error);
    failed_ = true;
    unload_runtime();
    return false;
  }
  width_ = buffer_params.width;
  height_ = buffer_params.height;

  for (int y = 0; y < buffer_params.height; ++y) {
    for (int x = 0; x < buffer_params.width; ++x) {
      const size_t image_index = size_t(y) * buffer_params.width + x;
      const size_t buffer_index = size_t(buffer_params.offset + y * buffer_params.stride + x) *
                                  buffer_params.pass_stride;
      for (int channel = 0; channel < 3; ++channel) {
        pixels[buffer_index + output_offset + channel] = output[image_index * 3 + channel] *
                                                        pixel_scale[image_index];
      }
      pixels[buffer_index + output_offset + 3] = pixels[buffer_index + noisy_offset + 3];
    }
  }
  render_buffers->copy_to_device();
  return true;
}

CCL_NAMESPACE_END
