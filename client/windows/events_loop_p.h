/*
   Copyright (C) 2009 Red Hat, Inc.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of
   the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef _H_EVENTS_LOOP_P
#define _H_EVENTS_LOOP_P

#include "common.h"

#include <vector>

class EventSourceOld;

class EventsLoop_p {
public:
    class Trigger_p;
public:
    std::vector<EventSourceOld*> _events;
    std::vector<HANDLE> _handles;
};

class EventsLoop_p::Trigger_p {
public:
    HANDLE get_handle() { return event;}

public:
    HANDLE event;
};

#endif

