﻿/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "WebRtcTransport.h"
#include <iostream>
#include "RtpExt.h"
#include "Rtcp/Rtcp.h"
#include "Rtcp/RtcpFCI.h"
#include "Rtsp/RtpReceiver.h"

#define RTX_SSRC_OFFSET 2
#define RTP_CNAME "zlmediakit-rtp"
#define RTP_LABEL "zlmediakit-label"
#define RTP_MSLABEL "zlmediakit-mslabel"
#define RTP_MSID RTP_MSLABEL " " RTP_LABEL

//RTC配置项目
namespace RTC {
#define RTC_FIELD "rtc."
//rtp和rtcp接受超时时间
const string kTimeOutSec = RTC_FIELD"timeoutSec";
//服务器外网ip
const string kExternIP = RTC_FIELD"externIP";
//设置remb比特率，非0时关闭twcc并开启remb。该设置在rtc推流时有效，可以控制推流画质
const string kRembBitRate = RTC_FIELD"rembBitRate";

static onceToken token([]() {
    mINI::Instance()[kTimeOutSec] = 15;
    mINI::Instance()[kExternIP] = "";
    mINI::Instance()[kRembBitRate] = 0;
});

}//namespace RTC

WebRtcTransport::WebRtcTransport(const EventPoller::Ptr &poller) {
    _poller = poller;
    _dtls_transport = std::make_shared<RTC::DtlsTransport>(poller, this);
    _ice_server = std::make_shared<RTC::IceServer>(this, makeRandStr(4), makeRandStr(28).substr(4));
}

void WebRtcTransport::onCreate(){

}

void WebRtcTransport::onDestory(){
    _dtls_transport = nullptr;
    _ice_server = nullptr;
}

const EventPoller::Ptr& WebRtcTransport::getPoller() const{
    return _poller;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void WebRtcTransport::OnIceServerSendStunPacket(const RTC::IceServer *iceServer, const RTC::StunPacket *packet, RTC::TransportTuple *tuple) {
    onSendSockData((char *) packet->GetData(), packet->GetSize(), (struct sockaddr_in *) tuple);
}

void WebRtcTransport::OnIceServerSelectedTuple(const RTC::IceServer *iceServer, RTC::TransportTuple *tuple) {
    InfoL;
}

void WebRtcTransport::OnIceServerConnected(const RTC::IceServer *iceServer) {
    InfoL;
}

void WebRtcTransport::OnIceServerCompleted(const RTC::IceServer *iceServer) {
    InfoL;
    if (_answer_sdp->media[0].role == DtlsRole::passive) {
        _dtls_transport->Run(RTC::DtlsTransport::Role::SERVER);
    } else {
        _dtls_transport->Run(RTC::DtlsTransport::Role::CLIENT);
    }
}

void WebRtcTransport::OnIceServerDisconnected(const RTC::IceServer *iceServer) {
    InfoL;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void WebRtcTransport::OnDtlsTransportConnected(
        const RTC::DtlsTransport *dtlsTransport,
        RTC::SrtpSession::CryptoSuite srtpCryptoSuite,
        uint8_t *srtpLocalKey,
        size_t srtpLocalKeyLen,
        uint8_t *srtpRemoteKey,
        size_t srtpRemoteKeyLen,
        std::string &remoteCert) {
    InfoL;
    _srtp_session_send = std::make_shared<RTC::SrtpSession>(RTC::SrtpSession::Type::OUTBOUND, srtpCryptoSuite, srtpLocalKey, srtpLocalKeyLen);
    _srtp_session_recv = std::make_shared<RTC::SrtpSession>(RTC::SrtpSession::Type::INBOUND, srtpCryptoSuite, srtpRemoteKey, srtpRemoteKeyLen);
    onStartWebRTC();
}

void WebRtcTransport::OnDtlsTransportSendData(const RTC::DtlsTransport *dtlsTransport, const uint8_t *data, size_t len) {
    onSendSockData((char *)data, len);
}

void WebRtcTransport::OnDtlsTransportConnecting(const RTC::DtlsTransport *dtlsTransport) {
    InfoL;
}

void WebRtcTransport::OnDtlsTransportFailed(const RTC::DtlsTransport *dtlsTransport) {
    InfoL;
    onShutdown(SockException(Err_shutdown, "dtls transport failed"));
}

void WebRtcTransport::OnDtlsTransportClosed(const RTC::DtlsTransport *dtlsTransport) {
    InfoL;
    onShutdown(SockException(Err_shutdown, "dtls close notify received"));
}

void WebRtcTransport::OnDtlsTransportApplicationDataReceived(const RTC::DtlsTransport *dtlsTransport, const uint8_t *data, size_t len) {
    InfoL << hexdump(data, len);
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void WebRtcTransport::onSendSockData(const char *buf, size_t len, bool flush){
    auto tuple = _ice_server->GetSelectedTuple();
    assert(tuple);
    onSendSockData(buf, len, (struct sockaddr_in *) tuple, flush);
}

const RtcSession& WebRtcTransport::getSdp(SdpType type) const{
    switch (type) {
        case SdpType::offer: return *_offer_sdp;
        case SdpType::answer: return *_answer_sdp;
        default: throw std::invalid_argument("不识别的sdp类型");
    }
}

RTC::TransportTuple* WebRtcTransport::getSelectedTuple() const{
    return  _ice_server->GetSelectedTuple();
}

void WebRtcTransport::sendRtcpRemb(uint32_t ssrc, size_t bit_rate) {
    auto remb = FCI_REMB::create({ssrc}, (uint32_t)bit_rate);
    auto fb = RtcpFB::create(PSFBType::RTCP_PSFB_REMB, remb.data(), remb.size());
    fb->ssrc = htonl(0);
    fb->ssrc_media = htonl(ssrc);
    sendRtcpPacket((char *) fb.get(), fb->getSize(), true);
    TraceL << ssrc << " " << bit_rate;
}

void WebRtcTransport::sendRtcpPli(uint32_t ssrc) {
    auto pli = RtcpFB::create(PSFBType::RTCP_PSFB_PLI);
    pli->ssrc = htonl(0);
    pli->ssrc_media = htonl(ssrc);
    sendRtcpPacket((char *) pli.get(), pli->getSize(), true);
}

string getFingerprint(const string &algorithm_str, const std::shared_ptr<RTC::DtlsTransport> &transport){
    auto algorithm = RTC::DtlsTransport::GetFingerprintAlgorithm(algorithm_str);
    for (auto &finger_prints : transport->GetLocalFingerprints()) {
        if (finger_prints.algorithm == algorithm) {
            return finger_prints.value;
        }
    }
    throw std::invalid_argument(StrPrinter << "不支持的加密算法:" << algorithm_str);
}

void WebRtcTransport::setRemoteDtlsFingerprint(const RtcSession &remote){
    //设置远端dtls签名
    RTC::DtlsTransport::Fingerprint remote_fingerprint;
    remote_fingerprint.algorithm = RTC::DtlsTransport::GetFingerprintAlgorithm(_offer_sdp->media[0].fingerprint.algorithm);
    remote_fingerprint.value = _offer_sdp->media[0].fingerprint.hash;
    _dtls_transport->SetRemoteFingerprint(remote_fingerprint);
}

void WebRtcTransport::onCheckSdp(SdpType type, RtcSession &sdp){
    for (auto &m : sdp.media) {
        if (m.type != TrackApplication && !m.rtcp_mux) {
            throw std::invalid_argument("只支持rtcp-mux模式");
        }
    }
    if (sdp.group.mids.empty()) {
        throw std::invalid_argument("只支持group BUNDLE模式");
    }
    if (type == SdpType::offer) {
        sdp.checkValidSSRC();
    }
}

void WebRtcTransport::onRtcConfigure(RtcConfigure &configure) const {
    //开启remb后关闭twcc，因为开启twcc后remb无效
    GET_CONFIG(size_t, remb_bit_rate, RTC::kRembBitRate);
    configure.enableTWCC(!remb_bit_rate);
}

std::string WebRtcTransport::getAnswerSdp(const string &offer){
    try {
        //// 解析offer sdp ////
        _offer_sdp = std::make_shared<RtcSession>();
        _offer_sdp->loadFrom(offer);
        onCheckSdp(SdpType::offer, *_offer_sdp);
        setRemoteDtlsFingerprint(*_offer_sdp);

        //// sdp 配置 ////
        SdpAttrFingerprint fingerprint;
        fingerprint.algorithm = _offer_sdp->media[0].fingerprint.algorithm;
        fingerprint.hash = getFingerprint(fingerprint.algorithm, _dtls_transport);
        RtcConfigure configure;
        configure.setDefaultSetting(_ice_server->GetUsernameFragment(), _ice_server->GetPassword(),
                                    RtpDirection::sendrecv, fingerprint);
        onRtcConfigure(configure);

        //// 生成answer sdp ////
        _answer_sdp = configure.createAnswer(*_offer_sdp);
        onCheckSdp(SdpType::answer, *_answer_sdp);
        return _answer_sdp->toString();
    } catch (exception &ex) {
        onShutdown(SockException(Err_shutdown, ex.what()));
        throw;
    }
}

bool is_dtls(char *buf) {
    return ((*buf > 19) && (*buf < 64));
}

bool is_rtp(char *buf) {
    RtpHeader *header = (RtpHeader *) buf;
    return ((header->pt < 64) || (header->pt >= 96));
}

bool is_rtcp(char *buf) {
    RtpHeader *header = (RtpHeader *) buf;
    return ((header->pt >= 64) && (header->pt < 96));
}

void WebRtcTransport::inputSockData(char *buf, size_t len, RTC::TransportTuple *tuple) {
    if (RTC::StunPacket::IsStun((const uint8_t *) buf, len)) {
        RTC::StunPacket *packet = RTC::StunPacket::Parse((const uint8_t *) buf, len);
        if (packet == nullptr) {
            WarnL << "parse stun error" << std::endl;
            return;
        }
        _ice_server->ProcessStunPacket(packet, tuple);
        return;
    }
    if (is_dtls(buf)) {
        _dtls_transport->ProcessDtlsData((uint8_t *) buf, len);
        return;
    }
    if (is_rtp(buf)) {
        if (_srtp_session_recv->DecryptSrtp((uint8_t *) buf, &len)) {
            onRtp(buf, len);
        } else {
            WarnL;
        }
        return;
    }
    if (is_rtcp(buf)) {
        if (_srtp_session_recv->DecryptSrtcp((uint8_t *) buf, &len)) {
            onRtcp(buf, len);
        } else {
            WarnL;
        }
        return;
    }
}

void WebRtcTransport::sendRtpPacket(char *buf, size_t len, bool flush, void *ctx) {
    if (_srtp_session_send) {
        CHECK(len + SRTP_MAX_TRAILER_LEN <= sizeof(_srtp_buf));
        memcpy(_srtp_buf, buf, len);
        onBeforeEncryptRtp((char *) _srtp_buf, len, ctx);
        if (_srtp_session_send->EncryptRtp(_srtp_buf, &len)) {
            onSendSockData((char *) _srtp_buf, len, flush);
        }
    }
}

void WebRtcTransport::sendRtcpPacket(char *buf, size_t len, bool flush, void *ctx){
    if (_srtp_session_send) {
        CHECK(len + SRTP_MAX_TRAILER_LEN <= sizeof(_srtp_buf));
        memcpy(_srtp_buf, buf, len);
        onBeforeEncryptRtcp((char *) _srtp_buf, len, ctx);
        if (_srtp_session_send->EncryptRtcp(_srtp_buf, &len)) {
            onSendSockData((char *) _srtp_buf, len, flush);
        }
    }
}

///////////////////////////////////////////////////////////////////////////////////
WebRtcTransportImp::Ptr WebRtcTransportImp::create(const EventPoller::Ptr &poller){
    WebRtcTransportImp::Ptr ret(new WebRtcTransportImp(poller), [](WebRtcTransportImp *ptr){
        ptr->onDestory();
       delete ptr;
    });
    ret->onCreate();
    return ret;
}

void WebRtcTransportImp::onCreate(){
    WebRtcTransport::onCreate();
    _socket = Socket::createSocket(getPoller(), false);
    //随机端口，绑定全部网卡
    _socket->bindUdpSock(0);
    weak_ptr<WebRtcTransportImp> weak_self = shared_from_this();
    _socket->setOnRead([weak_self](const Buffer::Ptr &buf, struct sockaddr *addr, int addr_len) mutable {
        auto strong_self = weak_self.lock();
        if (strong_self) {
            strong_self->inputSockData(buf->data(), buf->size(), addr);
        }
    });
    _self = shared_from_this();

    GET_CONFIG(float, timeoutSec, RTC::kTimeOutSec);
    _timer = std::make_shared<Timer>(timeoutSec / 2, [weak_self]() {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return false;
        }
        if (strong_self->_alive_ticker.elapsedTime() > timeoutSec * 1000) {
            strong_self->onShutdown(SockException(Err_timeout, "接受rtp和rtcp超时"));
        }
        return true;
    }, getPoller());
}

WebRtcTransportImp::WebRtcTransportImp(const EventPoller::Ptr &poller) : WebRtcTransport(poller) {
    InfoL << this;
}

WebRtcTransportImp::~WebRtcTransportImp() {
    InfoL << this;
}

void WebRtcTransportImp::onDestory() {
    WebRtcTransport::onDestory();
    uint64_t duration = _alive_ticker.createdTime() / 1000;

    //流量统计事件广播
    GET_CONFIG(uint32_t, iFlowThreshold, General::kFlowThreshold);

    if (_play_src) {
        WarnL << "RTC播放器("
              << _media_info._vhost << "/"
              << _media_info._app << "/"
              << _media_info._streamid
              << ")结束播放,耗时(s):" << duration;
        if (_bytes_usage >= iFlowThreshold * 1024) {
            NoticeCenter::Instance().emitEvent(Broadcast::kBroadcastFlowReport, _media_info, _bytes_usage, duration, true, static_cast<SockInfo &>(*_socket));
        }
    }

    if (_push_src) {
        WarnL << "RTC推流器("
              << _media_info._vhost << "/"
              << _media_info._app << "/"
              << _media_info._streamid
              << ")结束推流,耗时(s):" << duration;
        if (_bytes_usage >= iFlowThreshold * 1024) {
            NoticeCenter::Instance().emitEvent(Broadcast::kBroadcastFlowReport, _media_info, _bytes_usage, duration, false, static_cast<SockInfo &>(*_socket));
        }
    }
}

void WebRtcTransportImp::attach(const RtspMediaSource::Ptr &src, const MediaInfo &info, bool is_play) {
    assert(src);
    _media_info = info;
    if (is_play) {
        _play_src = src;
    } else {
        _push_src = src;
    }
}

void WebRtcTransportImp::onSendSockData(const char *buf, size_t len, struct sockaddr_in *dst, bool flush) {
    auto ptr = BufferRaw::create();
    ptr->assign(buf, len);
    _socket->send(ptr, (struct sockaddr *)(dst), sizeof(struct sockaddr), flush);
}

///////////////////////////////////////////////////////////////////

bool WebRtcTransportImp::canSendRtp() const{
    auto &sdp = getSdp(SdpType::answer);
    return _play_src && (sdp.media[0].direction == RtpDirection::sendrecv || sdp.media[0].direction == RtpDirection::sendonly);
}

bool WebRtcTransportImp::canRecvRtp() const{
    auto &sdp = getSdp(SdpType::answer);
    return _push_src && (sdp.media[0].direction == RtpDirection::sendrecv || sdp.media[0].direction == RtpDirection::recvonly);
}

const RtcSession& WebRtcTransportImp::getSdpWithSSRC() const{
    auto &offer = getSdp(SdpType::offer);
    if (offer.haveSSRC()) {
        return offer;
    }
    auto &answer = getSdp(SdpType::answer);
    CHECK(answer.haveSSRC());
    return answer;
}

void WebRtcTransportImp::onStartWebRTC() {
    //获取ssrc和pt相关信息,届时收到rtp和rtcp时分别可以根据pt和ssrc找到相关的信息
    for (auto &m : getSdp(SdpType::offer).media) {
        for (auto &plan : m.plan) {
            auto hit_pan = getSdp(SdpType::answer).getMedia(m.type)->getPlan(plan.pt);
            if (!hit_pan) {
                continue;
            }
            auto m_with_ssrc = getSdpWithSSRC().getMedia(m.type);
            //获取offer端rtp的ssrc和pt相关信息
            auto &ref = _rtp_info_pt[plan.pt];
            ref.plan = &plan;
            ref.media = m_with_ssrc;
            ref.is_common_rtp = getCodecId(plan.codec) != CodecInvalid;
            if (ref.is_common_rtp) {
                //rtp
                _rtp_info_ssrc[m_with_ssrc->rtp_rtx_ssrc[0].ssrc] = &ref;
            }
            ref.rtcp_context_recv = std::make_shared<RtcpContext>(ref.plan->sample_rate, true);
            ref.rtcp_context_send = std::make_shared<RtcpContext>(ref.plan->sample_rate, false);
            ref.receiver = std::make_shared<RtpReceiverImp>([&ref, this](RtpPacket::Ptr rtp) mutable{
                onSortedRtp(ref, std::move(rtp));
            }, [&ref, this](const RtpPacket::Ptr &rtp) mutable {
                onBeforeSortedRtp(ref, rtp);
            });
            ref.nack_ctx.setOnNack([&ref, this](const FCI_NACK &nack) mutable{
                onNack(ref, nack);
            });
        }
        if (m.type != TrackApplication) {
            //记录rtp ext类型与id的关系，方便接收或发送rtp时修改rtp ext id
            for (auto &ext : m.extmap) {
                auto ext_type = RtpExt::getExtType(ext.ext);
                _rtp_ext_id_to_type.emplace(ext.id, ext_type);
                _rtp_ext_type_to_id.emplace(ext_type, ext.id);
            }
        }
    }

    if (canRecvRtp()) {
        _push_src->setSdp(getSdp(SdpType::answer).toRtspSdp());
    }
    if (canSendRtp()) {
        _reader = _play_src->getRing()->attach(getPoller(), true);
        weak_ptr<WebRtcTransportImp> weak_self = shared_from_this();
        _reader->setReadCB([weak_self](const RtspMediaSource::RingDataType &pkt) {
            auto strongSelf = weak_self.lock();
            if (!strongSelf) {
                return;
            }
            size_t i = 0;
            pkt->for_each([&](const RtpPacket::Ptr &rtp) {
                strongSelf->onSendRtp(rtp, ++i == pkt->size());
            });
        });

        RtcSession rtsp_send_sdp;
        rtsp_send_sdp.loadFrom(_play_src->getSdp(), false);
        for (auto &m : getSdp(SdpType::answer).media) {
            if (m.type == TrackApplication) {
                continue;
            }
            auto rtsp_media = rtsp_send_sdp.getMedia(m.type);
            if (rtsp_media && getCodecId(rtsp_media->plan[0].codec) == getCodecId(m.plan[0].codec)) {
                auto it = _rtp_info_pt.find(m.plan[0].pt);
                CHECK(it != _rtp_info_pt.end());
                //记录发送rtp时约定的信息，届时发送rtp时需要修改pt和ssrc
                _send_rtp_info[m.type] = &it->second;
            }
        }
    }
}

void WebRtcTransportImp::onCheckSdp(SdpType type, RtcSession &sdp){
    WebRtcTransport::onCheckSdp(type, sdp);
    if (type != SdpType::answer) {
        //我们只修改answer sdp
        return;
    }

    //修改answer sdp的ip、端口信息
    GET_CONFIG(string, extern_ip, RTC::kExternIP);
    for (auto &m : sdp.media) {
        m.addr.reset();
        m.addr.address = extern_ip.empty() ? SockUtil::get_local_ip() : extern_ip;
        m.rtcp_addr.reset();
        m.rtcp_addr.address = m.addr.address;
        m.rtcp_addr.port = _socket->get_local_port();
        m.port = m.rtcp_addr.port;
        sdp.origin.address = m.addr.address;
    }

    if (!canSendRtp() || getSdp(SdpType::offer).haveSSRC()) {
        //offer sdp未包含ssrc相关信息，那么我们才在answer sdp中回复ssrc相关信息
        return;
    }

    for (auto &m : sdp.media) {
        if (m.type == TrackApplication) {
            continue;
        }
        //添加answer sdp的ssrc信息
        m.rtp_rtx_ssrc.emplace_back();
        m.rtp_rtx_ssrc[0].ssrc = _play_src->getSsrc(m.type);
        m.rtp_rtx_ssrc[0].cname = RTP_CNAME;
        m.rtp_rtx_ssrc[0].label = RTP_LABEL;
        m.rtp_rtx_ssrc[0].mslabel = RTP_MSLABEL;
        m.rtp_rtx_ssrc[0].msid = RTP_MSID;

        if (m.getRelatedRtxPlan(m.plan[0].pt)) {
            m.rtp_rtx_ssrc.emplace_back();
            m.rtp_rtx_ssrc[1] = m.rtp_rtx_ssrc[0];
            m.rtp_rtx_ssrc[1].ssrc += RTX_SSRC_OFFSET;
        }
    }
}

void WebRtcTransportImp::onRtcConfigure(RtcConfigure &configure) const {
    WebRtcTransport::onRtcConfigure(configure);

    if (_play_src) {
        //这是播放,同时也可能有推流
        configure.video.direction = _push_src ? RtpDirection::sendrecv : RtpDirection::sendonly;
        configure.audio.direction = configure.video.direction;
        configure.setPlayRtspInfo(_play_src->getSdp());
    } else if (_push_src) {
        //这只是推流
        configure.video.direction = RtpDirection::recvonly;
        configure.audio.direction = RtpDirection::recvonly;
    } else {
        throw std::invalid_argument("未设置播放或推流的媒体源");
    }

    //添加接收端口candidate信息
    configure.addCandidate(*getIceCandidate());
}

SdpAttrCandidate::Ptr WebRtcTransportImp::getIceCandidate() const{
    auto candidate = std::make_shared<SdpAttrCandidate>();
    candidate->foundation = "udpcandidate";
    //rtp端口
    candidate->component = 1;
    candidate->transport = "udp";
    //优先级，单candidate时随便
    candidate->priority = 100;
    GET_CONFIG(string, extern_ip, RTC::kExternIP);
    candidate->address = extern_ip.empty() ? SockUtil::get_local_ip() : extern_ip;
    candidate->port = _socket->get_local_port();
    candidate->type = "host";
    return candidate;
}

///////////////////////////////////////////////////////////////////

class RtpReceiverImp : public RtpReceiver {
public:
    RtpReceiverImp( function<void(RtpPacket::Ptr rtp)> cb,  function<void(const RtpPacket::Ptr &rtp)> cb_before = nullptr){
        _on_sort = std::move(cb);
        _on_before_sort = std::move(cb_before);
    }

    ~RtpReceiverImp() override = default;

    bool inputRtp(TrackType type, int samplerate, uint8_t *ptr, size_t len){
        return handleOneRtp((int) type, type, samplerate, ptr, len);
    }

protected:
    void onRtpSorted(RtpPacket::Ptr rtp, int track_index) override {
        _on_sort(std::move(rtp));
    }

    void onBeforeRtpSorted(const RtpPacket::Ptr &rtp, int track_index) override {
        if (_on_before_sort) {
            _on_before_sort(rtp);
        }
    }

private:
    function<void(RtpPacket::Ptr rtp)> _on_sort;
    function<void(const RtpPacket::Ptr &rtp)> _on_before_sort;
};

void WebRtcTransportImp::onRtcp(const char *buf, size_t len) {
    _bytes_usage += len;
    auto rtcps = RtcpHeader::loadFromBytes((char *) buf, len);
    for (auto rtcp : rtcps) {
        switch ((RtcpType) rtcp->pt) {
            case RtcpType::RTCP_SR : {
                //对方汇报rtp发送情况
                RtcpSR *sr = (RtcpSR *) rtcp;
                auto it = _rtp_info_ssrc.find(sr->ssrc);
                if (it != _rtp_info_ssrc.end()) {
                    it->second->rtcp_context_recv->onRtcp(sr);
                    auto rr = it->second->rtcp_context_recv->createRtcpRR(sr->items.ssrc, sr->ssrc);
                    sendRtcpPacket(rr->data(), rr->size(), true);
                } else {
                    WarnL << "未识别的sr rtcp包:" << (uint32_t)sr->ssrc;
                }
                break;
            }
            case RtcpType::RTCP_RR : {
                _alive_ticker.resetTime();
                //对方汇报rtp接收情况
                RtcpRR *rr = (RtcpRR *) rtcp;
                auto it = _rtp_info_ssrc.find(rr->ssrc);
                if (it != _rtp_info_ssrc.end()) {
                    auto sr = it->second->rtcp_context_send->createRtcpSR(rr->items.ssrc);
                    sendRtcpPacket(sr->data(), sr->size(), true);
                } else {
                    WarnL << "未识别的rr rtcp包:"  << (uint32_t)rr->ssrc;
                }
                break;
            }
            case RtcpType::RTCP_BYE : {
                //对方汇报停止发送rtp
                RtcpBye *bye = (RtcpBye *) rtcp;
                for (auto ssrc : bye->getSSRC()) {
                    auto it = _rtp_info_ssrc.find(*ssrc);
                    if (it == _rtp_info_ssrc.end()) {
                        WarnL << "未识别的bye rtcp包:" << *ssrc;
                        continue;
                    }
                    _rtp_info_pt.erase(it->second->plan->pt);
                    _rtp_info_ssrc.erase(it);
                }
                onShutdown(SockException(Err_eof, "rtcp bye message received"));
                break;
            }
            case RtcpType::RTCP_PSFB:
            case RtcpType::RTCP_RTPFB: {
                RtcpFB *fb = (RtcpFB *) rtcp;
                auto it = _rtp_info_ssrc.find(fb->ssrc);
                if (it == _rtp_info_ssrc.end()) {
                    WarnL << "未识别的 rtcp包:" << rtcp->dumpString();
                    return;
                }
                if ((RtcpType) rtcp->pt == RtcpType::RTCP_PSFB) {
//                    DebugL << "\r\n" << rtcp->dumpString();
                    break;
                }
                //RTPFB
                switch ((RTPFBType) rtcp->report_count) {
                    case RTPFBType::RTCP_RTPFB_NACK : {
                        auto &fci = fb->getFci<FCI_NACK>();
                        it->second->nack_list.for_each_nack(fci, [&](const RtpPacket::Ptr &rtp) {
                            //rtp重传
                            onSendRtp(rtp, true, true);
                        });
                        break;
                    }
                    default:
//                        DebugL << "\r\n" << rtcp->dumpString();
                        break;
                }
                break;
            }
            default: break;
        }
    }
}

void WebRtcTransportImp::onRtp(const char *buf, size_t len) {
    _bytes_usage += len;
    _alive_ticker.resetTime();
    RtpHeader *rtp = (RtpHeader *) buf;
    //根据接收到的rtp的pt信息，找到该流的信息
    auto it = _rtp_info_pt.find(rtp->pt);
    if (it == _rtp_info_pt.end()) {
        WarnL;
        return;
    }
    auto &info = it->second;

#if 1
    auto header = (RtpHeader *) buf;
    auto seq = ntohs(header->seq);
    if (info.is_common_rtp) {
        //此处模拟接受丢包
        if (info.media->type == TrackVideo && seq % 10 == 0) {
            //丢包
            DebugL << "模拟接受丢包:" << seq;
            return;
        } else {
            info.nack_ctx.received(seq);
        }
    } else {
        //收到重传包
        header->ssrc = info.media->rtp_rtx_ssrc[0].ssrc;
        InfoL << "收到重传包:" <<  seq;
    }
#endif

    //解析并排序rtp
    info.receiver->inputRtp(info.media->type, info.plan->sample_rate, (uint8_t *) buf, len);
}

void WebRtcTransportImp::onNack(RtpPayloadInfo &info, const FCI_NACK &nack) {
    auto rtcp = RtcpFB::create(RTPFBType::RTCP_RTPFB_NACK, &nack, FCI_NACK::kSize);
    rtcp->ssrc = htons(0);
    rtcp->ssrc_media = htonl(info.media->rtp_rtx_ssrc[0].ssrc);
    InfoL << rtcp->RtcpHeader::dumpString();
    sendRtcpPacket((char *) rtcp.get(), rtcp->getSize(), true);
}

///////////////////////////////////////////////////////////////////

void WebRtcTransportImp::onSortedRtp(RtpPayloadInfo &info, RtpPacket::Ptr rtp) {
    if (!info.is_common_rtp) {
        WarnL;
        return;
    }
    if (info.media->type == TrackVideo && _pli_ticker.elapsedTime() > 2000) {
        //定期发送pli请求关键帧，方便非rtc等协议
        _pli_ticker.resetTime();
        sendRtcpPli(rtp->getSSRC());

        //开启remb，则发送remb包调节比特率
        GET_CONFIG(size_t, remb_bit_rate, RTC::kRembBitRate);
        if (remb_bit_rate && getSdp(SdpType::answer).supportRtcpFb(SdpConst::kRembRtcpFb)) {
            sendRtcpRemb(rtp->getSSRC(), remb_bit_rate);
        }
    }

    if (_push_src) {
        _push_src->onWrite(std::move(rtp), false);
    }
}

static void setExtType(RtpExt &ext, uint8_t tp) {}
static void setExtType(RtpExt &ext, RtpExtType tp) {
    ext.setType(tp);
}

template<typename Type>
static void changeRtpExtId(const RtpHeader *header, const Type &map) {
    auto ext_map = RtpExt::getExtValue(header);
    for (auto &pr : ext_map) {
        auto it = map.find((typename Type::key_type) (pr.first));
        if (it == map.end()) {
            WarnL << "未处理的rtp ext, 类型不识别:" << (int) pr.first;
            pr.second.clearExt();
            continue;
        }
        setExtType(pr.second, it->first);
        setExtType(pr.second, it->second);
//        DebugL << pr.second.dumpString();
        pr.second.setExtId((uint8_t) it->second);
    }
}

void WebRtcTransportImp::onBeforeSortedRtp(RtpPayloadInfo &info, const RtpPacket::Ptr &rtp) {
    changeRtpExtId(rtp->getHeader(), _rtp_ext_id_to_type);
    //统计rtp收到的情况，好做rr汇报
    info.rtcp_context_recv->onRtp(rtp->getSeq(), rtp->getStampMS(), rtp->size() - RtpPacket::kRtpTcpHeaderSize);
}

void WebRtcTransportImp::onSendRtp(const RtpPacket::Ptr &rtp, bool flush, bool rtx){
    auto info = _send_rtp_info[rtp->type];
    if (!info) {
        //忽略，对方不支持该编码类型
        return;
    }
    if (!rtx) {
        //统计rtp发送情况，好做sr汇报
        info->rtcp_context_send->onRtp(rtp->getSeq(), rtp->getStampMS(), rtp->size() - RtpPacket::kRtpTcpHeaderSize);
        info->nack_list.push_back(rtp);
#if 0
        //此处模拟发送丢包
        if(rtp->getSeq() % 10 == 0){
            DebugL << "模拟发送丢包:" << rtp->getSeq();
            return;
        }
#endif
    } else {
        WarnL << "rtp发送重传:" << rtp->getSeq();
    }
    sendRtpPacket(rtp->data() + RtpPacket::kRtpTcpHeaderSize, rtp->size() - RtpPacket::kRtpTcpHeaderSize, flush, info);
    _bytes_usage += rtp->size() - RtpPacket::kRtpTcpHeaderSize;
}

void WebRtcTransportImp::onBeforeEncryptRtp(const char *buf, size_t len, void *ctx) {
    RtpPayloadInfo *info = reinterpret_cast<RtpPayloadInfo *>(ctx);
    auto header = (RtpHeader *)buf;
    //修改目标pt和ssrc
    header->pt = info->plan->pt;
    header->ssrc = htons(info->media->rtp_rtx_ssrc[0].ssrc);
    changeRtpExtId(header, _rtp_ext_type_to_id);
}

void WebRtcTransportImp::onBeforeEncryptRtcp(const char *buf, size_t len, void *ctx) {

}

void WebRtcTransportImp::onShutdown(const SockException &ex){
    InfoL << ex.what();
    _self = nullptr;
}

/////////////////////////////////////////////////////////////////////////////////////////////

bool WebRtcTransportImp::close(MediaSource &sender, bool force) {
    //此回调在其他线程触发
    if(!_push_src || (!force && _push_src->totalReaderCount())){
        return false;
    }
    string err = StrPrinter << "close media:" << sender.getSchema() << "/" << sender.getVhost() << "/" << sender.getApp() << "/" << sender.getId() << " " << force;
    onShutdown(SockException(Err_shutdown,err));
    return true;
}

int WebRtcTransportImp::totalReaderCount(MediaSource &sender) {
    return _push_src ? _push_src->totalReaderCount() : sender.readerCount();
}

MediaOriginType WebRtcTransportImp::getOriginType(MediaSource &sender) const {
    return MediaOriginType::rtc_push;
}

string WebRtcTransportImp::getOriginUrl(MediaSource &sender) const {
    return "";
}

std::shared_ptr<SockInfo> WebRtcTransportImp::getOriginSock(MediaSource &sender) const {
    return const_cast<WebRtcTransportImp *>(this)->shared_from_this();
}

/////////////////////////////////////////////////////////////////////////////////////////////

string WebRtcTransportImp::get_local_ip() {
    return getSdp(SdpType::answer).media[0].candidate[0].address;
}

uint16_t WebRtcTransportImp::get_local_port() {
    return _socket->get_local_port();
}

string WebRtcTransportImp::get_peer_ip() {
    return SockUtil::inet_ntoa(((struct sockaddr_in *) getSelectedTuple())->sin_addr);
}

uint16_t WebRtcTransportImp::get_peer_port() {
    return ntohs(((struct sockaddr_in *) getSelectedTuple())->sin_port);
}

string WebRtcTransportImp::getIdentifier() const {
    return StrPrinter << this;
}