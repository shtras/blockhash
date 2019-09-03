#include <string>
#include <sstream>
#include <vector>
#include <iostream>
#include <functional>
#include <thread>
#include <fstream>
#include <chrono>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}
#include <assert.h>
#include <map>
using std::cerr;
using std::cout;
using std::endl;
#define DEBUG_BLOCK_HASH
#ifdef DEBUG_BLOCK_HASH
static void pgm_save(unsigned char* buf, int wrap, int xsize, int ysize, const char* filename)
{
    FILE* f;
    int i;

    f = fopen(filename, "w");
    fprintf(f, "P5\n%d %d\n%d\n", xsize, ysize, 255);
    for (i = 0; i < ysize; i++)
        fwrite(buf + i * wrap, 1, xsize, f);
    fclose(f);
}
#endif

class BlockHashCalc
{
public:
    bool Init(AVCodecContext* codecContext);
    uint64_t Compute(AVFrame* frame);
    static float Diff(uint64_t h1, uint64_t h2);

private:
    std::shared_ptr<AVFrame> resize(AVFrame* frame);
    void fillBlocks(AVFrame* frame);
    int median(int start, int length) const;
    uint64_t getHash() const;

    std::shared_ptr<SwsContext> swCtx_ = nullptr;
    static constexpr int w_ = 64;
    static constexpr int h_ = 64;
    static constexpr int bits_ = 8;
    static constexpr int blockWidth_ = w_ / bits_;
    static constexpr int blockHeight_ = h_ / bits_;
    static constexpr int bandPortion_ = 4;
    static constexpr int bandSize_ = bits_ * bits_ / bandPortion_;

    int width_ = 0;
    int height_ = 0;
    AVPixelFormat pixFormat_ = AV_PIX_FMT_NONE;
    std::vector<int> blocks_;
};

bool BlockHashCalc::Init(AVCodecContext* codecContext)
{
    if (width_ == codecContext->width && height_ == codecContext->height && pixFormat_ == codecContext->pix_fmt) {
        return true;
    }
    width_ = codecContext->width;
    height_ = codecContext->height;
    pixFormat_ = codecContext->pix_fmt;
    swCtx_ = std::shared_ptr<SwsContext>(
        sws_getContext(width_, height_, pixFormat_, w_, h_, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL),
        sws_freeContext);

    blocks_.resize(bits_ * bits_);
    return true;
}

float BlockHashCalc::Diff(uint64_t h1, uint64_t h2)
{
    int res = 0;
    uint64_t diff = h1 ^ h2;
    while (diff) {
        res += diff & 1;
        diff >>= 1;
    }
    return res / (float)(bits_ * bits_);
}

void BlockHashCalc::fillBlocks(AVFrame* frame)
{
    int ix, iy;
    int ii;
    const auto data = frame->data;

    for (int y = 0; y < bits_; y++) {
        for (int x = 0; x < bits_; x++) {
            int value = 0;
            for (iy = 0; iy < blockHeight_; iy++) {
                for (ix = 0; ix < blockWidth_; ix++) {
                    ii = ((y * blockHeight_ + iy) * w_ + (x * blockWidth_ + ix));
                    value += data[0][ii] + data[1][ii / 4] + data[2][ii / 4];
                }
            }
            blocks_[y * bits_ + x] = value;
        }
    }
}

int BlockHashCalc::median(int start, int length) const
{
    int result = 0;

    std::vector<int> medianCalc(blocks_.begin() + start, blocks_.begin() + start + length);
    std::sort(medianCalc.begin(), medianCalc.end());

    if (length % 2 == 0) {
        result = (medianCalc[length / 2 - 1] + medianCalc[length / 2]) / 2;
    } else {
        result = medianCalc[length / 2];
    }

    return result;
}

uint64_t BlockHashCalc::getHash() const
{
    uint64_t res = 0;
    constexpr int medianThreshold = blockWidth_ * blockHeight_ * 256 * 3 / 200;

    for (int i = 0; i < bandPortion_; i++) {
        int m = (int)median(i * bandSize_, bandSize_);
        for (int j = i * bandSize_; j < (i + 1) * bandSize_; j++) {
            int v = blocks_[j];
            if (abs(v - m) > medianThreshold) {
                res |= 1LL << j;
            }
        }
    }
    return res;
}

std::shared_ptr<AVFrame> BlockHashCalc::resize(AVFrame* frame)
{
    auto resFrame = std::shared_ptr<AVFrame>(av_frame_alloc(), [](AVFrame* frame) {
        av_freep(&frame->data[0]);
        av_frame_free(&frame);
    });
    auto res = av_image_alloc(resFrame->data, resFrame->linesize, w_, h_, AV_PIX_FMT_YUV420P, 1);
    if (res < 0) {
        return nullptr;
    }
    resFrame->width = w_;
    resFrame->height = h_;
    resFrame->format = AV_PIX_FMT_YUV420P;

    res = sws_scale(swCtx_.get(), frame->data, frame->linesize, 0, frame->height, resFrame->data, resFrame->linesize);
    if (res < 0) {
        return nullptr;
    }
#ifdef DEBUG_BLOCK_HASH
    std::stringstream ss;
    ss << "hash"
       << ".pgm";
    pgm_save(resFrame.get()->data[0], resFrame.get()->linesize[0], resFrame.get()->width, resFrame.get()->height,
        ss.str().c_str());
#endif
    return resFrame;
}

uint64_t BlockHashCalc::Compute(AVFrame* frame)
{
    auto resFrame = resize(frame);
    if (!resFrame) {
        return 0;
    }
    fillBlocks(resFrame.get());
    uint64_t res = getHash();

    return res;
}
std::map<int, std::shared_ptr<AVCodecContext>> codecs;
std::shared_ptr<AVFormatContext> formatContext = nullptr;
BlockHashCalc c;
void initCodec(AVCodecParameters* codecPar, int streamIdx)
{
    auto decoder = avcodec_find_decoder(codecPar->codec_id);
    assert(decoder);
    auto codecContext = std::shared_ptr<AVCodecContext>(avcodec_alloc_context3(decoder), [](AVCodecContext* ctx) {
        avcodec_close(ctx);
        avcodec_free_context(&ctx);
    });
    assert(codecContext);
    codecs[streamIdx] = codecContext;
    avcodec_parameters_to_context(codecContext.get(), codecPar);
    auto ret = avcodec_open2(codecContext.get(), decoder, nullptr);
    assert(ret == 0);
    c.Init(codecContext.get());
}
bool processPacket(AVPacket& pkt)
{
    auto frame = std::shared_ptr<AVFrame>(av_frame_alloc(), [=](AVFrame* frame) { av_frame_free(&frame); });
    auto codecContext = codecs[pkt.stream_index];
    int ret = avcodec_send_packet(codecContext.get(), &pkt);
    if (ret != 0) {
        return true;
    }
    for (;;) {
        ret = avcodec_receive_frame(codecContext.get(), frame.get());
        if (ret < 0) {
            break;
        }
        switch (codecContext->codec_type) {
            case AVMediaType::AVMEDIA_TYPE_VIDEO:
                c.Init(codecContext.get());
                cout << c.Compute(frame.get()) << endl;
                break;
            case AVMediaType::AVMEDIA_TYPE_AUDIO:
                break;
            default:
                break;
        }
    }
    return true;
}
void usage()
{
    cout << "Usage:" << endl;
    cout << "\tblockhash <filename>" << endl;
    cout << "\tblockhash -d <hash1> <hash2>" << endl;
    exit(1);
}
int main(int argc, char* argv[])
{
    if (argc < 2) {
        usage();
    }
    if (strcmp(argv[1], "-d") == 0) {
        if (argc != 4) {
            usage();
        }
        unsigned long long h1 = 0;
        unsigned long long h2 = 0;
        try {
            h1 = std::stoull(argv[2]);
            h2 = std::stoull(argv[3]);
        } catch (const std::invalid_argument&) {
            cerr << "Not a number" << endl;
            return 1;
        } catch (const std::out_of_range&) {
            cerr << "Out of range" << endl;
            return 1;
        }
        cout << BlockHashCalc::Diff(h1, h2) << endl;
        return 0;
    }
    auto formatCtx = avformat_alloc_context();
    formatCtx->iformat = nullptr;
    avformat_open_input(&formatCtx, argv[1], nullptr, nullptr);
    formatContext =
        std::shared_ptr<AVFormatContext>(formatCtx, [](AVFormatContext* ctx) { avformat_close_input(&ctx); });

    for (unsigned int i = 0; i < formatContext->nb_streams; ++i) {
        int codecType = formatContext->streams[i]->codecpar->codec_type;
        if (codecType != AVMediaType::AVMEDIA_TYPE_VIDEO && codecType != AVMediaType::AVMEDIA_TYPE_AUDIO) {
            continue;
        }
        assert(codecs.count(i) == 0);
        //auto id = formatContext->streams[i]->codecpar->codec_id;
        initCodec(formatContext->streams[i]->codecpar, i);
    }

    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = nullptr;
    pkt.size = 0;

    while (true) {
        int ret = av_read_frame(formatContext.get(), &pkt);
        if (ret < 0) {
            break;
        }
        bool res = processPacket(pkt);
        av_packet_unref(&pkt);

        if (!res) {
            break;
        }
    }

    return 0;
}