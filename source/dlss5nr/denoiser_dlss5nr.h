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
  /* Same as ProcessFn with depth and motion guides inserted after the colour
   * buffers. Resolved separately so an older bridge without it still works. */
  using ProcessGuidedFn = int(__cdecl *)(const float *,
                                         float *,
                                         const float *,
                                         const float *,
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
  ProcessGuidedFn process_guided_ = nullptr;
  ShutdownFn shutdown_ = nullptr;
#endif
  bool initialized_ = false;
  bool failed_ = false;
  /* Log the guide state once, and again only when it changes. Tri-state, so
   * the first call reports even when guides are absent: that is precisely
   * the case where the user needs to be told why nothing is accumulating. */
  int guides_logged_ = -1;
  /* Report the actual guide ranges once, so a plausible looking but useless
   * depth or an all zero motion field cannot hide. */
  bool guide_stats_logged_ = false;
  int width_ = 0;
  int height_ = 0;
};

CCL_NAMESPACE_END
