#include "nova/rhi/vulkan/VulkanDevice.h"

namespace nova::rhi::vk {

void VulkanCommandList::reset(VkCommandBuffer cmd) {
    m_cmd = cmd;
    m_openTimers.clear();
}

void VulkanCommandList::beginGpuTimer(const char* name) {
    const u32 timer = m_device.openGpuTimer(name, static_cast<u32>(m_openTimers.size()));
    m_openTimers.push_back(timer);
    if (timer != ~0u) vkCmdWriteTimestamp2(m_cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_device.timerPool(), timer * 2);
}

void VulkanCommandList::endGpuTimer() {
    if (m_openTimers.empty()) return;
    const u32 timer = m_openTimers.back();
    m_openTimers.pop_back();
    if (timer != ~0u) vkCmdWriteTimestamp2(m_cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_device.timerPool(), timer * 2 + 1);
}

void VulkanCommandList::beginRendering(const RenderingDesc& desc) {
    std::vector<VkRenderingAttachmentInfo> colors;
    colors.reserve(desc.colors.size());
    Extent2D extent = desc.extent;

    for (const ColorAttachment& c : desc.colors) {
        if (!c.texture) { // an unused slot: the pipeline's writes to it are discarded
            colors.push_back({VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO});
            continue;
        }
        VulkanTexture* t = m_device.texture(c.texture);
        NOVA_ASSERT(t, "beginRendering: invalid color attachment");
        recordTransition(m_cmd, *t, ResourceState::RenderTarget);
        if (extent.isZero()) extent = {t->extent.width, t->extent.height};
        VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        a.imageView = t->view;
        a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        a.loadOp = toVk(c.load);
        a.storeOp = toVk(c.store);
        a.clearValue.color = {{c.clear[0], c.clear[1], c.clear[2], c.clear[3]}};
        colors.push_back(a);
    }

    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (desc.depth) {
        VulkanTexture* t = m_device.texture(desc.depth->texture);
        NOVA_ASSERT(t, "beginRendering: invalid depth attachment");
        recordTransition(m_cmd, *t, ResourceState::DepthWrite);
        if (extent.isZero()) extent = {t->extent.width, t->extent.height};
        depth.imageView = t->view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = toVk(desc.depth->load);
        depth.storeOp = toVk(desc.depth->store);
        depth.clearValue.depthStencil = {desc.depth->clearDepth, 0};
    }

    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {extent.width, extent.height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = static_cast<u32>(colors.size());
    ri.pColorAttachments = colors.data();
    ri.pDepthAttachment = desc.depth ? &depth : nullptr;
    vkCmdBeginRendering(m_cmd, &ri);

    setViewport(0, 0, static_cast<f32>(extent.width), static_cast<f32>(extent.height));
    setScissor(0, 0, extent.width, extent.height);
}

void VulkanCommandList::endRendering() { vkCmdEndRendering(m_cmd); }

void VulkanCommandList::bindPipeline(PipelineHandle h) {
    VulkanPipeline* p = m_device.pipeline(h);
    NOVA_ASSERT(p, "bindPipeline: invalid pipeline");
    vkCmdBindPipeline(m_cmd, p->bindPoint, p->pipeline);
    m_bound = h;
    VkDescriptorSet set = m_device.bindlessSet();
    vkCmdBindDescriptorSets(m_cmd, p->bindPoint, m_device.pipelineLayout(), 0, 1, &set, 0, nullptr);
}

void VulkanCommandList::pushConstants(const void* data, u32 size) {
    NOVA_ASSERT(size <= kPushConstantSize, "push constants too large");
    vkCmdPushConstants(m_cmd, m_device.pipelineLayout(), VK_SHADER_STAGE_ALL, 0, size, data);
}

void VulkanCommandList::setViewport(f32 x, f32 y, f32 width, f32 height) {
    VkViewport vp{x, y, width, height, 0.0f, 1.0f};
    vkCmdSetViewport(m_cmd, 0, 1, &vp);
}

void VulkanCommandList::setScissor(i32 x, i32 y, u32 width, u32 height) {
    VkRect2D r{{x, y}, {width, height}};
    vkCmdSetScissor(m_cmd, 0, 1, &r);
}

void VulkanCommandList::bindIndexBuffer(BufferHandle h, u64 offset) {
    VulkanBuffer* b = m_device.buffer(h);
    NOVA_ASSERT(b, "bindIndexBuffer: invalid buffer");
    vkCmdBindIndexBuffer(m_cmd, b->buffer, offset, VK_INDEX_TYPE_UINT32);
}

void VulkanCommandList::draw(u32 vertexCount, u32 instanceCount, u32 firstVertex, u32 firstInstance) {
    vkCmdDraw(m_cmd, vertexCount, instanceCount, firstVertex, firstInstance);
}

void VulkanCommandList::drawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex, i32 vertexOffset,
                                    u32 firstInstance) {
    vkCmdDrawIndexed(m_cmd, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void VulkanCommandList::drawIndexedIndirect(BufferHandle args, u64 offset, u32 drawCount, u32 stride) {
    VulkanBuffer* b = m_device.buffer(args);
    NOVA_ASSERT(b, "drawIndexedIndirect: invalid buffer");
    vkCmdDrawIndexedIndirect(m_cmd, b->buffer, offset, drawCount, stride);
}

void VulkanCommandList::dispatch(u32 x, u32 y, u32 z) { vkCmdDispatch(m_cmd, x, y, z); }

void VulkanCommandList::traceRays(u32 width, u32 height, u32 depth) {
    VulkanPipeline* p = m_device.pipeline(m_bound);
    NOVA_ASSERT(p && p->bindPoint == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, "traceRays: no ray tracing pipeline bound");
    m_device.ext().vkCmdTraceRaysKHR(m_cmd, &p->rayGen, &p->miss, &p->hit, &p->callable, width, height, depth);
}

void VulkanCommandList::transition(TextureHandle h, ResourceState state) {
    VulkanTexture* t = m_device.texture(h);
    NOVA_ASSERT(t, "transition: invalid texture");
    recordTransition(m_cmd, *t, state);
}

void VulkanCommandList::globalBarrier() {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(m_cmd, &dep);
}

void VulkanCommandList::copyBuffer(BufferHandle src, BufferHandle dst, u64 size, u64 srcOffset, u64 dstOffset) {
    VulkanBuffer* s = m_device.buffer(src);
    VulkanBuffer* d = m_device.buffer(dst);
    NOVA_ASSERT(s && d, "copyBuffer: invalid buffer");
    VkBufferCopy region{srcOffset, dstOffset, size};
    vkCmdCopyBuffer(m_cmd, s->buffer, d->buffer, 1, &region);
}

void VulkanCommandList::copyTextureToBuffer(TextureHandle src, BufferHandle dst, u64 dstOffset) {
    VulkanTexture* t = m_device.texture(src);
    VulkanBuffer* d = m_device.buffer(dst);
    NOVA_ASSERT(t && d, "copyTextureToBuffer: invalid texture or buffer");
    recordTransition(m_cmd, *t, ResourceState::CopySrc);
    VkBufferImageCopy region{};
    region.bufferOffset = dstOffset;
    region.imageSubresource = {t->aspect, 0, 0, 1};
    region.imageExtent = {t->extent.width, t->extent.height, 1};
    vkCmdCopyImageToBuffer(m_cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->buffer, 1, &region);
}

void VulkanCommandList::buildTLAS(AccelStructHandle h, u64 instanceAddress, u32 instanceCount) {
    VulkanAccelStruct* as = m_device.accelStruct(h);
    NOVA_ASSERT(as && as->topLevel, "buildTLAS: invalid TLAS");
    NOVA_ASSERT(instanceCount <= as->maxInstances, "buildTLAS: too many instances");

    // Shaders that trace against acceleration structures (ray tracing pipelines too, when enabled).
    const VkPipelineStageFlags2 kTraceStages =
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
        (m_device.caps().rayTracingPipeline ? VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR : VkPipelineStageFlags2(0));

    // WAR: earlier traces against this TLAS in the same command buffer must finish first.
    VkMemoryBarrier2 pre{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    pre.srcStageMask = kTraceStages | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    pre.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    pre.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    pre.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &pre;
    vkCmdPipelineBarrier2(m_cmd, &dep);

    VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geom.geometry.instances.arrayOfPointers = VK_FALSE;
    geom.geometry.instances.data.deviceAddress = instanceAddress;

    VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    // The same instance count as its last build: refit in place (much cheaper), with a full rebuild
    // every kTlasRefits frames of it so moving instances do not degrade the tree for long.
    constexpr u32 kTlasRefits = 16;
    const bool refit = as->builtInstances == instanceCount && as->refits < kTlasRefits;
    build.mode = refit ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    if (refit) build.srcAccelerationStructure = as->handle;
    as->refits = refit ? as->refits + 1 : 0;
    as->builtInstances = instanceCount;
    build.dstAccelerationStructure = as->handle;
    build.geometryCount = 1;
    build.pGeometries = &geom;
    build.scratchData.deviceAddress = as->scratchAddress;

    VkAccelerationStructureBuildRangeInfoKHR range{instanceCount, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    m_device.ext().vkCmdBuildAccelerationStructuresKHR(m_cmd, 1, &build, &ranges);

    VkMemoryBarrier2 post{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    post.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    post.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    post.dstStageMask = kTraceStages;
    post.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    dep.pMemoryBarriers = &post;
    vkCmdPipelineBarrier2(m_cmd, &dep);
}

void VulkanCommandList::updateBLAS(AccelStructHandle h) {
    VulkanAccelStruct* as = m_device.accelStruct(h);
    NOVA_ASSERT(as && !as->topLevel && as->allowUpdate, "updateBLAS: not an updatable BLAS");
    // Shaders that trace against acceleration structures (ray tracing pipelines too, when enabled).
    const VkPipelineStageFlags2 kTraceStages =
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
        (m_device.caps().rayTracingPipeline ? VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR : VkPipelineStageFlags2(0));

    // The vertices written by shaders before, and earlier traces / TLAS builds of this BLAS done.
    VkMemoryBarrier2 pre{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    pre.srcStageMask = kTraceStages | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    pre.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                        VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    pre.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    pre.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                        VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &pre;
    vkCmdPipelineBarrier2(m_cmd, &dep);

    VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
    build.srcAccelerationStructure = as->handle;
    build.dstAccelerationStructure = as->handle;
    build.geometryCount = 1;
    build.pGeometries = &as->geometry;
    build.scratchData.deviceAddress = as->scratchAddress;
    VkAccelerationStructureBuildRangeInfoKHR range{as->primitiveCount, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    m_device.ext().vkCmdBuildAccelerationStructuresKHR(m_cmd, 1, &build, &ranges);

    // Later TLAS builds and traces see it refitted.
    VkMemoryBarrier2 post{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    post.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    post.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    post.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | kTraceStages;
    post.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    dep.pMemoryBarriers = &post;
    vkCmdPipelineBarrier2(m_cmd, &dep);
}

void VulkanCommandList::beginDebugLabel(const char* name) {
    if (!m_device.ext().vkCmdBeginDebugUtilsLabelEXT) return;
    VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    label.pLabelName = name;
    m_device.ext().vkCmdBeginDebugUtilsLabelEXT(m_cmd, &label);
}

void VulkanCommandList::endDebugLabel() {
    if (m_device.ext().vkCmdEndDebugUtilsLabelEXT) m_device.ext().vkCmdEndDebugUtilsLabelEXT(m_cmd);
}

} // namespace nova::rhi::vk
