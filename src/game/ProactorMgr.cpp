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

#include "ProactorMgr.h"
#include "ProactorRunnable.h"
#include "Config/Config.h"

bool ProactorMgr::StartNetwork(uint16 port, std::string bindIp)
{
    // create and start proactor threads
    int proactorCount = sConfig.GetIntDefault("Network.Threads", 1);
    proactorCount = std::max(1, proactorCount);
    for (int i = 0; i < proactorCount; ++i)
    {
        ProactorRunnable* proactor = new ProactorRunnable();
        if (!proactor->Start())
        {
            // can't recover, clean up any threads that were already started
            StopNetwork();
            return false;
        }

        m_proactors.push_back(proactor);
    }

    // open listen socket
    if (m_listenAddress.set(port, bindIp.c_str()) == -1) return false;
    if (m_acceptor.open(m_listenAddress, 0, true) == -1) return false;

    // start acceptor thread
    return activate() != -1;
}

void ProactorMgr::StopNetwork()
{
    // stop all proactors running AsyncSockets
    for (ProactorSet::iterator it = m_proactors.begin(); it != m_proactors.end(); ++it)
    {
        (*it)->Stop();
        delete (*it);
    }
    m_proactors.clear();

    // now stop the proactor running the AsyncAcceptor
    ACE_Proactor::instance()->proactor_end_event_loop();
    wait();
    ACE_Proactor::close_singleton();
}

void ProactorMgr::Wait()
{
    wait();
}

// sockets will call this method in order to get a proactor instance
void ProactorMgr::Add(AsyncSocket* socket)
{
    ACE_GUARD(ACE_Thread_Mutex, Guard, m_lock);

    ProactorRunnable* runnable = NULL;
    uint32 low = 0xffffffff;
    uint32 count;

    // select the proactor with the fewest (or 0) clients
    for (ProactorSet::iterator it = m_proactors.begin(); it != m_proactors.end(); ++it)
    {
        count = (*it)->count();
        if (count < low)
        {
            runnable = (*it);
            if (count == 0) break;
            low = count;
        }
    }

    MANGOS_ASSERT(runnable != NULL);
    runnable->AddClient();
    socket->m_runner = runnable;
    socket->proactor(runnable->proactor());
}

// sockets will call this method when they are closed
void ProactorMgr::Remove(ACE_Proactor* proactor)
{
    // critical section:
    // Add() called from acceptor thread, Remove() called from any thread
    ACE_GUARD(ACE_Thread_Mutex, Guard, m_lock);

    for (ProactorSet::iterator it = m_proactors.begin(); it != m_proactors.end(); ++it)
        if ((*it)->proactor() == proactor)
        {
            (*it)->RemoveClient();
            break;
        }
}

int ProactorMgr::svc()
{
    // this thread will service the AsynchAcceptor
    ACE_Proactor::instance()->proactor_run_event_loop();
    m_acceptor.Close();

    return 0;
}
