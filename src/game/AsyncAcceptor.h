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

#ifndef _ASYNC_ACCEPTOR_H
#define _ASYNC_ACCEPTOR_H

#include "ace/Asynch_IO.h"
#include "ace/Asynch_Acceptor.h"

class AsyncSocket;

class AsyncAcceptor : public ACE_Asynch_Acceptor<AsyncSocket>
{
public:
    AsyncAcceptor();

    void Close();

protected:
    friend class AsyncSocket;

    virtual AsyncSocket* make_handler();

    bool m_TcpNodelay;
    int m_SockOutKBuff;
    int m_SockOutUBuff;
};

#endif
