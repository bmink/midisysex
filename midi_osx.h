#ifndef MIDI_OSX
#define MIDI_OSX

#include <CoreMIDI/CoreMIDI.h>

int midi_osx_init();
int midi_osx_uninit();

int midi_osx_sendmsg(unsigned char *, size_t);

#endif
