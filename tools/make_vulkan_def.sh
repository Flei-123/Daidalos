#!/bin/bash
# Rebuilds thirdparty/win/vulkan-1.def from the Vulkan headers and the calls the
# engine actually makes.
#
# Windows has no libvulkan.so to link against; it has vulkan-1.dll, which the
# graphics driver installs into System32. mingw can link against a DLL through
# an import library, and an import library can be generated from a list of
# names - no Vulkan SDK, no 250 MB download, nothing for a user to install.
#
# Only the functions the engine calls are listed. The loader does not export
# every extension entry point, so importing the whole header would produce a
# binary that refuses to start the moment one of them is referenced.
set -e
cd "$(dirname "$0")/.."
grep -ohE "VKAPI_CALL vk[A-Za-z0-9]+" /usr/include/vulkan/vulkan_core.h | awk '{print $2}' | sort -u > /tmp/vk_all.txt
grep -ohE "\bvk[A-Z][A-Za-z0-9]*" src/*.cpp src/*.hpp | sort -u > /tmp/vk_used.txt
{ sed -n '1,6p' thirdparty/win/vulkan-1.def
  comm -12 /tmp/vk_all.txt /tmp/vk_used.txt
  echo vkCreateWin32SurfaceKHR
  echo vkGetPhysicalDeviceWin32PresentationSupportKHR
} > /tmp/vulkan-1.def
mv /tmp/vulkan-1.def thirdparty/win/vulkan-1.def
echo "$(grep -c . thirdparty/win/vulkan-1.def) lines"
