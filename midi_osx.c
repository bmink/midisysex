/*
 * OS X wrapper for some simple MIDI functionality.
 *
 * Many thanks to Craig Stuart Sapp for his example code at:
 *
 *   https://ccrma.stanford.edu/~craig/articles/linuxmidi/osxmidi/
 *
 * which was greatly helpful in developing this wrapper for the
 * not-so-intuitive MIDI APIs of OS X.
 *
 */
#include "midi_osx.h"
#include "midi_queue.h"
#include <pthread.h>
 

#define MIDI_OSX_CLIENTNAME	"midi_osx.c"
#define MIDI_OSX_INPORTNAME	"midi_osx.c_in"
#define MIDI_OSX_OUTPORTNAME	"midi_osx.c_out"
#define MIDI_OSX_MAXMSG   	  65535

static MIDIPortRef osx_midiin;
static MIDIPortRef osx_midiout;
static MIDIClientRef osx_midiclient;

static int midi_osx_ready = 0;

/* Buffer to hold incoming sysex data across callbacks. */
static unsigned char sysex_in[MIDI_OSX_MAXMSG];
static size_t sysex_in_siz;
static int in_sysex;


static  MIDIPortRef osx_midiout;

extern midi_queue_t *midi_inq;
void midi_osx_reader_callback(const MIDIPacketList *, void *, void *);


int
midi_osx_init()
{
	OSStatus	oret;
        ItemCount	osx_srccnt;
        ItemCount	osx_i;
        MIDIEndpointRef	osx_midisrc;

	if(midi_osx_ready)
		return EEXIST;

	oret = MIDIClientCreate(CFSTR(MIDI_OSX_CLIENTNAME), NULL, NULL,
	    &osx_midiclient);
	if(oret) {
		fprintf(stderr, "Can't create MIDI client: OSStatus=%d\n",
		    oret);
		return ENOEXEC;
	}

	oret = MIDIInputPortCreate(osx_midiclient, CFSTR(MIDI_OSX_INPORTNAME),
	    midi_osx_reader_callback, NULL, &osx_midiin);
	if(oret) {
		fprintf(stderr, "Can't create MIDI input port: OSStatus=%d\n",
		    oret);
		return ENOEXEC;
	}

	osx_srccnt = MIDIGetNumberOfSources();
	if(osx_srccnt == 0) {
		fprintf(stderr, "No MIDI sources in the system\n");
		return ENOEXEC;
	}

	for(osx_i = 0; osx_i < osx_srccnt; ++osx_i) {
		osx_midisrc = MIDIGetSource(osx_i);
		if(osx_midisrc == 0) {
			fprintf(stderr, "Can't retrieve MIDI source %lu\n",
			    osx_i);
			return ENOEXEC;
		}
		oret = MIDIPortConnectSource(osx_midiin, osx_midisrc, NULL);
		if(oret) {
			fprintf(stderr, "Can't connect MIDI source %lu:"
			    " OSStatus=%d\n", osx_i, oret);
			return ENOEXEC;
		}
	}

	oret = MIDIOutputPortCreate(osx_midiclient, CFSTR(MIDI_OSX_OUTPORTNAME),
	    &osx_midiout);
	if(oret) {
		fprintf(stderr, "Can't create MIDI output port: OSStatus=%d\n",
		    oret);
		return ENOEXEC;
	}

	in_sysex = 0;
	sysex_in_siz = 0;

	++midi_osx_ready;
	return 0;
}


int
midi_osx_uninit()
{
	OSStatus	oret;

	if(!midi_osx_ready)
		return ENOEXEC;

	oret = MIDIPortDispose(osx_midiin);
	if(oret) {
		fprintf(stderr, "Can't dispose of MIDI in port: OSStatus=%d\n",
		    oret);
		return ENOEXEC;
        }

	oret = MIDIPortDispose(osx_midiout);
	if(oret) {
		fprintf(stderr, "Can't dispose of MIDI out port: OSStatus=%d\n",
		    oret);
		return ENOEXEC;
	}

	oret = MIDIClientDispose(osx_midiclient);
	if(oret) {
		fprintf(stderr, "Can't destroy MIDI client: OSStatus=%d\n",
		    oret);
		return ENOEXEC;
	}

	midi_osx_ready = 0;
	return 0;
}


void
midi_osx_reader_callback(const MIDIPacketList *packets, void* readconn,
	void* srcconn)
{
	/* In OS X, MIDI messages come in through a callback. We read the
	 * message from the OS here, put it on the in queue, and broadcast
	 * on the queue's condvar.  */

	const MIDIPacket	*packet;
	int			i;
	int			t;
	int			cnt;
	int			anyadded;
	int			ret;
	unsigned char		dat;

	packet = &packets->packet[0];
	cnt = packets->numPackets;
	anyadded = 0;

#if 0
	printf("MIDI reader callback called\n");
#endif

	ret = pthread_mutex_lock(&midi_inq->mq_mutex);
	if(ret != 0) {
		fprintf(stderr, "Can't lock queue: %s\n", strerror(ret));
		return;
	}


	for (i = 0; i < cnt; ++i) {

		if(packet == NULL)
			break;

		for(t = 0; t < packet->length; ++t) {
			dat = packet->data[t];

			switch(dat) {
			case 0xF8:
				/* Clock */
#if 0
	printf("Clock\n");
#endif

				ret = midi_queue_addmsg_sysrt(midi_inq,
				    MIDI_MSG_SYSRT_CLOCK);
				if(ret != 0) {
					fprintf(stderr,
					    "Can't add MIDI message: %s\n",
					    strerror(ret));
				}
				anyadded++;
				break;
			case 0xFA:
				/* Start */
				ret = midi_queue_addmsg_sysrt(midi_inq,
				    MIDI_MSG_SYSRT_START);
				if(ret != 0) {
					fprintf(stderr,
					    "Can't add MIDI message: %s\n",
					    strerror(ret));
				}
				anyadded++;
				break;
			case 0xFC:
				/* Stop */
				ret = midi_queue_addmsg_sysrt(midi_inq,
				    MIDI_MSG_SYSRT_STOP);
				if(ret != 0) {
					fprintf(stderr,
					    "Can't add MIDI message:"
					    " %s\n", strerror(ret));
				}
				anyadded++;
				break;
			case 0xF0:
				if(in_sysex) {
					fprintf(stderr, "Received sysex begin"
					    " while in sysex!\n");
					break;
				}
				in_sysex++;
				break;

			case 0xF7:
				if(!in_sysex) {
					fprintf(stderr,
					    "Received sysex end but never saw"
					    " beginning!\n");
					break;
				}
				if(sysex_in_siz == 0) {
					fprintf(stderr,
					    "Zero length Sysex received!\n");
					break;
				}
				ret = midi_queue_addmsg_sysex(midi_inq,
				    sysex_in, sysex_in_siz);
				if(ret != 0) {
					fprintf(stderr,
					    "Can't add MIDI message:"
					    " %s\n", strerror(ret));
				}
				anyadded++;

				in_sysex = 0;
				sysex_in_siz = 0;

				break;

			default:
				if(!in_sysex)
					break;
				if(sysex_in_siz >= MIDI_OSX_MAXMSG) {
					fprintf(stderr,
					    "Sysex data too long.\n");
					break;
				}
				sysex_in[sysex_in_siz] = dat;
				sysex_in_siz++;
				break;
			}
		}


		packet = MIDIPacketNext(packet);
	}

	if(anyadded) {
#if 0
		printf("%d messages on inqueue\n", midi_inq->mq_cnt);
#endif
	}

	ret = pthread_mutex_unlock(&midi_inq->mq_mutex);
	if(ret != 0) {
	fprintf(stderr, "Can't unlock queue: %s\n", strerror(ret));
		return;
	}

}



int
midi_osx_sendmsg(unsigned char *msg, size_t msgsiz)
{
	MIDITimeStamp   timestamp;
	MIDIPacketList  *packetlist;
	MIDIPacket      *currentpacket;
	ItemCount       destcnt;
	ItemCount       idest;
	MIDIEndpointRef destref;
	OSStatus        oret;
	unsigned char	buf[MIDI_OSX_MAXMSG];

	timestamp = 0;	/* "Send now." */


	if(!midi_osx_ready)
		return ENOEXEC;


	/* Send to all MIDI destinations in the system. */

	memset(buf, 0, MIDI_OSX_MAXMSG);
	packetlist = (MIDIPacketList *) buf;
	currentpacket = MIDIPacketListInit(packetlist);
	currentpacket = MIDIPacketListAdd(packetlist, MIDI_OSX_MAXMSG,
	    currentpacket, timestamp, msgsiz, msg);

	destcnt = MIDIGetNumberOfDestinations();

	for(idest = 0; idest < destcnt; idest++) {
		destref = MIDIGetDestination(idest);
		oret = MIDISend(osx_midiout, destref, packetlist);
		if(oret != 0)
			return ENOEXEC;
	}

	return 0;
}
