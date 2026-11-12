#include "instanceuploadpass.h"

#include "rhicommandbuffer.h"

namespace {
struct InstanceUploadPassData {
    FgBufferHandle source;
    FgBufferHandle instances;
};
} // namespace

auto addInstanceUploadPass(FrameGraph& fg, FgBufferHandle source, FgBufferHandle instances, std::vector<RhiBufferCopy> regions, const char* name) -> FgBufferHandle {
    const auto& data = fg.addPass<InstanceUploadPassData>(
        name,
        profile::Group::Upload,
        [&](FrameGraphBuilder& builder, InstanceUploadPassData& data) {
            data.source = builder.read(source, FgAccessFlags::TransferSrc);
            data.instances = builder.write(instances, FgAccessFlags::TransferDst);
        },
        [regions = std::move(regions)](FrameGraphContext& ctx, const InstanceUploadPassData& data) {
            ctx.cmd()->copyBufferRegions(ctx.buffer(data.source), ctx.buffer(data.instances), regions);
        });
    return data.instances;
}
