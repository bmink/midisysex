#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include "midi_queue.h"


int
midi_queue_init(midi_queue_t **res)
{
	/* NOTE: This function should only be called before any worker threads
	 * that could access this queue have been created. */

	int		ret;
	midi_queue_t	*mq;

	mq = (midi_queue_t *) calloc(1, sizeof(midi_queue_t));

	if(mq == NULL)
		return ENOMEM;

	ret = pthread_mutex_init(&mq->mq_mutex, NULL);
	if(ret != 0) {
		fprintf(stderr, "Can't create mutex for MIDI queue: %s\n",
		    strerror(ret));
		free(mq);
		return -1;
	}

	ret = pthread_cond_init(&mq->mq_cond, NULL);
	if(ret != 0) {
		fprintf(stderr, "Can't create condvar for MIDI queue: %s\n",
		    strerror(ret));

		(void) pthread_mutex_destroy(&mq->mq_mutex);
		free(mq);
		return -1;
	}
		
	*res = mq;
	return 0;
}


int
_midi_queue_addmsg(midi_queue_t *mq, midi_msg_t mmsg)
{
	midi_queue_ent_t	*newent;
	int			ret;

	if(mq == NULL)
		return EINVAL;

	newent = calloc(1, sizeof(midi_queue_ent_t));
	if(newent == NULL)
		return ENOMEM;

	newent->me_msg = mmsg;

	if(mq->mq_first == NULL) {
		/* First entry. */
		mq->mq_first = mq->mq_last = newent;
	} else {
		mq->mq_last->me_next = newent;
		mq->mq_last = newent;
	}

	++mq->mq_cnt; 

	/* Broadcast */
	ret = pthread_cond_broadcast(&mq->mq_cond);
	if(ret != 0) {
		fprintf(stderr, "Can't broadcast on convar: %s\n",
		    strerror(ret));
		return ENOEXEC;
	}

	return 0;
}


int
midi_queue_addmsg_sysrt(midi_queue_t *mq, int type)
{
	/* NOTE: This function should only be called while the caller
	 * is holding the queue's lock. */

	/* Adds a System Real-Time message to the queue.
	 * System Real-Time messages don't have parameters, just a type. */

	midi_msg_t	mmsg;

	if(mq == NULL)
		return EINVAL;

	memset(&mmsg, 0, sizeof(midi_msg_t));
	mmsg.mm_type = type;

	return _midi_queue_addmsg(mq, mmsg);
}


int
midi_queue_addmsg_sysex(midi_queue_t *mq, unsigned char *payload,
	size_t siz)
{
	/* NOTE: This function should only be called while the caller
	 * is holding the queue's lock. */

	/* Adds a System Exclusive message to the queue. The payload should
	 * be what's between the 0xF0 and 0xF7 bytes. */

	midi_msg_t	mmsg;
	int		ret;
	int		i;

	if(mq == NULL)
		return EINVAL;

	if(payload == NULL || siz == 0)
		return EINVAL;

#if 1
	printf("Sysex msg added, siz = %zu, msg = ", siz);
	for(i = 0; i < siz; ++i) {
		printf("0x%02x ", payload[i]);
	}
	printf("\n");
#endif

	memset(&mmsg, 0, sizeof(midi_msg_t));
	mmsg.mm_type = MIDI_MSG_SYSEX;

	mmsg.mm_payload = malloc(siz);
	if(mmsg.mm_payload == NULL)
		return ENOENT;

	memcpy(mmsg.mm_payload, payload, siz);
	mmsg.mm_payload_siz = siz;

	ret = _midi_queue_addmsg(mq, mmsg);
	if(ret != 0)
		midi_msg_free_payload(&mmsg);

	return ret;
}


int
midi_queue_getnext(midi_queue_t *mq, midi_msg_t *mmsg)
{
	/* NOTE: This function should only be called while the caller
	 * is holding the queue's lock. */

	/* Detaches the first message from the MIDI queue. Returns the
	 * message to the caller. The detached queue entry will be freed and
	 * the message values will be copied into the struct pointed to by the
	 * mmsg argument. When done with the message, caller should call
	 * midi_msg_free_payload() to make sure payload is freed correctly. */

	midi_queue_ent_t	*ent;

	if(mq == NULL)
		return EINVAL;

	if(midi_queue_isempty(mq))
		return ENOENT;

	ent = mq->mq_first;

	/* Detach */
	if(ent == mq->mq_last) {
		/* This was the only entry in the list */
		mq->mq_first = mq->mq_last = NULL;
		
	} else
		mq->mq_first = ent->me_next;


	/* Copy values */
	*mmsg = ent->me_msg;


	/* Free entry */
	free(ent);
	
	--mq->mq_cnt;

	return 0;

}


int
midi_queue_isempty(midi_queue_t *mq)
{
	/* NOTE: This function should only be called while the caller
	 * is holding the queue's lock. */

	if(mq == NULL) {
		fprintf(stderr, "mq==NULL in midi_queue_isempty()\n");
		return -1;
	}

	if(mq->mq_first)
		return 0;
	else
		return 1;
}


int
midi_queue_uninit(midi_queue_t **mq)
{
	/* NOTE: This function should only be called after all worker threads
	 * that could access this queue have exited. */

	midi_msg_t	foo;
	int		ret;

	if(mq == NULL)
		return EINVAL;

	/* Consume all remaining messages on queue. No need to acquire the
	 * lock. We assume there are no more worker threads running. */
	while(!midi_queue_isempty(*mq)) {
		ret = midi_queue_getnext(*mq, &foo);
		if(ret != 0)
			return ENOEXEC;
	}

	ret = pthread_mutex_destroy(&((*mq)->mq_mutex));
	if(ret != 0) {
		fprintf(stderr, "Can't destroy mutex for MIDI queue: %s\n",
		    strerror(ret));
		free(mq);
		return -1;
	}

	ret = pthread_cond_destroy(&((*mq)->mq_cond));
	if(ret != 0) {
		fprintf(stderr, "Can't destroy condvar for MIDI queue: %s\n",
		    strerror(ret));
		free(mq);
		return -1;
	}

	free(*mq);

	*mq = NULL;

	return 0;
}


int
midi_msg_free_payload(midi_msg_t *msg)
{
	if(msg == NULL)
		return EINVAL;

	if(msg->mm_payload)
		free(msg->mm_payload);

	msg->mm_payload = NULL;
	msg->mm_payload_siz = 0;

	return 0;
}
