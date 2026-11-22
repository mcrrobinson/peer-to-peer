#include "audio/opus_codec.h"

#include <opus.h>
#if __has_include(<p2p_dred_rev.h>)
#include <p2p_dred_rev.h> // written by tools/build-opus-dred.sh
#endif

#include <QLoggingCategory>

#include <cstring>

Q_LOGGING_CATEGORY(lcOpus, "p2p.opus")

namespace p2p::audio {

namespace {
int builtDredRevision()
{
#ifdef P2P_OPUS_DRED_REV
    return P2P_OPUS_DRED_REV;
#else
    // libopus without the header from our build script: known releases.
    const char* v = opus_get_version_string(); // "libopus 1.6.1"
    if (std::strncmp(v, "libopus 1.5", 11) == 0) return 10;
    if (std::strncmp(v, "libopus 1.6", 11) == 0) return 12;
    return 0;
#endif
}
} // namespace

int dredRevision()
{
    static const int rev = [] {
#ifdef OPUS_SET_DRED_DURATION_REQUEST
        // The DRED API is always declared; only a --enable-dred build implements it.
        int err = 0;
        OpusEncoder* e = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &err);
        if (!e) return 0;
        const bool enc = opus_encoder_ctl(e, OPUS_SET_DRED_DURATION(10)) == OPUS_OK;
        opus_encoder_destroy(e);
        OpusDREDDecoder* d = opus_dred_decoder_create(&err);
        if (d) opus_dred_decoder_destroy(d);
        return enc && d && err == OPUS_OK ? builtDredRevision() : 0;
#else
        return 0;
#endif
    }();
    return rev;
}

OpusEncoderWrapper::~OpusEncoderWrapper()
{
    if (enc_) opus_encoder_destroy(enc_);
}

bool OpusEncoderWrapper::init(const Config& cfg)
{
    int err = 0;
    enc_ = opus_encoder_create(cfg.sampleRate, cfg.channels,
                               cfg.restrictedLowDelay ? OPUS_APPLICATION_RESTRICTED_LOWDELAY : OPUS_APPLICATION_VOIP,
                               &err);
    if (err != OPUS_OK || !enc_) {
        qCWarning(lcOpus) << "opus_encoder_create failed:" << opus_strerror(err);
        return false;
    }
    opus_encoder_ctl(enc_, OPUS_SET_BITRATE(cfg.bitrate));
    opus_encoder_ctl(enc_, OPUS_SET_COMPLEXITY(9));
    opus_encoder_ctl(enc_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(enc_, OPUS_SET_VBR(1));
    opus_encoder_ctl(enc_, OPUS_SET_VBR_CONSTRAINT(1));
    // LBRR needs SILK, so restricted-low-delay (CELT only) has none.
    lbrrEnabled_ = cfg.protection == LossProtection::Lbrr && !cfg.restrictedLowDelay;
    opus_encoder_ctl(enc_, OPUS_SET_INBAND_FEC(lbrrEnabled_ ? 1 : 0));
    opus_encoder_ctl(enc_, OPUS_SET_PACKET_LOSS_PERC(cfg.expectedLossPct));
    opus_encoder_ctl(enc_, OPUS_SET_LSB_DEPTH(16));
    opus_encoder_ctl(enc_, OPUS_GET_LOOKAHEAD(&lookahead_));
#ifdef OPUS_SET_DRED_DURATION_REQUEST
    if (cfg.protection == LossProtection::Dred && cfg.dredFrames > 0) {
        dredEnabled_ = opus_encoder_ctl(enc_, OPUS_SET_DRED_DURATION(cfg.dredFrames)) == OPUS_OK;
        if (!dredEnabled_) qCInfo(lcOpus) << "DRED not available in this libopus build";
    }
#endif
    qCInfo(lcOpus) << "encoder:" << opus_get_version_string() << cfg.bitrate / 1000 << "kbps"
                   << (cfg.restrictedLowDelay ? "restricted-lowdelay" : "voip")
                   << "lookahead" << lookahead_ << "samples, LBRR" << lbrrEnabled_ << "DRED" << dredEnabled_;
    return true;
}

int OpusEncoderWrapper::encode(const int16_t* pcm, int frameSamples, uint8_t* out, int maxBytes)
{
    return opus_encode(enc_, pcm, frameSamples, out, maxBytes);
}

void OpusEncoderWrapper::setBitrate(int bps)
{
    opus_encoder_ctl(enc_, OPUS_SET_BITRATE(bps));
}

void OpusEncoderWrapper::setExpectedLoss(int pct)
{
    opus_encoder_ctl(enc_, OPUS_SET_PACKET_LOSS_PERC(pct));
}

OpusDecoderWrapper::~OpusDecoderWrapper()
{
    if (dred_) opus_dred_free(dred_);
    if (dredDec_) opus_dred_decoder_destroy(dredDec_);
    if (dec_) opus_decoder_destroy(dec_);
}

bool OpusDecoderWrapper::init(int sampleRate, int channels)
{
    sampleRate_ = sampleRate;
    int err = 0;
    dec_ = opus_decoder_create(sampleRate, channels, &err);
    if (err != OPUS_OK || !dec_) return false;
    // The default (0) leaves plain-PLC losses on the classic concealment.
    opus_decoder_ctl(dec_, OPUS_SET_COMPLEXITY(kComplexity));
    dredDec_ = opus_dred_decoder_create(&err);
    if (err == OPUS_OK) dred_ = opus_dred_alloc(&err);
    if (err != OPUS_OK) {
        qCInfo(lcOpus) << "DRED decoder unavailable:" << opus_strerror(err);
        if (dredDec_) { opus_dred_decoder_destroy(dredDec_); dredDec_ = nullptr; }
        dred_ = nullptr;
    }
    return true;
}

int OpusDecoderWrapper::decode(const uint8_t* data, int len, int16_t* pcm, int frameSamples)
{
    return opus_decode(dec_, data, len, pcm, frameSamples, 0);
}

int OpusDecoderWrapper::plc(int16_t* pcm, int frameSamples)
{
    return opus_decode(dec_, nullptr, 0, pcm, frameSamples, 0);
}

int OpusDecoderWrapper::conceal(const uint8_t* next, int nextLen, int distance, int16_t* pcm, int frameSamples,
                                Method* used)
{
    if (next && nextLen > 0 && distance > 0) {
        if (dredDec_ && dred_) {
            const int offset = distance * frameSamples; // start of the lost frame, before |next|
            int dredEnd = 0;
            const int avail = opus_dred_parse(dredDec_, dred_, next, nextLen, offset, sampleRate_, &dredEnd, 0);
            if (avail > 0 && offset <= avail) {
                const int n = opus_decoder_dred_decode(dec_, dred_, offset, pcm, frameSamples);
                if (n > 0) {
                    *used = Method::Dred;
                    return n;
                }
            }
        }
        if (distance == 1) {
            // LBRR copy of the previous frame inside |next| (PLC if absent).
            const int n = opus_decode(dec_, next, nextLen, pcm, frameSamples, 1);
            if (n > 0) {
                *used = Method::Fec;
                return n;
            }
        }
    }
    *used = Method::Plc;
    return plc(pcm, frameSamples);
}

} // namespace p2p::audio
