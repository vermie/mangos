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

#include "ReactorSocket.h"
#include "ReactorMgr.h"

#include "WorldPacket.h"
#include "Utilities/ByteConverter.h"
#include "Opcodes.h"

ReactorSocket::ReactorSocket (void) :
    WorldHandler (),
    WorldSocket(),
    m_RecvWPct (0),
    m_RecvPct (),
    m_Header (sizeof (ClientPktHeader)),
    m_OutBuffer (0),
    m_OutBufferSize (65536),
    m_OutActive (false)
{
    reference_counting_policy ().value (ACE_Event_Handler::Reference_Counting_Policy::ENABLED);

    msg_queue()->high_water_mark(8*1024*1024);
    msg_queue()->low_water_mark(8*1024*1024);
}

ReactorSocket::~ReactorSocket (void)
{
    if (m_RecvWPct)
        delete m_RecvWPct;

    if (m_OutBuffer)
        m_OutBuffer->release ();

    Close();
}

bool ReactorSocket::IsClosed() const
{
    return !m_open || closing_;
}

void ReactorSocket::Close(void)
{
    if (!m_open) return;

    WorldSocket::Close();

    {
        ACE_GUARD (LockType, Guard, m_OutBufferLock);

        if (closing_)
            return;

        closing_ = true;
        peer ().close_writer ();
    }
}

int ReactorSocket::SendPacket (const WorldPacket& pct)
{
    ACE_GUARD_RETURN (LockType, Guard, m_OutBufferLock, -1);

    if (IsClosed())
        return -1;

    // Dump outgoing packet.
    sLog.outWorldPacketDump(uint32(get_handle()), pct.GetOpcode(), LookupOpcodeName(pct.GetOpcode()), &pct, false);

    ServerPktHeader header(pct.size()+2, pct.GetOpcode());
    m_Crypt.EncryptSend ((uint8*)header.header, header.getHeaderLength());

    if (m_OutBuffer->space () >= pct.size () + header.getHeaderLength() && msg_queue()->is_empty())
    {
        // Put the packet on the buffer.
        if (m_OutBuffer->copy ((char*) header.header, header.getHeaderLength()) == -1)
            MANGOS_ASSERT (false);

        if (!pct.empty ())
            if (m_OutBuffer->copy ((char*) pct.contents (), pct.size ()) == -1)
                MANGOS_ASSERT (false);
    }
    else
    {
        // Enqueue the packet.
        ACE_Message_Block* mb;

        ACE_NEW_RETURN(mb, ACE_Message_Block(pct.size () + header.getHeaderLength()), -1);

        mb->copy((char*) header.header, header.getHeaderLength());

        if (!pct.empty ())
            mb->copy((const char*)pct.contents(), pct.size ());

        if(msg_queue()->enqueue_tail(mb,(ACE_Time_Value*)&ACE_Time_Value::zero) == -1)
        {
            sLog.outError("ReactorSocket::SendPacket enqueue_tail");
            mb->release();
            return -1;
        }
    }

    return 0;
}

int ReactorSocket::open (void *a)
{
    ACE_UNUSED_ARG (a);

    // Prevent double call to this func.
    if (m_OutBuffer)
        return -1;

    // This will also prevent the socket from being Updated
    // while we are initializing it.
    m_OutActive = true;

    // Hook for the manager.
    if (sReactorMgr->OnSocketOpen (this) == -1)
        return -1;

    // Allocate the buffer.
    ACE_NEW_RETURN (m_OutBuffer, ACE_Message_Block (m_OutBufferSize), -1);

    // Store peer address.
    ACE_INET_Addr remote_addr;

    if (peer ().get_remote_addr (remote_addr) == -1)
    {
        sLog.outError ("ReactorSocket::open: peer ().get_remote_addr errno = %s", ACE_OS::strerror (errno));
        return -1;
    }

    m_Address = remote_addr.get_host_addr ();
    m_open = true;

    if (!SendAuthChallenge())
        return -1;

    // Register with ACE Reactor
    if (reactor ()->register_handler(this, ACE_Event_Handler::READ_MASK | ACE_Event_Handler::WRITE_MASK) == -1)
    {
        sLog.outError ("ReactorSocket::open: unable to register client handler errno = %s", ACE_OS::strerror (errno));
        return -1;
    }

    // reactor takes care of the socket from now on
    remove_reference ();

    return 0;
}

int ReactorSocket::close (int)
{
    shutdown ();

    closing_ = true;

    remove_reference ();

    return 0;
}

int ReactorSocket::handle_input (ACE_HANDLE)
{
    if (closing_)
        return -1;

    switch (handle_input_missing_data ())
    {
        case -1 :
        {
            if ((errno == EWOULDBLOCK) ||
                (errno == EAGAIN))
            {
                return Update ();                           // interesting line ,isn't it ?
            }

            DEBUG_LOG ("ReactorSocket::handle_input: Peer error closing connection errno = %s", ACE_OS::strerror (errno));

            errno = ECONNRESET;
            return -1;
        }
        case 0:
        {
            DEBUG_LOG ("ReactorSocket::handle_input: Peer has closed connection");

            errno = ECONNRESET;
            return -1;
        }
        case 1:
            return 1;
        default:
            return Update ();                               // another interesting line ;)
    }

    ACE_NOTREACHED(return -1);
}

int ReactorSocket::handle_output (ACE_HANDLE)
{
    ACE_GUARD_RETURN (LockType, Guard, m_OutBufferLock, -1);

    if (closing_)
        return -1;

    const size_t send_len = m_OutBuffer->length ();

    if (send_len == 0)
        return handle_output_queue (Guard);

#ifdef MSG_NOSIGNAL
    ssize_t n = peer ().send (m_OutBuffer->rd_ptr (), send_len, MSG_NOSIGNAL);
#else
    ssize_t n = peer ().send (m_OutBuffer->rd_ptr (), send_len);
#endif // MSG_NOSIGNAL

    if (n == 0)
        return -1;
    else if (n == -1)
    {
        if (errno == EWOULDBLOCK || errno == EAGAIN)
            return schedule_wakeup_output (Guard);

        return -1;
    }
    else if (n < (ssize_t)send_len) //now n > 0
    {
        m_OutBuffer->rd_ptr (static_cast<size_t> (n));

        // move the data to the base of the buffer
        m_OutBuffer->crunch ();

        return schedule_wakeup_output (Guard);
    }
    else //now n == send_len
    {
        m_OutBuffer->reset ();

        return handle_output_queue (Guard);
    }

    ACE_NOTREACHED (return 0);
}

int ReactorSocket::handle_output_queue (GuardType& g)
{
    if(msg_queue()->is_empty())
        return cancel_wakeup_output(g);

    ACE_Message_Block *mblk;

    if(msg_queue()->dequeue_head(mblk, (ACE_Time_Value*)&ACE_Time_Value::zero) == -1)
    {
        sLog.outError("ReactorSocket::handle_output_queue dequeue_head");
        return -1;
    }

    const size_t send_len = mblk->length ();

#ifdef MSG_NOSIGNAL
    ssize_t n = peer ().send (mblk->rd_ptr (), send_len, MSG_NOSIGNAL);
#else
    ssize_t n = peer ().send (mblk->rd_ptr (), send_len);
#endif // MSG_NOSIGNAL

    if (n == 0)
    {
        mblk->release();

        return -1;
    }
    else if (n == -1)
    {
        if (errno == EWOULDBLOCK || errno == EAGAIN)
        {
            msg_queue()->enqueue_head(mblk, (ACE_Time_Value*) &ACE_Time_Value::zero);
            return schedule_wakeup_output (g);
        }

        mblk->release();
        return -1;
    }
    else if (n < (ssize_t)send_len) //now n > 0
    {
        mblk->rd_ptr (static_cast<size_t> (n));

        if (msg_queue()->enqueue_head(mblk, (ACE_Time_Value*) &ACE_Time_Value::zero) == -1)
        {
            sLog.outError("ReactorSocket::handle_output_queue enqueue_head");
            mblk->release();
            return -1;
        }

        return schedule_wakeup_output (g);
    }
    else //now n == send_len
    {
        mblk->release();

        return msg_queue()->is_empty() ? cancel_wakeup_output(g) : ACE_Event_Handler::WRITE_MASK;
    }

    ACE_NOTREACHED(return -1);
}

int ReactorSocket::handle_close (ACE_HANDLE h, ACE_Reactor_Mask)
{
    // Critical section
    {
        ACE_GUARD_RETURN (LockType, Guard, m_OutBufferLock, -1);

        closing_ = true;

        if (h == ACE_INVALID_HANDLE)
            peer ().close_writer ();
    }

    // Critical section
    {
        ACE_GUARD_RETURN (LockType, Guard, m_SessionLock, -1);

        m_Session = NULL;
    }

    reactor()->remove_handler(this, ACE_Event_Handler::DONT_CALL | ACE_Event_Handler::ALL_EVENTS_MASK);
    return 0;
}

int ReactorSocket::Update (void)
{
    if (!m_open)
        return -1;

    if (closing_)
        return -1;

    if (m_OutActive || (m_OutBuffer->length () == 0 && msg_queue()->is_empty()))
        return 0;

    int ret;
    do
        ret = handle_output (get_handle ());
    while( ret > 0 );

    return ret;
}

int ReactorSocket::handle_input_header (void)
{
    MANGOS_ASSERT (m_RecvWPct == NULL);

    MANGOS_ASSERT (m_Header.length () == sizeof (ClientPktHeader));

    m_Crypt.DecryptRecv ((uint8*) m_Header.rd_ptr (), sizeof (ClientPktHeader));

    ClientPktHeader& header = *((ClientPktHeader*) m_Header.rd_ptr ());

    EndianConvertReverse(header.size);
    EndianConvert(header.cmd);

    if ((header.size < 4) || (header.size > 10240) || (header.cmd  > 10240))
    {
        sLog.outError ("ReactorSocket::handle_input_header: client sent malformed packet size = %d , cmd = %d",
                       header.size, header.cmd);

        errno = EINVAL;
        return -1;
    }

    header.size -= 4;

    ACE_NEW_RETURN (m_RecvWPct, WorldPacket ((uint16) header.cmd, header.size), -1);

    if(header.size > 0)
    {
        m_RecvWPct->resize (header.size);
        m_RecvPct.base ((char*) m_RecvWPct->contents (), m_RecvWPct->size ());
    }
    else
    {
        MANGOS_ASSERT(m_RecvPct.space() == 0);
    }

    return 0;
}

int ReactorSocket::handle_input_payload (void)
{
    // set errno properly here on error !!!
    // now have a header and payload

    MANGOS_ASSERT (m_RecvPct.space () == 0);
    MANGOS_ASSERT (m_Header.space () == 0);
    MANGOS_ASSERT (m_RecvWPct != NULL);

    const int ret = ProcessIncoming (m_RecvWPct);

    m_RecvPct.base (NULL, 0);
    m_RecvPct.reset ();
    m_RecvWPct = NULL;

    m_Header.reset ();

    if (ret == -1)
        errno = EINVAL;

    return ret;
}

int ReactorSocket::handle_input_missing_data (void)
{
    char buf [4096];

    ACE_Data_Block db ( sizeof (buf),
                        ACE_Message_Block::MB_DATA,
                        buf,
                        0,
                        0,
                        ACE_Message_Block::DONT_DELETE,
                        0);

    ACE_Message_Block message_block(&db,
                                    ACE_Message_Block::DONT_DELETE,
                                    0);

    const size_t recv_size = message_block.space ();

    const ssize_t n = peer ().recv (message_block.wr_ptr (),
                                          recv_size);

    if (n <= 0)
        return (int)n;

    message_block.wr_ptr (n);

    while (message_block.length () > 0)
    {
        if (m_Header.space () > 0)
        {
            //need to receive the header
            const size_t to_header = (message_block.length () > m_Header.space () ? m_Header.space () : message_block.length ());
            m_Header.copy (message_block.rd_ptr (), to_header);
            message_block.rd_ptr (to_header);

            if (m_Header.space () > 0)
            {
                // Couldn't receive the whole header this time.
                MANGOS_ASSERT (message_block.length () == 0);
                errno = EWOULDBLOCK;
                return -1;
            }

            // We just received nice new header
            if (handle_input_header () == -1)
            {
                MANGOS_ASSERT ((errno != EWOULDBLOCK) && (errno != EAGAIN));
                return -1;
            }
        }

        // Its possible on some error situations that this happens
        // for example on closing when epoll receives more chunked data and stuff
        // hope this is not hack ,as proper m_RecvWPct is asserted around
        if (!m_RecvWPct)
        {
            sLog.outError ("Forcing close on input m_RecvWPct = NULL");
            errno = EINVAL;
            return -1;
        }

        // We have full read header, now check the data payload
        if (m_RecvPct.space () > 0)
        {
            //need more data in the payload
            const size_t to_data = (message_block.length () > m_RecvPct.space () ? m_RecvPct.space () : message_block.length ());
            m_RecvPct.copy (message_block.rd_ptr (), to_data);
            message_block.rd_ptr (to_data);

            if (m_RecvPct.space () > 0)
            {
                // Couldn't receive the whole data this time.
                MANGOS_ASSERT (message_block.length () == 0);
                errno = EWOULDBLOCK;
                return -1;
            }
        }

        //just received fresh new payload
        if (handle_input_payload () == -1)
        {
            MANGOS_ASSERT ((errno != EWOULDBLOCK) && (errno != EAGAIN));
            return -1;
        }
    }

    return size_t(n) == recv_size ? 1 : 2;
}

int ReactorSocket::cancel_wakeup_output (GuardType& g)
{
    if (!m_OutActive)
        return 0;

    m_OutActive = false;

    g.release ();

    if (reactor ()->cancel_wakeup
        (this, ACE_Event_Handler::WRITE_MASK) == -1)
    {
        // would be good to store errno from reactor with errno guard
        sLog.outError ("ReactorSocket::cancel_wakeup_output");
        return -1;
    }

    return 0;
}

int ReactorSocket::schedule_wakeup_output (GuardType& g)
{
    if (m_OutActive)
        return 0;

    m_OutActive = true;

    g.release ();

    if (reactor ()->schedule_wakeup
        (this, ACE_Event_Handler::WRITE_MASK) == -1)
    {
        sLog.outError ("ReactorSocket::schedule_wakeup_output");
        return -1;
    }

    return 0;
}
