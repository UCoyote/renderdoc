/*
 *******************************************************************************
 *
 * Copyright (c) 2017 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "baseProtocolClient.h"
#include "msgChannel.h"

namespace DevDriver
{
    BaseProtocolClient::BaseProtocolClient(IMsgChannel* pMsgChannel, Protocol protocol, Version minVersion, Version maxVersion)
        : m_pMsgChannel(pMsgChannel)
        , m_protocol(protocol)
        , m_minVersion(minVersion)
        , m_maxVersion(maxVersion)
        , m_pSession()
        , m_pendingOperationEvent(false)
        , m_connectResult(Result::Error)
        , m_state(ClientState::Disconnected)
    {
        DD_ASSERT(m_pMsgChannel != nullptr);
    }

    bool BaseProtocolClient::IsConnected() const
    {
        return (m_state == ClientState::Connected);
    }

    // Orphans the current session associated with the client object and moves to the disconnected state
    // This function is intended for situations where the external code knows the remote client has disconnected
    // before the message channel subsystem. In this type of situation, orphaning the client instead of using a regular
    // disconnect call can avoid the delay/timeout that occurs when the message channel subsystem attempts to disconnect
    // gracefully.
    void BaseProtocolClient::Orphan()
    {
        if (!m_pSession.IsNull())
        {
            m_pSession->OrphanSession();
            m_pSession->CloseSession(Result::Success);
            m_pSession.Clear();
        }

        m_state = ClientState::Disconnected;
    }

    ClientId BaseProtocolClient::GetRemoteClientId() const
    {
        if (!m_pSession.IsNull())
        {
            return m_pSession->GetDestinationClientId();
        }
        return 0;
    }

    BaseProtocolClient::~BaseProtocolClient()
    {
        if (!m_pSession.IsNull())
        {
            m_pSession->OrphanSession();
            m_pSession->CloseSession(Result::Success);
            m_pSession.Clear();
        }
    }

    Version BaseProtocolClient::GetSessionVersion() const
    {
        Version version = 0;
        if (!m_pSession.IsNull())
        {
            version = m_pSession->GetVersion();
        }
        return version;
    }

    void BaseProtocolClient::SessionEstablished(const SharedPointer<ISession>& pSession)
    {

        // We should never be overwriting an existing session pointer here.
        DD_ASSERT(m_pSession.IsNull());

        m_state = ClientState::Connected;
        m_connectResult = Result::Success;
        m_pSession = pSession;

        m_pendingOperationEvent.Signal();
    }

    void BaseProtocolClient::UpdateSession(const SharedPointer<ISession>& pSession)
    {
        DD_UNUSED(pSession);

        // Do nothing by default
    }

    void BaseProtocolClient::SessionTerminated(const SharedPointer<ISession>& pSession, Result terminationReason)
    {
        const bool wasConnecting = (m_state == ClientState::Connecting);
        DD_ASSERT(wasConnecting | (pSession == m_pSession));

        DD_UNUSED(wasConnecting);
        DD_UNUSED(pSession);

        m_state = ClientState::Disconnected;

        // If the session was terminated and we were previously trying to make a connection, then we need to signal
        // the connection finished event to unblock the connecting thread.
        m_connectResult = terminationReason;
        m_pendingOperationEvent.Signal();
        m_pSession.Clear();

        // Always reset the internal client state after the session is terminated.
        ResetState();
    }

    Result BaseProtocolClient::Connect(ClientId clientId)
    {
        Result result = Result::Error;

        if (m_state == ClientState::Disconnected)
        {
            // If a session terminates unexpectedly, we may end up with a valid session object
            // Even in the disconnected state. This dead session object should be deleted. It can't
            // be deleted immediately upon termination because other parts of the client code could
            // still be using it.
            if (!m_pSession.IsNull())
            {
                m_pSession.Clear();

                ResetState();
            }

            DD_ASSERT(m_pMsgChannel != nullptr);

            m_state = ClientState::Connecting;
            m_pendingOperationEvent.Clear();

            result = m_pMsgChannel->EstablishSession(clientId, this);
            if (result == Result::Success)
            {
                // Only wait on the event if we successfully establish the session. If we fail to establish the
                // session, the event will never be signaled.

                // todo - implement more robust timeout system
                m_pendingOperationEvent.Wait(kInfiniteTimeout);
                result = m_connectResult;
            }
            else
            {
                // Restore the state to Disconnected if we fail to establish the session.
                m_state = ClientState::Disconnected;
            }
        }
        return result;
    }

    void BaseProtocolClient::Disconnect()
    {
        ResetState();
        if (IsConnected())
        {
            m_pendingOperationEvent.Clear();
            m_pSession->CloseSession(Result::Success);
            int retries = 20;
            while (!m_pSession.IsNull())
            {
                // todo - implement more robust timeout system
                m_pendingOperationEvent.Wait(kDefaultRetryTimeoutInMs);
                --retries;
                if (retries < 1)
                {
                    // A reference to the session object is not being released and blocking the shutdown process.
                    // Mark the session as being orphaned.
                    // See comments in BaseProtocolClient::Orphan() for more details.
                    Orphan();
                }
            }
        }
    }

} // DevDriver
