#pragma once
#ifdef __SWITCH2__
#include <cstdio>

// Wraps `file` in a FILE* whose writes only append to an in-memory queue. A
// background thread writes the queue to `file` and flushes it. Logging runs on
// the streaming threads (moonlight-common-c logs FEC recovery and frame loss
// from the video receive thread), and a synchronous SD-card write there stalls
// the socket reads that keep up with the stream.
FILE* switch2OpenAsyncLog(FILE* file);

// Logs the UDP receive buffer size that Borealis's userAppInit configured.
void switch2LogSocketConfiguration();
#endif
