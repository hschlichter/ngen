#include "instanceuploadpass.h"

#include "rhicommandbuffer.h"

namespace {
struct InstanceUploadPassData {
    FgBufferHandle staging;
    FgBufferHandle instances;
};
} // namespace

auto addInstanceUploadPass(FrameGraph& fg, FgBufferHandle staging, FgBufferHandle instances, std::vector<RhiBufferCopy> regions) -> FgBufferHandle {
    const auto& data = fg.addPass<InstanceUploadPassData>(
        "InstanceUpload",
        [&](FrameGraphBuilder& builder, InstanceUploadPassData& data) {
            data.staging = builder.read(staging, FgAccessFlags::TransferSrc);
            data.instances = builder.write(instances, FgAccessFlags::TransferDst);
        },
        [regions = std::move(regions)](FrameGraphContext& ctx, const InstanceUploadPassData& data) {
            ctx.cmd()->copyBufferRegions(ctx.buffer(data.staging), ctx.buffer(data.instances), regions);
        });
    return data.instances;
}
