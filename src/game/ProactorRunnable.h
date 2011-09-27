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

#ifndef _PROACTOR_RUNNABLE_H
#define _PROACTOR_RUNNABLE_H

#include "ace/Proactor.h"
#include "ace/Task.h"

#ifdef ACE_HAS_AIO_CALLS
#   include "ace/POSIX_Proactor.h"
#   include "ace/POSIX_CB_Proactor.h"
#elif defined ACE_HAS_WIN32_OVERLAPPED_IO
#   include "ace/WIN32_Proactor.h"
#endif

#include <queue>

class AsyncSocket;

class ProactorRunnable : private ACE_Task<ACE_NULL_SYNCH>
{
public:
    ProactorRunnable();

    ~ProactorRunnable()
    {
        m_proactor->proactor_end_event_loop();
        wait();

        delete m_proactor;
    }

    bool Start()
    {
        return activate() != -1;
    }

    void Stop()
    {
        m_proactor->proactor_end_event_loop();
        wait();
    }

    ACE_Proactor* proactor() { return m_proactor; }

    uint32 count() { return m_clientCount; }
    void AddClient() { m_clientCount++; }
    void RemoveClient() { m_clientCount--; }

private:
    ACE_Proactor* m_proactor;
    uint32 m_clientCount;       // for load-balancing only

    int svc()
    {
        m_proactor->proactor_run_event_loop();
        return 0;
    }

    /*** begin concurrent io ops limits ***/
public:
    void DequeueOp();
    bool EnqueueWrite(AsyncSocket* socket);
    bool EnqueueRead(AsyncSocket* socket);
    
    static uint32 OpLimit() { return ProactorRunnable::s_opLimit; }

private:
    ACE_Thread_Mutex m_lock;

    static uint32 s_opLimit;    // maximum number of concurrent async operations
    uint32 m_opCount;           // the number of async operations initiated on the ACE_Proactor

    typedef std::queue<AsyncSocket*> IoQueue;
    IoQueue m_writeQueue;
    IoQueue m_readQueue;

    /*** end concurrent io ops limits ***/
};

#endif
