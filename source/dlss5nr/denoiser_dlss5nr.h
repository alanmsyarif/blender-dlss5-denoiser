/* SPDX-FileCopyrightText: 2026 Blender DLSS5 NR contributors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "integrator/denoiser.h"

#ifdef _WIN32
#  include <windows.h>
#endif

CCL_NAMESPACE_BEGIN

class DLSS5NRDenoiser final : public Denoiser {
 public:
  DLSS5NRDenoiser(Device *denoiser_device, const DenoiseParams &params);
  ~DLSS5NRDenoiser() override;

  bool denoise_buffer(const BufferParams &buffer_params,
                      RenderBuffers *render_buffers,
                      int num_samples,
                      bool allow_inplace_modification) override;

  static bool is_device_supported(const DeviceInfo &device);

 protected:
  uint get_device_type_mask() const override;

 private:
  bool ensure_runtime();
  void unload_runtime();

#ifdef _WIN32
  using InitFn = int(__cdecl *)(int, const wchar_t *, char *, int);
  using ProcessFn = int(__cdecl *)(const float *,
                                   float *,
                                   int,
                                   int,
                                   int,
                                   int,
                                   float,
                                   float,
                                   float,
                                   float,
                                   int,
                                   int,
                                   char *,
                                   int);
  using ShutdownFn = void(__cdecl *)();

  HMODULE bridge_module_ = nullptr;
  InitFn init_ = nullptr;
  ProcessFn process_ = nullptr;
  ShutdownFn shutdown_ = nullptr;
#endif
  bool initialized_ = false;
  bool failed_ = false;
  int width_ = 0;
  int height_ = 0;
};

CCL_NAMESPACE_END
