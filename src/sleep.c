/* $Id$ */

/*
 *   Copyright (c) 2001-2010 Aaron Turner <aturner at synfin dot net>
 *   Copyright (c) 2013 Fred Klassen <fklassen at appneta dot com> - AppNeta Inc.
 *
 *   The Tcpreplay Suite of tools is free software: you can redistribute it 
 *   and/or modify it under the terms of the GNU General Public License as 
 *   published by the Free Software Foundation, either version 3 of the 
 *   License, or with the authors permission any later version.
 *
 *   The Tcpreplay Suite is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with the Tcpreplay Suite.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include "defines.h"
#include "common.h"
#include "sleep.h"
#include "timestamp_trace.h"

#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>     
#include <errno.h>
#include <string.h>

#ifdef HAVE_SYS_EVENT
#include <sys/event.h>
#endif

/* necessary for ioport_sleep() functions */
#ifdef HAVE_SYS_IO_H /* Linux */
#include <sys/io.h>
#elif defined HAVE_ARCHITECTURE_I386_PIO_H /* OS X */
#include <architecture/i386/pio.h>
#endif

float gettimeofday_sleep_value;
int ioport_sleep_value;

static u_int32_t get_user_count(sendpacket_t *, COUNTER);

extern tcpreplay_opt_t options;
extern COUNTER bytes_sent, failed, pkts_sent;


void 
ioport_sleep_init(void) 
{
#ifdef HAVE_IOPERM
    ioperm(0x80,1,1);
    ioport_sleep_value = inb(0x80);    
#else
    err(-1, "Platform does not support IO Port for timing");
#endif
}

void 
ioport_sleep(const struct timespec UNUSED(nap))
{
#ifdef HAVE_IOPERM
    struct timeval nap_for;
    u_int32_t usec;
    time_t i;
    
    TIMESPEC_TO_TIMEVAL(&nap_for, &nap);
    
    /* 
     * process the seconds, we do this in a loop so we don't have to 
     * use slower 64bit integers or worry about integer overflows.
     */
    for (i = 0; i < nap_for.tv_sec; i ++) {
        usec = SEC_TO_MICROSEC(nap_for.tv_sec);
        while (usec > 0) {
            usec --;
            outb(ioport_sleep_value, 0x80);
        }
    }
    
    /* process the usec */
    usec = nap.tv_nsec / 1000;
    usec --; /* fudge factor for all the above */
    while (usec > 0) {
        usec --;
    	outb(ioport_sleep_value, 0x80);
    }
#else
    err(-1, "Platform does not support IO Port for timing");
#endif
}


/**
 * Given the timestamp on the current packet and the last packet sent,
 * calculate the appropriate amount of time to sleep and do so.
 */
void
do_sleep(struct timeval *time, struct timeval *last, int len, int accurate, 
    sendpacket_t *sp, COUNTER counter, timestamp_t *sent_timestamp,
    COUNTER *start_us, COUNTER *skip_length)
{
#ifdef DEBUG
    static struct timeval totalsleep = { 0, 0 };
#endif
    static struct timespec nap = { 0, 0 };
    struct timeval nap_for;
    struct timespec nap_this_time;
    static u_int32_t send = 0;      /* accellerator.   # of packets to send w/o sleeping */
    u_int64_t ppnsec; /* packets per nsec */
    static int first_time = 1;      /* need to track the first time through for the pps accelerator */
    COUNTER now_us;

    /* acclerator time? */
    if (send > 0) {
        send --;
        return;
    }

    /*
     * pps_multi accelerator.    This uses the existing send accelerator above
     * and hence requires the funky math to get the expected timings.
     */
    if (options.speed.mode == SPEED_PACKETRATE && options.speed.pps_multi) {
        send = options.speed.pps_multi - 1;
        if (first_time) {
            first_time = 0;
            return;
        }
    }

    dbgx(4, "This packet time: " TIMEVAL_FORMAT, time->tv_sec, time->tv_usec);
    dbgx(4, "Last packet time: " TIMEVAL_FORMAT, last->tv_sec, last->tv_usec);

    /* If top speed, you shouldn't even be here */
    assert(options.speed.mode != SPEED_TOPSPEED);

    /*
     * 1. First, figure out how long we should sleep for...
     */
    switch(options.speed.mode) {
    case SPEED_MULTIPLIER:
        /*
         * Replay packets a factor of the time they were originally sent.
         */
        if (timerisset(last)) {
            if (timercmp(time, last, <)) {
                /* Packet has gone back in time!  Don't sleep and warn user */
                warnx("Packet #" COUNTER_SPEC " has gone back in time!", counter);
                timesclear(&nap); 
            } else {
                /* time has increased or is the same, so handle normally */
                timersub(time, last, &nap_for);
                dbgx(3, "original packet delta time: " TIMEVAL_FORMAT, nap_for.tv_sec, nap_for.tv_usec);

                TIMEVAL_TO_TIMESPEC(&nap_for, &nap);
                dbgx(3, "original packet delta timv: " TIMESPEC_FORMAT, nap.tv_sec, nap.tv_nsec);
                timesdiv_float(&nap, options.speed.speed);
                dbgx(3, "original packet delta/div: " TIMESPEC_FORMAT, nap.tv_sec, nap.tv_nsec);
            }
        } else {
            /* Don't sleep if this is our first packet */
            timesclear(&nap);
        }
        break;

    case SPEED_MBPSRATE:
        /*
         * Ignore the time supplied by the capture file and send data at
         * a constant 'rate' (bytes per second).
         */
        now_us = TIMSTAMP_TO_MICROSEC(sent_timestamp);
        if (now_us) {
            COUNTER bps = (COUNTER)options.speed.speed;
            COUNTER bits_sent = ((bytes_sent + (COUNTER)len) * 8LL);
            /* bits * 1000000 divided by bps = microseconds */
            COUNTER next_tx_us = (bits_sent * 1000000) / bps;
            COUNTER tx_us = now_us - *start_us;
            if (next_tx_us > tx_us)
                NANOSEC_TO_TIMESPEC((next_tx_us - tx_us) * 1000LL, &nap);
            else if (tx_us > next_tx_us) {
                tx_us = now_us - *start_us;
                *skip_length = ((tx_us - next_tx_us) * bps) / 8000000;
            }
            update_current_timestamp_trace_entry(bytes_sent + (COUNTER)len, now_us, tx_us, next_tx_us);
        }

        dbgx(3, "packet size %d\t\tequals\tnap " TIMESPEC_FORMAT, len,
                nap.tv_sec, nap.tv_nsec);
        break;

    case SPEED_PACKETRATE:
        /*
         * Only need to calculate this the first time since this is a
         * constant time function
         */
        if (! timesisset(&nap)) {
            /* run in packets/sec */
            ppnsec = 1000000000 / options.speed.speed * (options.speed.pps_multi > 0 ? options.speed.pps_multi : 1);
            NANOSEC_TO_TIMESPEC(ppnsec, &nap);
            dbgx(1, "sending %d packet(s) per %lu nsec", (options.speed.pps_multi > 0 ? options.speed.pps_multi : 1), nap.tv_nsec);
        }
        break;

    case SPEED_ONEATATIME:
        /*
         * Prompt the user for sending each packet(s)
         */

        /* do we skip prompting for a key press? */
        if (send == 0) {
            send = get_user_count(sp, counter);
        }

        /* decrement our send counter */
        printf("Sending packet " COUNTER_SPEC " out: %s\n", counter,
               sp == options.intf1 ? options.intf1_name : options.intf2_name);
        send --;

        return; /* leave do_sleep() */

        break;

    default:
        errx(-1, "Unknown/supported speed mode: %d", options.speed.mode);
        break;
    }

    memcpy(&nap_this_time, &nap, sizeof(nap_this_time));

    /* don't sleep if nap = {0, 0} */
    if (!timesisset(&nap_this_time))
        return;

    /* do we need to limit the total time we sleep? */
    if (timesisset(&(options.maxsleep)) && (timescmp(&nap_this_time, &(options.maxsleep), >))) {
        dbgx(2, "Was going to sleep for " TIMESPEC_FORMAT " but maxsleeping for " TIMESPEC_FORMAT, 
            nap_this_time.tv_sec, nap_this_time.tv_nsec, options.maxsleep.tv_sec,
            options.maxsleep.tv_nsec);
        memcpy(&nap_this_time, &(options.maxsleep), sizeof(struct timespec));
    }

    dbgx(2, "Sleeping:                   " TIMESPEC_FORMAT, nap_this_time.tv_sec, nap_this_time.tv_nsec);

    /*
     * Depending on the accurate method & packet rate computation method
     * We have multiple methods of sleeping, pick the right one...
     */
    switch (accurate) {
#ifdef HAVE_SELECT
    case ACCURATE_SELECT:
        select_sleep(nap_this_time);
        break;
#endif

#ifdef HAVE_IOPERM
    case ACCURATE_IOPORT:
        ioport_sleep(nap_this_time);
        break;
#endif

#ifdef HAVE_ABSOLUTE_TIME
    case ACCURATE_ABS_TIME:
        absolute_time_sleep(nap_this_time);
        break;
#endif

    case ACCURATE_GTOD:
        gettimeofday_sleep(nap_this_time);
        break;

    case ACCURATE_NANOSLEEP:
        nanosleep_sleep(nap_this_time);
        break;
        /*
        timeradd(&didsleep, &nap_this_time, &didsleep);

        dbgx(4, "I will sleep " TIMEVAL_FORMAT, nap_this_time.tv_sec, nap_this_time.tv_usec);

        if (timercmp(&didsleep, &sleep_until, >)) {
            timersub(&didsleep, &sleep_until, &nap_this_time);
            
            TIMEVAL_TO_TIMESPEC(&nap_this_time, &sleep);
            dbgx(4, "Sleeping " TIMEVAL_FORMAT, nap_this_time.tv_sec, nap_this_time.tv_usec);
#ifdef DEBUG
            timeradd(&totalsleep, &nap_this_time, &totalsleep);
#endif
            if (nanosleep(&sleep, &ignore) == -1) {
                warnx("nanosleep error: %s", strerror(errno));
            }
        }
        break;
        */
    default:
        errx(-1, "Unknown timer mode %d", accurate);
    }

#ifdef DEBUG
    dbgx(4, "Total sleep time: " TIMEVAL_FORMAT, totalsleep.tv_sec, totalsleep.tv_usec);
#endif

    dbgx(2, "sleep delta: " TIMEVAL_FORMAT, sent_timestamp->tv_sec, sent_timestamp->tv_usec);

}

/**
 * Ask the user how many packets they want to send.
 */
static u_int32_t
get_user_count(sendpacket_t *sp, COUNTER counter) 
{
    struct pollfd poller[1];        /* use poll to read from the keyboard */
    char input[EBUF_SIZE];
    u_int32_t send = 0;
    
    printf("**** Next packet #" COUNTER_SPEC " out %s.  How many packets do you wish to send? ",
        counter, (sp == options.intf1 ? options.intf1_name : options.intf2_name));
    fflush(NULL);
    poller[0].fd = STDIN_FILENO;
    poller[0].events = POLLIN | POLLPRI | POLLNVAL;
    poller[0].revents = 0;

    if (fcntl(0, F_SETFL, fcntl(0, F_GETFL) & ~O_NONBLOCK)) 
           errx(-1, "Unable to clear non-blocking flag on stdin: %s", strerror(errno));

    /* wait for the input */
    if (poll(poller, 1, -1) < 0)
        errx(-1, "Error reading user input from stdin: %s", strerror(errno));
    
    /*
     * read to the end of the line or EBUF_SIZE,
     * Note, if people are stupid, and type in more text then EBUF_SIZE
     * then the next fgets() will pull in that data, which will have poor 
     * results.  fuck them.
     */
    if (fgets(input, sizeof(input), stdin) == NULL) {
        errx(-1, "Unable to process user input for fd %d: %s", fileno(stdin), strerror(errno));
    } else if (strlen(input) > 1) {
        send = strtoul(input, NULL, 0);
    }

    /* how many packets should we send? */
    if (send == 0) {
        dbg(1, "Input was less then 1 or non-numeric, assuming 1");

        /* assume send only one packet */
        send = 1;
    }

    return send;
}
