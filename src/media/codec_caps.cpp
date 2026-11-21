#include "media/codec_caps.h"

#include <QJsonArray>
#include <QStringList>

#include <algorithm>

namespace p2p::media {

QString codecName(Codec c)
{
    switch (c) {
    case Codec::HEVC: return QStringLiteral("hevc");
    case Codec::AV1: return QStringLiteral("av1");
    }
    return {};
}

std::optional<Codec> codecFromName(const QString& name)
{
    if (name == QLatin1String("hevc")) return Codec::HEVC;
    if (name == QLatin1String("av1")) return Codec::AV1;
    return std::nullopt;
}

QJsonObject CodecCap::toJson() const
{
    return {{"codec", codecName(codec)}, {"hw", hw}, {"maxW", maxWidth}, {"maxH", maxHeight}};
}

std::optional<CodecCap> CodecCap::fromJson(const QJsonObject& o)
{
    auto c = codecFromName(o.value("codec").toString());
    if (!c) return std::nullopt; // unknown codec from a newer peer: ignore
    CodecCap cap;
    cap.codec = *c;
    cap.hw = o.value("hw").toBool();
    cap.maxWidth = std::clamp(o.value("maxW").toInt(), 0, 16384);
    cap.maxHeight = std::clamp(o.value("maxH").toInt(), 0, 16384);
    return cap;
}

namespace {
QJsonArray toArray(const QList<CodecCap>& caps)
{
    QJsonArray a;
    for (const auto& c : caps) a.append(c.toJson());
    return a;
}
QList<CodecCap> fromArray(const QJsonArray& a)
{
    QList<CodecCap> out;
    for (const auto& v : a)
        if (auto c = CodecCap::fromJson(v.toObject())) out.append(*c);
    return out;
}
QString listSummary(const QList<CodecCap>& caps)
{
    QStringList parts;
    for (const auto& c : caps)
        parts << QStringLiteral("%1(%2)").arg(codecName(c.codec).toUpper(), c.hw ? "hw" : "sw");
    return parts.isEmpty() ? QStringLiteral("none") : parts.join(' ');
}
} // namespace

QJsonObject MediaCaps::toJson() const
{
    return {{"enc", toArray(encoders)}, {"dec", toArray(decoders)}};
}

MediaCaps MediaCaps::fromJson(const QJsonObject& o)
{
    return {fromArray(o.value("enc").toArray()), fromArray(o.value("dec").toArray())};
}

QString MediaCaps::summary() const
{
    return QStringLiteral("enc: %1 | dec: %2").arg(listSummary(encoders), listSummary(decoders));
}

bool MediaCaps::canDecode(Codec c) const
{
    return std::any_of(decoders.begin(), decoders.end(), [c](const CodecCap& d) { return d.codec == c; });
}

QJsonObject StreamSelection::toJson() const
{
    return {{"codec", codecName(codec)}, {"encHw", encoderHw}, {"decHw", decoderHw},
            {"maxW", maxWidth}, {"maxH", maxHeight}};
}

std::optional<StreamSelection> StreamSelection::fromJson(const QJsonObject& o)
{
    auto c = codecFromName(o.value("codec").toString());
    if (!c) return std::nullopt;
    StreamSelection s;
    s.codec = *c;
    s.encoderHw = o.value("encHw").toBool();
    s.decoderHw = o.value("decHw").toBool();
    s.maxWidth = std::clamp(o.value("maxW").toInt(), 0, 16384);
    s.maxHeight = std::clamp(o.value("maxH").toInt(), 0, 16384);
    return s;
}

QString StreamSelection::summary() const
{
    return QStringLiteral("%1 (enc %2, dec %3, max %4x%5)")
        .arg(codecName(codec).toUpper(), encoderHw ? "hw" : "sw", decoderHw ? "hw" : "sw")
        .arg(maxWidth)
        .arg(maxHeight);
}

std::optional<StreamSelection> negotiate(const QList<CodecCap>& senderEncoders,
                                         const QList<CodecCap>& receiverDecoders,
                                         const NegotiationPolicy& policy)
{
    // Hardware encode matters most (latency + CPU), then hardware decode.
    // AV1 gets a small bonus so AV1-hw/hw beats HEVC-hw/hw, but a hardware
    // HEVC path beats software AV1 unless the policy asks for AV1.
    auto score = [&](const CodecCap& e, const CodecCap& d) {
        int s = (e.hw ? 100 : 0) + (d.hw ? 40 : 0);
        if (e.codec == Codec::AV1) s += policy.preferAv1 ? 200 : 10;
        return s;
    };

    std::optional<StreamSelection> best;
    int bestScore = -1;
    for (const auto& e : senderEncoders) {
        for (const auto& d : receiverDecoders) {
            if (e.codec != d.codec) continue;
            const int s = score(e, d);
            if (s <= bestScore) continue; // first wins on ties: deterministic
            bestScore = s;
            StreamSelection sel;
            sel.codec = e.codec;
            sel.encoderHw = e.hw;
            sel.decoderHw = d.hw;
            auto minNonZero = [](int a, int b) { return a == 0 ? b : (b == 0 ? a : std::min(a, b)); };
            sel.maxWidth = minNonZero(e.maxWidth, d.maxWidth);
            sel.maxHeight = minNonZero(e.maxHeight, d.maxHeight);
            best = sel;
        }
    }
    return best;
}

} // namespace p2p::media
