// SPDX-FileCopyrightText: Copyright 2026 IFreemz (shadps4_dlss bridge), gow3 contributors
// SPDX-License-Identifier: MIT
//
// C interface between p4d and p4d_dlss.dll. The port contains no NVIDIA code: it loads the
// bridge at run time when it is present (next to p4d-probe.exe, with nvngx_dlss.dll) and keeps
// its other upscalers when it is absent. Adapted from IFreemz/shadPS4-Bloodborne-DLSS-FSR.

#pragma once

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

#define P4D_DLSS_BRIDGE_ABI 1

typedef void (*P4dDlssLogFn)(int warning, const char* message);

typedef struct P4dDlssImage {
    VkImage image;
    VkImageView view;
    VkImageSubresourceRange range;
    VkFormat format;
    uint32_t width;
    uint32_t height;
} P4dDlssImage;

typedef struct P4dDlssFeature {
    uint32_t input_width;
    uint32_t input_height;
    uint32_t output_width;
    uint32_t output_height;
    int32_t quality; // 0 DLAA, 1 Quality, 2 Balanced, 3 Performance, 4 Ultra Performance
    int32_t depth_inverted;
    uint32_t preset; // NVSDK_NGX_DLSS_Hint_Render_Preset value, 0 = driver default
    int32_t hdr;     // 1: linear HDR colour with automatic exposure, 0: tonemapped colour
} P4dDlssFeature;

typedef struct P4dDlssEvaluate {
    P4dDlssImage color;  // sampled
    P4dDlssImage depth;  // sampled, hardware depth
    P4dDlssImage motion; // sampled, render-resolution pixels, current to previous
    P4dDlssImage output; // storage, general layout
    float jitter_x;
    float jitter_y;
    int32_t reset;
    float frame_ms;
    float sharpness; // 0..1; 0 leaves the output unsharpened
} P4dDlssEvaluate;

typedef struct P4dDlssApi {
    uint32_t abi;
    // UTF-16 paths: the directory with nvngx_dlss.dll, and a writable data directory.
    int32_t (*Configure)(const wchar_t* dll_directory, const wchar_t* data_directory,
                         P4dDlssLogFn log);
    // Vulkan extensions NGX needs. The arrays stay valid until Shutdown.
    int32_t (*InstanceExtensions)(uint32_t* count, const VkExtensionProperties** extensions);
    int32_t (*DeviceExtensions)(VkInstance instance, VkPhysicalDevice physical, uint32_t* count,
                                const VkExtensionProperties** extensions);
    int32_t (*Initialize)(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                          PFN_vkGetInstanceProcAddr get_instance_proc,
                          PFN_vkGetDeviceProcAddr get_device_proc);
    int32_t (*CreateFeature)(VkCommandBuffer command, const P4dDlssFeature* feature);
    int32_t (*Evaluate)(VkCommandBuffer command, const P4dDlssEvaluate* evaluate);
    // The caller must have waited for the GPU work that used the feature.
    void (*ReleaseFeature)(void);
    void (*Shutdown)(void);
} P4dDlssApi;

typedef const P4dDlssApi* (*P4dDlssGetApiFn)(void);

#ifdef __cplusplus
}
#endif
