
#ifndef LLDBMIG_H
#define LLDBMIG_H

#include <lldb/API/LLDB.h>
using namespace lldb;

// use system constants
// PATH_MAX = 1024
// LINE_MAX = 2048
// NAME_MAX = 255
#ifdef __APPLE__
#include <sys/syslimits.h>
#else
#include <climits>
#include <cstring>
#include <unistd.h>

#include "strlxxx.h"
#endif
#include "stringb.h"

#define WAIT_DATA 0
#define MORE_DATA 1

#define FALSE 0
#define TRUE 1

#define THREADS_MAX 50
#define FRAMES_MAX 75

#define VALUE_MAX (NAME_MAX << 1)
#define BIG_VALUE_MAX (NAME_MAX << 3)
#define BIG_LINE_MAX (LINE_MAX << 3)

#define ENV_ENTRIES 200

// static context
struct LIMITS {
    bool istest;
    int frames_max;
    int children_max;
    int walk_depth_max;
    int change_depth_max;
};

// dynamic context
struct STATE {
    int ptyfd;
    bool eof;
    bool procstop;
    bool isrunning;
    bool wanttokill;
    char arch[NAME_MAX];
    int test_sequence;
    char test_script[PATH_MAX];
    const char* envp[ENV_ENTRIES];
    int envpentries;
    char envs[BIG_LINE_MAX];
    char* envspointer;
    char project_loc[PATH_MAX];
    StringB cdtbufferB;
    char cdtptyname[NAME_MAX];
    char logfilename[PATH_MAX];
    const char* gdbPrompt;
    char lldbmi2Prompt[NAME_MAX];
    char threadgroup[NAME_MAX];
    SBDebugger debugger;
    SBProcess process;
    SBListener listener;
    SBTarget target;
    SBLaunchInfo launchInfo = SBLaunchInfo(NULL);
    int threadids[THREADS_MAX];
};

const char* logarg(const char* arg);
void writetocdt(const char* line);
void cdtprintf(const char* format, ...);
void srcprintf(const char* format, ...);
void srlprintf(const char* format, ...);
void signalHandler(int vSigno);

#endif // LLDBMIG_H
