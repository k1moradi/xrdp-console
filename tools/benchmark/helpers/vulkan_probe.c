/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <vulkan/vulkan.h>

#include <stdio.h>
#include <stdlib.h>

static const char *result_name(VkResult result)
{
    switch (result) {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_ERROR_INCOMPATIBLE_DRIVER:
        return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_INITIALIZATION_FAILED:
        return "VK_ERROR_INITIALIZATION_FAILED";
    default:
        return "VkResult (see numeric value)";
    }
}

int main(void)
{
    uint32_t loader_api = VK_API_VERSION_1_0;
    PFN_vkEnumerateInstanceVersion enumerate_instance_version =
        (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(
            VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    if (enumerate_instance_version != NULL) {
        VkResult result = enumerate_instance_version(&loader_api);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "vkEnumerateInstanceVersion: %s (%d)\n",
                    result_name(result), result);
        }
    }
    printf("Vulkan loader API: %u.%u.%u\n", VK_VERSION_MAJOR(loader_api),
           VK_VERSION_MINOR(loader_api), VK_VERSION_PATCH(loader_api));

    VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "xrdp-x11vnc-vulkan-probe",
        .applicationVersion = 1,
        .pEngineName = "probe",
        .engineVersion = 1,
        .apiVersion = loader_api < VK_API_VERSION_1_1 ? loader_api
                                                       : VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
    };
    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&create_info, NULL, &instance);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateInstance: %s (%d)\n", result_name(result),
                result);
        return 2;
    }

    uint32_t device_count = 0;
    result = vkEnumeratePhysicalDevices(instance, &device_count, NULL);
    if (result != VK_SUCCESS || device_count == 0) {
        printf("Physical devices: 0 (result=%s/%d)\n", result_name(result),
               result);
        vkDestroyInstance(instance, NULL);
        return 0;
    }
    VkPhysicalDevice *devices = calloc(device_count, sizeof(*devices));
    if (devices == NULL) {
        fputs("calloc failed\n", stderr);
        vkDestroyInstance(instance, NULL);
        return 2;
    }
    result = vkEnumeratePhysicalDevices(instance, &device_count, devices);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkEnumeratePhysicalDevices: %s (%d)\n",
                result_name(result), result);
        free(devices);
        vkDestroyInstance(instance, NULL);
        return 2;
    }
    printf("Physical devices: %u\n", device_count);
    for (uint32_t i = 0; i < device_count; ++i) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(devices[i], &properties);
        printf("[%u] %s vendor=0x%04x device=0x%04x type=%u api=%u.%u.%u\n",
               i, properties.deviceName, properties.vendorID,
               properties.deviceID, properties.deviceType,
               VK_VERSION_MAJOR(properties.apiVersion),
               VK_VERSION_MINOR(properties.apiVersion),
               VK_VERSION_PATCH(properties.apiVersion));
    }
    free(devices);
    vkDestroyInstance(instance, NULL);
    return 0;
}
