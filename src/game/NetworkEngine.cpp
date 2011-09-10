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

#include "NetworkEngine.h"
#include "Config/Config.h"
#include "Log.h"

#include "ProactorMgr.h"
#include "ReactorMgr.h"
#include "World.h"

#if defined (ACE_HAS_WIN32_OVERLAPPED_IO) || defined (ACE_HAS_AIO_CALLS)
#define MANGOS_USE_AIO
#endif

bool NetworkEngine::Start(uint16 port, std::string bindIp)
{
    bool useAioConfig = !sConfig.GetBoolDefault("Network.OldEngine", false);
    bool failed;

#ifdef MANGOS_USE_AIO
    if (useAioConfig)
    {
        failed = !sProactorMgr->StartNetwork(port, bindIp);
        m_aio = true;
    }
    else
#endif
    {
#ifndef MANGOS_USE_AIO
        if (useAioConfig)
            sLog.outError("Could not use asynchronous network IO, your platform does not support it.");
#endif

        failed = sReactorMgr->StartNetwork(port, bindIp) == -1;
        m_aio = false;
    }

    if (failed)
    {
        sLog.outError("Failed to start network");
        Log::WaitBeforeContinueIfNeed();
        World::StopNow(ERROR_EXIT_CODE);
        // go down and shutdown the server
    }

    return !failed;
}

void NetworkEngine::Stop()
{
    if (m_aio)
        sProactorMgr->StopNetwork();
    else
        sReactorMgr->StopNetwork();
}

void NetworkEngine::Wait()
{
    if (m_aio)
        sProactorMgr->Wait();
    else
        sReactorMgr->Wait();
}

#undef MANGOS_USE_AIO
