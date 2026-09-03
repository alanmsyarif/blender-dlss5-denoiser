/* SPDX-FileCopyrightText: 2026 Blender DLSS5 NR contributors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "integrator/denoiser_dlss5nr.h"

#include <algorithm>
#include <cstdlib>
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

  const char *bridge_path = std::getenv("CYCLES_DLSS5NR_BRIDGE");
  bridge_module_ = LoadLibraryA(bridge_path && bridge_path[0] ? bridge_path :
                                                              "dlss5nr_bridge.dll");
  if (!bridge_module_) {
    set_error("DLSS 5 NR: dlss5nr_bridge.dll was not found. Set CYCLES_DLSS5NR_BRIDGE.");
    failed_ = true;
    return false;
  }

  init_ = reinterpret_cast<InitFn>(GetProcAddress(bridge_module_, "dlss5nr_init"));
  process_ = reinterpret_cast<ProcessFn>(GetProcAddress(bridge_module_, "dlss5nr_process"));
  shutdown_ = reinterpret_cast<ShutdownFn>(GetProcAddress(bridge_module_, "dlss5nr_shutdown"));
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
  if (bridge_module_) {
    FreeLibrary(bridge_module_);
    bridge_module_ = nullptr;
  }
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

  render_buffers->copy_from_device();
  float *pixels = render_buffers->buffer.data();
  const size_t pixel_count = size_t(buffer_params.width) * size_t(buffer_params.height);
  std::vector<float> input(pixel_count * 3);
  std::vector<float> output(pixel_count * 3);
  std::vector<float> pixel_scale(pixel_count, float(num_samples));

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
    }
  }

  char error[1024] = {};
  const bool reset = width_ != buffer_params.width || height_ != buffer_params.height;
  if (!process_(input.data(),
                output.data(),
                buffer_params.width,
                buffer_params.height,
                0,    /* style: Default */
                0,    /* preset: Default */
                1.0f, /* intensity */
                1.0f, /* local tone strength */
                1.0f, /* local structure strength */
                -1.0f, /* skin structure: negative leaves the model default */
                0,    /* auto mask */
                reset ? 1 : 0,
                error,
                sizeof(error)))
  {
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
