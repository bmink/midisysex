#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include "bstr.h"
#include "barr.h"
#include "midi_osx.h"
#include "midi_queue.h"
#include "btime.h"


void
usage(char *prognam)
{
	printf("Usage: %s <seqfile>\n", prognam);
}


midi_queue_t	*midi_inq;
midi_queue_t	*midi_outq;

void *midi_writer(void *);
int midi_get_resp();

#define RESPONSE_TIMEOUT_SEC	3
#define MIDIIO_WAKEUP_MS	50

#define PROG_STATE_NONE		0
#define PROG_STATE_RUNNING	1
#define PROG_STATE_SHUTDOWN	2

int prog_state = PROG_STATE_NONE;
pthread_rwlock_t prog_state_rwlock;

int set_prog_state(int);


int
main(int argc, char **argv)
{
	int		ret;
	pthread_t	write_thrd;
	unsigned char	midireq[] = { 0x42, 0x50, 0x00, 0x01 };
	//unsigned char	midireq[] = { 0x7E, 0x7F, 0x06, 0x01 };
	//unsigned char	midireq[] = { 0x42, 0x30, 0x00, 0x01, 0x23, 0x10 };

	midi_inq = NULL;
	midi_outq = NULL;

#if 0
	if(argc != 2 || xstrempty(argv[1])) {
		usage(argv[0]);
		exit(-1);
	}
#endif


	ret = midi_queue_init(&midi_inq);
	if(ret != 0) {
		fprintf(stderr, "Can't initialize MIDI in queue\n");
		exit(-1);
	}

	ret = midi_queue_init(&midi_outq);
	if(ret != 0) {
		fprintf(stderr, "Can't initialize MIDI out queue\n");
		exit(-1);
	}

	/* Create global state variable lock */
	ret = pthread_rwlock_init(&prog_state_rwlock, NULL);
	if(ret != 0) {
		fprintf(stderr, "Can't create global state variable rwlock\n");
		exit(-1);
	}

	ret = set_prog_state(PROG_STATE_RUNNING);
	if(ret != 0) {
		exit(-1);
	}




	ret = midi_osx_init();
	if(ret != 0) {
		fprintf(stderr, "Can't initialize system MIDI.\n");
		exit(-1);
	}

	/* Start thread(s). */
	ret = pthread_create(&write_thrd, NULL, midi_writer, NULL);
	if(ret != 0) {
		fprintf(stderr, "Can't start MIDI writer thread: %s\n",
		    strerror(ret));
		exit(-1);
	}


	ret = midi_queue_addmsg_sysex(midi_outq,
	    (unsigned char *) midireq, 4);

	/* Usually a program like this would have a writer and a reader thread.
	 * However, since here we're just waiting for a specific response,
	 * we're essentially executing the "reader thread" on the main
	 * thread. */

	ret = midi_get_resp();


	/* Signal to thread(s) to shut down. */
	ret = set_prog_state(PROG_STATE_SHUTDOWN);
	if(ret != 0) {
		exit(-1);
	}

	/* Wait for thread(d) to exit. */
	ret = pthread_join(write_thrd, NULL);
	if(ret != 0) {
		fprintf(stderr, "Can't join writer thread.\n");
	}

	ret = midi_osx_uninit();
	if(ret != 0) {
		fprintf(stderr, "Can't uninitialize system MIDI.\n");
	}

	ret = midi_queue_uninit(&midi_inq);
	if(ret != 0) {
		fprintf(stderr, "Can't uninitialize MIDI in queue\n");
	}

	ret = midi_queue_uninit(&midi_outq);
	if(ret != 0) {
		fprintf(stderr, "Can't uninitialize MIDI out queue\n");
	}

	(void) set_prog_state(PROG_STATE_NONE);

	ret = pthread_rwlock_destroy(&prog_state_rwlock);
	if(ret != 0) {
		fprintf(stderr, "Can't destroy global state variable rwlock\n");
	}

	return 0;

}




int
midi_get_resp()
{
	int		ret;
	midi_msg_t	msg;
	int		haveresp;
	int		timeout;
	struct timeval	now;
	struct timeval	timeoutat;
	struct timespec	condwaitto;

	haveresp = 0;
	timeout = 0;

	btimeval_tonow(&timeoutat);
	btimeval_adds(&timeoutat, RESPONSE_TIMEOUT_SEC);

	ret = pthread_mutex_lock(&midi_inq->mq_mutex);
	if(ret != 0) {
		fprintf(stderr, "Can't lock queue: %s\n", strerror(ret));
		return ENOEXEC;
	}

	while(!haveresp && !timeout) {

		(void) gettimeofday(&now, NULL);

		if(btimeval_cmp(&now, &timeoutat) >= 0) {
			fprintf(stderr, "Timeout: no answer from device.\n");
			timeout++;
			break;
		}

		while(!midi_queue_isempty(midi_inq)) {

			ret = midi_queue_getnext(midi_inq, &msg);
			if(ret != 0) {
				fprintf(stderr, "Can't get next message"
				    " from queue: %s\n"
				    " This is bad, exiting\n", strerror(ret));
				exit(-1);
			}

			if(msg.mm_type == MIDI_MSG_SYSEX) {
				printf("Sysex received!\n");
				haveresp++;
				break;
			}
				
			if(msg.mm_type == MIDI_MSG_SYSRT_STOP) {
				printf("\nStop received.\n");
			}
		}

		/* No more items to process, so go to sleep until
		 * something happens on the queue */
		btimespec_tonow(&condwaitto);
		btimespec_addus(&condwaitto, MIDIIO_WAKEUP_MS * 1000);
		ret = pthread_cond_timedwait(&midi_inq->mq_cond,
		    &midi_inq->mq_mutex, &condwaitto);

		if(ret != 0 && ret != ETIMEDOUT) {
			fprintf(stderr, "Error while waiting on condvar: %s\n"
			    " This is bad, exiting\n", strerror(ret));
			exit(-1);
		}

	}

	ret = pthread_mutex_unlock(&midi_inq->mq_mutex);
	if(ret != 0) {
		fprintf(stderr, "Can't unlock queue: %s\n", strerror(ret));
		return ENOEXEC;
	}

	if(timeout)
		return ETIMEDOUT;

	return 0;

}


void *
midi_writer(void *arg)
{
	int		ret;
	midi_msg_t	msg;
	bstr_t		*midimsg;
	int		doshutdown;
	struct timespec	condwaitto;

	midimsg = 0;
	doshutdown = 0;

	printf("MIDI writer thread started.\n");
	fflush(stdout);

	ret = pthread_mutex_lock(&midi_outq->mq_mutex);
	if(ret != 0) {
		fprintf(stderr, "Can't lock queue: %s\n", strerror(ret));
		return (void *) -1;
	}

	while(1) {

		/* Check whether it's time to quit. */
		ret = pthread_rwlock_rdlock(&prog_state_rwlock);
		if(ret != 0) {
			fprintf(stderr,
			    "Can't lock global state rwlock for reading: %s\n",
			    strerror(ret));
			exit(-1);	
		}
		if(prog_state == PROG_STATE_SHUTDOWN) {
			doshutdown++;
		}
		ret = pthread_rwlock_unlock(&prog_state_rwlock);
		if(ret != 0) {
			fprintf(stderr,
			    "Can't unlock global state rwlock"
			    " after reading: %s\n", strerror(ret));
			exit(-1);	
		}

		if(doshutdown)
			break;

		while(!midi_queue_isempty(midi_outq)) {


			ret = midi_queue_getnext(midi_outq, &msg);
			if(ret != 0) {
				fprintf(stderr, "Can't get next message"
				    " from queue: %s\n"
				    " This is bad, exiting\n", strerror(ret));
				exit(-1);
			}

			midimsg = binit();

			if(msg.mm_type == MIDI_MSG_SYSEX && msg.mm_payload
			    && msg.mm_payload_siz > 0) {
				
				/* Send message */
				bprintf(midimsg, "%c", 0xF0); /* SysEx begin */
				bmemcat(midimsg, (char *) msg.mm_payload,
				    msg.mm_payload_siz);      /* Payload     */
				bprintf(midimsg, "%c", 0xF7); /* SysEx end   */
			}

	
			if(!bstrempty(midimsg)) {
				ret = midi_osx_sendmsg(
				    (unsigned char *) bget(midimsg),
				    bstrlen(midimsg));
				if(ret != 0) {
					fprintf(stderr,
					    "Couldn't send MIDI message.\n");
				} else
					printf("MIDI message sent.\n");

			} else {
				printf("MIDI message not sent.\n");
			}


			buninit(&midimsg);
			(void) midi_msg_free_payload(&msg);
				
		}

		/* No more items to process, so go to sleep until
		 * something happens on the queue */
		btimespec_tonow(&condwaitto);
		btimespec_addus(&condwaitto, MIDIIO_WAKEUP_MS * 1000);
		ret = pthread_cond_timedwait(&midi_outq->mq_cond,
		    &midi_outq->mq_mutex, &condwaitto);
		if(ret != 0 && ret != ETIMEDOUT) {
			fprintf(stderr, "Error while waiting on condvar: %s\n"
			    " This is bad, exiting\n", strerror(ret));
			exit(-1);
		}
	}

	ret = pthread_mutex_unlock(&midi_outq->mq_mutex);
	if(ret != 0) {
		fprintf(stderr, "Can't unlock queue: %s\n", strerror(ret));
		return (void *) -1;
	}

	printf("MIDI writer thread exiting.\n");
	fflush(stdout);

	return (void *) 0;

}


int
set_prog_state(int newstate)
{
	int	ret;

	ret = pthread_rwlock_wrlock(&prog_state_rwlock);
	if(ret != 0) {
		fprintf(stderr,
		    "Can't lock global state rwlock for writing: %s\n",
		    strerror(ret));
		return ret;
	}

	prog_state = newstate;
	
	ret = pthread_rwlock_unlock(&prog_state_rwlock);
	if(ret != 0) {
		fprintf(stderr,
		    "Can't unlock global state rwlock after writing: %s\n",
		    strerror(ret));
		return ret;
	}

	return 0;

}



