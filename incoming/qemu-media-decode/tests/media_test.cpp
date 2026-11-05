// Copyright 2026 jinghero. SPDX-License-Identifier: Apache-2.0
#include "aemu/base/files/MemStream.h"
#include "android/base/system/System.h"
#include "host-common/FeatureControl.h"
#include "host-common/MediaCudaDriverHelper.h"
#include "host-common/MediaCudaVideoHelper.h"
#include "host-common/MediaH264Decoder.h"
#include "host-common/MediaHevcDecoder.h"
#include "host-common/MediaSnapshotHelper.h"
#include "host-common/MediaVpxDecoder.h"
#include "host-common/opengl/emugl_config.h"

#include <array>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>

using namespace android::emulation;
using android::base::MemStream;

static std::string mode;
static int contexts, attempts, uploads;
static std::vector<uint64_t> submissions;
static uint32_t nextTexture;
static std::set<uint32_t> textures;
static uint32_t pixelHash = 2166136261u;

static void check(bool ok, const char* message) {
    if (!ok) {
        std::cerr << mode << ": " << message << std::endl;
        std::exit(1);
    }
}

// Replace the driver and GL boundary. The manager, packet parser, libvpx,
// snapshot stream, renderer and texture pool are the production code.
std::string android::base::System::getEnvironmentVariable(std::string_view key) {
    const char* value = std::getenv(std::string(key).c_str());
    return value ? value : "";
}
bool android::featurecontrol::isEnabled(Feature) { return false; }
SelectedRenderer emuglConfig_get_current_gles_renderer() { return SELECTED_RENDERER_HOST; }
bool MediaCudaDriverHelper::initCudaDrivers() { return mode != "h264"; }
bool MediaCudaVideoHelper::s_isCudaDecoderGood = true;
MediaCudaVideoHelper::MediaCudaVideoHelper(OutputTreatmentMode, FrameStorageMode storage,
                                           cudaVideoCodec codec)
    : mUseGpuTexture(storage == FrameStorageMode::USE_GPU_TEXTURE), mCudaVideoCodecType(codec) {
    ++contexts;
}
MediaCudaVideoHelper::~MediaCudaVideoHelper() { --contexts; }
bool MediaCudaVideoHelper::init() { ++attempts; return mode != "fallback"; }
void MediaCudaVideoHelper::deInit() {}
void MediaCudaVideoHelper::flush() {}
void MediaCudaVideoHelper::decode(const uint8_t*, size_t, uint64_t pts) {
    submissions.push_back(pts);
    if (mIgnoreDecoderOutput) return;
    check(mTexturePool != nullptr, "expected texture pool");
    auto pair = mTexturePool->getTextureFrame(128, 96);
    mSavedDecodedFrames.push_back({{}, {pair.Ytex, pair.UVtex}, 128, 96, pts, {}});
}

AndroidVirtioGpuOps* android_getVirtioGpuOps() {
    static AndroidVirtioGpuOps ops{};
    ops.create_yuv_textures = [](uint32_t, uint32_t count, int, int, uint32_t* names) {
        for (unsigned i = 0; i < 2 * count; ++i) textures.insert(names[i] = ++nextTexture);
    };
    ops.destroy_yuv_textures = [](uint32_t, uint32_t count, uint32_t* names) {
        for (unsigned i = 0; i < 2 * count; ++i) check(textures.erase(names[i]) == 1, "double texture release");
    };
    ops.update_color_buffer_from_framework_format = [](uint32_t, int, int, int w, int h,
            uint32_t, uint32_t, uint32_t, void* pixels, void*) {
        check(pixels && w == 128 && h == 96, "invalid CPU frame");
        for (int i = 0; i < w*h*3/2; ++i) {
            pixelHash = (pixelHash ^ static_cast<uint8_t*>(pixels)[i]) * 16777619u;
        }
        ++uploads;
    };
    ops.swap_textures_and_update_color_buffer = [](uint32_t, int, int, int, int, uint32_t,
            uint32_t, uint32_t, uint32_t* pair, void*) {
        check(textures.count(pair[0]) && textures.count(pair[1]), "invalid texture upload");
    };
    return &ops;
}

using Packet = std::vector<uint8_t>;
static std::vector<Packet> readIvf(const std::filesystem::path& path, const char* fourcc) {
    std::ifstream input(path, std::ios::binary);
    std::array<char, 32> header;
    check(bool(input.read(header.data(), header.size())) &&
          std::string(header.data(), 4) == "DKIF" && std::string(header.data()+8, 4) == fourcc, "wrong IVF format");
    std::vector<Packet> packets;
    uint32_t len;
    while (input.read(reinterpret_cast<char*>(&len), 4)) {
        check(len > 0 && len <= 1 << 20, "bad packet length");
        input.ignore(8);
        Packet data(len);
        check(bool(input.read(reinterpret_cast<char*>(data.data()), len)), "truncated packet");
        packets.emplace_back(std::move(data));
    }
    check(!packets.empty(), "no packets");
    return packets;
}

struct Guest {
    MediaCodecType type;
    std::unique_ptr<MediaCodec> decoder;
    std::vector<uint64_t> words = std::vector<uint64_t>(64);
    uint64_t id;
    MediaCodec* create() const {
        switch (type) {
            case MediaCodecType::H264Codec: return MediaH264Decoder::create();
            case MediaCodecType::HevcCodec: return MediaHevcDecoder::create();
            default: return MediaVpxDecoder::create();
        }
    }
    explicit Guest(MediaCodecType t, int width = 1920, int height = 1080) : type(t), decoder(create()) {
        const bool vpx = type == MediaCodecType::VP9Codec || type == MediaCodecType::VP8Codec;
        words[0] = vpx ? 7 : 200;
        words[1] = vpx ? 200 : width;
        words[2] = words[4] = height;
        words[3] = width;
        ping(MediaOperation::InitContext);
        id = vpx ? 7 : words[32];
        words[0] = id;
    }
    void ping(MediaOperation op) { decoder->handlePing(type, op, words.data()); }
    void decode(const Packet& packet, uint64_t pts) {
        words.resize(64 + (packet.size()+7)/8);
        words[1] = 512;
        words[2] = packet.size();
        words[3] = pts;
        std::memcpy(words.data()+64, packet.data(), packet.size());
        ping(MediaOperation::DecodeImage);
    }
    int image() {
        words[6] = 1;
        words[32] = 0;
        ping(MediaOperation::GetImage);
        return static_cast<int>(words[32]);
    }
    void restore(MemStream& stream) {
        stream.rewind();
        decoder.reset(create());
        check(decoder->load(&stream), "decoder load failed");
    }
};

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    mode = argv[1];
    _putenv_s("ANDROID_EMU_MEDIA_DECODER_CUDA_VPX", mode == "default" ? "" : "1");
    _putenv_s("ANDROID_EMU_MEDIA_DECODER_CUDA", "");
    std::filesystem::path ivf(argv[2]);
    auto packets = readIvf(ivf, "VP90");
    if (mode == "owners") {
        bool clean = true;
        for (auto type : {MediaCodecType::VP9Codec, MediaCodecType::H264Codec, MediaCodecType::HevcCodec}) {
            int initial = contexts;
            size_t previousTextures = textures.size();
            Guest guest(type);
            if (type == MediaCodecType::VP9Codec) {
                guest.decode(packets.front(), 1);
            } else {
                std::ifstream input(ivf.parent_path() / (type == MediaCodecType::H264Codec ? "key.h264" : "key.hevc"),
                                    std::ios::binary);
                Packet key{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
                check(!key.empty(), "missing keyframe fixture");
                guest.decode(key, 1);
            }
            MemStream snapshot;
            guest.decoder->save(&snapshot);
            for (int i = 0; i < 10; ++i) guest.restore(snapshot);
            guest.decoder.reset();
            std::cout << "codec=" << int(type) << " outstanding helpers=" << contexts - initial
                      << " textures=" << textures.size() - previousTextures << std::endl;
            clean &= contexts == initial && textures.size() == previousTextures;
        }
        check(clean, "resources survived owner destruction");
    } else if (mode == "h264") {
        Guest guest(MediaCodecType::H264Codec, 128, 96);
        for (int i = 1; i <= 24; ++i) {
            char name[32];
            std::snprintf(name, sizeof(name), "access-%02d.h264", i);
            std::ifstream input(ivf.parent_path() / name, std::ios::binary);
            Packet packet{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            check(!packet.empty(), "missing H.264 access unit");
            guest.decode(packet, i);
            guest.words[2] = 1; // host color buffer ID for H.264 protocol 200
            guest.image();
        }
        guest.ping(MediaOperation::Flush);
        for (int i = 0; i <= 24 && guest.image() > 0; ++i) {}
        check(uploads == 24 && attempts == 0, "missing FFmpeg output");
        std::cout << "H.264 frames=" << uploads << " pixel hash=" << std::hex << pixelHash << std::endl;
        guest.ping(MediaOperation::DestroyContext);
    } else if (mode == "restored") {
        MediaSnapshotState saved;
        saved.saveDecodedFrame(std::vector<uint32_t>{10, 11}, 2, 2, 1);
        const Packet pixels{1,2,3,4,5,6};
        saved.saveDecodedFrame(pixels, 2, 2, 2);
        MemStream stream;
        saved.save(&stream);
        stream.rewind();
        MediaSnapshotState loaded;
        loaded.load(&stream);
        auto* frame = loaded.frontFrame();
        check(frame && frame->data == pixels && frame->pts == 2, "restored queue starts with an undeliverable frame");
        loaded.discardFrontFrame();
        check(!loaded.frontFrame(), "extra output");
    } else if (mode == "replay") {
        bool once = true;
        for (auto type : {MediaCodecType::VP8Codec, MediaCodecType::VP9Codec}) {
            Guest guest(type);
            const auto source = type == MediaCodecType::VP8Codec ? readIvf(ivf.parent_path() / "vp8.ivf", "VP80") : packets;
            check(source.size() >= 3, "replay needs three packets");
            MediaSnapshotHelper reference(type == MediaCodecType::VP8Codec
                                         ? MediaSnapshotHelper::CodecType::VP8 : MediaSnapshotHelper::CodecType::VP9);
            for (int i = 0; i < 3; ++i) {
                guest.decode(source[i], i + 1);
                reference.savePacket(source[i].data(), source[i].size(), i + 1);
            }
            std::vector<uint64_t> expected;
            reference.replay([&](uint8_t*, size_t, uint64_t pts) { expected.push_back(pts); });
            MemStream snapshot;
            guest.decoder->save(&snapshot);
            submissions.clear();
            guest.restore(snapshot);
            std::cout << "codec=" << int(type) << " saved=" << expected.size()
                      << " replay submissions=" << submissions.size() << std::endl;
            once &= !expected.empty() && submissions == expected;
        }
        check(once, "replay did not submit each saved PTS exactly once");
    } else {
        Guest guest(MediaCodecType::VP9Codec);
        for (size_t i = 0; i < packets.size(); ++i) {
            guest.decode(packets[i], i+1);
            check(guest.image() == 0 && guest.words[36] == i+1, "lost or reordered frame");
        }
        guest.ping(MediaOperation::Flush);
        check(guest.image() == 1, "unexpected output after flush");
        check(attempts == (mode == "fallback") && uploads == packets.size(), "wrong backend or frame count");
        std::cout << "frames=" << uploads << " pixel hash=" << std::hex << pixelHash << std::endl;
        guest.ping(MediaOperation::DestroyContext);
    }
    std::cout << mode << " PASS\n";
}
