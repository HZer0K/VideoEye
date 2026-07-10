#include "QualityAnalyzer.h"

#include <cmath>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/error.h>
}

namespace videoeye {
namespace analyzer {

namespace {

// PSNR (dB) on the luma channel. 返回 100.0 表示完全相同的帧 (MSE≈0)。
double ComputePsnrGray(const cv::Mat& a, const cv::Mat& b) {
    cv::Mat ga, gb;
    cv::cvtColor(a, ga, cv::COLOR_BGRA2GRAY);
    cv::cvtColor(b, gb, cv::COLOR_BGRA2GRAY);
    cv::Mat fa, fb;
    ga.convertTo(fa, CV_64F);
    gb.convertTo(fb, CV_64F);
    cv::Mat diff;
    cv::absdiff(fa, fb, diff);
    diff = diff.mul(diff);
    const double mse = cv::mean(diff)[0];
    if (mse <= 1e-10) return 100.0;
    return 10.0 * std::log10((255.0 * 255.0) / mse);
}

// SSIM on the luma channel, 使用 11x11 σ=1.5 高斯窗。
double ComputeSsimGray(const cv::Mat& a, const cv::Mat& b) {
    cv::Mat ga, gb;
    cv::cvtColor(a, ga, cv::COLOR_BGRA2GRAY);
    cv::cvtColor(b, gb, cv::COLOR_BGRA2GRAY);
    cv::Mat fa, fb;
    ga.convertTo(fa, CV_64F);
    gb.convertTo(fb, CV_64F);

    const double C1 = (0.01 * 255.0) * (0.01 * 255.0);
    const double C2 = (0.03 * 255.0) * (0.03 * 255.0);
    const cv::Size ksize(11, 11);

    cv::Mat mu1, mu2;
    cv::GaussianBlur(fa, mu1, ksize, 1.5);
    cv::GaussianBlur(fb, mu2, ksize, 1.5);

    cv::Mat mu1_sq, mu2_sq, mu1_mu2;
    cv::multiply(mu1, mu1, mu1_sq);
    cv::multiply(mu2, mu2, mu2_sq);
    cv::multiply(mu1, mu2, mu1_mu2);

    cv::Mat fa_sq, fb_sq, fa_fb;
    cv::multiply(fa, fa, fa_sq);
    cv::multiply(fb, fb, fb_sq);
    cv::multiply(fa, fb, fa_fb);

    cv::Mat sigma1_sq, sigma2_sq, sigma12;
    cv::GaussianBlur(fa_sq, sigma1_sq, ksize, 1.5);
    cv::GaussianBlur(fb_sq, sigma2_sq, ksize, 1.5);
    cv::GaussianBlur(fa_fb, sigma12, ksize, 1.5);
    sigma1_sq -= mu1_sq;
    sigma2_sq -= mu2_sq;
    sigma12 -= mu1_mu2;

    cv::Mat ssim_map;
    ssim_map = ((2.0 * mu1_mu2 + C1) * (2.0 * sigma12 + C2)) /
               ((mu1_sq + mu2_sq + C1) * (sigma1_sq + sigma2_sq + C2));

    return cv::mean(ssim_map)[0];
}

} // namespace

// ---------------------------------------------------------------------------
// FrameSource
// ---------------------------------------------------------------------------

bool QualityAnalyzer::FrameSource::Open(const std::string& path, std::string& err) {
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) != 0) {
        err = "无法打开文件: " + path;
        return false;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        err = "无法获取流信息: " + path;
        avformat_close_input(&fmt);
        return false;
    }
    const int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vs < 0) {
        err = "未找到视频流: " + path;
        avformat_close_input(&fmt);
        return false;
    }
    AVStream* st = fmt->streams[vs];
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        err = "找不到解码器 (codec_id=" + std::to_string(st->codecpar->codec_id) + ")";
        avformat_close_input(&fmt);
        return false;
    }
    AVCodecContext* ctx = avcodec_alloc_context3(dec);
    if (!ctx) {
        err = "avcodec_alloc_context3 失败";
        avformat_close_input(&fmt);
        return false;
    }
    if (avcodec_parameters_to_context(ctx, st->codecpar) < 0) {
        err = "avcodec_parameters_to_context 失败";
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return false;
    }
    // 强制软件解码, 避免硬件设备依赖。
    ctx->hw_device_ctx = nullptr;
    if (avcodec_open2(ctx, dec, nullptr) < 0) {
        err = "avcodec_open2 失败";
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return false;
    }

    fmt_ctx = fmt;
    codec_ctx = ctx;
    video_stream_idx = vs;
    width = ctx->width;
    height = ctx->height;
    time_base_num = st->time_base.num;
    time_base_den = st->time_base.den != 0 ? st->time_base.den : 1.0;

    sws_ctx = sws_getContext(width, height, ctx->pix_fmt,
                             width, height, AV_PIX_FMT_BGRA,
                             SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_ctx) {
        err = "sws_getContext 失败";
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        fmt_ctx = nullptr;
        return false;
    }
    pkt = av_packet_alloc();
    frame = av_frame_alloc();
    return true;
}

bool QualityAnalyzer::FrameSource::ReadNext(cv::Mat& out, double& ts) {
    if (finished_) return false;

    auto* fmt = static_cast<AVFormatContext*>(fmt_ctx);
    auto* ctx = static_cast<AVCodecContext*>(codec_ctx);
    auto* sws = static_cast<SwsContext*>(sws_ctx);
    auto* st = fmt->streams[video_stream_idx];

    while (true) {
        int ret = draining_ ? 0 : av_read_frame(fmt, static_cast<AVPacket*>(pkt));
        if (ret < 0) {
            if (!draining_) {
                draining_ = true;
                // 送入空包以冲刷解码器余帧
                avcodec_send_packet(ctx, nullptr);
            } else {
                finished_ = true;
                return false;
            }
        } else {
            auto* packet = static_cast<AVPacket*>(pkt);
            if (packet->stream_index != video_stream_idx) {
                av_packet_unref(packet);
                continue;
            }
            if (avcodec_send_packet(ctx, packet) < 0) {
                av_packet_unref(packet);
                continue;
            }
            av_packet_unref(packet);
        }

        // 取出解码帧
        while (true) {
            int r = avcodec_receive_frame(ctx, static_cast<AVFrame*>(frame));
            if (r == AVERROR(EAGAIN)) {
                break;
            }
            if (r == AVERROR_EOF) {
                finished_ = true;
                return false;
            }
            if (r < 0) {
                finished_ = true;
                return false;
            }
            auto* f = static_cast<AVFrame*>(frame);
            cv::Mat bgra(height, width, CV_8UC4);
            uint8_t* dst[4] = { bgra.data, nullptr, nullptr, nullptr };
            int dst_linesize[4] = { static_cast<int>(bgra.step), 0, 0, 0 };
            sws_scale(sws, f->data, f->linesize, 0, height, dst, dst_linesize);
            out = bgra;
            if (f->pts != AV_NOPTS_VALUE) {
                ts = static_cast<double>(f->pts) * time_base_num / time_base_den;
            } else {
                ts = 0.0;
            }
            return true;
        }
    }
}

void QualityAnalyzer::FrameSource::Close() {
    if (sws_ctx) { sws_freeContext(static_cast<SwsContext*>(sws_ctx)); sws_ctx = nullptr; }
    if (frame) { av_frame_free(reinterpret_cast<AVFrame**>(&frame)); frame = nullptr; }
    if (pkt) { av_packet_free(reinterpret_cast<AVPacket**>(&pkt)); pkt = nullptr; }
    if (codec_ctx) { avcodec_free_context(reinterpret_cast<AVCodecContext**>(&codec_ctx)); codec_ctx = nullptr; }
    if (fmt_ctx) { avformat_close_input(reinterpret_cast<AVFormatContext**>(&fmt_ctx)); fmt_ctx = nullptr; }
    video_stream_idx = -1;
    finished_ = true;
}

// ---------------------------------------------------------------------------
// QualityAnalyzer::Run
// ---------------------------------------------------------------------------

QualitySummary QualityAnalyzer::Run(const std::string& main_path,
                                    const std::string& ref_path,
                                    const ProgressCallback& on_progress,
                                    const ResultCallback& on_frame,
                                    const SummaryCallback& on_summary) {
    QualitySummary summary;
    cancel_ = false;

    FrameSource main;
    if (!main.Open(main_path, summary.error)) {
        summary.ok = false;
        if (on_summary) on_summary(summary);
        return summary;
    }

    FrameSource ref;
    if (!ref.Open(ref_path, summary.error)) {
        summary.ok = false;
        main.Close();
        if (on_summary) on_summary(summary);
        return summary;
    }

    const int W = main.width;
    const int H = main.height;

    cv::Mat m, r;
    double mts = 0.0, rts = 0.0;
    int idx = 0;
    double sum_psnr = 0.0, sum_ssim = 0.0;
    double min_psnr = 1e9, min_ssim = 1e9;
    int min_psnr_frame = -1, min_ssim_frame = -1;

    while (!cancel_ && main.ReadNext(m, mts)) {
        if (!ref.ReadNext(r, rts)) {
            summary.error = "参考视频帧数少于主视频, 比较提前结束。";
            break;
        }
        cv::Mat r2 = r;
        if (align_resolution && (r.cols != W || r.rows != H)) {
            cv::resize(r, r2, cv::Size(W, H), 0, 0, cv::INTER_AREA);
        }

        const double psnr = ComputePsnrGray(m, r2);
        const double ssim = ComputeSsimGray(m, r2);

        QualityFrameResult fr;
        fr.frame_index = idx;
        fr.timestamp = mts;
        fr.psnr = psnr;
        fr.ssim = ssim;

        sum_psnr += psnr;
        sum_ssim += ssim;
        if (psnr < min_psnr) { min_psnr = psnr; min_psnr_frame = idx; }
        if (ssim < min_ssim) { min_ssim = ssim; min_ssim_frame = idx; }

        if (on_frame) on_frame(fr);
        if (on_progress) on_progress(idx + 1, -1);
        ++idx;
    }

    main.Close();
    ref.Close();

    summary.compared_frames = idx;
    if (idx > 0) {
        summary.mean_psnr = sum_psnr / idx;
        summary.mean_ssim = sum_ssim / idx;
        summary.min_psnr = min_psnr;
        summary.min_psnr_frame = min_psnr_frame;
        summary.min_ssim = min_ssim;
        summary.min_ssim_frame = min_ssim_frame;
        summary.ok = (idx > 0);
    } else {
        summary.ok = false;
        if (summary.error.empty()) summary.error = "未比较到任何帧 (两路视频均无有效帧或已被取消)。";
    }
    if (cancel_) summary.error = "评估已被用户取消。";

    if (on_summary) on_summary(summary);
    return summary;
}

} // namespace analyzer
} // namespace videoeye
