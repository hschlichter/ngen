#include "videoencoder.h"

#include <wels/codec_api.h>

#include <cstring>
#include <format>

namespace {

// OpenH264, set up for a live picture: Constrained Baseline, one layer, no frame held back, no frame skipped, a keyframe only when
// asked for. The stream says its own colour (BT.709, limited range), so a browser doesn't have to guess.
class VideoEncoderOpenH264 final : public VideoEncoder {
public:
    ~VideoEncoderOpenH264() override { stop(); }

    auto name() const -> const char* override { return "OpenH264 (software)"; }

    auto start(const VideoSettings& settings) -> std::expected<void, std::string> override {
        stop();
        if (WelsCreateSVCEncoder(&encoder) != 0 || encoder == nullptr) {
            encoder = nullptr;
            return std::unexpected("OpenH264: cannot create an encoder");
        }
        int quiet = WELS_LOG_QUIET;
        encoder->SetOption(ENCODER_OPTION_TRACE_LEVEL, &quiet);

        SEncParamExt params = {};
        encoder->GetDefaultParams(&params);
        params.iUsageType = CAMERA_VIDEO_REAL_TIME;
        params.iPicWidth = (int) settings.width;
        params.iPicHeight = (int) settings.height;
        params.fMaxFrameRate = (float) settings.framesPerSecond;
        params.iRCMode = RC_BITRATE_MODE;
        params.iTargetBitrate = (int) settings.bitrate;
        params.iMaxBitrate = (int) settings.bitrate;
        params.iComplexityMode = LOW_COMPLEXITY;
        params.uiIntraPeriod = 0;
        params.iTemporalLayerNum = 1;
        params.iSpatialLayerNum = 1;
        params.bEnableFrameSkip = false;
        params.bEnableDenoise = false;
        params.bEnableBackgroundDetection = false;
        params.bEnableAdaptiveQuant = false;
        params.bEnableSceneChangeDetect = false;
        params.bPrefixNalAddingCtrl = false;
        params.eSpsPpsIdStrategy = CONSTANT_ID;
        params.iEntropyCodingModeFlag = 0;
        // As many slices as threads, so the frame is encoded on several cores.
        params.iMultipleThreadIdc = sliceThreads;

        auto& layer = params.sSpatialLayers[0];
        layer.iVideoWidth = (int) settings.width;
        layer.iVideoHeight = (int) settings.height;
        layer.fFrameRate = (float) settings.framesPerSecond;
        layer.iSpatialBitrate = (int) settings.bitrate;
        layer.iMaxSpatialBitrate = (int) settings.bitrate;
        layer.uiProfileIdc = PRO_BASELINE;
        layer.uiLevelIdc = LEVEL_UNKNOWN;
        layer.sSliceArgument.uiSliceMode = SM_FIXEDSLCNUM_SLICE;
        layer.sSliceArgument.uiSliceNum = sliceThreads;
        layer.bVideoSignalTypePresent = true;
        layer.uiVideoFormat = VF_UNDEF;
        layer.bFullRange = false;
        layer.bColorDescriptionPresent = true;
        layer.uiColorPrimaries = CP_BT709;
        layer.uiTransferCharacteristics = TRC_IEC61966_2_1;
        layer.uiColorMatrix = CM_BT709;

        if (int result = encoder->InitializeExt(&params); result != 0) {
            WelsDestroySVCEncoder(encoder);
            encoder = nullptr;
            return std::unexpected(std::format("OpenH264: cannot start at {}x{}, {} frames a second, {} bits a second (error {})",
                                               settings.width,
                                               settings.height,
                                               settings.framesPerSecond,
                                               settings.bitrate,
                                               result));
        }
        int format = videoFormatI420;
        encoder->SetOption(ENCODER_OPTION_DATAFORMAT, &format);
        current = settings;
        frameIndex = 0;
        return {};
    }

    auto encode(const VideoPlanes& planes, bool keyframe, VideoPacket& packet) -> std::expected<void, std::string> override {
        packet.data.clear();
        packet.keyframe = false;
        if (encoder == nullptr) {
            return std::unexpected("OpenH264: not started");
        }
        if (planes.width != current.width || planes.height != current.height) {
            return std::unexpected(std::format("OpenH264: a {}x{} picture for an encoder started at {}x{}", planes.width, planes.height, current.width, current.height));
        }
        if (keyframe) {
            encoder->ForceIntraFrame(true);
        }
        SSourcePicture picture = {};
        picture.iColorFormat = videoFormatI420;
        picture.iPicWidth = (int) planes.width;
        picture.iPicHeight = (int) planes.height;
        picture.iStride[0] = (int) planes.width;
        picture.iStride[1] = (int) planes.width / 2;
        picture.iStride[2] = (int) planes.width / 2;
        picture.pData[0] = const_cast<unsigned char*>(planes.y.data());
        picture.pData[1] = const_cast<unsigned char*>(planes.cb.data());
        picture.pData[2] = const_cast<unsigned char*>(planes.cr.data());
        picture.uiTimeStamp = (long long) (frameIndex * 1000 / current.framesPerSecond);
        frameIndex++;

        SFrameBSInfo info = {};
        if (int result = encoder->EncodeFrame(&picture, &info); result != 0) {
            return std::unexpected(std::format("OpenH264: encode failed (error {})", result));
        }
        if (info.eFrameType == videoFrameTypeSkip || info.eFrameType == videoFrameTypeInvalid) {
            return {};
        }
        // Each layer's NAL units lie one after another in its buffer, each already behind a start code.
        for (int layerIndex = 0; layerIndex < info.iLayerNum; layerIndex++) {
            const auto& layer = info.sLayerInfo[layerIndex];
            size_t bytes = 0;
            for (int nal = 0; nal < layer.iNalCount; nal++) {
                bytes += (size_t) layer.pNalLengthInByte[nal];
            }
            packet.data.insert(packet.data.end(), layer.pBsBuf, layer.pBsBuf + bytes);
        }
        packet.keyframe = info.eFrameType == videoFrameTypeIDR;
        return {};
    }

    auto stop() -> void override {
        if (encoder != nullptr) {
            encoder->Uninitialize();
            WelsDestroySVCEncoder(encoder);
            encoder = nullptr;
        }
    }

private:
    static constexpr int sliceThreads = 4;

    ISVCEncoder* encoder = nullptr;
    VideoSettings current;
    uint64_t frameIndex = 0;
};

} // namespace

auto makeVideoEncoderOpenH264() -> std::unique_ptr<VideoEncoder> {
    return std::make_unique<VideoEncoderOpenH264>();
}
