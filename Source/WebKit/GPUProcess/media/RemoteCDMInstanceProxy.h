/*
 * Copyright (C) 2020-2025 Apple Inc. All rights reserved.
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

#pragma once

#if ENABLE(GPU_PROCESS) && ENABLE(ENCRYPTED_MEDIA)

#include "Connection.h"
#include "MessageReceiver.h"
#include "RemoteCDMInstanceIdentifier.h"
#include "RemoteCDMInstanceSessionIdentifier.h"
#include "RemoteCDMProxy.h"
#include <WebCore/CDMInstance.h>
#include <wtf/Ref.h>
#include <wtf/RefCounted.h>
#include <wtf/UniqueRef.h>

namespace WebCore {
class CDMInstance;
struct CDMKeySystemConfiguration;
}

namespace IPC {
class SharedBufferReference;
}

namespace WebKit {

struct RemoteCDMInstanceConfiguration;
class RemoteCDMInstanceSessionProxy;

class RemoteCDMInstanceProxy : public WebCore::CDMInstanceClient, private IPC::MessageReceiver, public RefCounted<RemoteCDMInstanceProxy>  {
public:
    USING_CAN_MAKE_WEAKPTR(WebCore::CDMInstanceClient);

    static Ref<RemoteCDMInstanceProxy> create(RemoteCDMProxy&, Ref<WebCore::CDMInstance>&&, RemoteCDMInstanceIdentifier);
    ~RemoteCDMInstanceProxy();

    void ref() const final { RefCounted::ref(); }
    void deref() const final { RefCounted::deref(); }

    const RemoteCDMInstanceConfiguration& configuration() const { return m_configuration.get(); }
    WebCore::CDMInstance& instance() { return m_instance; }
    std::optional<SharedPreferencesForWebProcess> sharedPreferencesForWebProcess() const;

private:
    friend class RemoteCDMFactoryProxy;
    RemoteCDMInstanceProxy(RemoteCDMProxy&, Ref<WebCore::CDMInstance>&&, UniqueRef<RemoteCDMInstanceConfiguration>&&, RemoteCDMInstanceIdentifier);

    // CDMInstanceClient
    void unrequestedInitializationDataReceived(const String&, Ref<WebCore::SharedBuffer>&&) final;
#if !RELEASE_LOG_DISABLED
    const Logger& logger() const final { return m_logger; }
    uint64_t logIdentifier() const final { return m_logIdentifier; }
#endif

    // IPC::MessageReceiver
    void didReceiveMessage(IPC::Connection&, IPC::Decoder&) final;
    bool didReceiveSyncMessage(IPC::Connection&, IPC::Decoder&, UniqueRef<IPC::Encoder>&) final;

    using SuccessValue = WebCore::CDMInstance::SuccessValue;
    using AllowDistinctiveIdentifiers = WebCore::CDMInstance::AllowDistinctiveIdentifiers;
    using AllowPersistentState = WebCore::CDMInstance::AllowPersistentState;

    // Messages
    void initializeWithConfiguration(const WebCore::CDMKeySystemConfiguration&, AllowDistinctiveIdentifiers, AllowPersistentState, CompletionHandler<void(SuccessValue)>&&);
    void setServerCertificate(Ref<WebCore::SharedBuffer>&&, CompletionHandler<void(SuccessValue)>&&);
    void setStorageDirectory(const String&);
    void createSession(uint64_t logIdentifier, CompletionHandler<void(std::optional<RemoteCDMInstanceSessionIdentifier>)>&&);

    RefPtr<RemoteCDMProxy> protectedCdm() const;

    WeakPtr<RemoteCDMProxy> m_cdm;
    const Ref<WebCore::CDMInstance> m_instance;
    const UniqueRef<RemoteCDMInstanceConfiguration> m_configuration;
    RemoteCDMInstanceIdentifier m_identifier;
    HashMap<RemoteCDMInstanceSessionIdentifier, Ref<RemoteCDMInstanceSessionProxy>> m_sessions;

#if !RELEASE_LOG_DISABLED
    const Ref<const Logger> m_logger;
    const uint64_t m_logIdentifier;
#endif
};

}

#endif
