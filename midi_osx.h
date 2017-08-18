#ifndef MIDI_OSX
#define MIDI_OSX

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

#include <CoreMIDI/CoreMIDI.h>

int midi_osx_init();
int midi_osx_uninit();

int midi_osx_sendmsg(unsigned char *, size_t);

#endif
