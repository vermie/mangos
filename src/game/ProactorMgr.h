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

#ifndef _PROACTOR_MGR_H
#define _PROACTOR_MGR_H

#include "Platform/Define.h"
#include "ace/Proactor.h"

#include "AsyncSocket.h"
#include "AsyncAcceptor.h"

#include <vector>

class ProactorRunnable;

class ProactorMgr : private ACE_Task<ACE_NULL_SYNCH>
{
private:
    typedef ACE_Guard<ACE_Thread_Mutex> GuardType;
    ACE_Thread_Mutex m_lock;

public:
    bool StartNetwork(uint16 port, std::string bindIp);
    void StopNetwork();
    void Wait();

    void Add(AsyncSocket* socket);
    void Remove(ACE_Proactor* socket);

private:
    friend class AsyncAcceptor;

    virtual int svc();

    ACE_INET_Addr m_listenAddress;

    AsyncAcceptor m_acceptor;

    typedef std::vector<ProactorRunnable*> ProactorSet;

    ProactorSet m_proactors;
};

#define sProactorMgr ACE_Singleton<ProactorMgr,ACE_Thread_Mutex>::instance()

#endif
