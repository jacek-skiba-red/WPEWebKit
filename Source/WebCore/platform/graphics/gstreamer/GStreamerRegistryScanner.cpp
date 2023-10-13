/*
 * Copyright (C) 2019 Igalia S.L
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * aint with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include "GStreamerRegistryScanner.h"

#if USE(GSTREAMER)
#include "ContentType.h"
#include "GStreamerCommon.h"
#include <fnmatch.h>
#include <gst/pbutils/codec-utils.h>
#include <wtf/PrintStream.h>

namespace WebCore {

GST_DEBUG_CATEGORY_STATIC(webkit_media_gst_registry_scanner_debug);
#define GST_CAT_DEFAULT webkit_media_gst_registry_scanner_debug

// We shouldn't accept media that the player can't actually play.
// AAC supports up to 96 channels.
#define MEDIA_MAX_AAC_CHANNELS 96

// Assume hardware video decoding acceleration up to 8K@60fps for the generic case. Some embedded platforms might want to tune this.
#define MEDIA_MAX_WIDTH 7680.0f
#define MEDIA_MAX_HEIGHT 4320.0f
#define MEDIA_MAX_FRAMERATE 60.0f

GStreamerRegistryScanner& GStreamerRegistryScanner::singleton()
{
    static NeverDestroyed<GStreamerRegistryScanner> sharedInstance;
    return sharedInstance;
}

GStreamerRegistryScanner::GStreamerRegistryScanner(bool isMediaSource)
    : m_isMediaSource(isMediaSource)
{
    GST_DEBUG_CATEGORY_INIT(webkit_media_gst_registry_scanner_debug, "webkitregistryscanner", 0, "WebKit GStreamer registry scanner");
#if PLATFORM(BCM_NEXUS) || PLATFORM(BROADCOM)
    m_audioDecoderFactories = gst_element_factory_list_get_elements(GST_ELEMENT_FACTORY_TYPE_PARSER | GST_ELEMENT_FACTORY_TYPE_MEDIA_AUDIO, GST_RANK_MARGINAL);
    m_videoDecoderFactories = gst_element_factory_list_get_elements(GST_ELEMENT_FACTORY_TYPE_PARSER | GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO, GST_RANK_MARGINAL);
#else
    m_audioDecoderFactories = gst_element_factory_list_get_elements(GST_ELEMENT_FACTORY_TYPE_DECODER | GST_ELEMENT_FACTORY_TYPE_MEDIA_AUDIO, GST_RANK_MARGINAL);
    m_videoDecoderFactories = gst_element_factory_list_get_elements(GST_ELEMENT_FACTORY_TYPE_DECODER | GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO, GST_RANK_MARGINAL);
#endif
    m_audioParserFactories = gst_element_factory_list_get_elements(GST_ELEMENT_FACTORY_TYPE_PARSER | GST_ELEMENT_FACTORY_TYPE_MEDIA_AUDIO, GST_RANK_NONE);
    m_videoParserFactories = gst_element_factory_list_get_elements(GST_ELEMENT_FACTORY_TYPE_PARSER | GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO, GST_RANK_MARGINAL);
    m_demuxerFactories = gst_element_factory_list_get_elements(GST_ELEMENT_FACTORY_TYPE_DEMUXER, GST_RANK_MARGINAL);

    initialize();
#ifndef GST_DISABLE_GST_DEBUG
    GST_DEBUG("%s registry scanner initialized", m_isMediaSource ? "MSE" : "Regular playback");
    for (auto& mimeType : m_mimeTypeSet)
        GST_DEBUG("Mime-type registered: %s", mimeType.utf8().data());
    for (auto& item : m_codecMap)
        GST_DEBUG("%s codec pattern registered: %s", item.value ? "Hardware" : "Software", item.key.string().utf8().data());
#endif
}

GStreamerRegistryScanner::~GStreamerRegistryScanner()
{
    gst_plugin_feature_list_free(m_audioDecoderFactories);
    gst_plugin_feature_list_free(m_audioParserFactories);
    gst_plugin_feature_list_free(m_videoDecoderFactories);
    gst_plugin_feature_list_free(m_videoParserFactories);
    gst_plugin_feature_list_free(m_demuxerFactories);
}

GStreamerRegistryScanner::RegistryLookupResult GStreamerRegistryScanner::hasElementForMediaType(GList* elementFactories, const char* capsString, bool shouldCheckHardwareClassifier, Optional<Vector<String>> blackList) const
{
    GRefPtr<GstCaps> caps = adoptGRef(gst_caps_from_string(capsString));
    if (!caps)
        return { };
    GList* candidates = gst_element_factory_list_filter(elementFactories, caps.get(), GST_PAD_SINK, false);
    bool isSupported = candidates;
    bool isUsingHardware = false;

    if (blackList.hasValue() && !blackList->isEmpty()) {
        bool hasValidCandidate = false;
        for (GList* factories = candidates; factories; factories = g_list_next(factories)) {
            String name(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE_CAST(factories->data)));
            if (blackList->contains(name))
                continue;
            hasValidCandidate = true;
            break;
        }
        if (!hasValidCandidate) {
            GST_WARNING("All elements for caps %" GST_PTR_FORMAT " are blacklisted", caps.get());
            isSupported = false;
            shouldCheckHardwareClassifier = false;
        }
    }

    if (shouldCheckHardwareClassifier) {
        for (GList* factories = candidates; factories; factories = g_list_next(factories)) {
            auto* factory = reinterpret_cast<GstElementFactory*>(factories->data);
#if PLATFORM(BCM_NEXUS) || PLATFORM(BROADCOM)
            if (g_str_has_prefix(GST_OBJECT_NAME(factory), "brcm")) {
                isUsingHardware = true;
                break;
            }
#elif PLATFORM(REALTEK)
            if (g_str_has_prefix(GST_OBJECT_NAME(factory), "omx")) {
                isUsingHardware = true;
                break;
            }
#elif USE(WESTEROS_SINK)
            if (g_str_has_prefix(GST_OBJECT_NAME(factory), "westeros")) {
                isUsingHardware = true;
                break;
            }
#endif
            String metadata = gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_KLASS);
            auto components = metadata.split('/');
            if (components.contains("Hardware")) {
                isUsingHardware = true;
                break;
            }
        }
    }

    gst_plugin_feature_list_free(candidates);
#ifndef GST_DISABLE_GST_DEBUG
    const char* elementType = "";
    if (elementFactories == m_audioParserFactories)
        elementType = "Audio parser";
    else if (elementFactories == m_audioDecoderFactories)
        elementType = "Audio decoder";
    else if (elementFactories == m_videoParserFactories)
        elementType = "Video parser";
    else if (elementFactories == m_videoDecoderFactories)
        elementType = "Video decoder";
    else if (elementFactories == m_demuxerFactories)
        elementType = "Demuxer";
    else
        ASSERT_NOT_REACHED();
    GST_LOG("%s lookup result for caps %" GST_PTR_FORMAT " : isSupported=%s, isUsingHardware=%s", elementType, caps.get(), boolForPrinting(isSupported), boolForPrinting(isUsingHardware));
#endif
    return GStreamerRegistryScanner::RegistryLookupResult { isSupported, isUsingHardware };
}

void GStreamerRegistryScanner::fillMimeTypeSetFromCapsMapping(Vector<GstCapsWebKitMapping>& mapping)
{
    for (auto& current : mapping) {
        GList* factories;
        switch (current.elementType) {
        case Demuxer:
            factories = m_demuxerFactories;
            break;
        case AudioDecoder:
            factories = m_audioDecoderFactories;
            break;
        case VideoDecoder:
            factories = m_videoDecoderFactories;
            break;
        }

        if (hasElementForMediaType(factories, current.capsString)) {
            if (!current.webkitCodecPatterns.isEmpty()) {
                for (const auto& pattern : current.webkitCodecPatterns)
                    m_codecMap.add(pattern, false);
            }
            if (!current.webkitMimeTypes.isEmpty()) {
                for (const auto& mimeType : current.webkitMimeTypes)
                    m_mimeTypeSet.add(mimeType);
            } else
                m_mimeTypeSet.add(AtomString(current.capsString));
        }
    }
}

void GStreamerRegistryScanner::initialize()
{
    if (hasElementForMediaType(m_audioDecoderFactories, "audio/mpeg, mpegversion=(int)4")) {
        m_mimeTypeSet.add(AtomString("audio/aac"));
        m_mimeTypeSet.add(AtomString("audio/mp4"));
        m_mimeTypeSet.add(AtomString("audio/x-m4a"));
        m_codecMap.add(AtomString("mpeg"), false);
        m_codecMap.add(AtomString("mp4a*"), false);
    }

    auto opusSupported = hasElementForMediaType(m_audioDecoderFactories, "audio/x-opus");
    if (opusSupported && (!m_isMediaSource || hasElementForMediaType(m_audioParserFactories, "audio/x-opus"))) {
        m_mimeTypeSet.add(AtomString("audio/opus"));
        m_codecMap.add(AtomString("opus"), false);
        m_codecMap.add(AtomString("x-opus"), false);
    }

    auto vorbisSupported = hasElementForMediaType(m_audioDecoderFactories, "audio/x-vorbis");
    if (vorbisSupported && (!m_isMediaSource || hasElementForMediaType(m_audioParserFactories, "audio/x-vorbis"))) {
        m_codecMap.add(AtomString("vorbis"), false);
        m_codecMap.add(AtomString("x-vorbis"), false);
    }

    bool matroskaSupported = hasElementForMediaType(m_demuxerFactories, "video/x-matroska");
    if (matroskaSupported) {
        auto vp8DecoderAvailable = hasElementForMediaType(m_videoDecoderFactories, "video/x-vp8", true);
        auto vp9DecoderAvailable = hasElementForMediaType(m_videoDecoderFactories, "video/x-vp9", true);

        if (vp8DecoderAvailable || vp9DecoderAvailable)
            m_mimeTypeSet.add(AtomString("video/webm"));

        if (vp8DecoderAvailable) {
            m_codecMap.add(AtomString("vp8"), vp8DecoderAvailable.isUsingHardware);
            m_codecMap.add(AtomString("x-vp8"), vp8DecoderAvailable.isUsingHardware);
            m_codecMap.add(AtomString("vp8.0"), vp8DecoderAvailable.isUsingHardware);
        }
        if (vp9DecoderAvailable) {
            m_codecMap.add(AtomString("vp9"), vp9DecoderAvailable.isUsingHardware);
            m_codecMap.add(AtomString("x-vp9"), vp9DecoderAvailable.isUsingHardware);
            m_codecMap.add(AtomString("vp9.0"), vp9DecoderAvailable.isUsingHardware);
            m_codecMap.add(AtomString("vp09*"), vp9DecoderAvailable.isUsingHardware);
        }
        if (opusSupported)
            m_mimeTypeSet.add(AtomString("audio/webm"));
    }

    bool shouldAddMP4Container = false;

    auto h264DecoderAvailable = hasElementForMediaType(m_videoDecoderFactories, "video/x-h264, profile=(string){ constrained-baseline, baseline, high }", true);
    auto h264AllFormatsDecoderAvailable = GStreamerRegistryScanner::RegistryLookupResult::merge(
        hasElementForMediaType(m_videoDecoderFactories, "video/x-h264, profile=(string){ constrained-baseline, baseline, high }, stream-format=(string)avc", true),
        hasElementForMediaType(m_videoDecoderFactories, "video/x-h264, profile=(string){ constrained-baseline, baseline, high }, stream-format=(string)byte-stream", true)
    );
    auto needsH264Parse = h264DecoderAvailable != h264AllFormatsDecoderAvailable;

    if (h264DecoderAvailable && (!needsH264Parse || hasElementForMediaType(m_videoParserFactories, "video/x-h264"))) {
        shouldAddMP4Container = true;
        m_codecMap.add(AtomString("x-h264"), h264DecoderAvailable.isUsingHardware);
        m_codecMap.add(AtomString("avc*"), h264DecoderAvailable.isUsingHardware);
        m_codecMap.add(AtomString("mp4v*"), h264DecoderAvailable.isUsingHardware);
    }

    auto h265DecoderAvailable = hasElementForMediaType(m_videoDecoderFactories, "video/x-h265", true);
    auto h265AllFormatsDecoderAvailable = GStreamerRegistryScanner::RegistryLookupResult::merge(
        hasElementForMediaType(m_videoDecoderFactories, "video/x-h265, stream-format=(string)byte-stream", true),
        GStreamerRegistryScanner::RegistryLookupResult::merge(
            hasElementForMediaType(m_videoDecoderFactories, "video/x-h265, stream-format=(string)hev1", true),
            hasElementForMediaType(m_videoDecoderFactories, "video/x-h265, stream-format=(string)hvc1", true)
        )
    );
    auto needsH265Parse = h265DecoderAvailable != h265AllFormatsDecoderAvailable;

    if (h265DecoderAvailable && (!needsH265Parse || hasElementForMediaType(m_videoParserFactories, "video/x-h265"))) {
        shouldAddMP4Container = true;
        m_codecMap.add(AtomString("x-h265"), h265DecoderAvailable.isUsingHardware);
        m_codecMap.add(AtomString("hvc1*"), h265DecoderAvailable.isUsingHardware);
        m_codecMap.add(AtomString("hev1*"), h265DecoderAvailable.isUsingHardware);
    }

    if (shouldAddMP4Container) {
        m_mimeTypeSet.add(AtomString("video/mp4"));
        m_mimeTypeSet.add(AtomString("video/x-m4v"));
    }

    Vector<String> av1DecodersBlacklist { "av1dec"_s };
    if ((matroskaSupported || isContainerTypeSupported("video/mp4")) && hasElementForMediaType(m_videoDecoderFactories, "video/x-av1", false, makeOptional(WTFMove(av1DecodersBlacklist)))) {
        m_codecMap.add(AtomString("av01*"), false);
        m_codecMap.add(AtomString("x-av1"), false);
    }

    Vector<GstCapsWebKitMapping> mseCompatibleMapping = {
        { AudioDecoder, "audio/x-ac3", { }, {"x-ac3", "ac-3", "ac3"} },
        { AudioDecoder, "audio/x-eac3", {"audio/x-ac3"},  {"x-eac3", "ec3", "ec-3", "eac3"} },
        { AudioDecoder, "audio/x-flac", {"audio/x-flac", "audio/flac"}, {"x-flac", "flac" } },
    };
    fillMimeTypeSetFromCapsMapping(mseCompatibleMapping);

    if (m_isMediaSource)
        return;

    // The mime-types initialized below are not supported by the MSE backend.

    Vector<GstCapsWebKitMapping> mapping = {
        {AudioDecoder, "audio/midi", {"audio/midi", "audio/riff-midi"}, { }},
        {AudioDecoder, "audio/x-dts", { }, { }},
        {AudioDecoder, "audio/x-sbc", { }, { }},
        {AudioDecoder, "audio/x-sid", { }, { }},
        {AudioDecoder, "audio/x-speex", {"audio/speex", "audio/x-speex"}, { }},
        {AudioDecoder, "audio/x-wavpack", {"audio/x-wavpack"}, { }},
        {VideoDecoder, "video/mpeg, mpegversion=(int){1,2}, systemstream=(boolean)false", {"video/mpeg"}, {"mpeg"}},
        {VideoDecoder, "video/mpegts", { }, { }},
        {VideoDecoder, "video/x-dirac", { }, { }},
        {VideoDecoder, "video/x-flash-video", {"video/flv", "video/x-flv"}, { }},
        {VideoDecoder, "video/x-h263", { }, { }},
        {VideoDecoder, "video/x-msvideocodec", {"video/x-msvideo"}, { }},
        {Demuxer, "application/vnd.rn-realmedia", { }, { }},
        {Demuxer, "application/x-3gp", { }, { }},
        {Demuxer, "application/x-hls", {"application/vnd.apple.mpegurl", "application/x-mpegurl"}, { }},
        {Demuxer, "application/x-pn-realaudio", { }, { }},
        {Demuxer, "audio/x-aiff", { }, { }},
        {Demuxer, "audio/x-wav", {"audio/x-wav", "audio/wav", "audio/vnd.wave"}, {"1"}},
        {Demuxer, "video/quicktime", { }, { }},
        {Demuxer, "video/quicktime, variant=(string)3gpp", {"video/3gpp"}, { }},
        {Demuxer, "video/x-ms-asf", { }, { }},
    };
    fillMimeTypeSetFromCapsMapping(mapping);

    if (hasElementForMediaType(m_demuxerFactories, "application/ogg")) {
        m_mimeTypeSet.add(AtomString("application/ogg"));

        if (vorbisSupported) {
            m_mimeTypeSet.add(AtomString("audio/ogg"));
            m_mimeTypeSet.add(AtomString("audio/x-vorbis+ogg"));
        }

        if (hasElementForMediaType(m_audioDecoderFactories, "audio/x-speex")) {
            m_mimeTypeSet.add(AtomString("audio/ogg"));
            m_codecMap.add(AtomString("speex"), false);
        }

        if (hasElementForMediaType(m_videoDecoderFactories, "video/x-theora")) {
            m_mimeTypeSet.add(AtomString("video/ogg"));
            m_codecMap.add(AtomString("theora"), false);
        }
    }

    bool audioMpegSupported = false;
    if (hasElementForMediaType(m_audioDecoderFactories, "audio/mpeg, mpegversion=(int)1, layer=(int)[1, 3]")) {
        audioMpegSupported = true;
        m_mimeTypeSet.add(AtomString("audio/mp1"));
        m_mimeTypeSet.add(AtomString("audio/mp3"));
        m_mimeTypeSet.add(AtomString("audio/x-mp3"));
        m_codecMap.add(AtomString("audio/mp3"), false);
    }

    if (hasElementForMediaType(m_audioDecoderFactories, "audio/mpeg, mpegversion=(int)2")) {
        audioMpegSupported = true;
        m_mimeTypeSet.add(AtomString("audio/mp2"));
    }

    audioMpegSupported |= isContainerTypeSupported("audio/mp4");
    if (audioMpegSupported) {
        m_mimeTypeSet.add(AtomString("audio/mpeg"));
        m_mimeTypeSet.add(AtomString("audio/x-mpeg"));
    }

    if (matroskaSupported) {
        m_mimeTypeSet.add(AtomString("video/x-matroska"));

        if (hasElementForMediaType(m_videoDecoderFactories, "video/x-vp10"))
            m_mimeTypeSet.add(AtomString("video/webm"));
    }
}

bool GStreamerRegistryScanner::isCodecSupported(String codec, bool shouldCheckForHardwareUse) const
{
    // If the codec is named like a mimetype (eg: video/avc) remove the "video/" part.
    size_t slashIndex = codec.find('/');
    if (slashIndex != WTF::notFound)
        codec = codec.substring(slashIndex + 1);

    bool supported = false;
    if (codec.startsWith("avc1"))
        supported = isAVC1CodecSupported(codec, shouldCheckForHardwareUse);
    else {
        for (const auto& item : m_codecMap) {
            if (!fnmatch(item.key.string().utf8().data(), codec.utf8().data(), 0)) {
                supported = shouldCheckForHardwareUse ? item.value : true;
                if (supported)
                    break;
            }
        }
    }

    GST_LOG("Checked %s codec \"%s\" supported %s", shouldCheckForHardwareUse ? "hardware" : "software", codec.utf8().data(), boolForPrinting(supported));
    return supported;
}

bool GStreamerRegistryScanner::supportsFeatures(const String& features) const
{
    // Apple TV requires this one for DD+.
    constexpr auto dolbyDigitalPlusJOC = "joc";
    if (equalIgnoringASCIICase(features, dolbyDigitalPlusJOC))
        return true;

    return false;
}

MediaPlayerEnums::SupportsType GStreamerRegistryScanner::isContentTypeSupported(const ContentType& contentType, const Vector<ContentType>& contentTypesRequiringHardwareSupport) const
{
    using SupportsType = MediaPlayerEnums::SupportsType;

    const auto& containerType = contentType.containerType();
    if (!isContainerTypeSupported(containerType))
        return SupportsType::IsNotSupported;

    bool ok = false;
    int channels = contentType.parameter("channels"_s).toInt(&ok);
    if (ok && (channels > MEDIA_MAX_AAC_CHANNELS || channels <= 0))
        return SupportsType::IsNotSupported;

    String features = contentType.parameter("features"_s);
    if (!features.isEmpty() && !supportsFeatures(features))
        return SupportsType::IsNotSupported;

    float width = contentType.parameter("width"_s).toFloat(&ok);
    if (ok && width > MEDIA_MAX_WIDTH)
        return SupportsType::IsNotSupported;

    float height = contentType.parameter("height"_s).toFloat(&ok);
    if (ok && height > MEDIA_MAX_HEIGHT)
        return SupportsType::IsNotSupported;

    float framerate = contentType.parameter("framerate"_s).toFloat(&ok);
    if (ok && framerate > MEDIA_MAX_FRAMERATE)
        return SupportsType::IsNotSupported;

#if ENABLE(ENCRYPTED_MEDIA)
    String cryptoblockformat = contentType.parameter("cryptoblockformat");
    if (!cryptoblockformat.isEmpty() && cryptoblockformat != "subsample")
        return SupportsType::IsNotSupported;
#endif

    const auto& codecs = contentType.codecs();

    // Spec says we should not return "probably" if the codecs string is empty.
    if (codecs.isEmpty())
        return SupportsType::MayBeSupported;

    for (const auto& codec : codecs) {
        bool requiresHardwareSupport = contentTypesRequiringHardwareSupport
            .findMatching([containerType, codec](auto& hardwareContentType) -> bool {
            auto hardwareContainer = hardwareContentType.containerType();
            if (!hardwareContainer.isEmpty()
                && fnmatch(hardwareContainer.utf8().data(), containerType.utf8().data(), 0))
                return false;
            auto hardwareCodecs = hardwareContentType.codecs();
            return hardwareCodecs.isEmpty()
                || hardwareCodecs.findMatching([codec](auto& hardwareCodec) -> bool {
                    return !fnmatch(hardwareCodec.utf8().data(), codec.utf8().data(), 0);
            }) != notFound;
        }) != notFound;
        if (!isCodecSupported(codec, requiresHardwareSupport))
            return SupportsType::IsNotSupported;
    }
    return SupportsType::IsSupported;
}

bool GStreamerRegistryScanner::areAllCodecsSupported(const Vector<String>& codecs, bool shouldCheckForHardwareUse) const
{
    for (String codec : codecs) {
        if (!isCodecSupported(codec, shouldCheckForHardwareUse))
            return false;
    }

    return true;
}

bool GStreamerRegistryScanner::isAVC1CodecSupported(const String& codec, bool shouldCheckForHardwareUse) const
{
    auto components = codec.split('.');
    long int spsAsInteger = strtol(components[1].utf8().data(), nullptr, 16);
    uint8_t sps[3];
    sps[0] = spsAsInteger >> 16;
    sps[1] = spsAsInteger >> 8;
    sps[2] = spsAsInteger;

    const char* profile = gst_codec_utils_h264_get_profile(sps, 3);
    const char* level = gst_codec_utils_h264_get_level(sps, 3);

    // To avoid going through a class hierarchy for such a simple
    // string conversion, we use a little trick here: See
    // https://bugs.webkit.org/show_bug.cgi?id=201870.
    char levelAsStringFallback[2] = { '\0', '\0' };
    if (!level && sps[2] > 0 && sps[2] <= 5) {
        levelAsStringFallback[0] = static_cast<char>('0' + sps[2]);
        level = levelAsStringFallback;
    }

    if (!profile || !level) {
        GST_ERROR("H.264 profile / level was not recognised in codec %s", codec.utf8().data());
        return false;
    }

    GST_DEBUG("Codec %s translates to H.264 profile %s and level %s", codec.utf8().data(), profile, level);

    auto checkH264Caps = [&](const char* capsString) {
        bool supported = false;
        auto lookupResult = hasElementForMediaType(m_videoDecoderFactories, capsString, true);
        supported = lookupResult;
        if (shouldCheckForHardwareUse)
            supported = lookupResult.isUsingHardware;
        GST_DEBUG("%s decoding supported for codec %s: %s", shouldCheckForHardwareUse ? "Hardware" : "Software", codec.utf8().data(), boolForPrinting(supported));
        return supported;
    };

    if (const char* maxVideoResolution = g_getenv("WEBKIT_GST_MAX_AVC1_RESOLUTION")) {
        uint8_t levelAsInteger = gst_codec_utils_h264_get_level_idc(level);
        GST_DEBUG("Maximum video resolution requested: %s, supplied codec level IDC: %u", maxVideoResolution, levelAsInteger);
        uint8_t maxLevel = 0;
        const char* maxLevelString = "";
        if (!g_strcmp0(maxVideoResolution, "1080P")) {
            maxLevel = 40;
            maxLevelString = "4";
        } else if (!g_strcmp0(maxVideoResolution, "720P")) {
            maxLevel = 31;
            maxLevelString = "3.1";
        } else if (!g_strcmp0(maxVideoResolution, "480P")) {
            maxLevel = 30;
            maxLevelString = "3";
        } else {
            g_warning("Invalid value for WEBKIT_GST_MAX_AVC1_RESOLUTION. Currently supported, 1080P, 720P and 480P.");
            return false;
        }
        if (levelAsInteger > maxLevel)
            return false;
        return checkH264Caps(makeString("video/x-h264, level=(string)", maxLevelString).utf8().data());
    }

    if (webkitGstCheckVersion(1, 17, 0)) {
        GST_DEBUG("Checking video decoders for constrained caps");
        return checkH264Caps(makeString("video/x-h264, level=(string)", level, ", profile=(string)", profile).utf8().data());
    }

    GST_DEBUG("Falling back to unconstrained caps");
    return checkH264Caps("video/x-h264");
}

GStreamerRegistryScanner::RegistryLookupResult GStreamerRegistryScanner::isDecodingSupported(MediaConfiguration& configuration) const
{
    bool isSupported = false;
    bool isUsingHardware = false;

    if (configuration.video) {
        auto& videoConfiguration = configuration.video.value();
        GST_DEBUG("Checking support for video configuration: \"%s\" size: %ux%u bitrate: %" G_GUINT64_FORMAT " framerate: %f",
            videoConfiguration.contentType.utf8().data(),
            videoConfiguration.width, videoConfiguration.height,
            videoConfiguration.bitrate, videoConfiguration.framerate);

        auto contentType = ContentType(videoConfiguration.contentType);
        isSupported = isContainerTypeSupported(contentType.containerType());
        auto codecs = contentType.codecs();
        if (!codecs.isEmpty())
            isUsingHardware = areAllCodecsSupported(codecs, true);
    }

    if (configuration.audio) {
        auto& audioConfiguration = configuration.audio.value();
        GST_DEBUG("Checking support for audio configuration: \"%s\" %s channels, bitrate: %" G_GUINT64_FORMAT " samplerate: %u",
            audioConfiguration.contentType.utf8().data(), audioConfiguration.channels.utf8().data(),
            audioConfiguration.bitrate, audioConfiguration.samplerate);
        auto contentType = ContentType(audioConfiguration.contentType);
        isSupported = isContainerTypeSupported(contentType.containerType());
    }

    return GStreamerRegistryScanner::RegistryLookupResult { isSupported, isUsingHardware };
}

}
#endif
