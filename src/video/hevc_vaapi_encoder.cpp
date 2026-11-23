#include "video/hevc_vaapi_encoder.h"

#include <va/va.h>
#include <va/va_enc_hevc.h>

#include <QLoggingCategory>

#include <algorithm>
#include <cstring>
#include <deque>
#include <optional>
#include <vector>

Q_LOGGING_CATEGORY(lcNative, "p2p.encoder.native")

namespace p2p::video {

namespace {
constexpr int kCtb = 64;
constexpr int kMaxRefBack = 255;      // FragmentHeader::refBack is a byte
constexpr size_t kParentHistory = 1024;
} // namespace

struct HevcVaapiEncoder::Impl {
    VADisplay dpy = nullptr;
    VAConfigID config = VA_INVALID_ID;
    VAContextID context = VA_INVALID_ID;
    VABufferID coded = VA_INVALID_ID;
    std::vector<VASurfaceID> recon; // reconstructed pictures (the encoder's DPB)
    Config cfg;
    hevc::StreamParams sp;
    bool rcDirty = true;

    struct Pic {
        uint32_t id = 0;
        int poc = 0;
        int slot = 0; // index into recon
    };
    std::vector<Pic> dpb;          // pictures still referenceable: the last frame + anchors
    std::vector<uint32_t> anchors; // ids, oldest first (all in dpb)
    int anchorInterval = 8;
    int poc = 0;
    uint32_t lastId = 0;
    int lastRefBack = 0;
    // Recovery requested: predict the next frame from |recoverRef|, keep
    // only the frames in its chain.
    std::optional<uint32_t> recoverRef;
    std::vector<uint32_t> recoverKeep;
    // What each recent frame predicted from (nullopt: keyframe), to walk chains.
    std::deque<std::pair<uint32_t, std::optional<uint32_t>>> parents;

    const Pic* find(uint32_t id) const
    {
        for (const Pic& p : dpb)
            if (p.id == id) return &p;
        return nullptr;
    }
    std::optional<std::optional<uint32_t>> parentOf(uint32_t id) const
    {
        for (auto it = parents.rbegin(); it != parents.rend(); ++it)
            if (it->first == id) return it->second;
        return std::nullopt;
    }

    ~Impl()
    {
        if (!dpy) return;
        if (coded != VA_INVALID_ID) vaDestroyBuffer(dpy, coded);
        if (context != VA_INVALID_ID) vaDestroyContext(dpy, context);
        if (!recon.empty()) vaDestroySurfaces(dpy, recon.data(), int(recon.size()));
        if (config != VA_INVALID_ID) vaDestroyConfig(dpy, config);
    }

    VABufferID buffer(VABufferType type, size_t size, const void* data)
    {
        VABufferID id = VA_INVALID_ID;
        if (vaCreateBuffer(dpy, context, type, unsigned(size), 1, const_cast<void*>(data), &id) != VA_STATUS_SUCCESS)
            return VA_INVALID_ID;
        return id;
    }
    void packed(std::vector<VABufferID>& bufs, uint32_t type, const std::vector<uint8_t>& nal)
    {
        VAEncPackedHeaderParameterBuffer p = {};
        p.type = type;
        p.bit_length = uint32_t(nal.size() * 8);
        p.has_emulation_bytes = 1; // one NAL per buffer, already escaped
        bufs.push_back(buffer(VAEncPackedHeaderParameterBufferType, sizeof(p), &p));
        bufs.push_back(buffer(VAEncPackedHeaderDataBufferType, nal.size(), nal.data()));
    }
    template <class T>
    void misc(std::vector<VABufferID>& bufs, VAEncMiscParameterType type, const T& v)
    {
        std::vector<uint8_t> m(sizeof(VAEncMiscParameterBuffer) + sizeof(T));
        reinterpret_cast<VAEncMiscParameterBuffer*>(m.data())->type = type;
        std::memcpy(m.data() + sizeof(VAEncMiscParameterBuffer), &v, sizeof(T));
        bufs.push_back(buffer(VAEncMiscParameterBufferType, m.size(), m.data()));
    }

    void sequence(std::vector<VABufferID>& bufs)
    {
        VAEncSequenceParameterBufferHEVC s = {};
        s.general_profile_idc = 1;
        s.general_level_idc = uint8_t(sp.level());
        s.intra_period = 0;
        s.intra_idr_period = 0;
        s.ip_period = 1;
        s.bits_per_second = uint32_t(cfg.bitrateKbps) * 1000;
        s.pic_width_in_luma_samples = uint16_t(sp.codedWidth());
        s.pic_height_in_luma_samples = uint16_t(sp.codedHeight());
        s.seq_fields.bits.chroma_format_idc = 1;
        s.seq_fields.bits.amp_enabled_flag = 1;
        s.seq_fields.bits.sample_adaptive_offset_enabled_flag = 1;
        s.seq_fields.bits.low_delay_seq = 1;
        s.log2_min_luma_coding_block_size_minus3 = 0;
        s.log2_diff_max_min_luma_coding_block_size = 3;
        s.log2_min_transform_block_size_minus2 = 0;
        s.log2_diff_max_min_transform_block_size = 3;
        s.max_transform_hierarchy_depth_inter = 4;
        s.max_transform_hierarchy_depth_intra = 4;
        s.vui_parameters_present_flag = 1;
        s.vui_fields.bits.aspect_ratio_info_present_flag = 1;
        s.vui_fields.bits.vui_timing_info_present_flag = 1;
        s.aspect_ratio_idc = 1;
        s.vui_num_units_in_tick = 1;
        s.vui_time_scale = uint32_t(cfg.fps);
        bufs.push_back(buffer(VAEncSequenceParameterBufferType, sizeof(s), &s));
    }

    void rateControl(std::vector<VABufferID>& bufs)
    {
        const uint32_t bps = uint32_t(cfg.bitrateKbps) * 1000;
        const uint32_t vbv = bps / uint32_t(cfg.fps); // ~1 frame
        VAEncMiscParameterRateControl rc = {};
        rc.bits_per_second = bps;
        rc.target_percentage = 100;
        rc.window_size = uint32_t(uint64_t(vbv) * 1000 / bps);
        misc(bufs, VAEncMiscParameterTypeRateControl, rc);
        VAEncMiscParameterHRD hrd = {};
        hrd.buffer_size = vbv;
        hrd.initial_buffer_fullness = vbv * 3 / 4;
        misc(bufs, VAEncMiscParameterTypeHRD, hrd);
        VAEncMiscParameterFrameRate fr = {};
        fr.framerate = uint32_t(cfg.fps);
        misc(bufs, VAEncMiscParameterTypeFrameRate, fr);
    }
};

HevcVaapiEncoder::HevcVaapiEncoder() : d(std::make_unique<Impl>()) {}
HevcVaapiEncoder::~HevcVaapiEncoder() = default;

bool HevcVaapiEncoder::init(void* display, const Config& cfg, QString* why)
{
    auto fail = [&](const QString& what, VAStatus s = VA_STATUS_SUCCESS) {
        if (why) *why = s != VA_STATUS_SUCCESS ? QStringLiteral("%1: %2").arg(what, QString::fromLatin1(vaErrorStr(s))) : what;
        return false;
    };
    d->dpy = static_cast<VADisplay>(display);
    d->cfg = cfg;
    d->sp.width = cfg.width;
    d->sp.height = cfg.height;
    d->sp.fps = cfg.fps;
    d->sp.colour = cfg.colour;
    // Mesa (radeonsi) codes HEVC at CTB-aligned width, 16-aligned height and
    // crops by our conformance window. Other drivers: the same is valid HEVC.
    d->sp.alignW = kCtb;
    d->sp.alignH = 16;
    if (cfg.width % 2 || cfg.height % 2) return fail(QStringLiteral("odd picture size"));

    VAConfigAttrib want[3] = {{VAConfigAttribRTFormat, 0}, {VAConfigAttribEncPackedHeaders, 0}, {VAConfigAttribRateControl, 0}};
    VAStatus s = vaGetConfigAttributes(d->dpy, VAProfileHEVCMain, VAEntrypointEncSlice, want, 3);
    if (s != VA_STATUS_SUCCESS) return fail(QStringLiteral("no HEVC encode entrypoint"), s);
    const uint32_t packed = VA_ENC_PACKED_HEADER_SEQUENCE | VA_ENC_PACKED_HEADER_PICTURE | VA_ENC_PACKED_HEADER_SLICE;
    if (!(want[0].value & VA_RT_FORMAT_YUV420)) return fail(QStringLiteral("no 4:2:0 encode"));
    if (want[1].value == VA_ATTRIB_NOT_SUPPORTED || (want[1].value & packed) != packed)
        return fail(QStringLiteral("driver takes no packed headers"));
    if (want[2].value == VA_ATTRIB_NOT_SUPPORTED || !(want[2].value & VA_RC_CBR)) return fail(QStringLiteral("no CBR"));
    VAConfigAttrib attrs[3] = {{VAConfigAttribRTFormat, VA_RT_FORMAT_YUV420},
                               {VAConfigAttribEncPackedHeaders, packed},
                               {VAConfigAttribRateControl, VA_RC_CBR}};
    if ((s = vaCreateConfig(d->dpy, VAProfileHEVCMain, VAEntrypointEncSlice, attrs, 3, &d->config)) != VA_STATUS_SUCCESS)
        return fail(QStringLiteral("vaCreateConfig"), s);

    const int cw = d->sp.codedWidth(), ch = d->sp.codedHeight();
    d->recon.resize(size_t(d->sp.maxDpb) + 1);
    if ((s = vaCreateSurfaces(d->dpy, VA_RT_FORMAT_YUV420, unsigned(cw), unsigned(ch), d->recon.data(),
                              unsigned(d->recon.size()), nullptr, 0)) != VA_STATUS_SUCCESS) {
        d->recon.clear();
        return fail(QStringLiteral("reconstructed surfaces"), s);
    }
    if ((s = vaCreateContext(d->dpy, d->config, cw, ch, VA_PROGRESSIVE, d->recon.data(), int(d->recon.size()),
                             &d->context)) != VA_STATUS_SUCCESS)
        return fail(QStringLiteral("vaCreateContext"), s);
    // Room for a keyframe well over budget; CBR keeps frames near bps / fps.
    const unsigned codedSize = unsigned(std::max<int64_t>(int64_t(cw) * ch * 3 / 2, int64_t(cfg.bitrateKbps) * 1000 / 8));
    if ((s = vaCreateBuffer(d->dpy, d->context, VAEncCodedBufferType, codedSize, 1, nullptr, &d->coded)) !=
        VA_STATUS_SUCCESS)
        return fail(QStringLiteral("coded buffer"), s);
    return true;
}

void HevcVaapiEncoder::setBitrateKbps(int kbps)
{
    if (kbps <= 0 || kbps == d->cfg.bitrateKbps) return;
    d->cfg.bitrateKbps = kbps;
    d->rcDirty = true; // rate control (and the 1-frame VBV) resent with the next frame
}

int HevcVaapiEncoder::lastRefBack() const { return d->lastRefBack; }

void HevcVaapiEncoder::setAnchorInterval(int frames) { d->anchorInterval = std::max(1, frames); }

bool HevcVaapiEncoder::recoverFrom(uint32_t goodId)
{
    d->recoverRef.reset();
    d->recoverKeep.clear();
    if (d->dpb.empty() || int32_t(d->lastId - goodId) < 0) return false;
    // Walk back along goodId's chain: every frame on it was decoded. The
    // newest one still kept is the reference; the other kept ones stay.
    std::optional<uint32_t> id = goodId;
    for (size_t steps = 0; id && steps < kParentHistory; ++steps) {
        if (d->find(*id)) {
            if (!d->recoverRef) d->recoverRef = *id;
            d->recoverKeep.push_back(*id);
        }
        const auto parent = d->parentOf(*id);
        if (!parent) break; // older than we remember
        id = *parent;       // nullopt at a keyframe: the chain starts there
    }
    if (d->recoverRef && int32_t(d->lastId + 1 - *d->recoverRef) > kMaxRefBack) d->recoverRef.reset();
    return d->recoverRef.has_value();
}

bool HevcVaapiEncoder::encode(unsigned surface, uint32_t id, bool forceKeyframe, const Output& out)
{
    if (d->context == VA_INVALID_ID) return false;
    // Predict from the previous frame and keep the anchors; after a loss,
    // from the recovery reference, keeping only its chain.
    std::vector<Impl::Pic> keep;
    std::optional<Impl::Pic> refPic;
    if (!forceKeyframe && d->recoverRef) {
        for (const auto& p : d->dpb)
            if (std::find(d->recoverKeep.begin(), d->recoverKeep.end(), p.id) != d->recoverKeep.end()) keep.push_back(p);
        if (auto* p = d->find(*d->recoverRef)) refPic = *p;
    } else if (!forceKeyframe) {
        keep = d->dpb;
        if (auto* p = d->find(d->lastId)) refPic = *p;
    }
    d->recoverRef.reset();
    d->recoverKeep.clear();
    const bool idr = !refPic || int32_t(id - refPic->id) <= 0 || int32_t(id - refPic->id) > kMaxRefBack;
    if (idr) {
        keep.clear();
        refPic.reset();
        d->anchors.clear();
        d->poc = 0;
    } else {
        ++d->poc;
    }
    const int poc = d->poc;
    const Impl::Pic* ref = refPic ? &*refPic : nullptr;
    d->lastRefBack = ref ? int(id - ref->id) : 0;
    int slot = 0;
    while (std::any_of(keep.begin(), keep.end(), [&](const Impl::Pic& p) { return p.slot == slot; })) ++slot;

    std::vector<VABufferID> bufs;
    if (idr) d->sequence(bufs);
    if (idr || d->rcDirty) {
        d->rateControl(bufs);
        d->rcDirty = false;
    }

    VAEncPictureParameterBufferHEVC pp = {};
    pp.decoded_curr_pic.picture_id = d->recon[size_t(slot)];
    pp.decoded_curr_pic.pic_order_cnt = poc;
    for (auto& r : pp.reference_frames) {
        r.picture_id = VA_INVALID_SURFACE;
        r.flags = VA_PICTURE_HEVC_INVALID;
    }
    for (size_t i = 0; i < keep.size(); ++i) {
        pp.reference_frames[i].picture_id = d->recon[size_t(keep[i].slot)];
        pp.reference_frames[i].pic_order_cnt = keep[i].poc;
        pp.reference_frames[i].flags = keep[i].poc == ref->poc ? VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE : 0;
    }
    pp.coded_buf = d->coded;
    pp.collocated_ref_pic_index = 0xff;
    pp.pic_init_qp = 26;
    pp.nal_unit_type = idr ? 19 : 1;
    pp.pic_fields.bits.idr_pic_flag = idr;
    pp.pic_fields.bits.coding_type = idr ? 1 : 2;
    pp.pic_fields.bits.reference_pic_flag = 1;
    pp.pic_fields.bits.constrained_intra_pred_flag = d->sp.constrainedIntraPred;
    pp.pic_fields.bits.cu_qp_delta_enabled_flag = 1;
    pp.pic_fields.bits.pps_loop_filter_across_slices_enabled_flag = 1;
    bufs.push_back(d->buffer(VAEncPictureParameterBufferType, sizeof(pp), &pp));

    if (idr) {
        d->packed(bufs, VAEncPackedHeaderSequence, hevc::vps(d->sp));
        d->packed(bufs, VAEncPackedHeaderSequence, hevc::sps(d->sp));
        d->packed(bufs, VAEncPackedHeaderPicture, hevc::pps(d->sp));
    }
    std::vector<hevc::RefPic> refs;
    for (const auto& p : keep) refs.push_back({p.poc, ref && p.poc == ref->poc});
    d->packed(bufs, VAEncPackedHeaderSlice, hevc::sliceHeader(d->sp, idr, poc, refs));

    VAEncSliceParameterBufferHEVC sl = {};
    sl.slice_segment_address = 0;
    sl.num_ctu_in_slice = uint32_t(((d->sp.codedWidth() + kCtb - 1) / kCtb) * ((d->sp.codedHeight() + kCtb - 1) / kCtb));
    sl.slice_type = idr ? 2 : 1;
    for (int i = 0; i < 15; ++i) {
        sl.ref_pic_list0[i].picture_id = VA_INVALID_SURFACE;
        sl.ref_pic_list0[i].flags = VA_PICTURE_HEVC_INVALID;
        sl.ref_pic_list1[i] = sl.ref_pic_list0[i];
    }
    if (ref) {
        sl.ref_pic_list0[0].picture_id = d->recon[size_t(ref->slot)];
        sl.ref_pic_list0[0].pic_order_cnt = ref->poc;
        sl.ref_pic_list0[0].flags = 0;
    }
    sl.max_num_merge_cand = 5;
    sl.slice_fields.bits.last_slice_of_pic_flag = 1;
    sl.slice_fields.bits.slice_sao_luma_flag = 1;
    sl.slice_fields.bits.slice_sao_chroma_flag = 1;
    sl.slice_fields.bits.slice_loop_filter_across_slices_enabled_flag = 1;
    bufs.push_back(d->buffer(VAEncSliceParameterBufferType, sizeof(sl), &sl));

    auto destroyAll = [&] {
        for (VABufferID b : bufs)
            if (b != VA_INVALID_ID) vaDestroyBuffer(d->dpy, b);
    };
    if (std::find(bufs.begin(), bufs.end(), VA_INVALID_ID) != bufs.end()) {
        destroyAll();
        qCWarning(lcNative) << "parameter buffer allocation failed";
        return false;
    }
    VAStatus s = vaBeginPicture(d->dpy, d->context, VASurfaceID(surface));
    if (s == VA_STATUS_SUCCESS) s = vaRenderPicture(d->dpy, d->context, bufs.data(), int(bufs.size()));
    if (s == VA_STATUS_SUCCESS) s = vaEndPicture(d->dpy, d->context);
    if (s == VA_STATUS_SUCCESS) s = vaSyncSurface(d->dpy, VASurfaceID(surface));
    destroyAll();
    if (s != VA_STATUS_SUCCESS) {
        qCWarning(lcNative) << "encode failed:" << vaErrorStr(s);
        d->dpb.clear(); // next frame starts over with an IDR
        return false;
    }

    VACodedBufferSegment* seg = nullptr;
    if ((s = vaMapBuffer(d->dpy, d->coded, reinterpret_cast<void**>(&seg))) != VA_STATUS_SUCCESS) {
        qCWarning(lcNative) << "coded buffer map failed:" << vaErrorStr(s);
        d->dpb.clear();
        return false;
    }
    bool overflow = false;
    if (seg && !seg->next) {
        overflow = seg->status & VA_CODED_BUF_STATUS_SLICE_OVERFLOW_MASK;
        out(static_cast<const uint8_t*>(seg->buf), seg->size, idr);
    } else {
        std::vector<uint8_t> au;
        for (; seg; seg = static_cast<VACodedBufferSegment*>(seg->next)) {
            overflow |= (seg->status & VA_CODED_BUF_STATUS_SLICE_OVERFLOW_MASK) != 0;
            au.insert(au.end(), static_cast<const uint8_t*>(seg->buf), static_cast<const uint8_t*>(seg->buf) + seg->size);
        }
        out(au.data(), au.size(), idr);
    }
    vaUnmapBuffer(d->dpy, d->coded);
    if (overflow) qCWarning(lcNative) << "coded buffer overflow";

    // The new frame becomes an anchor every anchorInterval frames; at most
    // maxDpb - 2 anchors besides it (the DPB holds maxDpb - 1 references).
    auto& anchors = d->anchors;
    anchors.erase(std::remove_if(anchors.begin(), anchors.end(),
                                 [&](uint32_t a) {
                                     return std::none_of(keep.begin(), keep.end(),
                                                         [&](const Impl::Pic& p) { return p.id == a; });
                                 }),
                  anchors.end());
    if (anchors.empty() || int32_t(id - anchors.back()) >= d->anchorInterval) anchors.push_back(id);
    while (int(anchors.size()) > d->sp.maxDpb - 2) anchors.erase(anchors.begin());
    std::vector<Impl::Pic> next;
    for (const auto& p : keep)
        if (std::find(anchors.begin(), anchors.end(), p.id) != anchors.end()) next.push_back(p);
    next.push_back({id, poc, slot});
    d->dpb = std::move(next);
    d->parents.emplace_back(id, ref ? std::optional<uint32_t>(ref->id) : std::nullopt);
    while (d->parents.size() > kParentHistory) d->parents.pop_front();
    d->lastId = id;
    return true;
}

} // namespace p2p::video
