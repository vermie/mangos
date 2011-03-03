/*
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/** \addtogroup u2w User to World Communication
 * @{
 * \file WorldSocket.h
 * \author Derex <derex101@gmail.com>
 */

#ifndef _WORLDSOCKET_H
#define _WORLDSOCKET_H

#include <ace/Basic_Types.h>
#include <ace/Synch_Traits.h>
#include <ace/Svc_Handler.h>
#include <ace/Thread_Mutex.h>
#include <ace/Guard_T.h>
#include <ace/Unbounded_Queue.h>
#include <ace/Message_Block.h>

#if !defined (ACE_LACKS_PRAGMA_ONCE)
#pragma once
#endif /* ACE_LACKS_PRAGMA_ONCE */

#include "Common.h"
#include "Auth/AuthCrypt.h"
#include "Auth/BigNumber.h"
#include "Log.h"

class ACE_Message_Block;
class WorldPacket;
class WorldSession;

#if defined( __GNUC__ )
#pragma pack(1)
#else
#pragma pack(push,1)
#endif

struct ServerPktHeader
{
    /**
     * size is the length of the payload _plus_ the length of the opcode
     */
    ServerPktHeader(uint32 size, uint16 cmd) : size(size)
    {
        uint8 headerIndex=0;
        if(isLargePacket())
        {
            DEBUG_LOG("initializing large server to client packet. Size: %u, cmd: %u", size, cmd);
            header[headerIndex++] = 0x80|(0xFF &(size>>16));
        }
        header[headerIndex++] = 0xFF &(size>>8);
        header[headerIndex++] = 0xFF &size;

        header[headerIndex++] = 0xFF & cmd;
        header[headerIndex++] = 0xFF & (cmd>>8);
    }

    uint8 getHeaderLength()
    {
        // cmd = 2 bytes, size= 2||3bytes
        return 2+(isLargePacket()?3:2);
    }

    bool isLargePacket()
    {
        return size > 0x7FFF;
    }

    const uint32 size;
    uint8 header[5];
};

struct ClientPktHeader
{
    uint16 size;
    uint32 cmd;
};

#if defined( __GNUC__ )
#pragma pack()
#else
#pragma pack(pop)
#endif

class WorldSocket
{
public:
    /// Mutex type used for various synchronizations.
    typedef ACE_Thread_Mutex LockType;
    typedef ACE_Guard<LockType> GuardType;

    /// Do-nothing ctor
    WorldSocket();

    /// Check if socket is closed.
    virtual bool IsClosed() const { return !m_open; }
    bool IsOpen() const { return m_open; }

    /// Close the socket.
    virtual void Close();

    /// Get address of connected peer.
    const std::string& GetRemoteAddress() const { return m_Address; }

    /// Send A packet on the socket, this function is reentrant.
    /// @param pct packet to send
    /// @return -1 of failure
    virtual int SendPacket(const WorldPacket& pct) = 0;

    /// Return the session key
    BigNumber& GetSessionKey() { return m_s; }

protected:
    /// process one incoming packet.
    /// @param new_pct received packet ,note that you need to delete it.
    int ProcessIncoming(WorldPacket* new_pct);

    /// Called by ProcessIncoming() on CMSG_AUTH_SESSION.
    int HandleAuthSession(WorldPacket& recvPacket);

    /// Called by ProcessIncoming() on CMSG_PING.
    int HandlePing(WorldPacket& recvPacket);

    /// Initiates auth protocol
    bool SendAuthChallenge();

    virtual uint32 socketHandle() = 0;

    /// Time in which the last ping was received
    ACE_Time_Value m_LastPingTime;

    /// Keep track of over-speed pings ,to prevent ping flood.
    uint32 m_OverSpeedPings;

    /// Address of the remote peer
    std::string m_Address;

    /// Class used for managing encryption of the headers
    AuthCrypt m_Crypt;

    /// Mutex lock to protect m_Session
    LockType m_SessionLock;

    /// Session to which received packets are routed
    WorldSession* m_Session;

    /// Session key
    BigNumber m_s;

    /// Crypto seed
    uint32 m_Seed;

    /// socket status
    bool m_open;
};

#endif  /* _WORLDSOCKET_H */

/// @}
