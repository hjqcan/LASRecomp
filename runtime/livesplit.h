// LiveSplit Server client (runtime/livesplit.c): text commands over TCP on a thread of its own, so the game thread
// never waits on the network. Off unless the LiveSplit option is on; silent and retrying while LiveSplit isn't there.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Any thread: queue one command ("starttimer", "split", "setgametime 0:01:02.345", ...). Dropped while not connected
// (on connecting, run_timer_livesplit_sync brings LiveSplit up to date).
void livesplit_cmd(const char *line);
// The option is on: start the client if it isn't running (it connects in the background). Any thread.
void livesplit_poke(void);
// The connection state for the menu: true while connected.
bool livesplit_connected(void);

#ifdef __cplusplus
}
#endif
