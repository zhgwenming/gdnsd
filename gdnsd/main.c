/* Copyright © 2012 Brandon L Black <blblack@gmail.com>
 *
 * This file is part of gdnsd.
 *
 * gdnsd is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * gdnsd is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with gdnsd.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "main.h"

#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <pwd.h>
#include <time.h>

#include "socks.h"
#include "dnsio_tcp.h"
#include "dnsio_udp.h"
#include "dnspacket.h"
#include "statio.h"
#include "ztree.h"
#include "zsrc_rfc1035.h"
#include "zsrc_djb.h"
#include "gdnsd/log.h"
#include "gdnsd/plugapi-priv.h"
#include "gdnsd/net-priv.h"
#include "gdnsd/misc-priv.h"
#include "gdnsd/paths-priv.h"
#include "gdnsd/mon-priv.h"

// ev loop used for monitoring, statio, and watchdog
// (all of which shared a thread as well)
static struct ev_loop* mon_loop = NULL;

// Watchdog stuff
static ev_timer* watchdog_timer = NULL;

static void watchdog_cb(struct ev_loop* loop V_UNUSED, ev_timer* w V_UNUSED, int revents V_UNUSED) {
    dmn_assert(loop == mon_loop);
    dmn_assert(w == watchdog_timer);
    dmn_assert(revents == EV_TIMER);
    dmn_wdog_ping();
}

static void watchdog_start(struct ev_loop* loop) {
    dmn_assert(loop == mon_loop);
    unsigned msec = dmn_wdog_get_msec();
    if(msec) {
        double sec_dbl = ((double)msec) / 1000.0;
        watchdog_timer = malloc(sizeof(ev_timer));
        ev_timer_init(watchdog_timer, watchdog_cb, 0., sec_dbl);
        ev_timer_start(loop, watchdog_timer);
        log_info("watchdog starting, will ping every ~ %.3f seconds", sec_dbl);
    }
}

// custom atexit-like stuff, only for resource
//   de-allocation in debug builds to check for leaks

#ifndef NDEBUG

static void (**exitfuncs)(void) = NULL;
static unsigned exitfuncs_pending = 0;

void gdnsd_atexit_debug(void (*f)(void)) {
    dmn_assert(f);
    exitfuncs = realloc(exitfuncs, (exitfuncs_pending + 1) * sizeof(void (*)(void)));
    exitfuncs[exitfuncs_pending++] = f;
}

static void atexit_debug_execute(void) {
    while(exitfuncs_pending--)
       exitfuncs[exitfuncs_pending]();
}

#else

void gdnsd_atexit_debug(void (*f)(void) V_UNUSED) { }
static void atexit_debug_execute(void) { }

#endif

F_NONNULL
static void syserr_for_ev(const char* msg) { dmn_assert(msg); log_fatal("%s: %s", msg, dmn_logf_errno()); }

F_NONNULL F_NORETURN
static void usage(const char* argv0) {
    fprintf(stderr,
        PACKAGE_NAME " version " PACKAGE_VERSION "\n"
        "Usage: %s [-fsSD] [-c %s] <action>\n"
        "  -D - Enable verbose debug output\n"
        "  -f - Foreground mode for start/restart-like actions\n"
        "  -s - Force 'zones_strict_startup = true' for this invocation\n"
        "  -S - Force 'zones_strict_data = true' for this invocation\n"
        "  -c - Configuration directory\n"
        "Actions:\n"
        "  checkconf - Checks validity of config and zone files\n"
        "  start - Start " PACKAGE_NAME " as a regular daemon\n"
        "  stop - Stops a running daemon previously started by 'start'\n"
        "  reload - Send SIGHUP to running daemon for zone data reload\n"
        "  restart - Equivalent to checkconf && stop && start, but faster\n"
        "  force-reload - Aliases 'restart'\n"
        "  condrestart - Does 'restart' action only if already running\n"
        "  try-restart - Aliases 'condrestart'\n"
        "  status - Checks the status of the running daemon\n\n"
        "Optional compile-time features:"

#       ifndef NDEBUG
            " developer-debug-build"
#       endif
#       ifdef HAVE_QSBR
            " urcu"
#       endif
#       ifdef USE_SENDMMSG
            " mmsg"
#       endif
#       ifdef USE_INOTIFY
            " inotify"
#       endif
#       ifdef USE_SYSTEMD_HAX
            " systemd-hacks"
#       elif defined USE_SYSTEMD
            " systemd"
#       endif

#       if  defined NDEBUG \
        && !defined HAVE_QSBR \
        && !defined USE_SENDMMSG \
        && !defined USE_INOTIFY \
        && !defined USE_SYSTEMD
            " none"
#       endif

        "\nFor updates, bug reports, etc, please visit " PACKAGE_URL "\n",
        argv0, gdnsd_get_default_config_dir()
    );
    exit(2);
}

// thread entry point for zone data reloader thread
static void* zone_data_runtime(void* unused V_UNUSED) {
    gdnsd_thread_setname("gdnsd-zones");

    struct ev_loop* zdata_loop = ev_loop_new(EVFLAG_AUTO);
    if(!zdata_loop)
        log_fatal("Could not initialize the zone data libev loop");

    zsrc_djb_runtime_init(zdata_loop);
    zsrc_rfc1035_runtime_init(zdata_loop);

    ev_run(zdata_loop, 0);

    dmn_assert(0); // should never be reached as loop never terminates
    ev_loop_destroy(zdata_loop);
    return NULL;
}

// thread entry point for monitoring (+statio) thread
static void* mon_runtime(void* unused V_UNUSED) {
    gdnsd_thread_setname("gdnsd-mon");

    // mon_start already queued up its events in mon_loop earlier...
    statio_start(mon_loop);
    ev_run(mon_loop, 0);

    dmn_assert(0); // should never be reached as loop never terminates
    ev_loop_destroy(mon_loop);
    return NULL;
}

static void start_threads(void) {
    // Block all signals using the pthreads interface while starting threads,
    //  which causes them to inherit the same mask.
    sigset_t sigmask_all, sigmask_prev;
    sigfillset(&sigmask_all);
    pthread_sigmask(SIG_SETMASK, &sigmask_all, &sigmask_prev);

    // system scope scheduling, joinable threads
    pthread_attr_t attribs;
    pthread_attr_init(&attribs);
    pthread_attr_setdetachstate(&attribs, PTHREAD_CREATE_DETACHED);
    pthread_attr_setscope(&attribs, PTHREAD_SCOPE_SYSTEM);

    int pthread_err;

    for(unsigned i = 0; i < gconfig.num_dns_threads; i++) {
        dns_thread_t* t = &gconfig.dns_threads[i];
        if(t->is_udp)
            pthread_err = pthread_create(&t->threadid, &attribs, &dnsio_udp_start, (void*)t);
        else
            pthread_err = pthread_create(&t->threadid, &attribs, &dnsio_tcp_start, (void*)t);
        if(pthread_err)
            log_fatal("pthread_create() of DNS thread %u (for %s:%s) failed: %s",
                i, t->is_udp ? "UDP" : "TCP", dmn_logf_anysin(&t->ac->addr), dmn_logf_strerror(pthread_err));
    }

    pthread_t zone_data_threadid;
    pthread_err = pthread_create(&zone_data_threadid, &attribs, &zone_data_runtime, NULL);
    if(pthread_err)
        log_fatal("pthread_create() of zone data thread failed: %s", dmn_logf_strerror(pthread_err));

    // This waits for all of the stat structures to be allocated
    //  by the i/o threads before continuing on.  They must be ready
    //  before the monitoring thread starts below, as it will read
    //  those stat structures
    dnspacket_wait_stats();

    pthread_t mon_threadid;
    pthread_err = pthread_create(&mon_threadid, &attribs, &mon_runtime, NULL);
    if(pthread_err)
        log_fatal("pthread_create() of monitoring thread failed: %s", dmn_logf_strerror(pthread_err));

    // Restore the original mask in the main thread, so
    //  we can continue handling signals like normal
    pthread_sigmask(SIG_SETMASK, &sigmask_prev, NULL);
    pthread_attr_destroy(&attribs);
}

static void memlock_rlimits(const bool started_as_root) {
#ifdef RLIMIT_MEMLOCK
    struct rlimit rlim;
    if(getrlimit(RLIMIT_MEMLOCK, &rlim))
        log_fatal("getrlimit(RLIMIT_MEMLOCK) failed: %s", dmn_logf_errno());

    if(rlim.rlim_cur != RLIM_INFINITY) {
        if(!started_as_root) {
            // First, raise _cur to _max, which should never fail
            if(rlim.rlim_cur != rlim.rlim_max) {
                rlim.rlim_cur = rlim.rlim_max;
                if(setrlimit(RLIMIT_MEMLOCK, &rlim))
                    log_fatal("setrlimit(RLIMIT_MEMLOCK, cur = max) "
                        "failed: %s", dmn_logf_errno());
            }

            if(rlim.rlim_cur < 1048576)
                log_fatal("Not started as root, lock_mem was set, "
                    "and the rlimit for locked memory is unreasonably "
                    "low (%li bytes), failing", (long)rlim.rlim_cur);

            log_info("The rlimit for locked memory is %li MB, and the "
                "daemon can't do anything about that since it wasn't "
                "started as root.  This may or may not be too small at "
                "runtime, leading to failure.  You have been warned.",
                (long)(rlim.rlim_cur >> 20));
        }
        else {
            // Luckily, root can do as he pleases with the ulimits, but
            //  we'll do it in two steps just in case any platforms
            //  are braindead about it.  This does open a hole in the
            //  sense that if someone were to remotely take control
            //  of the daemon via exploit, they can now lock large
            //  amounts of memory even though the daemon has dropped
            //  privileges for most other dangerous operations.
            //  The other alternatives are trying to pre-calculate
            //  all future memory usage (possible, but a PITA to
            //  maintain), or simply letting the code fail post-
            //  daemonization in an unfortunately rather common case.
            // Another option would be to offer a configfile parameter
            //  for the rlimit value in this case.
            // If the daemon gets compromised, even with privdrop,
            //  memlock is probably the least of your worries anyways.
            rlim.rlim_max = RLIM_INFINITY;
            if(setrlimit(RLIMIT_MEMLOCK, &rlim))
                log_fatal("setrlimit(RLIMIT_MEMLOCK, max = INF) "
                    "failed: %s", dmn_logf_errno());

            rlim.rlim_cur = RLIM_INFINITY;
            if(setrlimit(RLIMIT_MEMLOCK, &rlim))
                log_fatal("setrlimit(RLIMIT_MEMLOCK, cur = INF, "
                    "max = INF) failed: %s", dmn_logf_errno());
        }
    }
}
#endif

typedef enum {
    ACT_CHECKCFG   = 0,
    ACT_START,
    ACT_STOP,
    ACT_RELOAD,
    ACT_RESTART,
    ACT_CRESTART, // downgrades to ACT_RESTART after checking...
    ACT_STATUS,
    ACT_UNDEF
} action_t;

typedef struct {
    const char* cmdstring;
    action_t action;
} actmap_t;

static actmap_t actionmap[] = {
    { "checkconf",    ACT_CHECKCFG }, // 1
    { "start",        ACT_START },    // 2
    { "stop",         ACT_STOP },     // 3
    { "reload",       ACT_RELOAD },   // 4
    { "restart",      ACT_RESTART },  // 5
    { "force-reload", ACT_RESTART },  // 6
    { "condrestart",  ACT_CRESTART }, // 7
    { "try-restart",  ACT_CRESTART }, // 8
    { "status",       ACT_STATUS },   // 9
};
#define ACTIONMAP_COUNT 9

F_NONNULL F_PURE
static action_t match_action(const char* arg) {
    dmn_assert(arg);

    unsigned i;
    for(i = 0; i < ACTIONMAP_COUNT; i++)
        if(!strcasecmp(actionmap[i].cmdstring, arg))
            return actionmap[i].action;
    return ACT_UNDEF;
}

typedef struct {
    const char* cfg_dir;
    bool force_zss;
    bool force_zsd;
    bool debug;
    bool foreground;
} cmdline_opts_t;

F_NONNULL
static action_t parse_args(const int argc, char** argv, cmdline_opts_t* copts) {
    action_t action = ACT_UNDEF;

    int optchar;
    while((optchar = getopt(argc, argv, "c:DfsS"))) {
        switch(optchar) {
            case 'c':
                copts->cfg_dir = optarg;
                break;
            case 'D':
                copts->debug = true;
                break;
            case 'f':
                copts->foreground = true;
                break;
            case 's':
                copts->force_zss = true;
                break;
            case 'S':
                copts->force_zsd = true;
                break;
            case -1:
                if(optind != (argc - 1))
                    usage(argv[0]);
                action = match_action(argv[optind]);
                if(action == ACT_UNDEF)
                    usage(argv[0]);
                return action;
                break;
            default:
                usage(argv[0]);
                break;
        }
    }

    usage(argv[0]);
}

int main(int argc, char** argv) {

    // Parse args, getting the config path
    //   returning the action.  Exits on cmdline errors,
    //   does not use libdmn assert/log stuff.
    cmdline_opts_t copts = {
        .cfg_dir = NULL,
        .force_zss = false,
        .force_zsd = false,
        .debug = false,
        .foreground = false,
    };
    action_t action = parse_args(argc, argv, &copts);

    // basic action-based parameters
    bool will_daemonize;
    conf_mode_t cmode;
    switch(action) {
        case ACT_STATUS: // fall-through
        case ACT_RELOAD: // fall-through
        case ACT_STOP:
            cmode = CONF_SIMPLE_ACTION;
            will_daemonize = false;
            break;
        case ACT_CHECKCFG:
            cmode = CONF_CHECK;
            will_daemonize = false;
            break;
        case ACT_START:
        case ACT_RESTART:
        case ACT_CRESTART:
            cmode = CONF_START;
            will_daemonize = !copts.foreground;
            break;
        default:
            dmn_assert(0);
            break;
    }

    // init1 lets us start using dmn log funcs for config errors, etc
    dmn_init1(copts.debug, copts.foreground, !will_daemonize, will_daemonize, PACKAGE_NAME);

    // Initialize net stuff in libgdnsd - needed for config load
    gdnsd_init_net();

    // Init meta-PRNG - needed for config load
    gdnsd_rand_meta_init();

    // Load config file
    conf_load(copts.cfg_dir, copts.force_zss, copts.force_zsd, cmode);

    // init2() lets us do daemon actions
    char* rundir = gdnsd_resolve_path_run(NULL, NULL);
    dmn_init2(rundir);
    free(rundir);

    // Take action
    if(action == ACT_STATUS) {
        const pid_t oldpid = dmn_status();
        if(!oldpid) {
            log_info("status: not running");
            exit(3);
        }
        log_info("status: running at pid %li", (long)oldpid);
        exit(0);
    }
    else if(action == ACT_STOP) {
        exit(dmn_stop() ? 1 : 0);
    }
    else if(action == ACT_RELOAD) {
        exit(dmn_signal(SIGHUP));
    }

    if(action == ACT_CRESTART) {
        const pid_t oldpid = dmn_status();
        if(!oldpid) {
            log_info("condrestart: not running, will not restart");
            exit(0);
        }
        action = ACT_RESTART;
    }

    // Set up and validate privdrop info if necc
    dmn_init3(gconfig.username, (action == ACT_RESTART));

    // Call plugin full_config actions
    gdnsd_plugins_action_full_config(gconfig.num_dns_threads);

    log_info("Loading zone data...");
    ztree_init();
    zsrc_djb_load_zones(action == ACT_CHECKCFG);
    zsrc_rfc1035_load_zones(action == ACT_CHECKCFG);

    if(action == ACT_CHECKCFG) {
        log_info("Configuration and zone data loads just fine");
        exit(0);
    }

    // from here out, all actions are attempting startup...
    dmn_assert(action == ACT_START || action == ACT_RESTART);

    // Did we start as root?  This determines how we handle memlock/setpriority
    const bool started_as_root = !geteuid();

    // Check/set rlimits for mlockall() if necessary and possible
    if(gconfig.lock_mem)
        memlock_rlimits(started_as_root);

    // Initialize DNS listening sockets, but do not bind() them yet
    dns_lsock_init();

    // init the stats summing/output code + listening sockets (again no bind yet)
    statio_init();

    // set up our pcall for socket binding later
    unsigned bind_socks_funcidx = dmn_add_pcall(socks_helper_bind_all);

    dmn_fork();

    // Call plugin post-daemonize actions
    gdnsd_plugins_action_post_daemonize();

    // If root, or if user explicitly set a priority...
    if(started_as_root || gconfig.priority != -21) {
        // If root and no explicit value, use -11
        if(started_as_root && gconfig.priority == -21)
            gconfig.priority = -11;
        if(setpriority(PRIO_PROCESS, getpid(), gconfig.priority))
            log_warn("setpriority(%i) failed: %s", gconfig.priority, dmn_logf_errno());
    }

    // Lock whole daemon into memory, including
    //  all future allocations.
    if(gconfig.lock_mem)
        if(mlockall(MCL_CURRENT | MCL_FUTURE))
            log_fatal("mlockall(MCL_CURRENT|MCL_FUTURE) failed: %s (you may need to disabled the lock_mem config option if your system or your ulimits do not allow it)",
                dmn_logf_errno());

    // Initialize dnspacket stuff
    dnspacket_global_setup();

    // Call plugin pre-privdrop actions
    gdnsd_plugins_action_pre_privdrop();

    // drop privs if started as root
    dmn_secure();

    // Call plugin pre-privdrop actions
    gdnsd_plugins_action_post_privdrop();

    // Set up libev error callback
    ev_set_syserr_cb(&syserr_for_ev);

    // Construct the monitoring loop, for monitors + statio,
    //   which will be executed in another thread for runtime
    mon_loop = ev_loop_new(EVFLAG_AUTO);
    if(!mon_loop)
        log_fatal("Could not initialize the mon libev loop");

    // set up monitoring, which expects an initially empty loop
    gdnsd_mon_start(mon_loop);

    // Call plugin pre-run actions
    gdnsd_plugins_action_pre_run();

    // ask the helper (which is still root) to bind our sockets,
    //   this blocks until completion.  This uses SO_REUSEPORT
    //   if available.  If the previous instance also uses SO_REUSEPORT
    //   (gdnsd 2.x), then we should get success here and overlapping
    //   sockets before we kill the old daemon.
    dmn_pcall(bind_socks_funcidx);

    // validate the results of the above, softly
    bool first_binds_failed = socks_daemon_check_all(true);

    // if the first binds didn't work (probably lack of SO_REUSEPORT,
    //   either in general or just in the old 1.x daemon we're taking over),
    //   we have to stop the old daemon before binding again.
    if(first_binds_failed) {
        // Kills old daemon on the way, if we're restarting
        dmn_acquire_pidfile();

        // This re-attempts binding any specific sockets that failed the
        //   first time around, e.g. in non-SO_REUSEPORT cases where we
        //   had to wait on daemon death above
        dmn_pcall(bind_socks_funcidx);

        // hard check this time - this function will fail fatally
        //   if any sockets can't be acquired.
        socks_daemon_check_all(false);
    }

    // Start the ev_timer for the watchdog, if applicable,
    //   which will run in the monitoring thread/loop.
    watchdog_start(mon_loop);

    // Start up all of the UDP and TCP threads, each of
    // which has all signals blocked and has its own
    // event loop (libev for TCP, manual blocking loop for UDP)
    // Also starts the zone data reload thread
    // and the statio+monitoring thread
    start_threads();

    // Notify the user that the listeners are up
    log_info("DNS listeners started");

    // If we succeeded at SO_REUSEPORT takeover earlier on the first
    //   bind() attempts, we still need to kill the old daemon (if restarting)
    //   at this point, since we didn't earlier for availability overlap.
    if(!first_binds_failed)
        dmn_acquire_pidfile();

    // The signals we'll listen for below
    sigset_t mainthread_sigs;
    sigemptyset(&mainthread_sigs);
    sigaddset(&mainthread_sigs, SIGINT);
    sigaddset(&mainthread_sigs, SIGTERM);
    sigaddset(&mainthread_sigs, SIGHUP);

    // Block the relevant signals before entering the sigwait() loop
    sigset_t sigmask_prev;
    pthread_sigmask(SIG_BLOCK, &mainthread_sigs, &sigmask_prev);

    // Report success back to whoever invoked "start" or "restart" command...
    //  (or in the foreground case, kill our helper process)
    // we do this under blocking so that there's no racing with someone
    //  expecting correct signal actions after the starter exits
    dmn_finish();

    int killed_by = 0;
    while(!killed_by) {
        int rcvd_sig = 0;
        int sw_rv;
        if((sw_rv = sigwait(&mainthread_sigs, &rcvd_sig))) {
            if(sw_rv != EINTR && sw_rv != EAGAIN)
                log_fatal("sigwait() failed with error %s", dmn_logf_strerror(sw_rv));
            continue;
        }

        switch(rcvd_sig) {
            case SIGTERM:
                log_info("Received TERM signal, exiting...");
                killed_by = SIGTERM;
                break;
            case SIGINT:
                log_info("Received INT signal, exiting...");
                killed_by = SIGINT;
                break;
            case SIGHUP:
                log_info("Received HUP signal");
                zsrc_djb_sighup();
                zsrc_rfc1035_sighup();
                break;
            default:
                dmn_assert(0);
                break;
        }
    }

    // deallocate resources in debug mode
    atexit_debug_execute();

    // Restore normal signal mask
    pthread_sigmask(SIG_SETMASK, &sigmask_prev, NULL);

    // kill self with same signal, so that our exit status is correct
    //   for any parent/manager/whatever process that may be watching
    raise(killed_by);

    // raise should not return
    dmn_assert(0);
}
