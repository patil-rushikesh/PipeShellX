#include "psx/transport/session.hpp"

#include "psx/transport/control_payloads.hpp"

#include <algorithm>
#include <limits>

namespace psx::transport {

namespace {
psx::Error protocolError(const char* what) {
    return psx::Error{psx::ErrorClass::Other, 0, what};
}
std::uint8_t channelFlag(Channel channel) {
    return channel == Channel::Stderr ? kFlagStderr : 0;
}
bool hasOnlyFlags(const Frame& frame, std::uint8_t allowed) {
    return (frame.flags & static_cast<std::uint8_t>(~allowed)) == 0;
}
psx::Result<void> validateConnectionFrame(const Frame& frame) {
    if (frame.streamId != 0 || frame.flags != 0 || !frame.payload.empty()) {
        return protocolError("connection frame must have stream 0, flags 0, and an empty payload");
    }
    return {};
}
psx::Result<void> validateStreamFrame(const Frame& frame, std::uint8_t allowedFlags) {
    if (frame.streamId == 0) {
        return protocolError("stream frame must have a nonzero stream id");
    }
    if (!hasOnlyFlags(frame, allowedFlags)) {
        return protocolError("frame contains unknown or reserved flags");
    }
    return {};
}
} // namespace

Session::Session(Role role, WriteFn write, SessionHandler& handler, std::uint32_t initialWindow)
    : role_(role), write_(std::move(write)), handler_(handler), initialWindow_(initialWindow) {}

void Session::send(const Frame& frame) {
    if (protocolFailed_) {
        return;
    }
    std::string wire;
    encodeFrameInto(wire, frame);
    write_(wire);
}

StreamId Session::open(const OpenRequest& request) {
    if (protocolFailed_ || role_ != Role::Controller || sentGoAway_ || receivedGoAway_ || nextStreamId_ == 0 ||
        !validateOpenRequest(request).ok()) {
        return 0;
    }
    const StreamId id = nextStreamId_++;
    streams_.try_emplace(id, initialWindow_);
    // A caller that populated lamportTs (see docs/ds-project/01-lamport-clocks.md)
    // gets the OPEN v2 payload automatically; everyone else keeps the exact v1
    // wire bytes this Session has always sent.
    const std::string payload = request.lamportTs != 0 ? encodeOpenV2(request) : encodeOpen(request);
    send(Frame{.type = FrameType::Open, .flags = 0, .streamId = id, .payload = payload});
    return id;
}

void Session::sendData(StreamId id, std::string_view bytes, bool endStream, Channel channel) {
    if (protocolFailed_ || (role_ == Role::Controller && channel != Channel::Stdout)) {
        return;
    }
    auto it = streams_.find(id);
    if (it == streams_.end()) {
        return; // unknown stream: nothing to send
    }
    Stream& stream = it->second;
    if (stream.localDataClosed || stream.exitPending) {
        return;
    }
    if (!bytes.empty()) {
        stream.sendQueue.push_back({channel, std::string(bytes)});
        stream.sendBytes += bytes.size();
    }
    if (endStream) {
        stream.localDataClosed = true;
        stream.sendEndPending = true;
    }
    flushStream(id, stream);
}

void Session::flushStream(StreamId id, Stream& stream) {
    const bool wasFull = stream.sendBytes >= initialWindow_;
    while (!stream.sendQueue.empty() && stream.sendCredit > 0) {
        Stream::Segment& front = stream.sendQueue.front();
        const auto chunk = static_cast<std::uint32_t>(std::min<std::size_t>(front.bytes.size(), stream.sendCredit));
        // The stream's endStream flag rides the last byte of the last segment.
        const bool drainsQueue = chunk == front.bytes.size() && stream.sendQueue.size() == 1;
        const bool last = drainsQueue && stream.sendEndPending;
        std::uint8_t flags = channelFlag(front.channel);
        if (last) {
            flags |= kFlagEndStream;
        }
        send(Frame{.type = FrameType::Data, .flags = flags, .streamId = id, .payload = front.bytes.substr(0, chunk)});
        stream.sendCredit -= chunk;
        stream.sendBytes -= chunk;
        if (chunk == front.bytes.size()) {
            stream.sendQueue.pop_front();
        } else {
            front.bytes.erase(0, chunk);
        }
        if (last) {
            stream.sendEndPending = false;
        }
    }
    // A pending end that has no (more) buffered bytes rides a zero-length frame.
    if (stream.sendQueue.empty() && stream.sendEndPending) {
        send(Frame{.type = FrameType::Data, .flags = kFlagEndStream, .streamId = id, .payload = {}});
        stream.sendEndPending = false;
    }
    // A deferred EXIT rides out once its DATA has fully drained (terminal).
    if (stream.sendQueue.empty() && !stream.sendEndPending && stream.exitPending) {
        const auto status = stream.exitStatus;
        const bool peerDataClosed = stream.peerDataClosed;
        send(Frame{.type = FrameType::Exit, .flags = kFlagEndStream, .streamId = id, .payload = encodeExit(status)});
        closeLocally(id, peerDataClosed); // invalidates `stream`; nothing below may touch it
        return;
    }
    // The buffer drained back below the high-water mark: the producer may resume.
    if (wasFull && stream.sendBytes < initialWindow_ && streamWritable_) {
        streamWritable_(id);
    }
}

void Session::closeLocally(StreamId id, bool peerDataClosed) {
    locallyClosedStreams_.insert_or_assign(id, LocallyClosedStream{.peerDataClosed = peerDataClosed});
    streams_.erase(id);
}

bool Session::streamWritable(StreamId id) const {
    auto it = streams_.find(id);
    return it == streams_.end() || it->second.sendBytes < initialWindow_;
}

void Session::consume(StreamId id, std::uint32_t n) {
    if (protocolFailed_) {
        return;
    }
    auto it = streams_.find(id);
    if (it == streams_.end()) {
        return;
    }
    const std::uint32_t delta = it->second.recvWindow.onConsumed(n);
    if (delta > 0) {
        send(Frame{.type = FrameType::WindowUpdate, .flags = 0, .streamId = id, .payload = encodeWindowUpdate(delta)});
    }
}

void Session::sendExit(StreamId id, const psx::os::ExitStatus& status) {
    if (protocolFailed_ || role_ != Role::Node) {
        return;
    }
    auto it = streams_.find(id);
    if (it == streams_.end()) {
        return; // already closed
    }
    Stream& stream = it->second;
    if (stream.exitPending) {
        return;
    }
    stream.localDataClosed = true;
    if (!stream.sendQueue.empty() || stream.sendEndPending) {
        // DATA is still flow-controlled in the buffer; EXIT must not overtake it.
        // flushStream() emits it once the buffer drains.
        stream.exitPending = true;
        stream.exitStatus = status;
        return;
    }
    const bool peerDataClosed = stream.peerDataClosed;
    send(Frame{.type = FrameType::Exit, .flags = kFlagEndStream, .streamId = id, .payload = encodeExit(status)});
    closeLocally(id, peerDataClosed); // terminal
}

void Session::ping() {
    if (protocolFailed_) {
        return;
    }
    send(Frame{.type = FrameType::Ping, .flags = 0, .streamId = 0, .payload = {}});
}

void Session::goAway() {
    if (protocolFailed_ || sentGoAway_) {
        return;
    }
    sentGoAway_ = true;
    send(Frame{.type = FrameType::GoAway, .flags = 0, .streamId = 0, .payload = {}});
}

psx::Result<void> Session::receive(std::string_view bytes) {
    if (protocolFailed_) {
        return protocolError("session poisoned by an earlier protocol error");
    }
    // Collect frames first, then dispatch, so a handler callback can safely call
    // back into this Session (e.g. reply on a stream) without reentering the
    // decoder's buffer.
    psx::Result<void> dispatchResult{};
    auto sink = [&](Frame&& frame) {
        if (dispatchResult.ok()) {
            dispatchResult = dispatch(std::move(frame));
        }
    };
    if (auto decoded = decoder_.push(std::span<const char>(bytes.data(), bytes.size()), sink); !decoded.ok()) {
        protocolFailed_ = true;
        return decoded.error();
    }
    if (!dispatchResult.ok()) {
        protocolFailed_ = true;
    }
    return dispatchResult;
}

psx::Result<void> Session::dispatch(Frame&& frame) {
    switch (frame.type) {
        case FrameType::Open: {
            PSX_TRY(validateStreamFrame(frame, 0));
            if (role_ != Role::Node) {
                return protocolError("OPEN received by a controller");
            }
            if (sentGoAway_ || receivedGoAway_) {
                return protocolError("OPEN received after GOAWAY");
            }
            if (frame.streamId != nextPeerStreamId_) {
                return protocolError("OPEN stream ids must be strictly sequential");
            }
            auto request = decodeOpen(frame.payload);
            if (!request.ok()) {
                return request.error();
            }
            nextPeerStreamId_ = frame.streamId == std::numeric_limits<StreamId>::max() ? 0 : frame.streamId + 1;
            streams_.try_emplace(frame.streamId, initialWindow_);
            handler_.onOpen(frame.streamId, request.value());
            return {};
        }
        case FrameType::Data: {
            PSX_TRY(validateStreamFrame(frame, kFlagEndStream | kFlagStderr));
            if (role_ == Role::Node && (frame.flags & kFlagStderr) != 0) {
                return protocolError("controller DATA cannot select an output channel");
            }
            const bool endStream = (frame.flags & kFlagEndStream) != 0;
            auto closed = locallyClosedStreams_.find(frame.streamId);
            if (closed != locallyClosedStreams_.end()) {
                if (closed->second.peerDataClosed) {
                    return protocolError("DATA received after END_STREAM");
                }
                if (endStream) {
                    closed->second.peerDataClosed = true;
                }
                return {}; // valid cross-direction race with our locally sent EXIT
            }
            auto it = streams_.find(frame.streamId);
            if (it == streams_.end()) {
                return protocolError("DATA for an unknown or peer-closed stream");
            }
            if (it->second.peerDataClosed) {
                return protocolError("DATA received after END_STREAM");
            }
            if (!it->second.recvWindow.onData(static_cast<std::uint32_t>(frame.payload.size()))) {
                return protocolError("DATA exceeds the stream's flow-control window");
            }
            if (endStream) {
                it->second.peerDataClosed = true;
            }
            const Channel channel = (frame.flags & kFlagStderr) != 0 ? Channel::Stderr : Channel::Stdout;
            handler_.onData(frame.streamId, frame.payload, endStream, channel);
            return {};
        }
        case FrameType::Exit: {
            PSX_TRY(validateStreamFrame(frame, kFlagEndStream));
            if (role_ != Role::Controller) {
                return protocolError("EXIT received by a node");
            }
            auto status = decodeExit(frame.payload);
            if (!status.ok()) {
                return status.error();
            }
            if (streams_.count(frame.streamId) == 0) {
                return protocolError("EXIT for an unknown stream");
            }
            streams_.erase(frame.streamId); // terminal
            handler_.onExit(frame.streamId, status.value());
            return {};
        }
        case FrameType::Ping:
            PSX_TRY(validateConnectionFrame(frame));
            send(Frame{.type = FrameType::Pong, .flags = 0, .streamId = 0, .payload = {}}); // auto-reply
            return {};
        case FrameType::Pong:
            PSX_TRY(validateConnectionFrame(frame));
            handler_.onPong();
            return {};
        case FrameType::GoAway:
            PSX_TRY(validateConnectionFrame(frame));
            receivedGoAway_ = true;
            handler_.onGoAway();
            return {};
        case FrameType::WindowUpdate: {
            PSX_TRY(validateStreamFrame(frame, 0));
            auto delta = decodeWindowUpdate(frame.payload);
            if (!delta.ok()) {
                return delta.error();
            }
            auto it = streams_.find(frame.streamId);
            if (it == streams_.end()) {
                // Credit crossing an EXIT that this endpoint sent is harmless.
                // A frame arriving after the peer's EXIT is same-direction and
                // cannot be explained by the close race.
                if (locallyClosedStreams_.count(frame.streamId) != 0) {
                    return {};
                }
                return protocolError("WINDOW_UPDATE for an unknown or peer-closed stream");
            }
            Stream& stream = it->second;
            if (static_cast<std::uint64_t>(stream.sendCredit) + delta.value() > kMaxStreamWindow) {
                return protocolError("WINDOW_UPDATE overflows the stream flow-control window");
            }
            stream.sendCredit += delta.value();
            flushStream(frame.streamId, stream);
            return {};
        }
    }
    return protocolError("unknown frame type");
}

} // namespace psx::transport
