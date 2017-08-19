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


void
usage(char *prognam)
{
	printf("Usage: %s <seqfile>\n", prognam);
}


midi_queue_t	*midi_inq;
midi_queue_t	*midi_outq;

void *midi_writer(void *);
int midi_get_resp();


int
main(int argc, char **argv)
{
	int		ret;
	pthread_t	proc_thrd;
	unsigned char	midireq[] = { 0x42, 0x50, 0x00, 0x00 };

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

	ret = midi_osx_init();
	if(ret != 0) {
		fprintf(stderr, "Can't initialize system MIDI.\n");
		exit(-1);
	}

	/* Start threads. */
	ret = pthread_create(&proc_thrd, NULL, midi_writer, NULL);
	if(ret != 0) {
		fprintf(stderr, "Can't start MIDI writer thread: %s\n",
		    strerror(ret));
		exit(-1);
	}

	/* Search device request for electribe 2 */

	ret = midi_queue_addmsg_sysex(midi_outq,
	    (unsigned char *) midireq, 4);

	/* Usually a program like this would have a writer and a reader thread.
	 * However, since here we're just waiting for a specific response,
	 * we're essentially executing the "reader thread" on the main
	 * thread. */

	ret = midi_get_resp();


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

	return 0;

}


int
midi_get_resp()
{
	int		ret;
	int		beatclkcnt;
	int		beatcnt;
	midi_msg_t	msg;

	beatclkcnt = 0;
	beatcnt = 0;

	ret = pthread_mutex_lock(&midi_inq->mq_mutex);
	if(ret != 0) {
		fprintf(stderr, "Can't lock queue: %s\n", strerror(ret));
		return ENOEXEC;
	}

	while(1) {

		while(!midi_queue_isempty(midi_inq)) {

			ret = midi_queue_getnext(midi_inq, &msg);
			if(ret != 0) {
				fprintf(stderr, "Can't get next message"
				    " from queue: %s\n"
				    " This is bad, exiting\n", strerror(ret));
				exit(-1);
			}

			if(msg.mm_type == MIDI_MSG_SYSRT_START) {
				printf("Start received!\n");
				continue;
			}
				
			if(msg.mm_type == MIDI_MSG_SYSRT_STOP) {
				printf("\nStop received.\n");
			}
		}

		/* No more items to process, so go to sleep until
		 * something happens on the queue */
		ret = pthread_cond_wait(&midi_inq->mq_cond,
		    &midi_inq->mq_mutex);

		if(ret != 0) {
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

	return 0;

}


void *
midi_writer(void *arg)
{
	int		ret;
	int		beatclkcnt;
	int		beatcnt;
	midi_msg_t	msg;
	bstr_t		*midimsg;

	beatclkcnt = 0;
	beatcnt = 0;
	midimsg = 0;

	printf("MIDI writer thread started.\n");

	ret = pthread_mutex_lock(&midi_outq->mq_mutex);
	if(ret != 0) {
		fprintf(stderr, "Can't lock queue: %s\n", strerror(ret));
		return (void *) -1;
	}

	while(1) {

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
		ret = pthread_cond_wait(&midi_outq->mq_cond,
		    &midi_outq->mq_mutex);

		if(ret != 0) {
			fprintf(stderr, "Error while waiting on condvar: %s\n"
			    " This is bad, exiting\n", strerror(ret));
			exit(-1);
		}
	}

	ret = pthread_mutex_unlock(&midi_inq->mq_mutex);
	if(ret != 0) {
		fprintf(stderr, "Can't unlock queue: %s\n", strerror(ret));
		return (void *) -1;
	}

	return (void *) 0;

}
