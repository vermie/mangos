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

#ifndef _ASYNC_SOCKET_H
#define _ASYNC_SOCKET_H

#include "WorldSocket.h"
#include "AsyncAcceptor.h"

class ProactorMgr;
class ProactorRunnable;

class AsyncSocket : public WorldSocket, public ACE_Service_Handler
{
public:
    ~AsyncSocket();

    virtual bool IsClosed() const;
    void Close();

    int SendPacket(const WorldPacket &packet);

    // called by ACE
    AsyncSocket();
    AsyncSocket(AsyncAcceptor *acceptor);
    virtual void addresses(ACE_INET_Addr &remoteAddress, ACE_INET_Addr &localAddress);
    virtual void open(ACE_HANDLE handle, ACE_Message_Block &message);

protected:
    friend class ProactorMgr;
    friend class ProactorRunnable;

    virtual uint32 socketHandle() { return uint32(handle()); }
    ProactorRunnable* m_runner;

    enum PendingOp { WRITE = 1, READ = 2 };
    uint32 m_pending;

    void Unrecoverable(const char* message);

    // network in
    AsyncAcceptor *m_acceptor;
    ACE_Message_Block *m_receiveBuffer;
    ACE_Asynch_Read_Stream m_reader;
    ClientPktHeader *m_header;

    bool GetNextHeader(ClientPktHeader &header);
    WorldPacket* GetNextPacket(ClientPktHeader* header);

    virtual void handle_read_stream(const ACE_Asynch_Read_Stream::Result &result);
    bool BeginRead();

    // network out
    enum WriteMode { IDLE, SENDING };

    ACE_Asynch_Write_Stream m_writer;
    WriteMode m_writeMode;
    LockType m_OutBufferLock;
    ACE_Message_Block* m_sendBuffer;
    ACE_Message_Block* m_busyBuffer;

    virtual void handle_write_stream(const ACE_Asynch_Write_Stream::Result &result);
    bool BeginWrite();
};

#endif
