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
#include "Database/DatabaseEnv.h"

uint32 ProactorRunnable::s_opLimit = 0;

ProactorRunnable::ProactorRunnable() :
    m_clientCount(0), m_opCount(0)
{
    ACE_Proactor_Impl* implementation;
    ACE_NEW(implementation, ProactorType);

    m_proactor = new ACE_Proactor(implementation, true);
}

void ProactorRunnable::DequeueOp()
{
    // this function will not return until one of the following is true:
    //     there is no queue, ie ACE has no limit to the number of concurrent async operations
    //     one queued operation has been started
    //     m_opCount is decremented

    if (!s_opLimit) return;

    ACE_GUARD(ACE_Thread_Mutex, Guard, m_lock);

    bool succeeded = false;

    // this loop dequeues operations until one succeeds
#define DEQUEUE_UNTIL_SUCCEED(queue)                                            \
    if (!queue.empty())                                                         \
    {                                                                           \
        while (!queue.empty() && !(succeeded = queue.front()->BeginWrite()))    \
            queue.pop();                                                        \
        if (succeeded) queue.pop();                                             \
    }

    // check if we are able to begin a new operation
    // if we begin, we don't change op count (dequeue + enqueue = no change)
    // else we decrement op count
    if (s_opLimit >= m_opCount)
    {
        // check for WRITE operation first, because deterministically they are more reliable
        DEQUEUE_UNTIL_SUCCEED(m_writeQueue);
        if (!succeeded) DEQUEUE_UNTIL_SUCCEED(m_readQueue);
        if (!succeeded) m_opCount--;
    }
    else
        m_opCount--;
}

bool ProactorRunnable::EnqueueRead(AsyncSocket* socket)
{
    if (!s_opLimit)
        return socket->BeginRead();

    ACE_GUARD_RETURN(ACE_Thread_Mutex, Guard, m_lock, false);

    if (s_opLimit > m_opCount)
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
    if (!s_opLimit)
        return socket->BeginWrite();

    ACE_GUARD_RETURN(ACE_Thread_Mutex, Guard, m_lock, false);

    if (s_opLimit > m_opCount)
    {
        m_opCount++;
        return socket->BeginWrite();
    }

    // too many io ops pending
    // asynchronous write will be started asynchronously (from DequeueOp)
    m_writeQueue.push(socket);
    return true;
}

int ProactorRunnable::svc()
{
    // setup thread-local stuff for login database
    LoginDatabase.ThreadStart();

    // have ACE begin handling network events
    m_proactor->proactor_run_event_loop();

    // teardown thread-local stuff for login database
    LoginDatabase.ThreadEnd();

    return 0;
}
