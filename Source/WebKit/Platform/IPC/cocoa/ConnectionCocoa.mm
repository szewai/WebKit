/*
 * Copyright (C) 2010-2020 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#import "config.h"
#import "Connection.h"

#import "Encoder.h"
#import "IPCUtilities.h"
#import "ImportanceAssertion.h"
#import "Logging.h"
#import "MachMessage.h"
#import "MachUtilities.h"
#import "WKCrashReporter.h"
#import "XPCUtilities.h"
#import <WebCore/AXObjectCache.h>
#import <mach/mach_error.h>
#import <mach/mach_init.h>
#import <mach/mach_traps.h>
#import <mach/vm_map.h>
#import <sys/mman.h>
#import <wtf/CheckedArithmetic.h>
#import <wtf/HexNumber.h>
#import <wtf/MachSendRight.h>
#import <wtf/RunLoop.h>
#import <wtf/spi/darwin/XPCSPI.h>
#import <wtf/text/MakeString.h>
#import <wtf/text/ParsingUtilities.h>

#if PLATFORM(IOS_FAMILY)
#import "ProcessAssertion.h"
#endif

namespace IPC {

static const size_t inlineMessageMaxSize = 4096;

// Arbitrary message IDs that do not collide with Mach notification messages (used my initials).
constexpr mach_msg_id_t inlineBodyMessageID = 0xdba0dba;
constexpr mach_msg_id_t outOfLineBodyMessageID = 0xdba1dba;

static void requestNoSenderNotifications(mach_port_t port, mach_port_t notify)
{
    mach_port_t previousNotificationPort = MACH_PORT_NULL;
    auto kr = mach_port_request_notification(mach_task_self(), port, MACH_NOTIFY_NO_SENDERS, 0, notify, MACH_MSG_TYPE_MAKE_SEND_ONCE, &previousNotificationPort);
    ASSERT(kr == KERN_SUCCESS);
    if (kr != KERN_SUCCESS) {
        // If mach_port_request_notification fails, 'previousNotificationPort' will be uninitialized.
        LOG_ERROR("mach_port_request_notification failed: (%x) %s", kr, mach_error_string(kr));
    } else
        deallocateSendRightSafely(previousNotificationPort);
}

static void requestNoSenderNotifications(mach_port_t port)
{
    requestNoSenderNotifications(port, port);
}

static void clearNoSenderNotifications(mach_port_t port)
{
    requestNoSenderNotifications(port, MACH_PORT_NULL);
}

void Connection::platformInvalidate()
{
    if (!m_isConnected) {
        if (MACH_PORT_VALID(m_sendPort)) {
            ASSERT(!m_isServer);
            deallocateSendRightSafely(m_sendPort);
            m_sendPort = MACH_PORT_NULL;
        }

        if (m_receiveSource) {
            // For a short period of time, when m_isServer is true and open() has been called, m_receiveSource has been initialized
            // but m_isConnected has not been set to true yet. In this case, we need to cancel m_receiveSource instead of destroying
            // m_receivePort ourselves.
            ASSERT(m_isServer);
            cancelReceiveSource();
        }

        if (m_receivePort) {
            ASSERT(m_isServer);
#if !PLATFORM(WATCHOS)
            mach_port_unguard(mach_task_self(), m_receivePort, reinterpret_cast<mach_port_context_t>(this));
#endif
            clearNoSenderNotifications(m_receivePort);
            mach_port_mod_refs(mach_task_self(), m_receivePort, MACH_PORT_RIGHT_RECEIVE, -1);
            m_receivePort = MACH_PORT_NULL;
        }

        return;
    }

    m_pendingOutgoingMachMessage = nullptr;
    m_isConnected = false;

    ASSERT(m_receivePort);

    cancelSendSource();
    cancelReceiveSource();
}

void Connection::cancelSendSource()
{
    m_sendPort = MACH_PORT_NULL;
    if (!m_sendSource)
        return;
    dispatch_source_cancel(m_sendSource.get());
    m_sendSource = nullptr;
}

void Connection::cancelReceiveSource()
{
    dispatch_source_cancel(m_receiveSource.get());
    m_receiveSource = nullptr;
    m_receivePort = MACH_PORT_NULL;
}

void Connection::platformInitialize(Identifier&& identifier)
{
    if (m_isServer) {
        RELEASE_ASSERT(MACH_PORT_VALID(identifier.port)); // Caller error. MACH_DEAD_NAME does not make sense, as we do not transfer receive rights.
        m_receivePort = identifier.port;
#if !PLATFORM(WATCHOS)
        mach_port_guard(mach_task_self(), m_receivePort, reinterpret_cast<mach_port_context_t>(this), true);
#endif
    } else {
        RELEASE_ASSERT(identifier.port != MACH_PORT_NULL);
        // MACH_DEAD_NAME means that the send port got closed while in transit through another connection.
        // Treat it similar to as if we got a valid port but the port got closed immediately after setting up the
        // connection.
        m_sendPort = identifier.port;
    }
    m_xpcConnection = identifier.xpcConnection;
}

void Connection::platformOpen()
{
    if (m_isServer) {
        ASSERT(!m_sendPort);
        // Client passed m_receivePort. Call Client::didClose() when there are no senders to that port.
        requestNoSenderNotifications(m_receivePort);
    } else {
        ASSERT(!m_receivePort);
        auto kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &m_receivePort);
        if (kr != KERN_SUCCESS) {
            LOG_ERROR("Could not allocate mach port, error %x: %s", kr, mach_error_string(kr));
            CRASH();
        }
#if !PLATFORM(WATCHOS)
        mach_port_guard(mach_task_self(), m_receivePort, reinterpret_cast<mach_port_context_t>(this), true);
#endif

#if PLATFORM(MAC)
        mach_port_set_attributes(mach_task_self(), m_receivePort, MACH_PORT_DENAP_RECEIVER, (mach_port_info_t)0, 0);
#endif

        m_isConnected = true;

        // Send the initialize message, which contains a send right for the server to use.
        auto serverSendRight = MachSendRight::createFromReceiveRight(m_receivePort);

        // Call Client::didClose() when the serverSendRight gets destroyed.
        requestNoSenderNotifications(m_receivePort);

        initializeSendSource();
        if (m_sendPort != MACH_PORT_DEAD) {
            auto encoder = makeUniqueRef<Encoder>(MessageName::InitializeConnection, 0);
            encoder.get() << WTFMove(serverSendRight);
            sendMessage(WTFMove(encoder), { });
        }
        // When send port is already dead, the serverSendRight goes out of scope and triggers
        // MACH_NOTIFY_NO_SENDERS. This way the connectionDidClose logic will be invoked for
        // dead-on-arrival connections.
    }

    // Change the message queue length for the receive port.
    setMachPortQueueLength(m_receivePort, largeOutgoingMessageQueueCountThreshold);

    m_receiveSource = adoptOSObject(dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_RECV, m_receivePort, 0, m_connectionQueue->dispatchQueue()));
    dispatch_source_set_event_handler(m_receiveSource.get(), [this, protectedThis = Ref { *this }] {
        receiveSourceEventHandler();
    });
    dispatch_source_set_cancel_handler(m_receiveSource.get(), [protectedThis = Ref { *this }, receivePort = m_receivePort] {
#if !PLATFORM(WATCHOS)
        mach_port_unguard(mach_task_self(), receivePort, reinterpret_cast<mach_port_context_t>(protectedThis.ptr()));
#endif
        clearNoSenderNotifications(receivePort);
        mach_port_mod_refs(mach_task_self(), receivePort, MACH_PORT_RIGHT_RECEIVE, -1);
    });

    m_connectionQueue->dispatch([strongRef = Ref { *this }, this] {
        dispatch_resume(m_receiveSource.get());
    });

    // Cache the audit token in case the XPC connection will be closed.
    getAuditToken();
}

bool Connection::sendMessage(std::unique_ptr<MachMessage> message)
{
    ASSERT(message);
    ASSERT(!m_pendingOutgoingMachMessage);
    // Send the message.
    kern_return_t kr = mach_msg(message->header(), MACH_SEND_MSG | MACH_SEND_TIMEOUT | MACH_SEND_NOTIFY, message->size(), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    switch (kr) {
    case MACH_MSG_SUCCESS:
        // The kernel has already adopted the descriptors.
        message->leakDescriptors();
        return true;

    case MACH_SEND_TIMED_OUT:
        // We timed out, stash away the message for later.
        m_pendingOutgoingMachMessage = WTFMove(message);
        return false;

    case MACH_SEND_INVALID_DEST:
        // The other end has destroyed the receive right to the port we are trying to send to.
        // Cancel the send source, so that we do not try to send more messages needlessly.
        cancelSendSource();

        // We do not yet invalidate this instance. When the send right to the port of this instance is
        // destroyed, this instance gets a NO_SENDERS notification which will cause this instance invalidation.
        // Noteworthy special case:
        // InitializeConnection message will hold our send right. If that send fails here, we will destroy
        // the send right inside the `message`that goes out of scope, and thus we get the NO_SENDERS.
        return false;

#if ENABLE(IPC_TESTING_API)
    case MACH_SEND_TOO_LARGE:
        RELEASE_LOG_ERROR(Process, "%" PUBLIC_LOG_STRING "Error MACH_SEND_TOO_LARGE", WTF_PRETTY_FUNCTION);
        return false;
#endif

    default:
        auto messageName = message->messageName();
        auto errorMessage = makeString("Unhandled error code 0x"_s, hex(kr), ", message '"_s, description(messageName), "' ("_s, messageName, ')');
        WebKit::logAndSetCrashLogMessage(errorMessage.utf8().data());
        CRASH_WITH_INFO(kr, WTF::enumToUnderlyingType(messageName));
    }
}

bool Connection::platformCanSendOutgoingMessages() const
{
    return !m_pendingOutgoingMachMessage && MACH_PORT_VALID(m_sendPort);
}

template<typename descriptorType>
static descriptorType& popDescriptorAndAdvance(std::span<uint8_t>& data)
{
    return consumeAndReinterpretCastTo<descriptorType>(data);
}

bool Connection::sendOutgoingMessage(UniqueRef<Encoder>&& encoder)
{
    ASSERT(canSendOutgoingMessages());

    auto attachments = encoder->releaseAttachments();
    auto numberOfPortDescriptors = attachments.size();

    bool messageBodyIsOOL = false;
    auto messageSize = MachMessage::messageSize(encoder->span().size(), numberOfPortDescriptors, messageBodyIsOOL);
    if (messageSize.hasOverflowed()) [[unlikely]]
        return false;

    if (messageSize > inlineMessageMaxSize) {
        messageBodyIsOOL = true;
        messageSize = MachMessage::messageSize(0, numberOfPortDescriptors, messageBodyIsOOL);
        if (messageSize.hasOverflowed()) [[unlikely]]
            return false;
    }

    size_t safeMessageSize = messageSize;
    auto message = MachMessage::create(encoder->messageName(), safeMessageSize);
    if (!message)
        return false;

    auto messageSpan = message->span();
    auto& header = consumeAndReinterpretCastTo<mach_msg_header_t>(messageSpan);
    header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    header.msgh_size = safeMessageSize;
    header.msgh_remote_port = m_sendPort;
    header.msgh_local_port = MACH_PORT_NULL;
    header.msgh_id = messageBodyIsOOL ? outOfLineBodyMessageID : inlineBodyMessageID;

    bool isComplex = numberOfPortDescriptors || messageBodyIsOOL;
    if (isComplex) {
        header.msgh_bits |= MACH_MSGH_BITS_COMPLEX;

        auto& body = consumeAndReinterpretCastTo<mach_msg_body_t>(messageSpan);
        body.msgh_descriptor_count = numberOfPortDescriptors + messageBodyIsOOL;

        for (auto& attachment : attachments) {
            auto& descriptor = popDescriptorAndAdvance<mach_msg_port_descriptor_t>(messageSpan);
            descriptor.name = attachment.leakSendRight();
            descriptor.disposition = MACH_MSG_TYPE_MOVE_SEND;
            descriptor.type = MACH_MSG_PORT_DESCRIPTOR;
        }

        if (messageBodyIsOOL) {
            auto& descriptor = popDescriptorAndAdvance<mach_msg_ool_descriptor_t>(messageSpan);
            auto buffer = encoder->mutableSpan();
            descriptor.address = buffer.data();
            descriptor.size = buffer.size();
            descriptor.copy = MACH_MSG_VIRTUAL_COPY;
            descriptor.deallocate = false;
            descriptor.type = MACH_MSG_OOL_DESCRIPTOR;
        }
    }

    // Copy the data if it is not being sent out-of-line.
    if (!messageBodyIsOOL)
        memcpySpan(messageSpan, encoder->span());

    return sendMessage(WTFMove(message));
}

void Connection::initializeSendSource()
{
    ASSERT(m_isConnected);
    if (m_sendPort == MACH_PORT_DEAD)
        return;
    RELEASE_ASSERT(m_sendPort != MACH_PORT_NULL);

    m_sendSource = adoptOSObject(dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_SEND, m_sendPort, DISPATCH_MACH_SEND_POSSIBLE, m_connectionQueue->dispatchQueue()));
    dispatch_source_set_registration_handler(m_sendSource.get(), [this, protectedThis = Ref { *this }] {
        if (!m_sendSource)
            return;
        resumeSendSource();
    });
    dispatch_source_set_event_handler(m_sendSource.get(), [this, protectedThis = Ref { *this }] {
        if (!m_sendSource)
            return;

        unsigned long data = dispatch_source_get_data(m_sendSource.get());

        if (data & DISPATCH_MACH_SEND_POSSIBLE) {
            // FIXME: Figure out why we get spurious DISPATCH_MACH_SEND_POSSIBLE events.
            resumeSendSource();
            return;
        }
    });

    mach_port_t sendPort = m_sendPort;
    dispatch_source_set_cancel_handler(m_sendSource.get(), ^{
        // Release our send right.
        deallocateSendRightSafely(sendPort);
    });
    dispatch_resume(m_sendSource.get());
}

void Connection::resumeSendSource()
{
    if (m_pendingOutgoingMachMessage)
        sendMessage(WTFMove(m_pendingOutgoingMachMessage));
    sendOutgoingMessages();
}

static std::unique_ptr<Decoder> createMessageDecoder(mach_msg_header_t* header, std::span<uint8_t> message)
{
    if (header->msgh_size > message.size()) [[unlikely]] {
        RELEASE_LOG_FAULT(IPC, "createMessageDecoder: msgh_size is greater than bufferSize (header->msgh_size: %lu, bufferSize: %lu)", static_cast<unsigned long>(header->msgh_size), message.size());
        ASSERT_NOT_REACHED();
        return nullptr;
    }

    auto remaining = message.subspan(sizeof(mach_msg_header_t));
    if (!(header->msgh_bits & MACH_MSGH_BITS_COMPLEX)) {
        // We have a simple message.
        auto bodySize = CheckedSize { header->msgh_size } - sizeof(mach_msg_header_t);
        if (bodySize.hasOverflowed()) [[unlikely]] {
            RELEASE_LOG_FAULT(IPC, "createMessageDecoder: Overflow when computing bodySize (header->msgh_size: %lu, sizeof(mach_msg_header_t): %lu)", static_cast<unsigned long>(header->msgh_size), sizeof(mach_msg_header_t));
            ASSERT_NOT_REACHED();
            return nullptr;
        }

        return Decoder::create(remaining.first(bodySize), { });
    }

    auto& body = consumeAndReinterpretCastTo<mach_msg_body_t>(remaining);
    mach_msg_size_t numberOfPortDescriptors = body.msgh_descriptor_count;
    ASSERT(numberOfPortDescriptors);
    if (!numberOfPortDescriptors) [[unlikely]]
        return nullptr;

    auto sizeWithPortDescriptors = CheckedSize { sizeof(mach_msg_header_t) + sizeof(mach_msg_body_t) } + CheckedSize { numberOfPortDescriptors } * sizeof(mach_msg_port_descriptor_t);
    if (sizeWithPortDescriptors.hasOverflowed() || sizeWithPortDescriptors.value() > message.size()) [[unlikely]] {
        RELEASE_LOG_FAULT(IPC, "createMessageDecoder: Overflow when computing sizeWithPortDescriptors (numberOfPortDescriptors: %lu)", static_cast<unsigned long>(numberOfPortDescriptors));
        ASSERT_NOT_REACHED();
        return nullptr;
    }

    // If the message body was sent out-of-line, don't treat the last descriptor
    // as an attachment, since it is really the message body.
    bool messageBodyIsOOL = header->msgh_id == outOfLineBodyMessageID;
    mach_msg_size_t numberOfAttachments = messageBodyIsOOL ? numberOfPortDescriptors - 1 : numberOfPortDescriptors;

    // Build attachment list
    Vector<Attachment> attachments(numberOfAttachments);

    for (mach_msg_size_t i = 0; i < numberOfAttachments; ++i) {
        auto& descriptor = consumeAndReinterpretCastTo<mach_msg_port_descriptor_t>(remaining);
        ASSERT(descriptor.type == MACH_MSG_PORT_DESCRIPTOR);
        if (descriptor.type != MACH_MSG_PORT_DESCRIPTOR)
            return nullptr;
        ASSERT(descriptor.disposition == MACH_MSG_TYPE_PORT_SEND);
        MachSendRight right = MachSendRight::adopt(descriptor.name);

        attachments[numberOfAttachments - i - 1] = Attachment { WTFMove(right) };
    }

    if (messageBodyIsOOL) {
        auto& descriptor = reinterpretCastSpanStartTo<mach_msg_descriptor_t>(remaining);
        ASSERT(descriptor.type.type == MACH_MSG_OOL_DESCRIPTOR);
        if (descriptor.type.type != MACH_MSG_OOL_DESCRIPTOR)
            return nullptr;

        uint8_t* messageBody = static_cast<uint8_t*>(descriptor.out_of_line.address);
        size_t messageBodySize = descriptor.out_of_line.size;
        descriptor.out_of_line.deallocate = false; // We are taking ownership of the memory.

        return Decoder::create(unsafeMakeSpan(messageBody, messageBodySize), [](auto buffer) {
            // FIXME: <rdar://problem/62086358> bufferDeallocator block ignores mach_msg_ool_descriptor_t->deallocate
            vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(buffer.data()), buffer.size_bytes());
        }, WTFMove(attachments));
    }

    ASSERT(std::to_address(message.subspan(sizeWithPortDescriptors.value()).begin()) == std::to_address(remaining.begin()));
    auto messageBodySize = header->msgh_size - sizeWithPortDescriptors;
    if (messageBodySize.hasOverflowed()) [[unlikely]] {
        RELEASE_LOG_FAULT(IPC, "createMessageDecoder: Overflow when computing bodySize (header->msgh_size: %lu, sizeWithPortDescriptors: %lu)", static_cast<unsigned long>(header->msgh_size), static_cast<unsigned long>(sizeWithPortDescriptors.value()));
        ASSERT_NOT_REACHED();
        return nullptr;
    }

    return Decoder::create(remaining.first(messageBodySize), WTFMove(attachments));
}

// The receive buffer size should always include the maximum trailer size.
static const size_t receiveBufferSize = inlineMessageMaxSize + MAX_TRAILER_SIZE;
typedef Vector<uint8_t, receiveBufferSize> ReceiveBuffer;

static mach_msg_header_t* readFromMachPort(mach_port_t machPort, ReceiveBuffer& buffer)
{
    ASSERT(MACH_PORT_VALID(machPort));

    buffer.resize(receiveBufferSize);

    auto* header = &reinterpretCastSpanStartTo<mach_msg_header_t>(buffer.mutableSpan());
    kern_return_t kr = mach_msg(header, MACH_RCV_MSG | MACH_RCV_LARGE | MACH_RCV_TIMEOUT | MACH_RCV_VOUCHER, 0, buffer.size(), machPort, 0, MACH_PORT_NULL);
    if (kr == MACH_RCV_TIMED_OUT)
        return nullptr;

    if (kr == MACH_RCV_TOO_LARGE) {
        // The message was too large, resize the buffer and try again.
        auto newBufferSize = checkedSum<size_t>(header->msgh_size, MAX_TRAILER_SIZE);
        if (newBufferSize.hasOverflowed())
            return nullptr;
        buffer.resize(newBufferSize);
        header = &reinterpretCastSpanStartTo<mach_msg_header_t>(buffer.mutableSpan());

        kr = mach_msg(header, MACH_RCV_MSG | MACH_RCV_LARGE | MACH_RCV_TIMEOUT | MACH_RCV_VOUCHER, 0, buffer.size(), machPort, 0, MACH_PORT_NULL);
        ASSERT(kr != MACH_RCV_TOO_LARGE);
    }

    if (kr != MACH_MSG_SUCCESS) {
#if ASSERT_ENABLED
        auto errorMessage = makeString("Unhandled error code 0x"_s, hex(kr), " from mach_msg, receive port is 0x"_s, hex(machPort));
        WebKit::logAndSetCrashLogMessage(errorMessage.utf8().data());
#endif
        ASSERT_NOT_REACHED();
        return nullptr;
    }

    return header;
}

static bool shouldLogIncomingMessageHandling()
{
    static dispatch_once_t once;
    static bool shouldLog;

    dispatch_once(&once, ^{
        shouldLog = !!getenv("WEBKIT_LOG_INCOMING_MESSAGES");
    });

    return shouldLog;
}

void Connection::receiveSourceEventHandler()
{
    ReceiveBuffer buffer;

    ASSERT(MACH_PORT_VALID(m_receivePort));
    mach_msg_header_t* header = readFromMachPort(m_receivePort, buffer);
    if (!header)
        return;

    switch (header->msgh_id) {
    case MACH_NOTIFY_NO_SENDERS:
        connectionDidClose();
        return;

    case inlineBodyMessageID:
    case outOfLineBodyMessageID:
        break;

    case MACH_NOTIFY_SEND_ONCE:
    default:
        return;
    }

    std::unique_ptr<Decoder> decoder = createMessageDecoder(header, buffer.mutableSpan());
    if (!decoder)
        return;

#if PLATFORM(MAC)
    decoder->setImportanceAssertion(ImportanceAssertion { header });
#endif

    if (decoder->messageName() == MessageName::InitializeConnection) {
        ASSERT(m_isServer);
        ASSERT(!m_sendPort);
        if (m_isConnected) {
            // The sender sent an invalid message deliberately, close immediately.
            ASSERT_IS_TESTING_IPC();
            connectionDidClose();
            return;
        }
        auto sendRight = decoder->decode<MachSendRight>();
        if (!sendRight) {
            // The sender sent an invalid message deliberately, close immediately.
            ASSERT_IS_TESTING_IPC();
            connectionDidClose();
            return;
        }

        m_isConnected = true;

        if (!MACH_PORT_VALID(sendRight->sendRight())) {
            // The InitializeConnection message was valid message. We received MACH_PORT_DEAD
            // because by the time we read the message, the port was already closed.
            // Do not initialize the send source, as there is nobody to send to.
            // Keep the receive source, so that we receive the sent messages and then
            // the NO_SENDERS notification.
            return;
        }
        m_sendPort = sendRight->leakSendRight();
        initializeSendSource();
        return;
    }

    if (shouldLogIncomingMessageHandling()) [[unlikely]]
        RELEASE_LOG(IPCMessages, "Connection::processIncomingMessage(%p) received %" PUBLIC_LOG_STRING " from port 0x%08x", this, description(decoder->messageName()).characters(), m_receivePort);

    processIncomingMessage(makeUniqueRefFromNonNullUniquePtr(WTFMove(decoder)));
}

IPC::Connection::Identifier Connection::identifier() const
{
    return Identifier(m_isServer ? m_receivePort : m_sendPort, m_xpcConnection);
}

std::optional<audit_token_t> Connection::getAuditToken()
{
    if (m_auditToken)
        return m_auditToken;

    if (!m_xpcConnection)
        return std::nullopt;

    audit_token_t auditToken;
    xpc_connection_get_audit_token(m_xpcConnection.get(), &auditToken);
    m_auditToken = auditToken;
    return WTFMove(auditToken);
}

#if !USE(EXTENSIONKIT_PROCESS_TERMINATION)
bool Connection::kill()
{
    if (m_xpcConnection) {
        terminateWithReason(m_xpcConnection.get(), WebKit::ReasonCode::ConnectionKilled, "Connection::kill");
        m_didRequestProcessTermination = true;
        return true;
    }
    return false;
}
#endif

pid_t Connection::remoteProcessID() const
{
    if (!m_xpcConnection)
        return 0;

    return xpc_connection_get_pid(m_xpcConnection.get());
}

std::optional<Connection::ConnectionIdentifierPair> Connection::createConnectionIdentifierPair()
{
    // Create the listening port.
    mach_port_t listeningPort = MACH_PORT_NULL;
    auto kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &listeningPort);
    if (kr != KERN_SUCCESS) {
        RELEASE_LOG_ERROR(Process, "Connection::createConnectionIdentifierPair: Could not allocate mach port, error %x", kr);
        return std::nullopt;
    }
    if (!MACH_PORT_VALID(listeningPort)) {
        RELEASE_LOG_ERROR(Process, "Connection::createConnectionIdentifierPair: Could not allocate mach port, returned port was invalid");
        return std::nullopt;
    }
    return ConnectionIdentifierPair { Identifier { listeningPort, nullptr }, MachSendRight::createFromReceiveRight(listeningPort) };
}

} // namespace IPC
