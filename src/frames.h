
#ifndef FRAMES_H
#define FRAMES_H

#include <string>

typedef enum { WITH_LEVEL = 0x1, WITH_ARGS = 0x2, WITH_LEVEL_AND_ARGS = 0x3, JUST_LEVEL_AND_ARGS = 0x7 } FrameDetails;

int getNumFrames(SBThread thread);
void selectValidFrame(SBThread thread);

std::string formatBreakpoint(SBBreakpoint breakpoint, Lldbmi2* pstate);
std::string formatFrame(SBFrame frame, FrameDetails details);
char* formatThreadInfo(StringB& threaddesc, SBProcess process, int threadindexid);


char* formatThreadInfo(SBProcess process, int threadindexid);

#endif // FORMFRAMES_HAT_H
