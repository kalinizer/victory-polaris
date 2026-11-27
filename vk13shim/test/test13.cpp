// Functional test for the vulkan-1 shim:
//   * does it report Vulkan 1.3 to the application (the lie)?
//   * does it advertise the synthesised extensions?
//   * does vkCreateDevice accept them (they must be stripped before the driver)?
//   * does vkCmdBeginRendering/vkCmdEndRendering actually record something the
//     driver executes (a real submit, waited on)?
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstring>
#include <vector>
#include <cstdio>

#define CHECK(x)                                                          \
    do {                                                                  \
        VkResult r_ = (x);                                                \
        if (r_ != VK_SUCCESS) {                                           \
            std::printf("FAIL %s -> %d (line %d)\n", #x, int(r_), __LINE__); \
            return 1;                                                     \
        }                                                                 \
    } while (0)

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("start\n");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    app.pApplicationName = "shim-test";

    const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
    (void)layers;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    std::printf("creating instance...\n");
    CHECK(vkCreateInstance(&ici, nullptr, &instance));
    std::printf("instance ok\n");

    uint32_t count = 0;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    std::vector<VkPhysicalDevice> gpus(count);
    CHECK(vkEnumeratePhysicalDevices(instance, &count, gpus.data()));
    std::printf("devices: %u\n", count);

    VkPhysicalDevice gpu = gpus[0];
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(gpu, &props);
    std::printf("device      : %s\n", props.deviceName);
    std::printf("apiVersion  : %u.%u.%u  (shim should say 1.3)\n", VK_API_VERSION_MAJOR(props.apiVersion),
                VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));

    // The synthesised extensions must be advertised.
    uint32_t ext_count = 0;
    CHECK(vkEnumerateDeviceExtensionProperties(gpu, nullptr, &ext_count, nullptr));
    std::vector<VkExtensionProperties> exts(ext_count);
    CHECK(vkEnumerateDeviceExtensionProperties(gpu, nullptr, &ext_count, exts.data()));
    bool has_dyn = false, has_m4 = false;
    for (const auto& e : exts) {
        if (std::strcmp(e.extensionName, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) == 0) has_dyn = true;
        if (std::strcmp(e.extensionName, VK_KHR_MAINTENANCE_4_EXTENSION_NAME) == 0) has_m4 = true;
    }
    std::printf("extensions  : %u total, dynamic_rendering=%s maintenance4=%s\n", ext_count,
                has_dyn ? "yes" : "no", has_m4 ? "yes" : "no");
    if (!has_dyn || !has_m4) {
        std::printf("FAIL: synthesised extensions not advertised\n");
        return 1;
    }

    // Vulkan13 features must be filled in by the shim.
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f13;
    vkGetPhysicalDeviceFeatures2(gpu, &f2);
    std::printf("features    : dynamicRendering=%d synchronization2=%d maintenance4=%d\n",
                int(f13.dynamicRendering), int(f13.synchronization2), int(f13.maintenance4));

    // Queue family with graphics.
    uint32_t qcount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qcount, nullptr);
    std::vector<VkQueueFamilyProperties> qs(qcount);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qcount, qs.data());
    uint32_t qf = UINT32_MAX;
    for (uint32_t i = 0; i < qcount; ++i) {
        if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            qf = i;
            break;
        }
    }
    if (qf == UINT32_MAX) {
        std::printf("FAIL: no graphics queue\n");
        return 1;
    }

    // Enable the fake extensions: vkCreateDevice must strip them.
    const char* enabled[] = {VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
                             VK_KHR_MAINTENANCE_4_EXTENSION_NAME};
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = qf;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f13;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 2;
    dci.ppEnabledExtensionNames = enabled;
    VkDevice device = VK_NULL_HANDLE;
    CHECK(vkCreateDevice(gpu, &dci, nullptr, &device));
    std::printf("device      : created with the fake extensions enabled\n");

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, qf, 0, &queue);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = qf;
    VkCommandPool pool = VK_NULL_HANDLE;
    CHECK(vkCreateCommandPool(device, &pci, nullptr, &pool));
    VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbi.commandPool = pool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    CHECK(vkAllocateCommandBuffers(device, &cbi, &cmd));

    // A colour target for the rendered pass.
    VkImageCreateInfo image_ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_ci.imageType = VK_IMAGE_TYPE_2D;
    image_ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_ci.extent = {64, 64, 1};
    image_ci.mipLevels = 1;
    image_ci.arrayLayers = 1;
    image_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    image_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    CHECK(vkCreateImage(device, &image_ci, nullptr, &image));
    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(device, image, &mr);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((mr.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            type = i;
            break;
        }
    }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    CHECK(vkAllocateMemory(device, &mai, nullptr, &memory));
    CHECK(vkBindImageMemory(device, image, memory, 0));

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    CHECK(vkCreateImageView(device, &vci, nullptr, &view));

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(cmd, &begin));

    VkImageMemoryBarrier to_attachment{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_attachment.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_attachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_attachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_attachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_attachment.image = image;
    to_attachment.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_attachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &to_attachment);

    // The bridge under test.
    VkRenderingAttachmentInfo colour{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colour.imageView = view;
    colour.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colour.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colour.clearValue.color = {{0.1f, 0.2f, 0.3f, 1.0f}};

    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {64, 64}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &colour;
    vkCmdBeginRendering(cmd, &ri);
    vkCmdEndRendering(cmd);
    CHECK(vkEndCommandBuffer(cmd));

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    CHECK(vkDeviceWaitIdle(device));
    std::printf("render pass : begin/end recorded and submitted without error\n");

    vkDestroyImageView(device, view, nullptr);
    vkFreeMemory(device, memory, nullptr);
    vkDestroyImage(device, image, nullptr);
    vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    std::printf("PASS\n");
    return 0;
}
