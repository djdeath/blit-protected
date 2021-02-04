/*
 * Copyright © 2021 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Usage : blit-protected input.png output.png
 */

#include <stdbool.h>
#include <stdint.h>

#include <gdk-pixbuf/gdk-pixbuf.h>

#include <vulkan/vulkan.h>

struct data {
   VkInstance instance;
   VkPhysicalDevice physical_device;
   VkPhysicalDeviceMemoryProperties memory_properties;
   VkDevice device;
   VkQueue queue;

   VkCommandPool cmd_pool;

   VkBuffer src_buffer;
   VkDeviceMemory src_mem;

   VkImage dst_image;
   VkDeviceMemory dst_image_mem;

   VkBuffer dst_buffer;
   VkDeviceMemory dst_mem;

   uint32_t width, height;
   uint32_t row_stride, size;
};

static int find_image_memory(struct data *vc, unsigned allowed, bool host, bool protected)
{
   VkMemoryPropertyFlags flags =
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
      (host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT : 0) |
      (protected ? VK_MEMORY_PROPERTY_PROTECTED_BIT : 0);

    for (unsigned i = 0; (1u << i) <= allowed && i <= vc->memory_properties.memoryTypeCount; ++i) {
        if ((allowed & (1u << i)) && (vc->memory_properties.memoryTypes[i].propertyFlags & flags))
            return i;
    }
    return -1;
}

static void
init_vk(struct data *vc)
{
   vkCreateInstance(&(VkInstanceCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
         .pApplicationInfo = &(VkApplicationInfo) {
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = "protected blit",
            .apiVersion = VK_MAKE_VERSION(1, 1, 0),
         },
         .enabledExtensionCount = 0,
         .ppEnabledExtensionNames = NULL,
      },
      NULL,
      &vc->instance);

   uint32_t count = 0;
   VkResult res = vkEnumeratePhysicalDevices(vc->instance, &count, NULL);
   g_assert(res == VK_SUCCESS && count > 0);
   VkPhysicalDevice pd[count];
   vkEnumeratePhysicalDevices(vc->instance, &count, pd);
   vc->physical_device = pd[0];
   g_info("%d physical devices\n", count);

   VkPhysicalDeviceProtectedMemoryFeatures protected_features = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES,
   };
   VkPhysicalDeviceFeatures2 features = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
      .pNext = &protected_features,
   };
   vkGetPhysicalDeviceFeatures2(vc->physical_device, &features);

   g_assert(protected_features.protectedMemory);

   VkPhysicalDeviceProperties properties;
   vkGetPhysicalDeviceProperties(vc->physical_device, &properties);
   g_info("Vendor id %04x, device name %s\n", properties.vendorID, properties.deviceName);

   vkGetPhysicalDeviceMemoryProperties(vc->physical_device, &vc->memory_properties);

   vkGetPhysicalDeviceQueueFamilyProperties(vc->physical_device, &count, NULL);
   g_assert(count > 0);
   VkQueueFamilyProperties props[count];
   vkGetPhysicalDeviceQueueFamilyProperties(vc->physical_device, &count, props);
   g_assert(props[0].queueFlags & VK_QUEUE_GRAPHICS_BIT);

   vkCreateDevice(vc->physical_device,
                  &(VkDeviceCreateInfo) {
                     .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                     .pNext = &protected_features,
                     .queueCreateInfoCount = 1,
                     .pQueueCreateInfos = &(VkDeviceQueueCreateInfo) {
                        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                        .queueFamilyIndex = 0,
                        .queueCount = 1,
                        .flags = VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT,
                        .pQueuePriorities = (float []) { 1.0f },
                     },
                     .enabledExtensionCount = 1,
                     .ppEnabledExtensionNames = (const char * const []) {
                        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                     },
                  },
                  NULL,
                  &vc->device);

   vkGetDeviceQueue(vc->device, 0, 0, &vc->queue);
}

static void
init_image(struct data *vc, const char *filename)
{
   GError *error = NULL;
   GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(filename, &error);

   if (!pixbuf) {
      g_error(error->message);
      exit(-1);
   }

   vc->width = gdk_pixbuf_get_width(pixbuf);
   vc->height = gdk_pixbuf_get_height(pixbuf);
   vc->row_stride = gdk_pixbuf_get_rowstride(pixbuf);
   vc->size = gdk_pixbuf_get_byte_length(pixbuf);

   VkMemoryRequirements requirements;

   /* SRC */
   vkCreateBuffer(vc->device,
                  &(VkBufferCreateInfo) {
                     .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                     .flags = 0,
                     .size = vc->size,
                     .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                  },
                  NULL,
                  &vc->src_buffer);

   vkGetBufferMemoryRequirements(vc->device, vc->src_buffer, &requirements);

   vkAllocateMemory(vc->device,
                    &(VkMemoryAllocateInfo) {
                       .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                       .allocationSize = requirements.size,
                       .memoryTypeIndex = find_image_memory(vc, requirements.memoryTypeBits, true /* host */, false /* protected */),
                    },
                    NULL,
                    &vc->src_mem);

   vkBindBufferMemory(vc->device, vc->src_buffer, vc->src_mem, 0);

   void *map;
   vkMapMemory(vc->device, vc->src_mem, 0, vc->size, 0, &map);
   memcpy(map, gdk_pixbuf_read_pixels(pixbuf), vc->size);
   vkUnmapMemory(vc->device, vc->src_mem);

   g_object_unref(G_OBJECT(pixbuf));

   /* DST */
   vkCreateImage(vc->device,
                 &(VkImageCreateInfo) {
                    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                    .imageType = VK_IMAGE_TYPE_2D,
                    .format = VK_FORMAT_R8G8B8A8_UNORM,
                    .extent = { .width = vc->width, .height = vc->height, .depth = 1 },
                    .mipLevels = 1,
                    .arrayLayers = 1,
                    .samples = 1,
                    .tiling = VK_IMAGE_TILING_OPTIMAL,
                    .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    .flags = VK_IMAGE_CREATE_PROTECTED_BIT,
                 },
                 NULL,
                 &vc->dst_image);

   vkGetImageMemoryRequirements(vc->device, vc->dst_image, &requirements);

   vkAllocateMemory(vc->device,
                    &(VkMemoryAllocateInfo) {
                       .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                       .allocationSize = requirements.size,
                       .memoryTypeIndex = find_image_memory(vc, requirements.memoryTypeBits, false /* host */, true /* protected */),
                    },
                    NULL,
                    &vc->dst_image_mem);

   vkBindImageMemory(vc->device, vc->dst_image, vc->dst_image_mem, 0);

   /* OUTPUT MEMORY */
   vkCreateBuffer(vc->device,
                  &(VkBufferCreateInfo) {
                     .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                     .flags = 0,
                     .size = vc->size,
                     .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                  },
                  NULL,
                  &vc->dst_buffer);

   vkGetBufferMemoryRequirements(vc->device, vc->dst_buffer, &requirements);

   vkAllocateMemory(vc->device,
                    &(VkMemoryAllocateInfo) {
                       .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                       .allocationSize = requirements.size,
                       .memoryTypeIndex = find_image_memory(vc, requirements.memoryTypeBits, true /* host */, false /* protected */),
                    },
                    NULL,
                    &vc->dst_mem);

   vkBindBufferMemory(vc->device, vc->dst_buffer, vc->dst_mem, 0);
}

static void
write_image_output(struct data *vc, const char *filename)
{
   void *map;
   vkMapMemory(vc->device, vc->dst_mem, 0, vc->size, 0, &map);

   GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data(map,
                                                GDK_COLORSPACE_RGB,
                                                true,
                                                8,
                                                vc->width,
                                                vc->height,
                                                vc->row_stride,
                                                NULL,
                                                NULL);

   GError *error = NULL;
   if (!gdk_pixbuf_save(pixbuf, filename, "png", &error, NULL))
      g_error("Could not write output file: %s", error->message);

   vkUnmapMemory(vc->device, vc->dst_mem);
}

int
main(int argc, char *argv[])
{
   struct data data = {}, *vc = &data;
   VkCommandBuffer cmd_buffer;

   if (argc < 3)
      g_error("Require 2 arguments : input_file output_file");

   init_vk(&data);

   init_image(&data, argv[1]);

   vkCreateCommandPool(vc->device,
                       &(const VkCommandPoolCreateInfo) {
                          .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                          .queueFamilyIndex = 0,
                          .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
                                   VK_COMMAND_POOL_CREATE_PROTECTED_BIT,
                       },
                       NULL,
                       &vc->cmd_pool);

   vkAllocateCommandBuffers(vc->device,
      &(VkCommandBufferAllocateInfo) {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = vc->cmd_pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1,
      },
      &cmd_buffer);

   vkBeginCommandBuffer(cmd_buffer,
                        &(VkCommandBufferBeginInfo) {
                           .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                           .flags = 0
                        });

   vkCmdPipelineBarrier(cmd_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        0, 0, NULL,
                        1, &(const VkBufferMemoryBarrier) {
                           .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                           .srcAccessMask = 0,
                           .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                           .buffer = vc->src_buffer,
                           .offset = 0,
                           .size = VK_WHOLE_SIZE,
                        },
                        1, &(const VkImageMemoryBarrier) {
                           .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                           .srcAccessMask = 0,
                           .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                           .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                           .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           .image = vc->dst_image,
                           .subresourceRange = {
                              .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                              .baseMipLevel = 0,
                              .levelCount = 1,
                              .baseArrayLayer = 0,
                              .layerCount = 1,
                           },
                        });

   vkCmdCopyBufferToImage(cmd_buffer, vc->src_buffer, vc->dst_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                          &(const VkBufferImageCopy) {
                             .bufferOffset = 0,
                             .bufferRowLength = vc->width,
                             .bufferImageHeight = vc->height,
                             .imageSubresource = {
                                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                .mipLevel = 0,
                                .baseArrayLayer = 0,
                                .layerCount = 1,
                             },
                             .imageOffset = { 0, 0, 0, },
                             .imageExtent = { vc->width, vc->height, 1 },
                          });

   vkCmdPipelineBarrier(cmd_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        0, 0, NULL,
                        1, &(const VkBufferMemoryBarrier) {
                           .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                           .srcAccessMask = 0,
                           .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                           .buffer = vc->dst_buffer,
                           .offset = 0,
                           .size = VK_WHOLE_SIZE,
                        },
                        1, &(const VkImageMemoryBarrier) {
                           .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                           .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                           .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           .image = vc->dst_image,
                           .subresourceRange = {
                              .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                              .baseMipLevel = 0,
                              .levelCount = 1,
                              .baseArrayLayer = 0,
                              .layerCount = 1,
                           },
                        });

   vkCmdCopyImageToBuffer(cmd_buffer, vc->dst_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vc->dst_buffer, 1,
                          &(const VkBufferImageCopy) {
                             .bufferOffset = 0,
                             .bufferRowLength = vc->width,
                             .bufferImageHeight = vc->height,
                             .imageSubresource = {
                                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                .mipLevel = 0,
                                .baseArrayLayer = 0,
                                .layerCount = 1,
                             },
                             .imageOffset = { 0, 0, 0, },
                             .imageExtent = { vc->width, vc->height, 1 },
                          });

   vkEndCommandBuffer(cmd_buffer);

   vkQueueSubmit(vc->queue, 1,
                 &(const VkSubmitInfo) {
                    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                    .commandBufferCount = 1,
                    .pCommandBuffers = &cmd_buffer,
                 },
                 VK_NULL_HANDLE);

   vkDeviceWaitIdle(vc->device);

   write_image_output(vc, argv[2]);

   return 0;
}
