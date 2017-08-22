P = midisysex
OBJS = main.o midi_queue.o midi_osx.o
CFLAGS = -g -Wall
LDLIBS = -lb -framework CoreMIDI -framework CoreServices

$(P): $(OBJS)
	$(CC) -o $(P) $(LDFLAGS) $(OBJS) $(LDLIBS)

clean:
	rm -f *o; rm -f $(P)

