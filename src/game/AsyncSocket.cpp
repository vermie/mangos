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

#include "AsyncSocket.h"
#include "ProactorMgr.h"
#include "WorldPacket.h"
#include "ProactorRunnable.h"

#include "Utilities/ByteConverter.h"
#include "Opcodes.h"

#include <ace/os_include/netinet/os_tcp.h>

#define LOW_WATERMARK 1024u

#define LOCK_SEND_RETURN(retval)                                \
    ACE_GUARD_REACTION                                          \
    (                                                           \
        LockType, Guard, m_OutBufferLock,                       \
        sLog.outError("Could not acquire send lock for %s", GetRemoteAddress().c_str());    \
        return retval;                                          \
    )

#define LOCK_SEND() LOCK_SEND_RETURN((void)0)

AsyncSocket::AsyncSocket() :
    WorldSocket(),
    m_acceptor(NULL),
    m_receiveBuffer(NULL),
    m_sendBuffer(NULL),
    m_busyBuffer(NULL),
    m_header(NULL),
    m_writeMode(IDLE)
{
}

AsyncSocket::AsyncSocket(AsyncAcceptor *acceptor) :
    WorldSocket(),
    m_acceptor(acceptor),
    m_receiveBuffer(NULL),
    m_sendBuffer(NULL),
    m_busyBuffer(NULL),
    m_header(NULL),
    m_writeMode(IDLE)
{
}

AsyncSocket::~AsyncSocket()
{
    Close();

    delete m_header;

    delete m_receiveBuffer;
    delete m_sendBuffer;
    delete m_busyBuffer;
}

void AsyncSocket::Close()
{
    if (!m_open) return;

    WorldSocket::Close();

    m_reader.cancel();
    m_writer.cancel();
    sProactorMgr->Remove(proactor());

    ACE_OS::closesocket(handle());
}

void AsyncSocket::addresses(ACE_INET_Addr &remoteAddress, ACE_INET_Addr &localAddress)
{
    m_Address = remoteAddress.get_host_addr();
}

void AsyncSocket::open(ACE_HANDLE handle, ACE_Message_Block &message)
{
    // set socket options first
    if (m_acceptor->m_SockOutKBuff >= 0)
        if (ACE_OS::setsockopt(handle, SOL_SOCKET, SO_SNDBUF, (char*) &m_acceptor->m_SockOutKBuff, sizeof(int)) == -1)
            sLog.outError("AsyncAcceptor::handle_accept SO_SNDBUF %s (%d)", ACE_OS::strerror(errno), errno);

    int ndoption = 1;
    if (m_acceptor->m_TcpNodelay)
        if (ACE_OS::setsockopt(handle, ACE_IPPROTO_TCP, TCP_NODELAY, (char*)&ndoption, sizeof(int)) == -1)
            sLog.outError("AsyncAcceptor::handle_accept TCP_NODELAY: %s (%d)", ACE_OS::strerror(errno), errno);

    // first get the proactor+runner from the manager
    sProactorMgr->Add(this);
    // now open async read/write streams
    m_reader.open(*this, handle, NULL, proactor());
    m_writer.open(*this, handle, NULL, proactor());

    m_open = true;
    m_receiveBuffer = new ACE_Message_Block(65536);
    m_sendBuffer = new ACE_Message_Block(m_acceptor->m_SockOutUBuff);
    m_busyBuffer = new ACE_Message_Block(m_acceptor->m_SockOutUBuff);

    // safer to send first
    SendAuthChallenge();

    // begin ansynchronous read
    if (!m_runner->EnqueueRead(this))
        return;
}

/************** read **************/

// this function should only be called by ProactorRunnable
bool AsyncSocket::BeginRead()
{
    // return true only if an async operation started successfully
    if (!m_open) return false;

    if (m_reader.read(*m_receiveBuffer, m_receiveBuffer->space()) == -1)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            // if we get this error even with our own queue, need to stop using async io
            sLog.outError("Your platform has poor support for asynchronous IO - switch to Network.OldEngine = 1 in mangosd.conf");

        if (errno != ECANCELED)
        {
            sLog.outError("AsyncSocket::BeginRead: %s (%d)", ACE_OS::strerror(errno), errno);
            Close();
        }

        return false;
    }

    return true;
}

void AsyncSocket::handle_read_stream(const ACE_Asynch_Read_Stream::Result &result)
{
    m_runner->DequeueOp();

    if (!m_open) return;

    if (!result.success())
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            m_runner->EnqueueRead(this);    // these indicate that the data couldn't be sent immediately, and we must try again
        else
        {
            // unrecoverable error
            if (errno != ECANCELED)
                sLog.outError("AsyncSocket::handle_read_stream: %s (%d)", ACE_OS::strerror(errno), errno);

            Close();
        }

        return;
    }
    else if (result.bytes_transferred() == 0)
    {
        // client disconnected
        Close();
        return;
    }

    ClientPktHeader header;
    while (m_header || GetNextHeader(header))
        if (WorldPacket* packet = GetNextPacket(m_header ? m_header : &header))
            ProcessIncoming(packet);
        else
            break;

    // reset if we don't have to move data
    if (m_receiveBuffer->length() == 0)
        m_receiveBuffer->reset();
    // else crunch if we need the space, or to prevent wastefully small reads
    else if ((m_header && (m_header->size > (m_receiveBuffer->length() + m_receiveBuffer->space())))
        || m_receiveBuffer->space() < LOW_WATERMARK)
        m_receiveBuffer->crunch();

    m_runner->EnqueueRead(this);
}

bool AsyncSocket::GetNextHeader(ClientPktHeader &header)
{
    if (m_receiveBuffer->length() < sizeof(ClientPktHeader))
        return false;

    // consume header from message block
    header = *(ClientPktHeader*)m_receiveBuffer->rd_ptr();
    m_receiveBuffer->rd_ptr(sizeof(ClientPktHeader));

    // decrypt and fixup header
    m_Crypt.DecryptRecv((uint8*)&header, sizeof(ClientPktHeader));
    EndianConvertReverse(header.size);
    EndianConvert(header.cmd);
    header.size -= 4;

    return true;
}

WorldPacket* AsyncSocket::GetNextPacket(ClientPktHeader* header)
{
    if (m_receiveBuffer->length() < header->size)
    {
        // not done reading this message yet
        // store the header
        m_header = new ClientPktHeader(*header);
        return NULL;
    }

    WorldPacket* packet = new WorldPacket(header->cmd, header->size);
    if (header->size)
    {
        packet->resize(header->size);
        packet->put(0, (uint8*)m_receiveBuffer->rd_ptr(), header->size);
        m_receiveBuffer->rd_ptr(header->size);
    }

    // if this was a stored header, we don't need it anymore
    if (m_header)
    {
        delete m_header;
        m_header = NULL;
    }

    return packet;
}

/************** write **************/

// this function should only be called by ProactorRunnable
bool AsyncSocket::BeginWrite()
{
    // return true only if an async operation started successfully
    if (!m_open) return false;

    // only write from busyBuffer - sendBuffer and swaps handled elsewhere
    if (m_writer.write(*m_busyBuffer, m_busyBuffer->length()) == -1)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            // if we get this error even with our own queue, need to stop using async io
            sLog.outError("Your platform has poor support for asynchronous IO - switch to Network.OldEngine = 1 in mangosd.conf");

        if (errno != ECANCELED)
        {
            sLog.outError("AsyncSocket::BeginWrite: %s (%d)", ACE_OS::strerror(errno), errno);
            Close();
        }

        return false;
    }

    return true;
}

int AsyncSocket::SendPacket(const WorldPacket &packet)
{
    sLog.outWorldPacketDump(uint32(handle()), packet.GetOpcode(), LookupOpcodeName(packet.GetOpcode()), &packet, false);

    if (!m_open) return 0;

    {
        LOCK_SEND_RETURN(-1);
        // packets must be buffered/sent in the same order they were encrypted
        // critical section ensures that this is the case

        ServerPktHeader header(packet.size()+2, packet.GetOpcode());
        m_Crypt.EncryptSend((uint8*)header.header, header.getHeaderLength());

        size_t size = header.getHeaderLength() + packet.size();

        // make sure the buffer is large enough
        if (m_sendBuffer->space() < size)
            if (m_sendBuffer->size(m_sendBuffer->wr_ptr() - m_sendBuffer->base() + size) == -1)
                MANGOS_ASSERT(false);

        // Put the packet on the buffer.
        if (m_sendBuffer->copy((char*)header.header, header.getHeaderLength()) == -1)
            MANGOS_ASSERT(false);

        if (!packet.empty())
            if (m_sendBuffer->copy((char*)packet.contents(), packet.size()) == -1)
                MANGOS_ASSERT(false);

        // cannot initiate concurrent sends
        // if there is a send operation pending, we must leave the packet in the buffer and send it later
        if (m_writeMode == SENDING)
            return 0;

        m_writeMode = SENDING;

        std::swap(m_busyBuffer, m_sendBuffer);
        m_sendBuffer->reset();

        // try send packet now
        return m_runner->EnqueueWrite(this) ? 0 : -1;
    }
}

void AsyncSocket::handle_write_stream(const ACE_Asynch_Write_Stream::Result &result)
{
    m_runner->DequeueOp();

    if (!m_open) return;

    if (!result.success() && !(errno == EAGAIN || errno == EWOULDBLOCK))
    {
        if (errno != ECANCELED)
            sLog.outError("AsyncSocket::handle_write_stream %s (%d)", ACE_OS::strerror(errno), errno);

        Close();
        return;
    }

    // continue writing until buffer is empty
    if (result.bytes_to_write() > result.bytes_transferred())
    {
        m_runner->EnqueueWrite(this);
        return;
    }

    {
        LOCK_SEND();

        if (m_sendBuffer->length() > 0)
        {
            // there is data waiting to be sent
            // swap the buffers and send

            std::swap(m_busyBuffer, m_sendBuffer);
            m_sendBuffer->reset();

            m_runner->EnqueueWrite(this);
        }
        else
            m_writeMode = IDLE;
    }
}
