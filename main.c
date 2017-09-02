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

unsigned char	*midi_resp;
size_t		midi_resp_siz;

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

int decode_payload(bstr_t *, unsigned char *, size_t);


int
main(int argc, char **argv)
{
	int		ret;
	pthread_t	write_thrd;
	//unsigned char	midireq[] = { 0x42, 0x50, 0x00, 0x01 };
	//unsigned char	midireq[] = { 0x7E, 0x7F, 0x06, 0x01 };
	unsigned char	midireq[] = { 0x42, 0x30, 0x00, 0x01, 0x23, 0x10 };
	bstr_t		*sysex_payload;
#if 0
	unsigned char	*buf;
	int		i;
#endif

	midi_inq = NULL;
	midi_outq = NULL;

	midi_resp = NULL;
	midi_resp_siz = 0;

	sysex_payload = NULL;

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
	    (unsigned char *) midireq, 6);

	/* Usually a MIDI program would have a writer and a reader thread.
	 * However, since here we're just waiting for a specific response,
	 * we're essentially executing the "reader thread" on the main
	 * thread. */

	ret = midi_get_resp();

	if(ret == ETIMEDOUT) {
		fprintf(stderr, "Timeout: no answer from device.\n");
	} else
	if(ret != 0) {
		fprintf(stderr, "Error: %s.\n", strerror(ret));
	} else
	if(midi_resp == NULL || midi_resp_siz == 0) {
		fprintf(stderr, "Empty response.\n");
	} else {
#if 0
		printf("MIDI response, siz = %zu, msg = ", midi_resp_siz);
		for(i = 0; i < midi_resp_siz; ++i) {
			printf("0x%02x ", midi_resp[i]);
		}
		printf("\n");
#endif

		sysex_payload = binit();
		if(sysex_payload == NULL) {
			fprintf(stderr,
			    "Can't allocate memory for decoded payload.\n");
		} else {
			ret = decode_payload(sysex_payload, midi_resp + 6,
			    midi_resp_siz - 6);
			if(ret != 0) {
				fprintf(stderr,
				    "Can't decode payload.\n");
			} else {
#if 0
				buf = (unsigned char *) bget(sysex_payload);
				if(buf) {
					printf("decoded, siz = %i, msg =\n",
					    bstrlen(sysex_payload));
					for(i = 0; i < bstrlen(sysex_payload);
					    ++i) {
						printf("%d. 0x%02x\n",
						    i, buf[i]);
					}
					printf("\n");
				}
#endif

				ret = btofilep(stdout, sysex_payload);
				if(ret != 0) {
					fprintf(stderr,
					    "Can't write sysex to output.\n");
				}
			}

		}
	}


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

	if(midi_resp)
		free(midi_resp);
	midi_resp_siz = 0;

	buninit(&sysex_payload);

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
	int		err;

	haveresp = 0;
	timeout = 0;
	err = 0;

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
			timeout++;
			err = ETIMEDOUT;
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
#if 0
				printf("Sysex received!\n");
#endif
				haveresp++;
				
				midi_resp = memdup(msg.mm_payload,
				    msg.mm_payload_siz);
				if(midi_resp == NULL) {
					fprintf(stderr,
					    "Can't copy MIDI response.\n");
					break;
				}

				midi_resp_siz = msg.mm_payload_siz;
				break;
			}
				
			(void) midi_msg_free_payload(&msg);
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

	return err;
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

#if 0
	printf("MIDI writer thread started.\n");
	fflush(stdout);
#endif

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
				} else {
#if 0
					printf("MIDI message sent.\n");
#endif
				}

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

#if 0
	printf("MIDI writer thread exiting.\n");
	fflush(stdout);
#endif

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


int
decode_payload(bstr_t *dec, unsigned char *enc, size_t encsiz)
{
	/* All bytes in MIDI payloads have to have their MSB set to 0
	 * (ie. < 0x80) so that interleaved MIDI commands can be distinguished
	 * during transfer (all MIDI commands are >= 0x80). This is done by
	 * taking 7 bytes of payload, stripping them of their MSBs and adding
	 * an 8th byte that contains the 7 bytes' MSBs.
	 */

	unsigned char	cur;
	unsigned char	msb_byte;
	int		i;

	if(dec == NULL || enc == 0 || encsiz == 0)
		return EINVAL;

	for(i = 0; i < encsiz; ++i) {
		cur = enc[i];
	
		if(i % 8 == 0) {
			/* This is the byte that contains the MSBs. */
			msb_byte = cur;
			continue;
		}

		/* Add the MSB. */
		cur += (msb_byte & 1) << 7;

		bmemcat(dec, (char *) &cur, 1);

		/* Move up the bits in the MSB byte. */
		msb_byte >>= 1;
	
	}

	return 0;
}

