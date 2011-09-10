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

#include "ProactorRunnable.h"
#include "AsyncSocket.h"

#ifdef ACE_HAS_AIO_CALLS
// need to get the size of the aio list
class DummyProactor : private ACE_POSIX_AIOCB_Proactor
{
    DummyProactor()
    {
        aiocb_list_max_size_ = ACE_AIO_MAX_SIZE;
        check_max_aio_num();
    }

    size_t GetListSize() { return aiocb_list_max_size_; }
};
#endif

ProactorRunnable::ProactorRunnable() :
    m_clientCount(0), m_opCount(0), m_opLimit(0)
{
    ACE_Proactor_Impl* implementation;

    // copied form ACE_Proactor ctor, but with increased aio list size
#if defined (ACE_HAS_AIO_CALLS)
      // POSIX Proactor.
#  if defined (ACE_POSIX_AIOCB_PROACTOR)
      ACE_NEW (implementation, ACE_POSIX_AIOCB_Proactor(ACE_AIO_MAX_SIZE));
#  elif defined (ACE_POSIX_SIG_PROACTOR)
      ACE_NEW (implementation, ACE_POSIX_SIG_Proactor(ACE_AIO_MAX_SIZE));
#  else /* Default order: CB, SIG, AIOCB */
#    if !defined(ACE_HAS_BROKEN_SIGEVENT_STRUCT)
      ACE_NEW (implementation, ACE_POSIX_CB_Proactor(ACE_AIO_MAX_SIZE));
#    else
#      if defined(ACE_HAS_POSIX_REALTIME_SIGNALS)
      ACE_NEW (implementation, ACE_POSIX_SIG_Proactor(ACE_AIO_MAX_SIZE));
#      else
      ACE_NEW (implementation, ACE_POSIX_AIOCB_Proactor(ACE_AIO_MAX_SIZE));
#      endif /* ACE_HAS_POSIX_REALTIME_SIGNALS */
#    endif /* !ACE_HAS_BROKEN_SIGEVENT_STRUCT */
#  endif /* ACE_POSIX_AIOCB_PROACTOR */
#elif (defined (ACE_WIN32) && !defined (ACE_HAS_WINCE))
    // WIN_Proactor.
    ACE_NEW (implementation,
            ACE_WIN32_Proactor);
#endif /* ACE_HAS_AIO_CALLS */

    m_proactor = new ACE_Proactor(implementation, true);

#ifdef ACE_HAS_AIO_CALLS
    DummyProactor temp;
    m_opLimit = temp.GetListSize();
#endif
}

bool ProactorRunnable::DequeueOp()
{
    if (!m_opLimit)
        return true;

    bool ret = true;
    ACE_GUARD_RETURN(ACE_Thread_Mutex, Guard, m_lock, false);

    // check if we are able to begin a new operation
    // if we begin, we don't change op count (dequeue + enqueue = no change)
    // else we decrement op count
    if (m_opLimit >= m_opCount)
    {
        // check for WRITE operation first, because deterministically they are more reliable
        if (!m_writeQueue.empty())
        {
            ret = m_writeQueue.front()->BeginWrite();
            m_writeQueue.pop();
        }
        else if (!m_readQueue.empty())
        {
            ret = m_readQueue.front()->BeginRead();
            m_readQueue.pop();
        }
        else
            m_opCount--;
    }
    else
        m_opCount--;

    return ret;
}

bool ProactorRunnable::EnqueueRead(AsyncSocket* socket)
{
    if (!m_opLimit)
        return socket->BeginRead();

    ACE_GUARD_RETURN(ACE_Thread_Mutex, Guard, m_lock, false);

    if (m_opLimit > m_opCount)
    {
        m_opCount++;
        return socket->BeginRead();
    }

    // too many io ops pending
    // asynchronous read will be started asynchronously (from DequeueOp)
    m_readQueue.push(socket);
    return true;
}

bool ProactorRunnable::EnqueueWrite(AsyncSocket* socket)
{
    if (!m_opLimit)
        return socket->BeginWrite();

    ACE_GUARD_RETURN(ACE_Thread_Mutex, Guard, m_lock, false);

    if (m_opLimit > m_opCount)
    {
        m_opCount++;
        return socket->BeginWrite();
    }

    // too many io ops pending
    // asynchronous write will be started asynchronously (from DequeueOp)
    m_writeQueue.push(socket);
    return true;
}
