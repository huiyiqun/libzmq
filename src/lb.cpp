/*
    Copyright (c) 2007-2014 Contributors as noted in the AUTHORS file

    This file is part of 0MQ.

    0MQ is free software; you can redistribute it and/or modify it under
    the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    0MQ is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <stdio.h>
#include "lb.hpp"
#include "pipe.hpp"
#include "err.hpp"
#include "msg.hpp"

#define DB_TRACE(tag) int my_seq = ++seq;  \
				pthread_t self = pthread_self(); \
				fprintf(stderr, "=> %12.12s thread=%lu this=%p seq=%d active=%lu\n", tag, self, (void *)this, my_seq, active) ; \
			
#define DB_TRACE_EXIT(tag) fprintf(stderr, "<= %12.12s thread=%lu this=%p seq=%d active=%lu\n", tag, self, (void *)this, my_seq, active) ; \

int zmq_lb_race_window_1_size = 0 ;
int zmq_lb_race_window_2_size = 0 ;
static int seq = 0 ;

zmq::lb_t::lb_t () :
    active (0), 
    current (0),
    more (false),
    dropping (false)
{
	DB_TRACE("lb_cons") ;
}

zmq::lb_t::~lb_t ()
{
    zmq_assert (pipes.empty ());
}

void zmq::lb_t::attach (pipe_t *pipe_)
{
    pipes.push_back (pipe_);
    activated (pipe_);
}

void zmq::lb_t::pipe_terminated (pipe_t *pipe_)
{
	DB_TRACE("lb_term") ;
    pipes_t::size_type index = pipes.index (pipe_);

    //  If we are in the middle of multipart message and current pipe
    //  have disconnected, we have to drop the remainder of the message.
    if (index == current && more)
        dropping = true;

    //  Remove the pipe from the list; adjust number of active pipes
    //  accordingly.
    if (index < active) {
        active--;
        pipes.swap (index, active);
        if (current == active)
            current = 0;
    }
    pipes.erase (pipe_);
	DB_TRACE_EXIT("lb_term") ;
}

void zmq::lb_t::activated (pipe_t *pipe_)
{
    //  Move the pipe to the list of active pipes.
    pipes.swap (pipes.index (pipe_), active);
    active++;
}

int zmq::lb_t::send (msg_t *msg_)
{
    return sendpipe (msg_, NULL);
}

#ifdef ZMQ_HAVE_WINDOWS
#  define msleep(milliseconds)    Sleep (milliseconds);
#else
#  include <unistd.h>
#  define msleep(milliseconds)   usleep (static_cast <useconds_t> (milliseconds) * 1000);
#endif

int zmq::lb_t::sendpipe (msg_t *msg_, pipe_t **pipe_)
{
	DB_TRACE("lb_sendpipe") ;
    //  Drop the message if required. If we are at the end of the message
    //  switch back to non-dropping mode.
    if (dropping) {

        more = msg_->flags () & msg_t::more ? true : false;
        dropping = more;

        int rc = msg_->close ();
        errno_assert (rc == 0);
        rc = msg_->init ();
        errno_assert (rc == 0);
        return 0;
    }

    while (active > 0) {
	// DAB - bias the race to provoke problems with write
		msleep(zmq_lb_race_window_1_size ) ;

       if (pipes [current]->write (msg_))
        {
            if (pipe_)
                *pipe_ = pipes [current];
            break;
        }
		if (!(!more))
		{
			DB_TRACE("lb_assert");
			fflush(stderr) ;
		}
        zmq_assert (!more);
        active--;
        if (current < active)
            pipes.swap (current, active);
        else
            current = 0;
    }

    //  If there are no pipes we cannot send the message.
    if (active == 0) {
        errno = EAGAIN;
		DB_TRACE_EXIT("lb_sendpipe") ;
        return -1;
    }

    //  If it's final part of the message we can flush it downstream and
    //  continue round-robining (load balance).
    more = msg_->flags () & msg_t::more? true: false;

	// DAB - bias the race to provoke problems with %
	msleep(zmq_lb_race_window_2_size) ;

    if (!more) {
        pipes [current]->flush ();
        current = (current + 1) % active;
    }

    //  Detach the message from the data buffer.
    int rc = msg_->init ();
    errno_assert (rc == 0);

	DB_TRACE_EXIT("lb_sendpipe") ;
    return 0;
}

bool zmq::lb_t::has_out ()
{
    //  If one part of the message was already written we can definitely
    //  write the rest of the message.
    if (more)
        return true;

    while (active > 0) {

        //  Check whether a pipe has room for another message.
        if (pipes [current]->check_write ())
            return true;

        //  Deactivate the pipe.
        active--;
        pipes.swap (current, active);
        if (current == active)
            current = 0;
    }

    return false;
}
