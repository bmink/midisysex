#ifndef MIDI_QUEUE_H
#define MIDI_QUEUE_H

#include <pthread.h>

#define MIDI_MSG_SYSRT_CLOCK		0
#define MIDI_MSG_SYSRT_START		1
#define MIDI_MSG_SYSRT_STOP		2
#define MIDI_MSG_SYSEX			3

typedef struct midi_msg {
	int			mm_type;
	int			mm_chan;
	int			mm_val;
	unsigned char	        *mm_payload;
	size_t			mm_payload_siz;
} midi_msg_t;


typedef struct midi_queue_ent {
	midi_msg_t		me_msg;
	struct midi_queue_ent	*me_next;
} midi_queue_ent_t;


typedef struct midi_queue {
	int			mq_cnt;
	midi_queue_ent_t	*mq_first;
	midi_queue_ent_t	*mq_last;

	pthread_mutex_t		mq_mutex;
	pthread_cond_t		mq_cond;
} midi_queue_t;

/* NOTE: The below functions should only be called from the main thread,
 * when there are no other threads (yet or anymore) running that could access
 * the queue. */
int midi_queue_init(midi_queue_t **);
int midi_queue_uninit(midi_queue_t **);

/* NOTE: The below functions must only be called after the queue's lock has
 * been acquired. */
int midi_queue_addmsg_sysrt(midi_queue_t *, int);
int midi_queue_addmsg_chancc(midi_queue_t *, int, int, int);
int midi_queue_addmsg_sysex(midi_queue_t *, unsigned char *, size_t);
int midi_queue_isempty(midi_queue_t *);
int midi_queue_getnext(midi_queue_t *, midi_msg_t *);

/* NOTE: the below functions can be called at any time. */
int midi_msg_free_payload(midi_msg_t *);

#endif
