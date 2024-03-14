
#include "lldbmi2.h"

#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <iostream>

#include <string.h>
#include <string>
#include "linenoise.h"

#include <lldb/API/SBDebugger.h>

#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/timeb.h>
#ifdef __APPLE__
#include <util.h>
#endif
#ifdef UNLINKED
#include <dlfcn.h>
#endif

#include "engine.h"
#include "variables.h"
#include "names.h"
#include "frames.h"
#include "log.h"
#include "test.h"
#include "version.h"

#include "events.h"

Lldbmi2::Lldbmi2()
{
    logprintf(LOG_TRACE, "Lldbmi2 ctor (0x%x)\n", this);
    SBDebugger::Initialize();
    debugger = SBDebugger::Create();
    debugger.SetAsync(true);
    listener = debugger.GetListener();
}

void Lldbmi2::help()
{
    std::cerr << lldbmi2Prompt << std::endl <<
R"(Description:
   A MI2 interface to LLDB
Authors:
   Didier Bertrand, 2015, 2016, 2018
   Eduard Matveev, 2016
   David Jenkins, 2018
Syntax:
   lldbmi2 --version [options]
   lldbmi2 --interpreter mi2 [options]
Arguments:
   --version:           Return GDB's version (GDB 7.12.1) and exits.
   --interpreter mi2:   Standard mi2 interface.
   --interpreter=mi2:   Standard mi2 interface.
Options:
   --log:                Create log file in project root directory.
   --logmask mask:       Select log categories. 0xFFF. See source code for values.
   --arch arch_name:     Force a different architecture from host architecture: arm64, x86_64, i386
   --test n:             Execute test sequence (to debug lldmi2).
   --script file_path:   Execute test script or replay logfile (to debug lldmi2).
   --nx:                 Ignored.
)"
    << "   --frames frames:      Max number of frames to display (" << FRAMES_MAX << ")." << std::endl
    << "   --children children:  Max number of children to check for update (" << CHILDREN_MAX << ")." << std::endl
    << "   --walkdepth depth:    Max walk depth in search for variables (" << WALK_DEPTH_MAX << ")." << std::endl
    << "   --changedepth depth:  Max depth to check for updated variables (" << CHANGE_DEPTH_MAX << ")." << std::endl;
}

bool Lldbmi2::addEnvironment(const char* entrystring) {
    logprintf(LOG_NONE, "addEnvironment (0x%x, %s)\n", this, entrystring);
    size_t entrysize = strlen(entrystring);
    if (envpentries >= ENV_ENTRIES - 2) { // keep size for final NULL
        logprintf(LOG_ERROR, "addEnvironment: envp size (%d) too small\n", sizeof(envs));
        return false;
    }
    if (envspointer - envs + 1 + entrysize >= sizeof(envs)) {
        logprintf(LOG_ERROR, "addEnvironment: envs size (%d) too small\n", sizeof(envs));
        return false;
    }
    envp[envpentries++] = envspointer;
    envp[envpentries] = NULL;
    strcpy(envspointer, entrystring);
    envspointer += entrysize + 1;
    logprintf(LOG_ARGS | LOG_RAW, "envp[%d]=%s\n", envpentries - 1, envp[envpentries - 1]);
    return true;
}

Lldbmi2::~Lldbmi2() {
    logprintf(LOG_TRACE, "Lldbmi2 dtor\n");
    waitProcessListener();
    SBDebugger::Terminate();
}

LIMITS limits;
Lldbmi2* gpstate;

int main(int argc, char** argv, char** envp) {
#ifdef UNLINKED
    void* ret = dlopen("liblldb.so", RTLD_NOW | RTLD_GLOBAL);
    if (ret == NULL) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return EXIT_FAILURE;
    }
#endif

    gpstate = new Lldbmi2();

    char commandLine[BIG_LINE_MAX]; // data from cdt
    int isVersion = 0, isInterpreter = 0;
    int isLog = 0;
    unsigned int logmask = LOG_DEV;

    gpstate->ptyfd = EOF;
    gpstate->cdtptyfd = EOF;
    gpstate->gdbPrompt = "GNU gdb (GDB) 7.12.1";
    snprintf(gpstate->lldbmi2Prompt, NAME_MAX, "lldbmi2 version %s", LLDBMI2_VERSION);

    gpstate->cdtbufferB.grow(BIG_LINE_MAX);

    limits.frames_max = FRAMES_MAX;
    limits.children_max = CHILDREN_MAX;
    limits.walk_depth_max = WALK_DEPTH_MAX;
    limits.change_depth_max = CHANGE_DEPTH_MAX;

    // create a log filename from program name and open log file
    if (true) {
        setlogfile(gpstate->logfilename, sizeof(gpstate->logfilename), argv[0], "lldbmi2.log");
        openlogfile(gpstate->logfilename);
        setlogmask(logmask);
    }

    // get args
    for (int narg = 0; narg < argc; narg++) {
        logarg(argv[narg]);
        if (strcmp(argv[narg], "--version") == 0)
            isVersion = 1;
        else if (strcmp(argv[narg], "--interpreter") == 0) {
            isInterpreter = 1;
            if (++narg < argc)
                logarg(argv[narg]);
        } else if (strcmp(argv[narg], "--interpreter=mi2") == 0)
            isInterpreter = 1;
        else if ((strcmp(argv[narg], "-i") == 0) && (strcmp(argv[narg + 1], "mi") == 0))
            isInterpreter = 1;
        else if (strcmp(argv[narg], "--arch") == 0) {
            if (++narg < argc)
                strcpy(gpstate->arch, logarg(argv[narg]));
        } else if (strcmp(argv[narg], "--test") == 0) {
            limits.istest = true;
            if (++narg < argc)
                sscanf(logarg(argv[narg]), "%d", &(gpstate->test_sequence));
            if (gpstate->test_sequence)
                setTestSequence(gpstate->test_sequence);
        } else if (strcmp(argv[narg], "--script") == 0) {
            limits.istest = true;
            if (++narg < argc)
                strcpy(gpstate->test_script, logarg(argv[narg])); // no spaces allowed in the name
            if (gpstate->test_script[0])
                setTestScript(gpstate->test_script);
        } else if (strcmp(argv[narg], "--log") == 0)
            isLog = 1;
        else if (strcmp(argv[narg], "--logmask") == 0) {
            isLog = 1;
            if (++narg < argc)
                sscanf(logarg(argv[narg]), "%x", &logmask);
        } else if (strcmp(argv[narg], "--frames") == 0) {
            if (++narg < argc)
                sscanf(logarg(argv[narg]), "%d", &limits.frames_max);
        } else if (strcmp(argv[narg], "--children") == 0) {
            if (++narg < argc)
                sscanf(logarg(argv[narg]), "%d", &limits.children_max);
        } else if (strcmp(argv[narg], "--walkdepth") == 0) {
            if (++narg < argc)
                sscanf(logarg(argv[narg]), "%d", &limits.walk_depth_max);
        } else if (strcmp(argv[narg], "--changedepth") == 0) {
            if (++narg < argc)
                sscanf(logarg(argv[narg]), "%d", &limits.change_depth_max);
        }
        else if (strcmp (argv[narg],"-ex") == 0) {
            if (++narg<argc) {
                if (strncmp(argv[narg], "new-ui", strlen("new-ui")) == 0) {
                    sscanf(argv[narg], "new-ui mi %s", gpstate->cdtptyname);
                    logprintf (LOG_INFO, "pty %s\n", gpstate->cdtptyname);
                    
                    gpstate->cdtptyfd = open (gpstate->cdtptyname, O_RDWR);

                    #if 0
                    // set pty in raw mode
                    struct termios t;
                    if (tcgetattr(gpstate->cdtptyfd, &t) != -1) {
                        logprintf (LOG_INFO, "setting pty\n");
                        // Noncanonical mode, disable signals, extended input processing, and echoing
                        t.c_lflag &= ~(ICANON | ISIG | IEXTEN | ECHO);
                        // Disable special handling of CR, NL, and BREAK.
                        // No 8th-bit stripping or parity error handling
                        // Disable START/STOP output flow control
                        t.c_iflag &= ~(BRKINT | ICRNL | IGNBRK | IGNCR | INLCR |
                                INPCK | ISTRIP | IXON | PARMRK);
                        // Disable all output processing
                        t.c_oflag &= ~OPOST;
                        t.c_cc[VMIN] = 1;        // Character-at-a-time input
                        t.c_cc[VTIME] = 0;        // with blocking
                        int ret = tcsetattr(gpstate->cdtptyfd, TCSAFLUSH, &t);
                        logprintf (LOG_INFO, "setting pty %d\n", ret);
                    }
                    #endif
                }
            }
        }
    }

    // log program args
    addlog("\n");
    logprintf(LOG_ARGS, NULL);

    gpstate->envp[0] = NULL;
    gpstate->envpentries = 0;
    gpstate->envspointer = gpstate->envs;
    const char* wl = "PWD="; // want to get eclipse project_loc if any
    int wll = strlen(wl);
    // copy environment for tested program
    for (int ienv = 0; envp[ienv]; ienv++) {
        gpstate->addEnvironment(envp[ienv]);
        if (strncmp(envp[ienv], wl, wll) == 0)
            strcpy(gpstate->project_loc, envp[ienv] + wll);
    }

    // return gdb version if --version
    if (isVersion) {
        cdtprintf ("%s, %s, %s\n", gpstate->gdbPrompt, gpstate->lldbmi2Prompt, SBDebugger::GetVersionString());
        return EXIT_SUCCESS;
    }
    // check if --interpreter mi2
    else if (!isInterpreter) {
        gpstate->help();
        return EXIT_FAILURE;
    }

    signal(SIGINT, signalHandler);
    //signal(SIGSTOP, signalHandler);

    logprintf(LOG_TRACE, "printing prompt\n");
    cdtprintf("(gdb)\n");
    const char *prompt = gpstate->debugger.GetPrompt();

    /* Asynchronous mode using the multiplexing API: wait for
    * data on stdin, and simulate async data coming from some source
    * using the select(2) timeout. */
    struct linenoiseState ls;
    char buf[1024];
    char *line = linenoiseEditMore;
    linenoiseEditStart(&ls,-1,-1,buf,sizeof(buf), prompt);

    // main loop
    while (!gpstate->eof) {

        fd_set readfds;
        FD_ZERO(&readfds);

        struct timeval tv;
        tv.tv_sec = 0; // 0.1 sec timeout
        tv.tv_usec = 100000;

        int nfds = STDIN_FILENO+1;
        // check command from CDT
        FD_SET(STDIN_FILENO, &readfds);
        if (gpstate->cdtptyfd != EOF) {
            // check data from Eclipse's console
            FD_SET (gpstate->cdtptyfd, &readfds);
            logprintf (LOG_TRACE, "select from both\n");
            nfds = std::max(nfds, gpstate->cdtptyfd+1);
        }
        if (gpstate->ptyfd != EOF) {
            // check data from Eclipse's console
            FD_SET(gpstate->ptyfd, &readfds);
            logprintf (LOG_TRACE, "select from both\n");
            nfds = std::max(nfds, gpstate->ptyfd+1);
        }

        int retval = select(nfds, &readfds, NULL, NULL, &tv);

        if (FD_ISSET(STDIN_FILENO, &readfds) && !gpstate->eof) {
            line = linenoiseEditFeed(&ls);
            /* A NULL return means: line editing is continuing.
            * Otherwise the user hit enter or stopped editing
            * (CTRL+C/D). */
            if (line != linenoiseEditMore) {
                
                linenoiseEditStop(&ls);
                
                long chars = strlen(line);
                if (chars>0) {
                    SBCommandInterpreter interp = gpstate->debugger.GetCommandInterpreter();
                    SBCommandReturnObject result;
                    ReturnStatus retcode = interp.HandleCommand(line, result);
                    
                    write(STDOUT_FILENO, result.GetOutput(), result.GetOutputSize());
                    write(STDERR_FILENO, result.GetError(), result.GetErrorSize());            
                }
                else
                    gpstate->eof = true;
                 
                free(line);
                linenoiseEditStart(&ls,-1,-1,buf,sizeof(buf), prompt);

            } 
        }

        if (line == NULL) exit(0); /* Ctrl+D/C. */

        char consoleLine[LINE_MAX];            // data from eclipse's console
        if (gpstate->cdtptyfd!=EOF) {            // input from user to program
            if (FD_ISSET(gpstate->cdtptyfd, &readfds) && !gpstate->eof && !limits.istest) {
                logprintf (LOG_TRACE, "cdt pty read in\n");
                long chars = read (gpstate->cdtptyfd, consoleLine, sizeof(consoleLine)-1);
                logprintf (LOG_TRACE, "cdt pty read out %d chars\n", chars);
                if (chars>0) {
                    logprintf (LOG_PROG_OUT, "cdt pty read %d chars: '%s'\n", chars, consoleLine);
                    consoleLine[chars] = '\0';

                while (gpstate->fromCDT (consoleLine,sizeof(consoleLine)) == MORE_DATA)
                    consoleLine[0] = '\0';
                }
            }
        }


        if (gpstate->ptyfd!=EOF) {            // input from user to program

            if (FD_ISSET(gpstate->ptyfd, &readfds) && !gpstate->eof && !limits.istest) {
                char consoleLine[LINE_MAX]; // data from eclipse's console
                long chars = read(gpstate->ptyfd, consoleLine, sizeof(consoleLine) - 1);
                if (chars > 0) {
                    consoleLine[chars] = '\0';
                }
            }
        }

        // execute test command if test mode
        if (!gpstate->eof && limits.istest && !gpstate->isrunning) {
            const char* testCommand = NULL;
            if ((testCommand = getTestCommand()) != NULL) {
                snprintf(commandLine, sizeof(commandLine), "%s\n", testCommand);
                gpstate->fromCDT(commandLine, sizeof(commandLine));
            }
        }
        // execute stacked commands if many command arrived once
        if (!gpstate->eof && gpstate->cdtbufferB.size() > 0) {
            commandLine[0] = '\0';
            while (gpstate->fromCDT(commandLine, sizeof(commandLine)) == MORE_DATA)
                ;
        }
    }

    if (gpstate->ptyfd != EOF)
        close(gpstate->ptyfd);

    logprintf(LOG_INFO, "main exit\n");
    closelogfile();
    delete gpstate;

    return EXIT_SUCCESS;
}

void Lldbmi2::setSignals()
{
    logprintf(LOG_TRACE, "setSignals (0x%x)\n", this);
    SBUnixSignals us = process.GetUnixSignals();
    if (!limits.istest || true) {
        const char* signame = "SIGINT";
        int signo = us.GetSignalNumberFromName(signame);
        logprintf(LOG_NONE, "signals before for %s (%d): suppress=%d, stop=%d, notify=%d\n", signame, signo,
                  us.GetShouldSuppress(signo), us.GetShouldStop(signo), us.GetShouldNotify(signo));
        us.SetShouldSuppress(signo, false); // !pass
        us.SetShouldStop(signo, false);
        us.SetShouldNotify(signo, true);
        logprintf(LOG_NONE, "signals after for %s (%d): suppress=%d, stop=%d, notify=%d\n", signame, signo,
                  us.GetShouldSuppress(signo), us.GetShouldStop(signo), us.GetShouldNotify(signo));
    }
}

int Lldbmi2::startProcessListener()
{
    procstop = false;
    logprintf(LOG_TRACE, "startProcessListener (0x%x)\n", this);
    int ret = pthread_create(&sbTID, NULL, &processListener, this);
    if (ret)
        sbTID = 0;
    return ret;
}

void Lldbmi2::waitProcessListener() {
    logprintf(LOG_TRACE, "waitProcessListener ()\n");
    if (sbTID)
        pthread_join(sbTID, NULL);
}

void Lldbmi2::handleBreakpointCommand(CDT_COMMAND& cc, int nextarg) {
    if (strcmp(cc.argv[0], "-break-insert") == 0) {
        // break-insert --thread-group i1 -f /project_path/tests/Sources/tests.cpp:17
        // break-insert --thread-group i1 -t -f main
        bool isoneshot = false;
        bool ispending = false;
        bool isenabled = true;
        char path[PATH_MAX];
        for (; nextarg < cc.argc; nextarg++) {
            if (strcmp(cc.argv[nextarg], "-t") == 0)
                isoneshot = true;
            else if (strcmp(cc.argv[nextarg], "-f") == 0)
                ispending = true;
            else if (strcmp(cc.argv[nextarg], "-d") == 0)
                isenabled = false;
            snprintf(path, sizeof(path), cc.argv[nextarg], project_loc);
            if (strstr(cc.argv[nextarg], "%s") != NULL)
                logprintf(LOG_VARS, "%%s -> %s\n", path);
        }
        char* pline;
        SBBreakpoint breakpoint;
        if ((pline = strchr(path, ':')) != NULL) {
            *pline++ = '\0';
            int iline = 0;
            sscanf(pline, "%d", &iline);
            breakpoint = target.BreakpointCreateByLocation(path, iline);
        } else if ((pline = strchr(path, '*')) != NULL) { // address
            *pline++ = '\0';
            addr_t addr = 0;
#ifdef __APPLE__
            sscanf(pline, "%lld", &addr);
#else
            sscanf(pline, "%lu", &addr);
#endif
            breakpoint = target.BreakpointCreateByAddress(addr);
        } else // function
            breakpoint = target.BreakpointCreateByName(path, target.GetExecutable().GetFilename());
        breakpoint.SetEnabled(isenabled);
        if ((breakpoint.GetNumLocations() > 0) || ispending) {
            breakpoint.SetOneShot(isoneshot);
            char* breakpointdesc = formatBreakpoint(breakpoint, this);
            cdtprintf("%d^done,bkpt=%s\n(gdb)\n", cc.sequence, breakpointdesc);
        } else {
            target.BreakpointDelete(breakpoint.GetID());
            cdtprintf("^error,msg=\"could not find %s\"\n(gdb) \n", path);
        }
    } else if (strcmp(cc.argv[0], "-break-delete") == 0) {
        // 11-break-delete 1
        // 11^done
        int bpid = 0;
        sscanf(cc.argv[nextarg], "%d", &bpid);
        target.BreakpointDelete(bpid);
        cdtprintf("%d^done\n(gdb)\n", cc.sequence);
    } else if (strcmp(cc.argv[0], "-break-enable") == 0) {
        // 11-break-enable 1
        // 11^done
        int bpid = 0;
        sscanf(cc.argv[nextarg], "%d", &bpid);
        SBBreakpoint breakpoint = target.FindBreakpointByID(bpid);
        breakpoint.SetEnabled(1);
        cdtprintf("%d^done\n(gdb)\n", cc.sequence);
    } else if (strcmp(cc.argv[0], "-break-disable") == 0) {
        // 11-break-disable 1
        // 11^done
        int bpid = 0;
        sscanf(cc.argv[nextarg], "%d", &bpid);
        SBBreakpoint breakpoint = target.FindBreakpointByID(bpid);
        breakpoint.SetEnabled(0);
        cdtprintf("%d^done\n(gdb)\n", cc.sequence);
    } else if (strcmp(cc.argv[0], "-break-watch") == 0) {
        // -break-watch [-r|-a] expression
        // Set a watch on address that results from evaluating 'expression'
        char expression[LINE_MAX];
        char watchExpr[LINE_MAX + 10];
        bool isRead = false;
        bool isWrite = true;
        if (strcmp(cc.argv[nextarg], "-a") == 0) {
            isRead = true;
            nextarg++;
        }
        if (strcmp(cc.argv[nextarg], "-r") == 0) {
            isRead = true;
            isWrite = false;
            nextarg++;
        }
        if (nextarg < cc.argc)
            strlcpy(expression, cc.argv[nextarg++], sizeof(expression));

        /* Convert Pascal expression to C
         *  Expected formats from laz-ide are:
         *    type(addr_t^)
         *    ^type(addr_t^)
         *  In spite of '^' at end of addresses we want straight address not dereference
         */
        char* typeStr = expression;
        if (*typeStr == '^')
            typeStr++;
        char* addrStr = strchr(typeStr, '(');
        if (addrStr != NULL) {
            *addrStr++ = '\0';
            char* tmp = strchr(addrStr, ')');
            if ((tmp != NULL) && (*(tmp - 1) == '^')) {
                tmp--;
                *tmp = '\0';
            }
        }
        snprintf(watchExpr, sizeof(watchExpr), "(%s *)(%s)", typeStr, addrStr);
        /*
         * End of Pascal manipulation
         */

        SBValue val = target.EvaluateExpression(watchExpr);
        if (val.IsValid()) {
            addr_t watchAddr = val.GetValueAsUnsigned();
            if (watchAddr != 0) {
                SBError error;
                SBWatchpoint watch = target.WatchAddress(watchAddr, val.GetByteSize(), isRead, isWrite, error);
                if (watch.IsValid() && error.Success()) {
                    cdtprintf("%d^done,wpt={number=\"%d\",\"%s\"}\n(gdb)\n", cc.sequence, watch.GetID(), watchExpr);
                } else
                    cdtprintf("^error,msg=\"Could not create watch: %s\"\n(gdb) \n", error.GetCString());
            } else
                cdtprintf("^error,msg=\"Value failed to return valid address (%s %s %p)\"\n(gdb) \n", watchExpr,
                          val.GetValue(), watchAddr);
        } else {
            SBError err = val.GetError();
            cdtprintf("^error,msg=\"Expression does not return valid value: %s\"\n(gdb) \n", err.GetCString());
        }
    } else {
        logprintf(LOG_WARN, "command not understood: ");
        logdata(LOG_NOHEADER, cc.argv[0], strlen(cc.argv[0]));
        cdtprintf("%d^error,msg=\"%s, command: '%s'\"\n(gdb)\n", cc.sequence, "Command unimplemented.", cc.argv[nextarg]);
    }
}

void Lldbmi2::handleVariableCommand(CDT_COMMAND& cc, int nextarg) {
    if (strcmp(cc.argv[0], "-var-create") == 0) {
        // TODO: strlen(variable) not updated in expression pane
        // var-create --thread 1 --frame 0 - * a
        //     name="var1",numchild="0",value="1",type="int",thread-id="1",has_more="0"
        //     name="var2",numchild="1",value="0x100000f76 \"2\"",type="char *",thread-id="1",has_more=a"0"
        char expression[NAME_MAX];
        const char* sep = "";
        *expression = '\0';
        if (*cc.argv[nextarg] == '-' && *cc.argv[nextarg + 1] == '*') {
            nextarg += 2;
            while (nextarg < cc.argc) {
                strlcat(expression, sep, sizeof(expression));
                strlcat(expression, cc.argv[nextarg++], sizeof(expression));
                sep = " ";
            }
            SBThread thread = process.GetSelectedThread();
            if (thread.IsValid()) {
                SBFrame frame = thread.GetSelectedFrame();
                if (frame.IsValid()) {
                    // Find then Evaluate to avoid recreate variable
                    SBValue var = getVariable(frame, expression);
                    std::string varName = "var";
                    varName += std::to_string(nextSessionVariableId++);
                    sessionVariables[varName] = var;

                    if (var.IsValid() && var.GetError().Success()) {
                        // should remove var.GetError().Success() but update do not work very well
                        updateVarState(var, limits.change_depth_max);
                        int varnumchildren = var.GetNumChildren();
                        SBType vartype = var.GetType();
                        static StringB vardescB(VALUE_MAX);
                        vardescB.clear();
                        formatValue(vardescB, var, FULL_SUMMARY); // was NO_SUMMARY
                        char* vardesc = vardescB.c_str();
                        if (var.IsDynamic() || var.IsSynthetic() || var.IsSyntheticChildrenGenerated()) {
                            cdtprintf ("%d^done,name=\"%s\",numchild=\"0\",value=\"%s\","
                                "type=\"%s\",thread-id=\"%d\",dynamic=\"1\",has_more=\"1\"\n(gdb)\n",
                                cc.sequence, varName.c_str(), vardesc,
                                vartype.GetDisplayTypeName(), thread.GetIndexID());
                        }
                        else {
                            cdtprintf ("%d^done,name=\"%s\",numchild=\"%d\",value=\"%s\","
                                "type=\"%s\",thread-id=\"%d\",has_more=\"0\"\n(gdb)\n",
                                cc.sequence, varName.c_str(), varnumchildren, vardesc,
                                vartype.GetDisplayTypeName(), thread.GetIndexID());
                        }
                    } else
                        cdtprintf("%d^error\n(gdb)\n", cc.sequence);
                } else
                    cdtprintf("%d^error\n(gdb)\n", cc.sequence);
            } else
                cdtprintf("%d^error\n(gdb)\n", cc.sequence);
        } else
            cdtprintf("%d^error\n(gdb)\n", cc.sequence);
    } else if (strcmp(cc.argv[0], "-var-update") == 0) {
        // 47-var-update 1 var2
        // 47^done,changelist=[]
        // 41^done,changelist=[{name="var3",value="44",in_scope="true",type_changed="false",has_more="0"}]
        int printvalues = 1;
        char expression[NAME_MAX];
        if (cc.argv[nextarg] != NULL) // print-values
            if (isdigit(*cc.argv[nextarg]))
                sscanf(cc.argv[nextarg++], "%d", &printvalues);
        if (nextarg < cc.argc) // variable name
            strlcpy(expression, cc.argv[nextarg++], sizeof(expression));
        SBThread thread = process.GetSelectedThread();
        if (thread.IsValid()) {
            SBFrame frame = thread.GetSelectedFrame();
            if (frame.IsValid()) {
                SBValue var = getVariable(frame, expression); // find variable
                if (var.IsValid() && var.GetError().Success()) {
                    bool separatorvisible = false;
                    SBFunction function = frame.GetFunction();
                    char* changedesc = formatChangedList(var, separatorvisible, limits.change_depth_max);
                    cdtprintf("%d^done,changelist=[%s]\n(gdb)\n", cc.sequence, changedesc);
                } else
                    cdtprintf("%d^done,changelist=[]\n(gdb)\n", cc.sequence);
            } else
                cdtprintf("%d^error\n(gdb)\n", cc.sequence);
        } else
            cdtprintf("%d^error\n(gdb)\n", cc.sequence);
    } else if (strcmp(cc.argv[0], "-var-list-children") == 0) {
        // 34-var-list-children var2
        // 34^done,numchild="1",children=[child={name="var2.*b",exp="*b",numchild="0",type="char",thread-id="1"}],has_more="0"
        // 52-var-list-children var5
        // gdb:
        // 52^done,numchild="3",children=[child={name="var5.a",exp="a",numchild="0",type="int",thread-id="1"},child={name="var5.b",exp="b",numchild="1",type="char
        // *",thread-id="1"},child={name="var5.y",exp="y",numchild="4",type="Y *",thread-id="1"}],has_more="0" mi2:
        // 52^done,numchild="3",children=[child={name="z.a",exp="a",numchild="0",type"int",thread-id="1"},child={name="z.b",exp="b",numchild="1",type"char
        // *",thread-id="1"},child={name="z.y",exp="y",numchild="4",type"Y *",thread-id="1"}]",has_more="0"\n(gdb)\n|
        // 61-var-list-children var5.y
        // 52^done,numchild="3",children=[child={name="var5.a",exp="a",numchild="0",type="int",thread-id="1"},child={name="var5.b",exp="b",numchild="1",type="char
        // *",thread-id="1"},child={name="var5.y",exp="y",numchild="4",type="Y *",thread-id="1"}],has_more="0"
        //|52^done,numchild="3",children=[child={name="z->a",exp="a",numchild="0",type"int",thread-id="1"},child={name="z->b",exp="b",numchild="1",type"char
        //*",thread-id="1"},child={name="z->y",exp="y",numchild="4",type"Y *",thread-id="1"}]",has_more="0"\n(gdb)\n|

        char name[NAME_MAX];
        *name = '\0';
        if (nextarg < cc.argc)
            strlcpy (name, cc.argv[nextarg++], sizeof(name));
        //SBValue var = getVariable (frame, expression);
        SBValue var = sessionVariables[name];
        if (var.IsValid() && var.GetError().Success()) {
            int varnumchildren = var.GetNumChildren();
            var.SetPreferDynamicValue(DynamicValueType::eDynamicCanRunTarget);
            var.SetPreferSyntheticValue(true);

            std::string childrendesc;
            for (int i = 0; i < var.GetNumChildren(); ++i) {

                SBValue child = var.GetChildAtIndex(i);

                std::string fullName = std::string(name) + "." + child.GetName();

                if (!childrendesc.empty()) {
                    childrendesc += ",";
                }
                childrendesc += "child={";
                childrendesc += "name=\"";
                childrendesc += fullName;
                childrendesc += "\",exp=\"";
                SBStream s;
                child.GetExpressionPath(s);
                childrendesc += child.GetName();
                childrendesc += "\",numchild=\"";
                childrendesc += std::to_string(child.GetNumChildren());
                childrendesc += "\",type=\"";
                childrendesc += child.GetType().GetDisplayTypeName();
                childrendesc += "\"}";

                sessionVariables[fullName] = child;
            }

            // 34^done,numchild="1",children=[child={name="var2.*b",exp="*b",numchild="0",type="char",thread-id="1"}],has_more="0"
            cdtprintf ("%d^done,numchild=\"%d\",children=[%s]\",has_more=\"0\"\n(gdb)\n",
                    cc.sequence, varnumchildren, childrendesc.c_str());

        } else
            cdtprintf("%d^error\n(gdb)\n", cc.sequence);
    } else if (strcmp(cc.argv[0], "-var-info-path-expression") == 0) {
        // 35-var-info-path-expression var2.*b
        // 35^done,path_expr="*(b)"
        // 55-var-info-path-expression var5.y
        // 55^done,path_expr="(z)->y"
        // 65-var-info-path-expression var5.y.d
        // 65^done,path_expr="((z)->y)->d"
        char expression[NAME_MAX];
        *expression = '\0';
        if (nextarg < cc.argc)
            strlcpy(expression, cc.argv[nextarg++], sizeof(expression));
        if (*expression != '$') // it is yet a path
            cdtprintf("%d^done,path_expr=\"%s\"\n(gdb)\n", cc.sequence, expression);
        else {
            SBThread thread = process.GetSelectedThread();
            if (thread.IsValid()) {
                SBFrame frame = thread.GetSelectedFrame();
                if (frame.IsValid()) {
                    SBValue var = sessionVariables[expression];
                    if (var.IsValid() && var.GetError().Success()) {
                        char* expressionpathdesc = formatExpressionPath(var);
                        cdtprintf("%d^done,path_expr=\"%s\"\n(gdb)\n", cc.sequence, expressionpathdesc);
                    } else
                        cdtprintf("%d^error\n(gdb)\n", cc.sequence);
                } else
                    cdtprintf("%d^error\n(gdb)\n", cc.sequence);
            } else
                cdtprintf("%d^error\n(gdb)\n", cc.sequence);
        }
    } else if (strcmp(cc.argv[0], "-var-evaluate-expression") == 0) {
        // 36-var-evaluate-expression --thread-group i1 "sizeof (void*)"
        // 36^done,value="50 '2'"
        // 58-var-evaluate-expression var5.y
        // 58^done,value="0x100001028 <y>"
        // 66-var-evaluate-expression var5.y.a
        // 66^done,value="0"
        // 36-data-evaluate-expression --thread-group i1 "sizeof (void*)"
        // 36^done,value="8"
        char expression[NAME_MAX];
        if (nextarg < cc.argc)
            strlcpy(expression, cc.argv[nextarg++], sizeof(expression));
        if (strcmp(expression, "sizeof (void*)") == 0)
            cdtprintf("%d^done,value=\"8\"\n(gdb)\n", cc.sequence);
        else {
            SBThread thread = process.GetSelectedThread();
            if (thread.IsValid()) {
                SBFrame frame = thread.GetSelectedFrame();
                if (frame.IsValid()) {
                    SBValue var = sessionVariables[expression];
                    if (var.IsValid()) {
                        char* vardesc = formatValue(var, FULL_SUMMARY);
                        cdtprintf("%d^done,value=\"%s\"\n(gdb)\n", cc.sequence, vardesc);
                    } else
                        cdtprintf("%d^error\n(gdb)\n", cc.sequence);
                } else
                    cdtprintf("%d^error\n(gdb)\n", cc.sequence);
            } else
                cdtprintf("%d^error\n(gdb)\n", cc.sequence);
        }
    }
    if (strcmp(cc.argv[0], "-var-set-format") == 0) {
        // 36-var-set-format var3 natural
        // 36-var-set-format var3 binary
        // 36-var-set-format var3 octal
        // 36-var-set-format var3 decimal
        // 36-var-set-format var3 hexadecimal
        // 36^done,format="octal",value="062"
        // 36^done,format="natural",value="0x100000f94 \"22\""
        // 36^done,format="natural",value="50 '2'"
        char expression[NAME_MAX], format[NAME_MAX];
        if (nextarg < cc.argc)
            strlcpy(expression, cc.argv[nextarg++], sizeof(expression));
        if (nextarg < cc.argc)
            strlcpy(format, cc.argv[nextarg++], sizeof(format));
        Format formatcode;
        if (strcmp(format, "binary") == 0)
            formatcode = eFormatBinary;
        else if (strcmp(format, "octal") == 0)
            formatcode = eFormatOctal;
        else if (strcmp(format, "decimal") == 0)
            formatcode = eFormatDecimal;
        else if (strcmp(format, "hexadecimal") == 0)
            formatcode = eFormatHex;
        else
            formatcode = eFormatDefault;
        SBThread thread = process.GetSelectedThread();
        if (thread.IsValid()) {
            SBFrame frame = thread.GetSelectedFrame();
            if (frame.IsValid()) {
                SBValue var = getVariable(frame, expression);
                if (var.IsValid() && var.GetError().Success()) {
                    var.SetFormat(formatcode);
                    char* vardesc = formatValue(var, FULL_SUMMARY); // was NO_SUMMARY
                    cdtprintf("%d^done,format=\"%s\",value=\"%s\"\n(gdb)\n", cc.sequence, format, vardesc);
                } else
                    cdtprintf("%d^error\n(gdb)\n", cc.sequence);
            } else
                cdtprintf("%d^error\n(gdb)\n", cc.sequence);
        } else
            cdtprintf("%d^error\n(gdb)\n", cc.sequence);
    } else {
        logprintf(LOG_WARN, "command not understood: ");
        logdata(LOG_NOHEADER, cc.argv[0], strlen(cc.argv[0]));
        cdtprintf("%d^error,msg=\"%s, command: '%s'\"\n(gdb)\n", cc.sequence, "Command unimplemented.", cc.argv[nextarg]);
    }
}

void Lldbmi2::handleStackCommand(CDT_COMMAND& cc, int nextarg) {
    if (strcmp(cc.argv[0], "-stack-info-depth") == 0) {
        // stack-info-depth --thread 1 11
        // 26^done,depth="1"
        int maxdepth = -1;
        if (cc.argv[nextarg] != NULL)
            if (isdigit(*cc.argv[nextarg]))
                sscanf(cc.argv[nextarg++], "%d", &maxdepth);
        if (process.IsValid()) {
            SBThread thread = process.GetSelectedThread();
            if (thread.IsValid()) {
                int numframes = getNumFrames(thread);
                cdtprintf("%d^done,depth=\"%d\"\n(gdb)\n", cc.sequence, numframes);
            } else
                cdtprintf("%d^error\n(gdb)\n", cc.sequence);
        } else
            cdtprintf("%d^error\n(gdb)\n", cc.sequence);
    } else if (strcmp(cc.argv[0], "-stack-list-frames") == 0) {
        // stack-list-frame --thread 1 1 1 (min max)
        int startframe = 0, endframe = -1;
        if (cc.argv[nextarg] != NULL)
            if (isdigit(*cc.argv[nextarg]))
                sscanf(cc.argv[nextarg++], "%d", &startframe);
        if (cc.argv[nextarg] != NULL)
            if (isdigit(*cc.argv[nextarg]))
                sscanf(cc.argv[nextarg++], "%d", &endframe);
        SBThread thread = process.GetSelectedThread();
        if (thread.IsValid()) {
            if (endframe < 0)
                endframe = getNumFrames(thread);
            else
                ++endframe;
            if (endframe - startframe > limits.frames_max)
                endframe = startframe + limits.frames_max; // limit # frames
            const char* separator = "";
            cdtprintf("%d^done,stack=[", cc.sequence);
            static StringB framedescB(LINE_MAX);
            for (int iframe = startframe; iframe < endframe; iframe++) {
                SBFrame frame = thread.GetFrameAtIndex(iframe);
                if (!frame.IsValid())
                    continue;
                framedescB.clear();
                char* framedesc = formatFrame(framedescB, frame, WITH_LEVEL);
                cdtprintf("%s%s", separator, framedesc);
                separator = ",";
            }
            cdtprintf("]\n(gdb)\n");
        } else
            cdtprintf("%d^error\n(gdb)\n", cc.sequence);
    } else if (strcmp(cc.argv[0], "-stack-list-arguments") == 0) {
        // stack-list-arguments --thread 1 1 (print-values) {1 2 (min max)}
        int printvalues = 0, startframe = 0, endframe = -1;
        if (cc.argv[nextarg] != NULL)
            if (isdigit(*cc.argv[nextarg]))
                sscanf(cc.argv[nextarg++], "%d", &printvalues);
        if (cc.argv[nextarg] != NULL)
            if (isdigit(*cc.argv[nextarg]))
                sscanf(cc.argv[nextarg++], "%d", &startframe);
        if (cc.argv[nextarg] != NULL)
            if (isdigit(*cc.argv[nextarg]))
                sscanf(cc.argv[nextarg++], "%d", &endframe);
        SBThread thread = process.GetSelectedThread();
        if (thread.IsValid()) {
            if (endframe < 0)
                endframe = getNumFrames(thread);
            else
                ++endframe;
            if (endframe - startframe > limits.frames_max)
                endframe = startframe + limits.frames_max; // limit # frames
            const char* separator = "";
            cdtprintf("%d^done,stack-args=[", cc.sequence);
            static StringB argsdescB(BIG_LINE_MAX);
            for (int iframe = startframe; iframe < endframe; iframe++) {
                SBFrame frame = thread.GetFrameAtIndex(iframe);
                if (!frame.IsValid())
                    continue;
                argsdescB.clear();
                char* argsdesc = formatFrame(argsdescB, frame, JUST_LEVEL_AND_ARGS);
                cdtprintf("%s%s", separator, argsdesc);
                separator = ",";
            }
            cdtprintf("]\n(gdb)\n");
        } else
            cdtprintf("%d^error\n(gdb)\n", cc.sequence);
    } else if (strcmp(cc.argv[0], "-thread-select") == 0) {
        int threadid = -1;
        if (cc.argv[nextarg] != NULL)
            if (isdigit(*cc.argv[nextarg]))
                sscanf(cc.argv[nextarg++], "%d", &threadid);

        // ^done,new-thread-id="3",
        // frame={level="0",func="vprintf",
        // args=[{name="format",value="0x8048e9c \"%*s%c %d %c\\n\""},
        // {name="arg",value="0x2"}],file="vprintf.c",line="31",arch="i386:x86_64"}

        const char *framedesc = formatFrame(process.GetThreadByID(threadid).GetFrameAtIndex(0), WITH_LEVEL_AND_ARGS);

        if (framedesc[0] != '\0')
            cdtprintf("%d^done,%s\n(gdb)\n", cc.sequence, framedesc);
        else
            cdtprintf("%d^error,msg=\"%s\"\n(gdb)\n", cc.sequence, "Can not fetch data now.");
    } else if (strcmp(cc.argv[0], "-stack-list-locals") == 0) {
        // stack-list-locals --thread 1 --frame 0 1
        // stack-list-locals --thread 2 --frame 0 1
        char printvalues[NAME_MAX]; // 1 or --all-values OR 2 or --simple-values
        printvalues[0] = '\0';
        if (++nextarg < cc.argc)
            strlcpy(printvalues, cc.argv[nextarg], sizeof(printvalues));
        bool isValid = false;
        if (process.IsValid()) {
            SBThread thread = process.GetSelectedThread();
            if (thread.IsValid()) {
                SBFrame frame = thread.GetSelectedFrame();
                if (frame.IsValid()) {
                    SBFunction function = frame.GetFunction();
                    if (function.IsValid()) {
                        isValid = true;
                        SBValueList localvars = frame.GetVariables(0, 1, 0, 0);
                        char* varsdesc = formatVariables(localvars);
                        cdtprintf("%d^done,locals=[%s]\n(gdb)\n", cc.sequence, varsdesc);
                    }
                }
            }
        }
        if (!isValid)
            cdtprintf("%d^error\n(gdb)\n", cc.sequence);
    } else if (strcmp(cc.argv[0], "-stack-select-frame") == 0) {
        unsigned int selectframe = 0;
        if (cc.argv[nextarg] != NULL)
            if (isdigit(*cc.argv[nextarg]))
                sscanf(cc.argv[nextarg++], "%u", &selectframe);
        SBThread thread = process.GetSelectedThread();
        if (thread.IsValid()) {
            if ((selectframe >= 0) && (selectframe < thread.GetNumFrames())) {
                thread.SetSelectedFrame(selectframe);
                cdtprintf("%d^done\n(gdb)\n", cc.sequence);
            } else
                cdtprintf("%d^error,msg=\"%s\"\n(gdb)\n", cc.sequence, "No such frame.");
        } else
            cdtprintf("%d^error,msg=\"%s\"\n(gdb)\n", cc.sequence, "Invalid Thread.");
    } else {
        logprintf(LOG_WARN, "command not understood: ");
        logdata(LOG_NOHEADER, cc.argv[0], strlen(cc.argv[0]));
        cdtprintf("%d^error,msg=\"%s, command: '%s'\"\n(gdb)\n", cc.sequence, "Command unimplemented.", cc.argv[nextarg]);
    }
}

void Lldbmi2::handleExecCommand(CDT_COMMAND& cc, int nextarg) {
    if (strcmp(cc.argv[0], "-exec-arguments") == 0) {
        int firstarg = nextarg;
        for (; nextarg < cc.argc; nextarg++) {
            if (cc.argv[nextarg][0] == '\'') {
                size_t copyarg = 0;
                size_t length = strlen(cc.argv[nextarg]) - 2;
                for (; copyarg < length; ++copyarg)
                    ((char*)(cc.argv[nextarg]))[copyarg] = cc.argv[nextarg][copyarg + 1];
                ((char*)(cc.argv[nextarg]))[copyarg] = '\0';
            }
        }
        launchInfo.SetArguments(&cc.argv[firstarg], false);
        cdtprintf("%d^done\n(gdb)\n", cc.sequence);
    } else if (strcmp(cc.argv[0], "-exec-run") == 0) {
        // exec-run --thread-group i1
        SBError error;
        SBLaunchInfo targLaunchInfo(NULL);
        const char* largv[] = { 0, 0 };
        for (unsigned int i = 0; i < launchInfo.GetNumArguments(); i++) {
            largv[0] = launchInfo.GetArgumentAtIndex(i);
            targLaunchInfo.SetArguments(largv, true);
        }
        targLaunchInfo.SetWorkingDirectory(launchInfo.GetWorkingDirectory());
        targLaunchInfo.SetEnvironmentEntries(envp, false);
        logprintf(LOG_NONE, "launchInfo: args=%d env=%d, pwd=%s\n", targLaunchInfo.GetNumArguments(),
                  targLaunchInfo.GetNumEnvironmentEntries(), targLaunchInfo.GetWorkingDirectory());
        // TODO: use a temporary process at first?
        process = target.Launch(targLaunchInfo, error);
        if (!process.IsValid() || error.Fail()) {
            cdtprintf("%d^error,msg=\"%s %s\"\n(gdb)\n", cc.sequence, "Can not start process.", error.GetCString());
            logprintf(LOG_INFO, "process_error=%s\n", error.GetCString());
        } else {
            isrunning = true;
            startProcessListener();
            setSignals();
            cdtprintf("=thread-group-started,id=\"%s\",pid=\"%lld\"\n", threadgroup, process.GetProcessID());
            checkThreadsLife();
            cdtprintf("%d^running\n", cc.sequence);
            cdtprintf("*running,thread-id=\"all\"\n(gdb)\n");
        }
    } else if (strcmp(cc.argv[0], "-exec-continue") == 0) {
        // 37-exec-continue --thread 1
        // 37^running
        // *running,thread-id="1"
        // Ignore a --thread argument. restart all threads
        if (process.IsValid()) {
            int state = process.GetState();
            if (state == eStateStopped) {
                SBThread thread = process.GetSelectedThread();
                cdtprintf("%d^running\n", cc.sequence);
                cdtprintf("*running,thread-id=\"%d\"\n(gdb)\n", thread.IsValid() ? thread.GetIndexID() : 0);
                process.Continue();
                isrunning = true;
            }
        } else
            cdtprintf("%d^error\n(gdb)\n", cc.sequence);
    } else if (strcmp(cc.argv[0], "-exec-step") == 0 || strcmp(cc.argv[0], "-exec-next") == 0) {
        // 37-exec-next --thread 1 1
        // 37-exec-step --thread 1 1
        // 37^running
        // *running,thread-id="1"
        int times = 1; // not used for now
        if (cc.argv[nextarg] != NULL)
            if (isdigit(*cc.argv[nextarg]))
                sscanf(cc.argv[nextarg], "%d", &times);
        if (process.IsValid()) {
            int state = process.GetState();
            if (state == eStateStopped) {
                SBThread thread = process.GetSelectedThread();
                if (thread.IsValid()) {
                    cdtprintf("%d^running\n", cc.sequence);
                    cdtprintf("*running,thread-id=\"%d\"\n(gdb)\n", thread.IsValid() ? thread.GetIndexID() : 0);
                    if (strcmp(cc.argv[0], "-exec-step") == 0)
                        thread.StepInto();
                    else
                        thread.StepOver();
                } else
                    cdtprintf("%d^error\n(gdb)\n", cc.sequence);
            }
        } else
            cdtprintf("%d^error\n(gdb)\n", cc.sequence);
    } else if ((strcmp(cc.argv[0], "-exec-step-instruction") == 0) ||
               (strcmp(cc.argv[0], "-exec-next-instruction") == 0)) {
        if (process.IsValid()) {
            int state = process.GetState();
            if (state == eStateStopped) {
                SBThread thread = process.GetSelectedThread();
                if (thread.IsValid()) {
                    cdtprintf("%d^running\n", cc.sequence);
                    cdtprintf("*running,thread-id=\"%d\"\n(gdb)\n", thread.IsValid() ? thread.GetIndexID() : 0);
                    thread.StepInstruction(strcmp(cc.argv[0], "-exec-next-instruction") == 0);
                } else
                    cdtprintf("%d^error\n(gdb)\n", cc.sequence);
            }
        } else
            cdtprintf("%d^error\n(gdb)\n", cc.sequence);
    } else if (strcmp(cc.argv[0], "-exec-finish") == 0) {
        // 37-exec-finish --thread 1 --frame 0
        // 37^running
        // *running,thread-id="all"
        if (process.IsValid()) {
            int state = process.GetState();
            if (state == eStateStopped) {
                SBThread thread = process.GetSelectedThread();
                if (thread.IsValid()) {
                    cdtprintf("%d^running\n", cc.sequence);
                    cdtprintf("*running,thread-id=\"all\"\n(gdb)\n");
                    thread.StepOut();
                } else
                    cdtprintf("%d^error\n(gdb)\n", cc.sequence);
            }
        } else
            cdtprintf("%d^error\n(gdb)\n", cc.sequence);
    } else if (strcmp(cc.argv[0], "-exec-until") == 0) {
        char path[NAME_MAX];
        if (nextarg < cc.argc)
            strlcpy(path, cc.argv[nextarg++], sizeof(path));
        if (process.IsValid()) {
            int state = process.GetState();
            if (state == eStateStopped) {
                SBThread thread = process.GetSelectedThread();
                if (thread.IsValid()) {
                    char* pline;
                    if ((pline = strchr(path, ':')) != NULL) {
                        *pline++ = '\0';
                        int iline = 0;
                        sscanf(pline, "%d", &iline);
                        SBFileSpec fspec(path, true);
                        SBFrame frame = thread.GetSelectedFrame();
                        if (frame.IsValid()) {
                            cdtprintf("%d^running\n", cc.sequence);
                            cdtprintf("*running,thread-id=\"all\"\n(gdb)\n");
                            thread.StepOverUntil(frame, fspec, iline);
                        }
                    }
                } else
                    cdtprintf("%d^error\n(gdb)\n", cc.sequence);
            }
        } else
            cdtprintf("%d^error\n(gdb)\n", cc.sequence);
    } else if (strcmp(cc.argv[0], "-exec-abort") == 0) { // NOTE: "kill" used to be a synonym for this
        srcprintf("kill\n");
        if (target.GetProcess().IsValid()) {
            if (target.GetProcess().GetState() == eStateStopped) // if process is stopped. restart it before kill
                target.GetProcess().Continue();
            target.GetProcess().Destroy();
            target.GetProcess().Clear();
            cdtprintf("%d^done\n(gdb)\n", cc.sequence);
        } else
            cdtprintf("%d^error\n(gdb)\n", cc.sequence);
    } else {
        logprintf(LOG_WARN, "command not understood: ");
        logdata(LOG_NOHEADER, cc.argv[0], strlen(cc.argv[0]));
        cdtprintf("%d^error,msg=\"%s, command: '%s'\"\n(gdb)\n", cc.sequence, "Command unimplemented.", cc.argv[nextarg]);
    }
}

void Lldbmi2::handleDataCommand(CDT_COMMAND& cc, int nextarg) {
    if (strcmp(cc.argv[0], "-data-evaluate-expression") == 0) {
        char expression[PATH_MAX];
        char expressionPath[PATH_MAX];
        bool doDeref = false;
        if (nextarg < cc.argc)
            strlcpy(expression, cc.argv[nextarg++], sizeof(expression));

        char* pathStart = strchr(expression, '.');
        if (pathStart != NULL) {
            strlcpy(expressionPath, pathStart, sizeof(expressionPath));
            if (expressionPath[strlen(expressionPath) - 1] == '^') {
                doDeref = true;
                expressionPath[strlen(expressionPath) - 1] = '\0';
            }
            *pathStart = '\0';
        } else {
            if (expression[strlen(expression) - 1] == '^') {
                doDeref = true;
                expression[strlen(expression) - 1] = '\0';
            }
            if (strcasecmp(expression, "sizeof(^char)") == 0) {
                strlcpy(expression, "sizeof(char*)", sizeof(expression));
            }
        }
        char* takeAddrOp = strchr(expression, '@');
        if (takeAddrOp != NULL)
            *takeAddrOp = '&';

        SBValue val = target.EvaluateExpression(expression);
        if (val.IsValid() && (pathStart != NULL)) {
            val = val.GetValueForExpressionPath(expressionPath);
        }
        if (val.IsValid() && doDeref) {
            val = val.Dereference();
        }

        if (val.IsValid()) {
            if (val.GetError().Fail())
                cdtprintf("%d^error,msg=\"%s.\"\n(gdb)\n", cc.sequence, val.GetError().GetCString());
            else {
                if (doDeref) {
                    StringB s(VALUE_MAX);
                    s.clear();
                    char* vardesc = formatDesc(s, val);
                    cdtprintf("%d^done,value=\"%s\"\n(gdb)\n", cc.sequence, vardesc);
                } else {
                    SBType valtype = val.GetType();
                    if ((valtype.GetTypeClass() & eTypeClassTypedef) != 0)
                        valtype = valtype.GetTypedefedType();

                    if ((valtype.GetTypeClass() & eTypeClassPointer) != 0) {
                        if (strcasecmp(valtype.GetName(), "char *") == 0) {
                            SBStream s;
                            val.GetDescription(s);
                            const char* str = strchr(s.GetData(), '=');
                            str = str + 2;
                            cdtprintf("%d^done,value=\"%s\"\n(gdb)\n", cc.sequence, str);
                        } else
                            cdtprintf("%d^done,value=\"%s\"\n(gdb)\n", cc.sequence, val.GetValue());
                    } else if ((valtype.GetTypeClass() & eTypeClassStruct) != 0) {
                        StringB s(VALUE_MAX);
                        s.clear();
                        char* vardesc = formatStruct(s, val);
                        cdtprintf("%d^done,value=\"%s\"\n(gdb)\n", cc.sequence, vardesc);
                    } else
                        cdtprintf("%d^done,value=\"%s\"\n(gdb)\n", cc.sequence, val.GetValue());
                }
            }
        } else
            cdtprintf("%d^error,msg=\"No valid value.\"\n(gdb)\n", cc.sequence);
    } else if (strcmp(cc.argv[0], "-data-list-register-names") == 0) {
        // 95-data-list-register-names --thread-group i1
        SBThread thread = process.GetSelectedThread();
        if (thread.IsValid()) {
            SBFrame frame = thread.GetSelectedFrame();
            SBValueList reglist = frame.GetRegisters();
            cdtprintf("%d^done,register-names=[", cc.sequence);
            for (unsigned int i = 0; i < reglist.GetSize(); i++) {
                SBValue val = reglist.GetValueAtIndex(i);
                for (unsigned int k = 0; k < val.GetNumChildren(); k++) {
                    const char* name = val.GetChildAtIndex(k).GetName();
                    if ((i == 0) && (k == 0))
                        cdtprintf("\"%s\"", name);
                    else
                        cdtprintf(",\"%s\"", name);
                }
            }
            cdtprintf("]\n(gdb)\n");
        } else
            cdtprintf("%d^error,msg=\"%s\"\n(gdb)\n", cc.sequence, "thread not found");
    } else if (strcmp(cc.argv[0], "-data-list-register-values") == 0) {
        SBThread thread = process.GetSelectedThread();
        if (thread.IsValid()) {
            SBFrame frame = thread.GetSelectedFrame();
            SBValueList reglist = frame.GetRegisters();
            int regnum = 0;
            cdtprintf("%d^done,register-values=[", cc.sequence);
            for (unsigned int i = 0; i < reglist.GetSize(); i++) {
                SBValue val = reglist.GetValueAtIndex(i);
                for (unsigned int k = 0; k < val.GetNumChildren(); k++) {
                    const char* value = val.GetChildAtIndex(k).GetValue();
                    if (regnum == 0)
                        cdtprintf("{number=\"%d\",value=\"%s\"}", regnum, value);
                    else
                        cdtprintf(",{number=\"%d\",value=\"%s\"}", regnum, value);
                    regnum++;
                }
            }
            cdtprintf("]\n(gdb)\n");
        } else
            cdtprintf("%d^error,msg=\"%s\"\n(gdb)\n", cc.sequence, "thread not found");
    } else if (strcmp(cc.argv[0], "-data-disassemble") == 0) {
        // Limited to following form:
        // -data-disassemble -s dddd -e ddddd -- 0
        addr_t startaddr = -1;
        addr_t endaddr = -1;
        if ((strcmp(cc.argv[nextarg], "-s") == 0) && isdigit(*cc.argv[nextarg + 1])) {
            nextarg += 1;
#ifdef __APPLE__
            sscanf(cc.argv[nextarg++], "%lld", &startaddr);
#else
            sscanf(cc.argv[nextarg++], "%lu", &startaddr);
#endif
        }
        if ((strcmp(cc.argv[nextarg], "-e") == 0) && isdigit(*cc.argv[nextarg + 1])) {
            nextarg += 1;
#ifdef __APPLE__
            sscanf(cc.argv[nextarg++], "%lld", &endaddr);
#else
            sscanf(cc.argv[nextarg++], "%lu", &endaddr);
#endif
        }
        if ((startaddr != LLDB_INVALID_ADDRESS) && (endaddr != LLDB_INVALID_ADDRESS)) {
            SBAddress saddr = target.ResolveFileAddress(startaddr);
            SBAddress eaddr = target.ResolveFileAddress(endaddr);
            int cnt = eaddr.GetFileAddress() - saddr.GetFileAddress();
            if (saddr.IsValid() && eaddr.IsValid()) {
                SBInstructionList ilist = target.ReadInstructions(saddr, cnt);
                if (ilist.IsValid()) {
                    cdtprintf("%d^done,asm_insns=[", cc.sequence);
                    for (int i = 0; i < cnt; i++) {
                        SBInstruction instr = ilist.GetInstructionAtIndex(i);
                        SBAddress iaddr = instr.GetAddress();
                        if (iaddr.GetFileAddress() > eaddr.GetFileAddress()) {
                            break;
                        }
                        SBAddress laddr = target.ResolveLoadAddress(iaddr.GetFileAddress());
                        SBSymbol symb = laddr.GetSymbol();
                        addr_t off = laddr.GetFileAddress() - symb.GetStartAddress().GetFileAddress();
                        if (i != 0)
                            cdtprintf(",");
                        cdtprintf("{address=\"%p\",func-name=\"%s\",offset=\"%d\",inst=\"%-12s%-25s %s\"}",
                                  iaddr.GetFileAddress(), symb.GetName(), off, instr.GetMnemonic(target),
                                  instr.GetOperands(target), instr.GetComment(target));
                    }
                    cdtprintf("]\n(gdb)\n");
                } else
                    cdtprintf("%d^error,msg=\"%s\"\n(gdb)\n", cc.sequence, "no valid instructions");
            } else
                cdtprintf("%d^error,msg=\"%s\"\n(gdb)\n", cc.sequence, "Could not resolve addresses");
        } else
            cdtprintf("%d^error,msg=\"%s\"\n(gdb)\n", cc.sequence, "Could not parse addresses");
    } else if (strcmp(cc.argv[0], "-data-read-memory") == 0 || strcmp(cc.argv[0], "-data-read-memory-bytes") == 0) {
        //-data-read-memory 4297035496 x 1 1 1359
        //-data-read-memory 4297035496 x 2 1 679
        //-data-read-memory 4297035496 x 4 1 339
        //-data-read-memory 4297035496 x 8 1 169
        //-data-read-memory-bytes 93824992260560 320
        addr_t address = -1;
        int wordSize = 0, nrRows = 0, nrCols = 0;
        char wordFormat = 'x';
        char expression[NAME_MAX];
        if (nextarg < cc.argc)
            strlcpy(expression, cc.argv[nextarg++], sizeof(expression));
        if (strcmp(cc.argv[0], "-data-read-memory-bytes") == 0) {
            wordSize = 1;
            nrRows = 1;
        } else {
            if ((nextarg < cc.argc) && (strlen(cc.argv[nextarg]) == 1)) {
                wordFormat = cc.argv[nextarg++][0];
            }
            if ((nextarg < cc.argc) && isdigit(*cc.argv[nextarg])) {
                sscanf(cc.argv[nextarg++], "%d", &wordSize);
            }
            if ((nextarg < cc.argc) && isdigit(*cc.argv[nextarg])) {
                sscanf(cc.argv[nextarg++], "%d", &nrRows);
            }
        }
        if ((nextarg < cc.argc) && isdigit(*cc.argv[nextarg])) {
            sscanf(cc.argv[nextarg++], "%d", &nrCols);
        }
        SBValue value = target.EvaluateExpression(expression);
        if (!value.IsValid()) {
            cdtprintf("%d^error,msg=\"Could not find value for %s\"\n(gdb)\n", cc.sequence, expression);
        } else {
            SBError error;
            address = value.GetValueAsUnsigned(error);
            if (error.Fail()) {
                cdtprintf("%d^error,msg=\"Could not convert value to address\"\n(gdb)\n", cc.sequence);
            } else {
                size_t size = wordSize * nrCols * nrRows;
                void* buf = malloc(size);
                size_t readCnt = process.ReadMemory(address, buf, size, error);
                if (error.Fail() || (readCnt == 0)) {
                    SBStream s;
                    error.GetDescription(s);
                    printf("Read failed (%d %d): %s\n", error.GetError(), error.GetType(), s.GetData());
                    cdtprintf("%d^error,msg=\"%s\"\n(gdb)\n", cc.sequence, s.GetData());
                } else {
                    addr_t rowAddr = address;
                    addr_t bufAddr = (addr_t)buf;
                    cdtprintf("%d^done,addr=\"%p\",nr-bytes=\"%d\",total-bytes=\"%d\",", cc.sequence, (void*)address,
                              readCnt, size);
                    cdtprintf("next-row=\"%p\",prev-row=\"%p\",next-page=\"%p\",prev-page=\"%p\",",
                              rowAddr + (wordSize * nrCols), rowAddr - (wordSize * nrCols), rowAddr + size,
                              rowAddr - size);

                    char format_string[NAME_MAX];
                    const char* prefix;

                    switch (wordFormat) {
                    case 'x':
                        prefix = "0x";
                        break;
                    case 'o':
                        prefix = "0";
                        break;
                    case 't':
                        prefix = "0b";
                        break;
                    default:
                        prefix = "";
                        break;
                    }
                    switch (wordSize) {
                    case 1:
                        snprintf(format_string, NAME_MAX, "\"%s%%02%c\"", prefix, wordFormat);
                        break;
                    case 2:
                        snprintf(format_string, NAME_MAX, "\"%s%%04%c\"", prefix, wordFormat);
                        break;
                    case 4:
                        snprintf(format_string, NAME_MAX, "\"%s%%08%c\"", prefix, wordFormat);
                        break;
                    case 8:
                        snprintf(format_string, NAME_MAX, "\"%s%%016ll%c\"", prefix, wordFormat);
                    }

                    cdtprintf("memory=[");
                    for (int row = 0; row < nrRows; row++) {
                        if (row != 0)
                            cdtprintf(",");
                        cdtprintf("{addr=\"%p\",data=[", rowAddr);
                        for (int col = 0; col < nrCols; col++) {
                            if (col != 0)
                                cdtprintf(",");
                            switch (wordSize) {
                            case 1:
                                cdtprintf(format_string, *(uint8_t*)(bufAddr + col * 1));
                                break;
                            case 2:
                                cdtprintf(format_string, *(uint16_t*)(bufAddr + col * 2));
                                break;
                            case 4:
                                cdtprintf(format_string, *(uint32_t*)(bufAddr + col * 4));
                                break;
                            case 8:
                                cdtprintf(format_string, *(uint64_t*)(bufAddr + col * 8));
                                break;
                            }
                        }
                        cdtprintf("]}");
                        rowAddr += wordSize * nrCols;
                        bufAddr += wordSize * nrCols;
                    }
                    cdtprintf("]\n(gdb)\n");
                }
            }
        }
    } else {
        logprintf(LOG_WARN, "command not understood: ");
        logdata(LOG_NOHEADER, cc.argv[0], strlen(cc.argv[0]));
        cdtprintf("%d^error,msg=\"%s, command: '%s'\"\n(gdb)\n", cc.sequence, "Command unimplemented.", cc.argv[nextarg]);
    }
}

// command interpreter
//   decode the line in input
//   execute the command
//   respond on stdout
int Lldbmi2::fromCDT(const char* commandLine, int linesize) // from cdt
{
    logprintf(LOG_NONE, "fromCDT (0x%x, ..., %d)\n", this, linesize);
    StringB cdtcommandB(BIG_LINE_MAX);
    CDT_COMMAND cc;

    int dataflag = MORE_DATA;
    logdata(LOG_CDT_IN | LOG_RAW, commandLine, strlen(commandLine));
    // put CDT input in the big CDT buffer
    cdtbufferB.append(commandLine);
    char* endofline = strstr(cdtbufferB.c_str(), "\n");
    if (endofline != NULL) {
        // multiple command in cdtbuffer. take the first one and shift the buffer
        int commandsize = endofline + 1 - cdtbufferB.c_str();
        cdtcommandB.copy(cdtbufferB.c_str(), commandsize);
        cdtbufferB.clear(commandsize); // shift buffered data
        if (cdtbufferB.size() == 0)
            dataflag = WAIT_DATA;
        // remove trailing \r and \n
        endofline = strstr(cdtcommandB.c_str(), "\n");
        while (endofline > cdtcommandB.c_str() && (*(endofline - 1) == '\n' || *(endofline - 1) == '\r'))
            --endofline;
        cdtcommandB.clear(BIG_LIMIT, endofline - cdtcommandB.c_str());
        logdata(LOG_CDT_IN, cdtcommandB.c_str(), cdtcommandB.size());
    } else
        return WAIT_DATA;

    int nextarg = evalCDTCommand(cdtcommandB.c_str(), &cc);
    if (nextarg == 0) {
        // no-op
    }
    // MISCELLANOUS COMMANDS
    else if (strcmp(cc.argv[0], "-gdb-exit") == 0) {
        terminateProcess(AND_EXIT);
    } else if (strcmp(cc.argv[0], "-gdb-version") == 0) {
        cdtprintf ("~\"%s, %s, %s\\n\"\n", gdbPrompt, lldbmi2Prompt, SBDebugger::GetVersionString());
        cdtprintf("%d^done\n(gdb)\n", cc.sequence);
    } else if (strcmp(cc.argv[0], "-list-features") == 0) {
        cdtprintf(
            "%d^done,%s\n(gdb)\n", cc.sequence,
            //	"features=[\"frozen-varobjs\",\"pending-breakpoints\",\"thread-info\",\"data-read-memory-bytes\",\"breakpoint-notifications\",\"ada-task-info\",\"python\"]");
            "features=[\"frozen-varobjs\",\"pending-breakpoints\",\"thread-info\",\"breakpoint-notifications\",\"ada-"
            "task-info\",\"python\"]");
    } else if (strcmp(cc.argv[0], "-environment-cd") == 0) {
        // environment-cd /project_path/tests
        char path[PATH_MAX];
        snprintf(path, sizeof(path), cc.argv[nextarg], project_loc);
        if (strstr(cc.argv[nextarg], "%s") != NULL)
            logprintf(LOG_VARS, "%%s -> %s\n", path);
        chdir(path);
        logprintf (LOG_NONE, "pwd=%s\n", path);
        cdtprintf("%d^done\n(gdb)\n", cc.sequence);
    } else if (strcmp(cc.argv[0], "unset") == 0) {
        // unset env
        if (strcmp(cc.argv[nextarg], "env") == 0) {
            envp[0] = NULL;
            envpentries = 0;
            envspointer = envs;
        }
        cdtprintf("%d^done\n(gdb)\n", cc.sequence);
    } else if (strcmp(cc.argv[0], "-gdb-set") == 0) {
        // -gdb-set args ...
        // -gdb-set env LLDB_DEBUGSERVER_PATH = /pro/ll/release/bin/debugserver
        // -gdb-set breakpoint pending on
        // -gdb-set detach-on-fork on
        // -gdb-set python print-stack none
        // -gdb-set print object on
        // -gdb-set print sevenbit-strings on
        // -gdb-set host-charset UTF-8
        // -gdb-set target-charset US-ASCII
        // -gdb-set target-wide-charset UTF-32
        // -gdb-set target-async off
        // -gdb-set auto-solib-add on
        // -gdb-set language c
        if (strcmp(cc.argv[nextarg], "args") == 0) {
            if (strcmp(cc.argv[++nextarg], "%s") == 0) {
                logprintf(LOG_VARS, "YYYYYYYYYYYY\n");
                snprintf((char*)cc.argv[nextarg], NAME_MAX, "%2d", test_sequence);
                logprintf(LOG_VARS, "%%s -> %s\n", cc.argv[nextarg]);
            }
            int firstarg = nextarg;
            for (; nextarg < cc.argc; nextarg++)
                if (cc.argv[nextarg][0] == '\'') {
                    size_t copyarg = 0;
                    size_t length = strlen(cc.argv[nextarg]) - 2;
                    for (; copyarg < length; ++copyarg)
                        ((char*)(cc.argv[nextarg]))[copyarg] = cc.argv[nextarg][copyarg + 1];
                    ((char*)(cc.argv[nextarg]))[copyarg] = '\0';
                }
            launchInfo.SetArguments(&cc.argv[firstarg], false);
        } else if (strcmp(cc.argv[nextarg], "env") == 0) {
            // eclipse put a space around the equal in VAR = value. we have to combine all 3 part to form env entry
            char enventry[LINE_MAX];
            ++nextarg;
            if (cc.argc + 1 - nextarg > 0) {
                if (cc.argc + 1 - nextarg == 1)
                    snprintf(enventry, sizeof(enventry), "%s", cc.argv[nextarg]);
                else if (cc.argc + 1 - nextarg == 2)
                    snprintf(enventry, sizeof(enventry), "%s%s", cc.argv[nextarg], cc.argv[nextarg + 1]);
                else
                    snprintf(enventry, sizeof(enventry), "%s%s%s", cc.argv[nextarg], cc.argv[nextarg + 1],
                             cc.argv[nextarg + 2]);
                addEnvironment(enventry);
            }
        }
        cdtprintf("%d^done\n(gdb)\n", cc.sequence);
    } else if (strcmp(cc.argv[0], "-gdb-show") == 0) {
        // 21-gdb-show --thread-group i1 language
        // do no work. eclipse send it too early. must not rely on frame
        SBThread thread = process.GetSelectedThread();
        if (thread.IsValid()) {
            SBFrame frame = thread.GetSelectedFrame();
            if (frame.IsValid()) {
                SBCompileUnit compileunit = frame.GetCompileUnit();
                LanguageType languagetype = compileunit.GetLanguage();
                const char* languagename = getNameForLanguageType(languagetype);
                cdtprintf("%d^done,value=\"%s\"\n(gdb)\n", cc.sequence, languagename);
            } else
                cdtprintf("%d^error\n(gdb)\n", cc.sequence);
        } else
            cdtprintf("%d^done,value=\"auto\"\n(gdb)\n", cc.sequence);
    } else if (strcmp(cc.argv[0], "-enable-pretty-printing") == 0) {
        cdtprintf("%d^done\n", cc.sequence);
    } else if (strcmp(cc.argv[0], "source") == 0) {
        // source .gdbinit
        cdtprintf("%d^done\n(gdb)\n", cc.sequence);
    } else if ((strcmp(cc.argv[0], "-inferior-tty-set") == 0) ||
               ((strcmp(cc.argv[0], "set") == 0) && (strcmp(cc.argv[1], "inferior-tty") == 0))) {
        // inferior-tty-set --thread-group i1 /dev/ttyp0
        if (strcmp(cc.argv[0], "set") == 0)
            nextarg++;
        strlcpy(cdtptyname, cc.argv[nextarg], sizeof(cdtptyname));
        if (strcmp(cdtptyname, "%s") == 0)
            ptyfd = EOF;
        else {
            logprintf(LOG_NONE, "ptyname=%s\n", cdtptyname);
            ptyfd = open(cdtptyname, O_RDWR);
            #if 0
            // set pty in raw mode
            struct termios t;
            if (tcgetattr(ptyfd, &t) != -1) {
                // Noncanonical mode, disable signals, extended input processing, and echoing
                t.c_lflag &= ~(ICANON | ISIG | IEXTEN | ECHO);
                // Disable special handling of CR, NL, and BREAK.
                // No 8th-bit stripping or parity error handling
                // Disable START/STOP output flow control
                t.c_iflag &= ~(BRKINT | ICRNL | IGNBRK | IGNCR | INLCR | INPCK | ISTRIP | IXON | PARMRK);
                // Disable all output processing
                t.c_oflag &= ~OPOST;
                t.c_cc[VMIN] = 1;  // Character-at-a-time input
                t.c_cc[VTIME] = 0; // with blocking
                tcsetattr(ptyfd, TCSAFLUSH, &t);
            }
            #endif
        }
        logprintf(LOG_NONE, "pty = %d\n", ptyfd);
        cdtprintf("%d^done\n(gdb)\n", cc.sequence);
    }
    // TARGET AND RUN COMMANDS
    else if (strcmp(cc.argv[0], "-file-exec-and-symbols") == 0) {
        // file-exec-and-symbols --thread-group i1 /project_path/tests/Debug/tests
        if (cc.argc > 1) {
            char path[PATH_MAX];
            snprintf(path, sizeof(path), cc.argv[nextarg], project_loc);
            if (strstr(cc.argv[nextarg], "%s") != NULL)
                logprintf(LOG_VARS, "%%s -> %s\n", path);
            char programpath[LINE_MAX];
            strlcpy(programpath, path, sizeof(programpath));
            if (strlen(arch) > 0)
                target = debugger.CreateTargetWithFileAndArch(programpath, arch);
            else
                target = debugger.CreateTargetWithFileAndArch(programpath, LLDB_ARCH_DEFAULT);
            if (!target.IsValid())
                cdtprintf("%d^error\n(gdb)\n", cc.sequence);
            else
                cdtprintf("%d^done\n(gdb)\n", cc.sequence);
        } else { // no arg to file-exec-and-symbols so clear executable and symbol informat.
            if (process.IsValid()) {
                process.Destroy();
            }
            debugger.DeleteTarget(target);
            cdtprintf("%d^done\n(gdb)\n", cc.sequence);
        }
    } else if (strcmp(cc.argv[0], "-target-attach") == 0) {
        // target-attach --thread-group i1 40088
        // =thread-group-started,id="i1",pid="40123"
        // =thread-created,id="1",group-id="i1"
        lldb::pid_t pid = 0;
        SBError error;
        char processname[PATH_MAX];
        processname[0] = '\0';
        if (cc.argv[nextarg] != NULL) {
            if (isdigit(*cc.argv[nextarg]))
#ifdef __APPLE__
                sscanf(cc.argv[nextarg], "%llu", &pid);
#else
                sscanf(cc.argv[nextarg], "%lu", &pid);
#endif
            else
                strlcpy(processname, cc.argv[nextarg], PATH_MAX);
        }
        target = debugger.CreateTarget(NULL);
        // TODO make a temporary process first?
        //	debugger.SetAsync (false);
        if (pid > 0)
            process = target.AttachToProcessWithID(listener, pid, error);
        else if (processname[0] != '\0')
            process = target.AttachToProcessWithName(listener, processname, false, error);
        //	debugger.SetAsync (true);
        if (!process.IsValid() || error.Fail()) {
            cdtprintf("%d^error,msg=\"%s\"\n(gdb)\n", cc.sequence, "Can not start process.");
            logprintf(LOG_INFO, "process_error=%s\n", error.GetCString());
        } else {
            isrunning = true;
            startProcessListener();
            setSignals();
            cdtprintf("=thread-group-started,id=\"%s\",pid=\"%lld\"\n", threadgroup, process.GetProcessID());
            checkThreadsLife();
            cdtprintf("%d^done\n(gdb)\n", cc.sequence);
        }
    } else if (strcmp(cc.argv[0], "-target-detach") == 0) {
        // target-detach --thread-group i1
        if (process.IsValid()) {
            terminateProcess(PRINT_THREAD | PRINT_GROUP | AND_EXIT);
            cdtprintf("%d^done\n(gdb)\n", cc.sequence);
        } else
            cdtprintf("%d^error,msg=\"%s\"\n(gdb)\n", cc.sequence, "The program is not being run.");

    } else if (strcmp(cc.argv[0], "-interpreter-exec") == 0) {
        // 18-interpreter-exec --thread-group i1 console "show endian"
        //    ~"The target endianness is set automatically (currently little endian)\n"
        // 18-interpreter-exec --thread-group i1 console "p/x (char)-1"
        //    ~"$1 = 0xff\n"
        // 30-interpreter-exec --thread-group i1 console kill
        //    =thread-exited,id="1",group-id="i1"
        //    =thread-group-exited,id="i1"
        //    30^done
        //    (gdb)
        if (strcmp(cc.argv[nextarg], "console") == 0) {
            if (++nextarg >= cc.argc)
                cdtprintf("%d^error\n(gdb)\n", cc.sequence);
            else if (strcmp(cc.argv[nextarg], "show endian") == 0) {
                cdtprintf("%s\n"
                          "%d^done\n"
                          "(gdb)\n",
                          "~\"The target endianness is set automatically (currently little endian)\\n\"", cc.sequence);
            } else if (strcmp(cc.argv[nextarg], "p/x (char)-1") == 0) {
                cdtprintf("%s\n"
                          "%d^done\n"
                          "(gdb)\n",
                          "~\"$1 = 0xff\\n\"", cc.sequence);
            } else if (strcmp(cc.argv[nextarg], "kill") == 0) {
                if (process.IsValid()) {
                    int state = process.GetState();
                    if (state == eStateStopped) { // if process is stopped. restart it before kill
                        logprintf(LOG_INFO, "console kill: restart process\n");
                        SBThread thread = process.GetSelectedThread();
                        //	cdtprintf ("%d^running\n", cc.sequence);
                        //	cdtprintf ("*running,thread-id=\"%d\"\n(gdb)\n", thread.IsValid()? thread.GetIndexID():
                        // 0);
                        process.Continue();
                        isrunning = true;
                        wanttokill = true; // wait for process running to kill it
                    }
                    cdtprintf("%d^done\n(gdb)\n", cc.sequence);
                    if (!wanttokill) {
                        //	logprintf (LOG_INFO, "console kill: send SIGINT\n");
                        //	process.Signal(SIGINT);
                        logprintf(LOG_INFO, "console kill: terminateProcess\n");
                        terminateProcess(PRINT_GROUP | AND_EXIT);
                    }
                } else
                    cdtprintf("%d^error,msg=\"%s\"\n(gdb)\n", cc.sequence, "The program is not being run.");
            } else
                cdtprintf("%d^error,msg=\"%s, command: '%s'\"\n(gdb)\n", cc.sequence, "Command unimplemented.", cc.argv[nextarg]);
        }
    } else if (strncmp(cc.argv[0], "-break", strlen("-break")) == 0) {
        handleBreakpointCommand(cc, nextarg);
    } else if (strncmp(cc.argv[0], "-var", strlen("-var")) == 0) {
        handleVariableCommand(cc, nextarg);
    } else if (strncmp(cc.argv[0], "-stack", strlen("-stack")) == 0) {
        handleStackCommand(cc, nextarg);
    } else if (strncmp(cc.argv[0], "-exec", strlen("-exec")) == 0) {
        handleExecCommand(cc, nextarg);
    } else if (strncmp(cc.argv[0], "-data", strlen("-data")) == 0) {
        handleDataCommand(cc, nextarg);
    }
    // STACK COMMANDS
    else if (strcmp(cc.argv[0], "-list-thread-groups") == 0) {
        // list-thread-groups --available
        //    ^error,msg="Can not fetch data now."
        // list-thread-groups
        //    ^done,groups=[{id="i1",type="process",pid="1186",executable="/project_path/tests/Debug/tests"}]
        // list-thread-groups i1
        //    ^done,threads=[{id="1",target-id="Thread 0x1503 of process
        //    1186",frame={level="0",addr="0x0000000100000f46",
        //    func="main",args=[],file="../Sources/tests.cpp",fullname="/project_path/tests/Sources/tests.cpp",
        //    line="15"},state="stopped"}]
        //		233857.215 <<=  |65^done,threads=[{id="1",target-id="Thread 0x89b52 of process
        // 94273",frame={level="0",addr="0x0000000000000ebf",func="main",args=[{name="argc",value="3"},{name="argv",value="0x00007fff5fbffeb0"},{name="envp",value="0x00007fff5fbffed0"}],file="tests.cpp",fullname="/project_path/tests/Debug/../Sources/tests.cpp",line="69"},state="stopped"},{id="2",target-id="Thread
        // 0x89b61 of process
        // 94273",frame={level="0",addr="0x000000000001648a",func="__semwait_signal"},state="stopped"}]\n(gdb)\n|
        // 491,415 47^done,threads=[{id="2",target-id="Thread 0x110f of process
        // 96806",frame={level="0",addr="0x00007fff88a3848a",func="??",args=[]},state="stopped"},{id="1",target-id="Thread
        // 0x1003 of process
        // 96806",frame={level="0",addr="0x0000000100000ebf",func="main",args=[{name="argc",value="1"},{name="argv",value="0x7fff5fbff5c0"},{name="envp",value="0x7fff5fbff5d0"}],file="../Sources/tests.cpp",fullname="/project_path/tests/Sources/tests.cpp",line="69"},state="stopped"}]

        if (cc.available > 0) {
            cdtprintf("%d^error,msg=\"%s\"\n(gdb)\n", cc.sequence, "Can not fetch data now.");
        } else if (cc.argv[nextarg] == NULL) {
            char groupsdesc[LINE_MAX];
            int pid = 0;
            int groupdesclength = 0;
            SBFileSpec execspec;
            const char *filename = NULL, *filedir = NULL;
            snprintf(groupsdesc, sizeof(groupsdesc), "id=\"%s\",type=\"process\"", threadgroup);
            groupdesclength = strlen(groupsdesc);
            if (process.IsValid()) {
                pid = process.GetProcessID();
                snprintf(groupsdesc + groupdesclength, sizeof(groupsdesc) - groupdesclength, ",pid=\"%d\"", pid);
                groupdesclength = strlen(groupsdesc);
            }
            if (target.IsValid()) {
                execspec = target.GetExecutable();
                filename = execspec.GetFilename();
                filedir = execspec.GetDirectory();
            }
            if (filename != NULL && filedir != NULL)
                snprintf(groupsdesc + groupdesclength, sizeof(groupsdesc) - groupdesclength, ",executable=\"%s/%s\"",
                         filedir, filename);
            cdtprintf("%d^done,groups=[{%s}]\n(gdb)\n", cc.sequence, groupsdesc);
        } else if (strcmp(cc.argv[nextarg], threadgroup) == 0) {
            char* threaddesc = formatThreadInfo(process, -1);
            if (threaddesc[0] != '\0')
                cdtprintf("%d^done,threads=[%s]\n(gdb)\n", cc.sequence, threaddesc);
            else
                cdtprintf("%d^error,msg=\"%s\"\n(gdb)\n", cc.sequence, "Can not fetch data now.");
        }
    } else if (strcmp(cc.argv[0], "thread") == 0) {
        if (process.IsValid()) {
            int pid = process.GetProcessID();
            SBThread thread = process.GetSelectedThread();
            if (thread.IsValid()) {
                int tid = thread.GetThreadID();
                int threadindexid = thread.GetIndexID();
                cdtprintf("~\"[Current thread is %d (Thread 0x%x of process %d)]\\n\"\n", threadindexid, tid, pid);
                cdtprintf("%d^done\n(gdb)\n", cc.sequence);
            } else
                cdtprintf("%d^error,msg=\"%s\"\n(gdb)\n", cc.sequence, "Can not fetch data now.");
        } else
            cdtprintf("%d^error,msg=\"%s\"\n(gdb)\n", cc.sequence, "Can not fetch data now.");
    } else if (strcmp(cc.argv[0], "-thread-info") == 0) {
        int threadindexid = -1;
        if (cc.argv[nextarg] != NULL)
            if (isdigit(*cc.argv[nextarg]))
                sscanf(cc.argv[nextarg++], "%d", &threadindexid);
        char* threaddesc = formatThreadInfo(process, threadindexid);
        if (threaddesc[0] != '\0')
            cdtprintf("%d^done,threads=[%s]\n(gdb)\n", cc.sequence, threaddesc);
        else
            cdtprintf("%d^error,msg=\"%s\"\n(gdb)\n", cc.sequence, "Can not fetch data now.");
    } else if (strcmp(cc.argv[0], "ptype") == 0) {
        // MI cmd: -symbol-type
        char expression[NAME_MAX];
        char expressionPath[NAME_MAX];
        if (nextarg < cc.argc)
            strlcpy(expression, cc.argv[nextarg++], sizeof(expression));
        srcprintf("ptype %s\n", expression);
        SBTypeList list = target.FindTypes(expression);
        SBType type = findClassOfType(list, eTypeClassClass);
        if (!type.IsValid())
            type = findClassOfType(list, eTypeClassAny);
        if (!type.IsValid()) {
            char* pathStart = strchr(expression, '.');
            if (pathStart != NULL) {
                strlcpy(expressionPath, pathStart, sizeof(expressionPath));
                *pathStart = '\0';
            }
            SBValue val = target.EvaluateExpression(expression);
            if (val.IsValid() && (pathStart != NULL))
                val = val.GetValueForExpressionPath(expressionPath);
            if (val.IsValid()) {
                list = target.FindTypes(val.GetDisplayTypeName());
                type = findClassOfType(list, eTypeClassClass);
                if (!type.IsValid())
                    type = findClassOfType(list, eTypeClassAny);
                if (!type.IsValid()) {
                    SBSymbolContextList list = target.FindFunctions(expression);
                    SBSymbolContext ctxt = list.GetContextAtIndex(0);
                    type = ctxt.GetFunction().GetType();
                    if (!type.IsValid()) {
                        type = val.GetType();
                    }
                }
            }
        }
        if (type.IsValid()) {
            if ((type.GetTypeClass() & eTypeClassClass) != 0) {
                const char* name = type.GetDisplayTypeName();
                int numfields = type.GetNumberOfFields();
                int numbase = type.GetNumberOfDirectBaseClasses();
                int numfuncs = type.GetNumberOfMemberFunctions();

                if (numbase > 0) {
                    SBTypeMember mbr = type.GetDirectBaseClassAtIndex(0);
                    srlprintf("type = %s = class : public %s \n", name, mbr.GetName());
                } else
                    srlprintf("type = %s = class\n", name);

                if (numfields > 0) {
                    for (int i = 0; i < numfields; i++) {
                        SBTypeMember mbr = type.GetFieldAtIndex(i);
                        srlprintf("    %s : %s;\n", mbr.GetName(), mbr.GetType().GetDisplayTypeName());
                    }
                }

                if (numfuncs > 0) {
                    for (int i = 0; i < numfuncs; i++) {
                        StringB funcs(BIG_LINE_MAX);
                        SBTypeMemberFunction mbr = type.GetMemberFunctionAtIndex(i);
                        if (mbr.GetReturnType().GetBasicType() == eBasicTypeVoid)
                            funcs.append("    procedure");
                        else
                            funcs.append("    function ");
                        funcs.catsprintf(" %s (", mbr.GetName());
                        int cnt = mbr.GetNumberOfArguments();
                        if (cnt > 0) {
                            for (int i = 0; i < cnt; i++) {
                                if (i != 0)
                                    funcs.catsprintf(", ");
                                funcs.catsprintf("%s", mbr.GetArgumentTypeAtIndex(i).GetDisplayTypeName());
                            }
                        }
                        funcs.append(")");
                        if (mbr.GetReturnType().GetBasicType() != eBasicTypeVoid)
                            funcs.catsprintf(" : %s", mbr.GetReturnType().GetDisplayTypeName());
                        srlprintf("%s;\n", funcs.c_str());
                    }
                }
                srlprintf("end\n");
            } else if ((type.GetTypeClass() & eTypeClassFunction) != 0) {
                SBType funcReturnType = type.GetFunctionReturnType();
                SBTypeList argList = type.GetFunctionArgumentTypes();
                StringB func(BIG_LINE_MAX);
                if (funcReturnType.GetBasicType() == eBasicTypeVoid)
                    func.append("type = procedure");
                else
                    func.append("type = function");
                int cnt = argList.GetSize();
                if (cnt > 0) {
                    func.append(" (");
                    for (int i = 0; i < cnt; i++) {
                        if (i != 0)
                            func.append(", ");
                        func.catsprintf("%s", argList.GetTypeAtIndex(i).GetDisplayTypeName());
                    }
                    func.append(")");
                }
                if (funcReturnType.GetBasicType() != eBasicTypeVoid)
                    func.catsprintf(" : %s", funcReturnType.GetDisplayTypeName());
                srlprintf("%s\n", func.c_str());
            }
            // Check for enums? tuidlgicontype
            else {
                srlprintf("type = %s\n", type.GetDisplayTypeName());
            }
            cdtprintf("%d^done\n(gdb)\n", cc.sequence);
        } else
            cdtprintf("%d^error,msg=\"No symbol \\\"%s\\\" in current context.\"\n(gdb)\n", cc.sequence, expression);

    }
    // OTHER COMMANDS
    else if ((strcmp(cc.argv[0], "-file-list-exec-sections") == 0) ||
             ((cc.argc == 2) && (strcmp(cc.argv[0], "info") == 0) && (strcmp(cc.argv[1], "file") == 0))) {
        if (target.IsValid()) {
            addr_t NOTLOADED = (addr_t)(-1);
            char filename[PATH_MAX];
            SBFileSpec execFile = target.GetExecutable();
            execFile.GetPath(filename, sizeof(filename));
            const char* filetype = target.GetTriple();
            addr_t entrypt = -1;
            SBModule execMod = target.FindModule(execFile);
            if (execMod.IsValid()) {
                SBSection txtSect = execMod.FindSection("__TEXT");
                if (txtSect.IsValid()) {
                    SBSection subTxtSect = txtSect.FindSubSection("__text");
                    if (subTxtSect.IsValid()) {
                        entrypt = subTxtSect.GetLoadAddress(target);
                        if (entrypt == NOTLOADED)
                            entrypt = subTxtSect.GetFileAddress();
                    }
                }
            }
            if (strcmp(cc.argv[0], "info") == 0) {
                srcprintf("info file\n");
                srlprintf("Symbols from \"%s\".\n", filename);
                srlprintf("\"%s\"\n", filetype);
            }
            cdtprintf("%d^done,section-info={filename=\"%s\",filetype=\"%s\",entry-point=\"%p\",sections={",
                      cc.sequence, filename, filetype, entrypt);
            for (unsigned int mndx = 0; mndx < target.GetNumModules(); mndx++) {
                SBModule mod = target.GetModuleAtIndex(mndx);
                if (!mod.IsValid())
                    continue;
                SBFileSpec modfilespec = mod.GetFileSpec();
                char modfilename[PATH_MAX];
                modfilespec.GetPath(modfilename, sizeof(modfilename));
                for (unsigned int sndx = 0; sndx < mod.GetNumSections(); sndx++) {
                    SBSection sect = mod.GetSectionAtIndex(sndx);
                    if (!sect.IsValid())
                        continue;
                    const char* sectName = sect.GetName();
                    addr_t faddr = sect.GetLoadAddress(target);
                    if (faddr == NOTLOADED)
                        faddr = sect.GetFileAddress();
                    addr_t eaddr = faddr + sect.GetByteSize();
                    if ((sndx != 0) || (mndx != 0))
                        cdtprintf(",");
                    cdtprintf("section={addr=\"%p\",endaddr=\"%p\",name=\"%s\",filename=\"%s\"}", faddr, eaddr,
                              sectName, modfilename);
                    for (unsigned int sbndx = 0; sbndx < sect.GetNumSubSections(); sbndx++) {
                        SBSection subsect = sect.GetSubSectionAtIndex(sbndx);
                        if (!subsect.IsValid())
                            continue;
                        faddr = subsect.GetLoadAddress(target);
                        if (faddr == NOTLOADED)
                            faddr = subsect.GetFileAddress();
                        eaddr = faddr + subsect.GetByteSize();
                        cdtprintf(",section={addr=\"%p\",endaddr=\"%p\",name=\"%s.%s\",filename=\"%s\"}", faddr, eaddr,
                                  sectName, subsect.GetName(), modfilename);
                    }
                }
            }
            cdtprintf("}}\n(gdb)\n");
        } else
            cdtprintf("%d^error,msg=\"%s\"\n(gdb)\n", cc.sequence, "Target not loaded.");
    } else if (strcmp(cc.argv[0], "info") == 0) {
        // 96info sharedlibrary
        if (nextarg >= cc.argc)
            cdtprintf("%d^error\n(gdb)\n", cc.sequence);
        else if (strcmp(cc.argv[nextarg], "sharedlibrary") == 0) {
            // code from lldm-mi
            int nLibraries = 0;
            int nModules = target.GetNumModules();
            for (int iModule = 0; iModule < nModules; iModule++) {
                SBModule module = target.GetModuleAtIndex(iModule);
                if (module.IsValid()) {
                    const char* moduleFilePath = module.GetFileSpec().GetDirectory();
                    const char* moduleFileName = module.GetFileSpec().GetFilename();
                    const char* moduleHasSymbols = (module.GetNumSymbols() > 0) ? "Yes" : "No";
                    addr_t addrLoadS = 0xffffffffffffffff;
                    addr_t addrLoadSize = 0;
                    bool bHaveAddrLoad = false;
                    const int nSections = module.GetNumSections();
                    for (int iSection = 0; iSection < nSections; iSection++) {
                        SBSection section = module.GetSectionAtIndex(iSection);
                        addr_t addrLoad = section.GetLoadAddress(target);
                        if (addrLoad != (addr_t)-1) {
                            if (!bHaveAddrLoad) {
                                bHaveAddrLoad = true;
                                addrLoadS = addrLoad;
                            }
                            addrLoadSize += section.GetByteSize();
                        }
                    }
                    cdtprintf("~\"%p\t%p\t%s\t\t%s/%s\"\n", addrLoadS, addrLoadS + addrLoadSize, moduleHasSymbols,
                              moduleFilePath, moduleFileName);
                    ++nLibraries;
                }
            }
            if (nLibraries == 0)
                cdtprintf("%s\n", "~\"No shared libraries loaded at this time.\n\"");
            cdtprintf("%d^done\n(gdb)\n", cc.sequence);
        }
        // Symbol Commands
        else if (strcmp(cc.argv[nextarg], "address") == 0) {
            //-symbol-info-address
            char symbol[NAME_MAX];
            if (nextarg < cc.argc)
                strlcpy(symbol, cc.argv[++nextarg], sizeof(symbol));
            srcprintf("info address %s\n", symbol);
            SBSymbolContextList list = target.FindSymbols(symbol);
            if (list.IsValid()) {
                SBSymbolContext ctxt = list.GetContextAtIndex(0);
                SBSymbol symb = ctxt.GetSymbol();
                if (symb.GetType() == eSymbolTypeData) {
                    SBValue val = ctxt.GetModule().FindFirstGlobalVariable(target, symb.GetName());
                    if (val.IsValid()) {
                        srlprintf("Symbol \"%s\" is %s at %s\n", symbol, val.GetTypeName(), val.GetLocation());
                        cdtprintf("%d^done\n(gdb)\n", cc.sequence);
                    } else
                        cdtprintf("%d^error,msg=\"No symbol \\\"%s\\\" in current context.\"\n(gdb)\n", cc.sequence,
                                  symbol);
                } else {
                    SBAddress startaddr = symb.GetStartAddress();
                    addr_t vaddr = startaddr.GetLoadAddress(target);
                    if (vaddr == LLDB_INVALID_ADDRESS)
                        vaddr = startaddr.GetFileAddress();
                    if (symb.GetType() == eSymbolTypeCode)
                        srlprintf("Symbol \"%s\" is a function at address %p.\n", symb.GetName(), vaddr);
                    else
                        srlprintf("Symbol \"%s\" is at address %p.\n", symbol, vaddr);
                    cdtprintf("%d^done\n(gdb)\n", cc.sequence);
                }
            } else
                cdtprintf("%d^error,msg=\"No symbol \\\"%s\\\" in current context.\"\n(gdb)\n", cc.sequence, symbol);
        } else if (strcmp(cc.argv[nextarg], "functions") == 0) {
            // -symbol-list-functions
            char symbol[NAME_MAX];
            if (nextarg < cc.argc)
                strlcpy(symbol, cc.argv[++nextarg], sizeof(symbol));
            SBSymbolContextList list = target.FindFunctions(symbol, eFunctionNameTypeAny);
            srcprintf("info functions %s\n", symbol);
            static StringB funcdescB(BIG_LINE_MAX);
            funcdescB.clear();
            srlprintf("All functions matching regular expression \"%s\"\n\n", symbol);
            for (size_t i = 0; i < list.GetSize(); i++) {
                SBSymbolContext ctxt = list.GetContextAtIndex(i);
                if (ctxt.IsValid()) {
                    SBCompileUnit cu = ctxt.GetCompileUnit();
                    SBFileSpec fspec = ctxt.GetCompileUnit().GetFileSpec();
                    SBFunction func = ctxt.GetFunction();
                    srlprintf("File %s/%s:\n", fspec.GetDirectory(), fspec.GetFilename());
                    srlprintf("%s %s;\n", func.GetType().GetFunctionReturnType().GetName(), func.GetName());
                }
            }
            cdtprintf("%d^done\n(gdb)\n", cc.sequence);
        } else if (strcmp(cc.argv[nextarg], "line") == 0) {
            // -symbol-info-line
            char path[NAME_MAX];
            if (nextarg < cc.argc)
                strlcpy(path, cc.argv[++nextarg], sizeof(path));
            char* pline;
            srcprintf("info line %s\n", path);
            if ((pline = strchr(path, ':')) != NULL) {
                *pline++ = '\0';
                int iline = 0;
                sscanf(pline, "%d", &iline);
                SBFileSpec fspec;
                SBCompileUnit foundCU = findCUForFile(path, target, fspec);
                if (foundCU.IsValid()) {
                    bool HasCode = true;
                    int linendx = foundCU.FindLineEntryIndex(0, iline, &fspec, HasCode);
                    if (linendx < 0) {
                        HasCode = false;
                        linendx = foundCU.FindLineEntryIndex(0, iline, &fspec, HasCode);
                    }
                    SBLineEntry lEntry = foundCU.GetLineEntryAtIndex(linendx);
                    if (HasCode) {
                        addr_t startaddr = lEntry.GetStartAddress().GetFileAddress();
                        SBFunction startfunc = lEntry.GetStartAddress().GetFunction();
                        addr_t startfuncaddr = startfunc.GetStartAddress().GetFileAddress();
                        addr_t endaddr = lEntry.GetEndAddress().GetFileAddress();
                        SBFunction endfunc = lEntry.GetEndAddress().GetFunction();
                        addr_t endfuncaddr = startfunc.GetStartAddress().GetFileAddress();
                        srlprintf("Line %d of \"%s\" starts at address %p <%s+%d> and ends at %p <%s+%d>\n", iline,
                                  lEntry.GetFileSpec().GetFilename(), startaddr, startfunc.GetName(),
                                  (startaddr - startfuncaddr), endaddr, endfunc.GetName(), (endaddr - endfuncaddr));
                    } else {
                        SBAddress startsbaddr = lEntry.GetStartAddress();
                        srlprintf("Line %d of \"%s\" is at address %p <%s> but contains no code.\n", iline,
                                  lEntry.GetFileSpec().GetFilename(), startsbaddr.GetOffset(),
                                  startsbaddr.GetFunction().GetName());
                    }
                    cdtprintf("%d^done\n(gdb)\n", cc.sequence);
                } else {
                    srcprintf("No source file named %s.\n", path);
                    cdtprintf("%d^error,msg=\"No source file named %s.\"\n(gdb)\n", cc.sequence, path);
                }
            } else {
                srcprintf("Function \"%s\" not defined.\n", path);
                cdtprintf("%d^error,msg=\"Function \"%s\" not defined.\"\n(gdb)\n", cc.sequence, path);
            }
        } else if (strcmp(cc.argv[nextarg], "program") == 0) {
            srcprintf("info program\n");
            if (process.IsValid()) {
                int state = process.GetState();
                if (state == eStateStopped) {
                    SBThread thrd = process.GetSelectedThread();
                    srlprintf("Using the running image of child Thread 0x%llx (LWP %lld) .\n", thrd.GetThreadID(),
                              process.GetProcessID());
                    srlprintf("Program stopped at %p.\n", thrd.GetSelectedFrame().GetPC());
                    char why[LINE_MAX];
                    thrd.GetStopDescription(why, LINE_MAX);
                    srlprintf("Stopped for: %s\n", why);
                } else if (state == eStateCrashed) {
                    srlprintf("The program being debugged has crashed.\n");
                } else if (state == eStateExited) {
                    srlprintf("The program being debugged is not being run.\n");
                } else if (state == eStateSuspended) {
                    srlprintf("The program being debugged is currently suspended.\n");
                } else
                    srlprintf("state is %d\n", state);
            } else
                srlprintf("process invalid\n");
            cdtprintf("%d^done\n(gdb)\n", cc.sequence);
        } else
            cdtprintf("%d^error,msg=\"%s, command: '%s'\"\n(gdb)\n", cc.sequence, "Command unimplemented.", cc.argv[nextarg]);
    } else if (strcmp(cc.argv[0], "-symbol-list-lines") == 0) {
        char path[NAME_MAX];
        if (nextarg < cc.argc)
            strlcpy(path, cc.argv[nextarg++], sizeof(path));
        SBFileSpec fspec;
        SBCompileUnit foundCU = findCUForFile(path, target, fspec);

        if (foundCU.IsValid()) {
            cdtprintf("%d^done,lines={", cc.sequence);
            for (unsigned int ndx = 0; ndx < foundCU.GetNumLineEntries(); ndx++) {
                SBLineEntry line = foundCU.GetLineEntryAtIndex(ndx);
                addr_t startaddr = line.GetStartAddress().GetFileAddress();
                SBFileSpec searchspec(path, true);
                if (strcmp(line.GetFileSpec().GetFilename(), searchspec.GetFilename()) == 0) {
                    if (ndx != 0)
                        cdtprintf(",");
                    cdtprintf("{pc=\"%p\",line=\"%d\"}", startaddr, line.GetLine());
                }
            }
            cdtprintf("}\n(gdb)\n");
        } else
            cdtprintf("%d^error,msg=\"-symbol-list-lines: Unknown source file name.\"\n(gdb)\n", cc.sequence);
    } else if (strcmp(cc.argv[0], "catch") == 0 && strcmp(cc.argv[1], "catch") == 0) {
        SBBreakpoint breakpoint =
            target.BreakpointCreateForException(lldb::LanguageType::eLanguageTypeC_plus_plus, true, false);

        cdtprintf("&\"catch catch\\n\"\n");
        cdtprintf("~\"Catchpoint %d (catch)\\n\"\n", breakpoint.GetID());
        cdtprintf("=breakpoint-created,bkpt={number=\"%d\",type=\"breakpoint\",disp=\"keep\",enabled=\"y\",addr=\"<"
                  "PENDING>\",what=\"exception catch\",catch-type=\"catch\",times=\"0\"}\n",
                  breakpoint.GetID());
        cdtprintf("%d^done\n(gdb)\n", cc.sequence);
    } else if (strcmp(cc.argv[0], "catch") == 0 && strcmp(cc.argv[1], "throw") == 0) {
        SBBreakpoint breakpoint =
            target.BreakpointCreateForException(lldb::LanguageType::eLanguageTypeC_plus_plus, false, true);

        cdtprintf("&\"catch throw\\n\"\n");
        cdtprintf("~\"Catchpoint %d (throw)\\n\"\n", breakpoint.GetID());
        cdtprintf("=breakpoint-created,bkpt={number=\"%d\",type=\"breakpoint\",disp=\"keep\",enabled=\"y\",addr=\"<"
                  "PENDING>\",what=\"exception throw\",catch-type=\"throw\",times=\"0\"}\n",
                  breakpoint.GetID());
        cdtprintf("%d^done\n(gdb)\n", cc.sequence);
    } else {
        logprintf(LOG_WARN, "command not understood: ");
        logdata(LOG_NOHEADER, cc.argv[0], strlen(cc.argv[0]));
        cdtprintf("%d^error,msg=\"%s, command: '%s'\"\n(gdb)\n", cc.sequence, "Command unimplemented.", cc.argv[nextarg]);
    }
    return dataflag;
}


// decode command line and fill the cc CDT_COMMAND structure
//   get sequence number
//   convert arguments line in a argv vector
//   decode optional (--option) arguments
int Lldbmi2::evalCDTCommand(const char* cdtcommand, CDT_COMMAND* cc)
{
    logprintf(LOG_NONE, "evalCDTLine (0x%x, %s, 0x%x)\n", this, cdtcommand, cc);
    cc->sequence = 0;
    cc->argc = 0;
    cc->arguments[0] = '\0';
    if (cdtcommand[0] == '\0') // just ENTER
        return 0;
    // decode command with sequence number. ^\n should be ^\0
    int fields = sscanf(cdtcommand, "%d%[^\n]", &cc->sequence, cc->arguments);
    if (fields == 0) {
        // try decode command without sequence number. ^\n should be ^\0
        fields = sscanf(cdtcommand, "%[^\n]", cc->arguments);
        if (fields != 1)
            return 0;
        cc->sequence = 0;
    } else if (fields < 2) {
        logprintf(LOG_WARN, "invalid command format: ");
        logdata(LOG_NOHEADER, cdtcommand, strlen(cdtcommand));
        cdtprintf("%d^error,msg=\"%s\"\n(gdb)\n", cc->sequence, "invalid command format.");
        return 0;
    }

    cc->threadgroup[0] = '\0';
    cc->thread = cc->frame = cc->available = cc->all = -1;

    fields = cc->scanArgs();

    int field;
    for (field = 1; field < fields; field++) { // arg 0 is the command
        if (strcmp(cc->argv[field], "--thread-group") == 0) {
            strlcpy(cc->threadgroup, cc->argv[++field], sizeof(cc->threadgroup));
            strlcpy(threadgroup, cc->threadgroup, sizeof(this));
        } else if (strcmp(cc->argv[field], "--thread") == 0) {
            int actual_thread = cc->thread;
            sscanf(cc->argv[++field], "%d", &cc->thread);
            if (cc->thread != actual_thread && cc->thread >= 0)
                process.SetSelectedThreadByIndexID(cc->thread);
        } else if (strcmp(cc->argv[field], "--frame") == 0) {
            int actual_frame = cc->frame;
            sscanf(cc->argv[++field], "%d", &cc->frame);
            if (cc->frame != actual_frame && cc->frame >= 0) {
                SBThread thread = process.GetSelectedThread();
                if (thread.IsValid())
                    thread.SetSelectedFrame(cc->frame);
                else
                    cdtprintf("%d^error,msg=\"%s\"\n(gdb)\n", cc->sequence, "Can not select frame. thread is invalid.");
            }
        } else if (strcmp(cc->argv[field], "--available") == 0)
            cc->available = 1;
        else if (strcmp(cc->argv[field], "--all") == 0)
            cc->all = 1;
        else if (strncmp(cc->argv[field], "--", 2) == 0) {
            logprintf(LOG_WARN, "unexpected qualifier %s\n", cc->argv[field]);
            break;
        } else
            break;
    }

    return field;
}

void Lldbmi2::onStopped()
{
    logprintf(LOG_TRACE, "onStopped (0x%x, 0x%x)\n", this, &process);
    //	-3-38-5.140 <<=  |=breakpoint-modified,bkpt={number="breakpoint
    //1",type="breakpoint",disp="del",enabled="y",addr="0x0000000100000f06",func="main",file="tests.c",fullname="tests.c",line="33",thread-groups=["i1"],times="1",original-location="tests.c:33"}\n|
    //	-3-38-5.140 <<=  |*stopped,reason="breakpoint-hit",disp="keep",bkptno="breakpoint
    //1",frame={addr="0x0000000100000f06",func="main",args=[],file="tests.c",fullname="tests.c",line="33"},thread-id="1",stopped-threads="all"\n|
    //	-3-40-7.049 <<=
    //|*stopped,reason="breakpoint-hit",disp="keep",bkptno="1",frame={addr="0000000000000f06",func="main",args=[],file="tests.c",fullname="/project_path/tests/Debug/../Sources/tests.c",line="33"},thread-id="1",stopped-threads="all"(gdb)\n|
    //                    *stopped,reason="signal-received",signal-name="SIGSEGV",signal-meaning="Segmentation
    //                    fault",frame={addr="0x0000000100000f7b",func="main",args=[],file="../Sources/tests.cpp",fullname="/project_path/test_hello_cpp/Sources/tests.cpp",line="44"},thread-id="1",stopped-threads="all"
    checkThreadsLife();
    updateSelectedThread(process); // search which thread is stopped
    SBTarget target = process.GetTarget();
    SBThread thread = process.GetSelectedThread();
    if (!thread.IsValid()) {
        logprintf(LOG_ERROR, "thread invalid on event eStateStopped\n");
        return;
    }
    int stopreason = thread.GetStopReason();
    //	logprintf (LOG_EVENTS, "stopreason=%d\n", stopreason);
    if (stopreason == eStopReasonBreakpoint || stopreason == eStopReasonPlanComplete) {
        int bpid = 0;
        const char* dispose = "keep";
        char reasondesc[LINE_MAX];
        if (stopreason == eStopReasonBreakpoint) {
            if (thread.GetStopReasonDataCount() > 0) {
                int bpid = thread.GetStopReasonDataAtIndex(0);
                SBBreakpoint breakpoint = target.FindBreakpointByID(bpid);
                if (breakpoint.IsOneShot())
                    dispose = "del";
                char* breakpointdesc = formatBreakpoint(breakpoint, this);
                cdtprintf("=breakpoint-modified,bkpt=%s\n", breakpointdesc);
                snprintf(reasondesc, sizeof(reasondesc), "reason=\"breakpoint-hit\",disp=\"%s\",bkptno=\"%d\",",
                         dispose, bpid);
            } else
                snprintf(reasondesc, sizeof(reasondesc), "reason=\"function-finished\",");
        } else
            reasondesc[0] = '\0';
        SBFrame frame = thread.GetSelectedFrame();
        if (!frame.IsValid()) {
            logprintf(LOG_ERROR, "frame invalid on event eStateStopped (eStopReasonBreakpoint)\n");
            return;
        }
        char* framedesc = formatFrame(frame, WITH_ARGS);
        int threadindexid = thread.GetIndexID();
        cdtprintf("*stopped,%s%s,thread-id=\"%d\",stopped-threads=\"all\"\n(gdb)\n", reasondesc, framedesc,
                  threadindexid);
        //	cdtprintf
        //("*stopped,reason=\"breakpoint-hit\",disp=\"keep\",bkptno=\"1\",frame={addr=\"0x0000000100000f06\",func=\"main\",args=[],file=\"../Sources/tests.c\",fullname=\"/project_path/tests/Sources/tests.c\",line=\"33\"},thread-id=\"1\",stopped-threads=\"all\"\n(gdb)
        //\n");
        if (strcmp(dispose, "del") == 0) {
            target.BreakpointDelete(bpid);
            cdtprintf("=breakpoint-deleted,id=\"%d\"\n", bpid);
        }
    } else if (stopreason == eStopReasonWatchpoint) {
        if (thread.GetStopReasonDataCount() > 0) {
            int wpid = thread.GetStopReasonDataAtIndex(0);
            SBWatchpoint watch = target.FindWatchpointByID(wpid);
            cdtprintf("*stopped,reason=\"watchpoint-trigger\",wpt={number=\"%d\",exp=\"%p\"},", watch.GetID(),
                      watch.GetWatchAddress());
            SBStream str;
            watch.GetDescription(str, eDescriptionLevelVerbose);
            const char* desc = str.GetData();
            uint64_t oldValue = 0, newValue = 0;
            const char* s = strstr(desc, "old value:");
#ifdef __APPLE__
            sscanf(s, "%*[^0-9]%lli", &oldValue);
#else
            sscanf(s, "%*[^0-9]%lu", &oldValue);
#endif
            s = strstr(desc, "new value:");
#ifdef __APPLE__
            sscanf(s, "%*[^0-9]%lli", &newValue);
#else
            sscanf(s, "%*[^0-9]%lu", &newValue);
#endif
            cdtprintf("value={old=\"%llu\",new=\"%llu\"},", oldValue, newValue);
            SBFrame frame = thread.GetSelectedFrame();
            SBLineEntry lentry = frame.GetLineEntry();
            SBFileSpec fspec = lentry.GetFileSpec();
            cdtprintf("frame={func=\"%s\",args=[],file=\"%s\",line=\"%d\"}\n(gdb)\n", frame.GetFunctionName(),
                      fspec.GetFilename(), lentry.GetLine());
        } else
            cdtprintf("*stopped,reason=\"watchpoint-trigger\"}\n(gdb)\n");
    } else if (stopreason == eStopReasonSignal) {
        // raised when attaching to a process
        // raised when program crashed
        int stopreason = thread.GetStopReasonDataAtIndex(0);
        SBUnixSignals unixsignals = process.GetUnixSignals();
        const char* signalname = unixsignals.GetSignalAsCString(stopreason);
        char reasondesc[LINE_MAX];
        snprintf(reasondesc, sizeof(reasondesc), "reason=\"signal-received\",signal-name=\"%s\",", signalname);
        SBFrame frame = thread.GetSelectedFrame();
        if (!frame.IsValid()) {
            logprintf(LOG_ERROR, "frame invalid on event eStateStopped (eStopReasonSignal)\n");
            return;
        }
        char* framedesc = formatFrame(frame, WITH_ARGS);
        int threadindexid = thread.GetIndexID();
        // signal-name="SIGSEGV",signal-meaning="Segmentation fault"
        cdtprintf("*stopped,%s%s,thread-id=\"%d\",stopped-threads=\"all\"\n(gdb)\n", reasondesc, framedesc,
                  threadindexid);
        // *stopped,reason="signal-received",signal-name="SIGSEGV",signal-meaning="Segmentation
        // fault",frame={addr="0x0000000100000f7b",func="main",args=[],file="../Sources/tests.cpp",fullname="/project_path/test_hello_cpp/Sources/tests.cpp",line="44"},thread-id="1",stopped-threads="all"
    } else if (stopreason == eStopReasonNone) {
        // raised when a thread different from the selected thread stops
    } else if (stopreason == eStopReasonInvalid) {
        // raised when the program exits
    } else if (stopreason == eStopReasonException) {
        //  "*stopped,reason=\"exception-received\",exception=\"%s\",thread-id=\"%d\",stopped-threads=\"all\""
        char exceptiondesc[LINE_MAX];
        thread.GetStopDescription(exceptiondesc, LINE_MAX);
        writelog((ptyfd != EOF) ? ptyfd : STDOUT_FILENO, exceptiondesc, strlen(exceptiondesc));
        writelog((ptyfd != EOF) ? ptyfd : STDOUT_FILENO, "\n", 1);
        char reasondesc[LINE_MAX + 50];
        snprintf(reasondesc, sizeof(reasondesc), "reason=\"exception-received\",exception=\"%s\",", exceptiondesc);
        int threadindexid = thread.GetIndexID();
        cdtprintf("*stopped,%sthread-id=\"%d\",stopped-threads=\"all\"\n(gdb)\n", reasondesc, threadindexid);
    } else
        logprintf(LOG_WARN, "unexpected stop reason %d\n", stopreason);
    isrunning = false;
}

void Lldbmi2::checkThreadsLife()
{
    logprintf(LOG_TRACE, "checkThreadsLife (0x%x, 0x%x)\n", this, &process);
    if (!process.IsValid())
        return;
    SBThread thread;
    const size_t nthreads = process.GetNumThreads();
    int indexlist;
    bool stillalive[THREADS_MAX];
    for (indexlist = 0; indexlist < THREADS_MAX; indexlist++) // init live list
        stillalive[indexlist] = false;
    for (size_t indexthread = 0; indexthread < nthreads; indexthread++) {
        SBThread thread = process.GetThreadAtIndex(indexthread);
        if (thread.IsValid()) {
            int stopreason = thread.GetStopReason();
            int threadindexid = thread.GetIndexID();
            logprintf(LOG_NONE, "thread threadindexid=%d stopreason=%d\n", threadindexid, stopreason);
            for (indexlist = 0; indexlist < THREADS_MAX; indexlist++) {
                if (threadindexid == threadids[indexlist]) // existing thread
                    break;
            }
            if (indexlist < THREADS_MAX) // existing thread. mark as alive
                stillalive[indexlist] = true;
            else { // new thread. add to the thread list list
                for (indexlist = 0; indexlist < THREADS_MAX; indexlist++) {
                    if (threadids[indexlist] == 0) {
                        threadids[indexlist] = threadindexid;
                        stillalive[indexlist] = true;
                        cdtprintf("=thread-created,id=\"%d\",group-id=\"%s\"\n", threadindexid, threadgroup);
                        break;
                    }
                }
                if (indexlist >= THREADS_MAX)
                    logprintf(LOG_ERROR, "threads table too small (%d)\n", THREADS_MAX);
            }
        }
    }
    for (indexlist = 0; indexlist < THREADS_MAX; indexlist++) { // find finished threads
        if (threadids[indexlist] > 0 && !stillalive[indexlist]) {
            cdtprintf("=thread-exited,id=\"%d\",group-id=\"%s\"\n", threadids[indexlist], threadgroup);
            threadids[indexlist] = 0;
        }
    }
}

void Lldbmi2::terminateProcess(int how)
{
    logprintf(LOG_TRACE, "terminateProcess (0x%x, 0x%x)\n", this, how);
    procstop = true;
    if (process.IsValid()) {
        SBThread thread = process.GetSelectedThread();
        int tid = thread.IsValid() ? thread.GetIndexID() : 0;
        if ((how & PRINT_THREAD))
            cdtprintf("=thread-exited,id=\"%d\",group-id=\"%s\"\n", tid, threadgroup);
        process.Destroy();
        //	process.Kill();
    } else
        logprintf(LOG_INFO, "process not valid\n");
    if ((how & PRINT_GROUP))
        cdtprintf("=thread-group-exited,id=\"%s\",exit-code=\"0\"\n", threadgroup);
    if ((how & AND_EXIT))
        eof = true;
}

// log an argument and return the argument
const char* logarg(const char* arg) {
    addlog(arg);
    addlog(" ");
    return arg;
}

void writetocdt(const char* line) {
    logprintf(LOG_TRACE, "writetocdt (...)\n", line);
    logdata(LOG_CDT_OUT, line, strlen(line));
    writelog(gpstate->cdtptyfd > 0 ? gpstate->cdtptyfd : STDOUT_FILENO, line, strlen(line));
}

void cdtprintf(const char* format, ...) {
    logprintf(LOG_NONE, "cdtprintf (...)\n");
    static StringB buffer(BIG_LINE_MAX);
    va_list args;

    if (format != NULL) {
        va_start(args, format);
        buffer.vosprintf(0, format, args);
        va_end(args);
        if ((buffer.c_str()[0] == '0') && (buffer.c_str()[1] == '^'))
            buffer.clear(1, 0);
        writetocdt(buffer.c_str());
    }
}

std::string ReplaceString(std::string subject, const std::string& search, const std::string& replace) {
    size_t pos = 0;
    while ((pos = subject.find(search, pos)) != std::string::npos) {
        subject.replace(pos, search.length(), replace);
        pos += replace.length();
    }
    return subject;
}

void strrecprintf(const char* typestr, const char* format, va_list args) {
    logprintf(LOG_NONE, "srcprintf (...)\n");
    static StringB buffer(BIG_LINE_MAX);
    static StringB lineout(BIG_LINE_MAX);
    static StringB prepend(10);

    buffer.vosprintf(0, format, args);
    prepend.clear();
    prepend.append(typestr);
    prepend.append("\"");
    lineout.clear();
    lineout.append(prepend.c_str());
    for (int ndx = 0; ndx < buffer.size(); ndx++) {
        if (buffer.c_str()[ndx] == '\"')
            lineout.append("\\\"");
        else if (buffer.c_str()[ndx] == '\n') {
            lineout.append("\\n\"\n");
            writetocdt(lineout.c_str());
            lineout.clear();
            lineout.append(prepend.c_str());
        } else
            lineout.append(buffer.c_str()[ndx]);
    }
    if (lineout.size() > 2) {
        lineout.append("\n\"");
        writetocdt(lineout.c_str());
    }
}

void srcprintf(const char* format, ...) {
    va_list args;

    if (format != NULL) {
        va_start(args, format);
        strrecprintf("&", format, args);
        va_end(args);
    }
}

void srlprintf(const char* format, ...) {
    va_list args;

    if (format != NULL) {
        va_start(args, format);
        strrecprintf("~", format, args);
        va_end(args);
    }
}

static int signals_received = 0;

void signalHandler(int signo) {
    logprintf(LOG_TRACE, "signalHandler (%d)\n", signo);
    if (signo == SIGINT)
        logprintf(LOG_INFO, "signal SIGINT\n");
    else
        logprintf(LOG_INFO, "signal %s\n", signo);
    if (signo == SIGINT) {
        if (gpstate->process.IsValid() && signals_received == 0) {
            int selfPID = getpid();
            int processPID = gpstate->process.GetProcessID();
            logprintf(LOG_INFO, "signal_handler: signal SIGINT. self PID = %d, process pid = %d\n", selfPID,
                      processPID);
            logprintf(LOG_INFO, "send signal SIGSTOP to process %d\n", processPID);
            //	gprocess.Signal (SIGSTOP);
            logprintf(LOG_INFO, "Stop process\n");
            gpstate->process.Stop();
            //	++signals_received;
        } else
            gpstate->debugger.DispatchInputInterrupt();
    }
}

/*

BUTTON PAUSE (SIGSTOP or ^Z)
173728.927 ---  signal SIGINT
173728.927 ---  signal_handler: signal SIGINT. self PID = 15659, process pid = 15660
173728.927 ---  send signal SIGSTOP to process 15660
173728.930 ###  eStateStopped
173728.930 <<=  |=thread-created,id="2",group-id="i1"\n|
173728.931 <<=
|*stopped,reason="signal-received",signal-name="SIGSTOP",frame={addr="0x000000000001710a",func="__semwait_signal",args=[],file="libsystem_kernel.dylib"}thread-id="1",stopped-threads="all"\n(gdb)\n|
173728.939 >>=  |32thread|
173728.939 !!!  command not understood: 173728.939   |thread|
173728.940 <<=  |32^error,msg="Command unimplemented."\n(gdb)\n|

BUTTON STOP (SIGINT or ^C)
173504.979 ###  eBroadcastBitSTDOUT
173504.979 <<<  |loop 0\n|
173505.222 ---  signal SIGINT
173505.222 ---  signal_handler: signal SIGINT. self PID = 15615, process pid = 15616
173505.222 ---  send signal SIGSTOP to process 15616
173505.223 >>=  |32-interpreter-exec --thread-group i1 console kill|
173505.223 ---  console kill: send SIGINT
173505.223 <<=  |32^done\n(gdb)\n|
173505.233 ###  eStateStopped
173505.233 <<=  |=thread-created,id="2",group-id="i1"\n|

005910.640 <<=
|*stopped,reason="breakpoint-hit",disp="keep",bkptno="1",frame={addr="0x000000000000127a",func="waitthread()",args=[],file="tests.cpp",fullname="/Users/didier/Projets/git-lldbmi2/lldbmi2/tests/src/tests.cpp",line="50"},thread-id="1",stopped-threads="all"\n(gdb)\n|
005910.722 <<=
|30^done,groups=[{id="i1",type="process",pid="20408",executable="/Users/didier/Projets/git-lldbmi2/lldbmi2/build/tests"}]\n(gdb)\n|

005610.309 <<=
|*stopped,reason="signal-received",signal-name="SIGSTOP",frame={addr="0x0000000000001286",func="waitthread()",args=[],file="tests.cpp",fullname="/Users/didier/Projets/git-lldbmi2/lldbmi2/tests/src/tests.cpp",line="50"},thread-id="1",stopped-threads="all"\n(gdb)\n|
005610.319 <<=
|30^done,groups=[{id="i1",type="process",pid="20359",executable="/Users/didier/Projets/git-lldbmi2/lldbmi2/build/tests"}]\n(gdb)\n|

*/
