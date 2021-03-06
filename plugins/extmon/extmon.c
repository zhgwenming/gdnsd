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

#define GDNSD_PLUGIN_NAME extmon

#include "config.h"
#include "extmon_comms.h"
#include <gdnsd/plugin.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/socket.h>
#include <netdb.h>

typedef struct {
    char* name;
    char** args;
    unsigned num_args;
    unsigned timeout;
    unsigned interval;
} svc_t;

typedef struct {
    const char* desc;
    const svc_t* svc;
    ev_timer* local_timeout;
    const char* thing;
    unsigned idx;
    bool seen_once;
} mon_t;

static unsigned num_svcs = 0;
static svc_t* svcs = NULL;

static unsigned int num_mons = 0;
static mon_t* mons = NULL;

static const char* helper_path = NULL;
static pid_t helper_pid = 0;
static int helper_write_fd = -1;
static int helper_read_fd = -1;

static ev_io* helper_read_watcher = NULL;

// whether we're in the init phase or runtime phase,
//   and how many distinct mon_t have been updated
//   so far during init phase.
static bool init_phase = true;
static unsigned init_phase_count = 0;

// if we experience total helper failure at any point
//   (which is in turn signalled by pipe close)
static bool total_helper_failure_flag = false;

// behavior on helper failure
static bool die_on_helper_failure = false;

static const char fail_msg[] = "plugin_extmon: Cannot continue monitoring, child process gdnsd_extmon_helper failed!";
static void total_helper_failure(struct ev_loop* loop) {
    dmn_assert(loop);

    for(unsigned i = 0; i < num_mons; i++)
        ev_timer_stop(loop, mons[i].local_timeout);

    if(die_on_helper_failure)
        log_fatal(fail_msg);

    log_err(fail_msg);
    close(helper_read_fd);
    ev_io_stop(loop, helper_read_watcher);
    total_helper_failure_flag = true;
}

// common code to bump the local_timeout timer for (interval+timeout)*2,
//   starting it if not already running.
static void bump_local_timeout(struct ev_loop* loop, mon_t* mon) {
    mon->local_timeout->repeat = ((mon->svc->timeout + mon->svc->interval) << 1);
    ev_timer_again(loop, mon->local_timeout);
}

static void helper_read_cb(struct ev_loop* loop, ev_io* w, int revents V_UNUSED) {
    dmn_assert(loop); dmn_assert(w); dmn_assert(revents == EV_READ);
    dmn_assert(w->fd == helper_read_fd);

    while(1) { // loop on all immediately-available results
        uint32_t data;
        int rv = read(helper_read_fd, &data, 4);
        if(rv != 4) {
            if(rv < 0) {
                if(errno == EAGAIN || errno == EINTR)
                    return;
                else
                    log_err("plugin_extmon: pipe read() failed: %s", dmn_logf_strerror(errno));
            }
            else if(rv != 0) {
                log_err("plugin_extmon: BUG: short pipe read for mon results");
            }
            else {
                log_err("plugin_extmon: helper pipe closed, no more results");
            }
            total_helper_failure(loop);
            return;
        }

        const unsigned idx = emc_decode_mon_idx(data);
        const bool failed = emc_decode_mon_failed(data);
        if(idx >= num_mons)
            log_fatal("plugin_extmon: BUG: got helper result for out of range index %u", idx);
        mon_t* this_mon = &mons[idx];
        gdnsd_mon_state_updater(this_mon->idx, !failed); // wants true for success
        if(init_phase) {
            ev_timer_stop(loop, this_mon->local_timeout);
            if(!this_mon->seen_once) {
                this_mon->seen_once = true;
                if(++init_phase_count == num_mons) {
                    ev_io_stop(loop, w);
                    return;
                }
            }
        }
        else {
            bump_local_timeout(loop, this_mon);
        }
    }
}

// This fires if it's been way too long since helper
//   updated us about a given monitored resource
static void local_timeout_cb(struct ev_loop* loop, ev_timer* w, int revents V_UNUSED) {
    dmn_assert(loop); dmn_assert(w);
    dmn_assert(revents == EV_TIMER);

    mon_t* this_mon = w->data;
    dmn_assert(this_mon->local_timeout == w);

    log_info("plugin_extmon: '%s': helper is very late for a status update, locally applying a negative update...", this_mon->desc);
    gdnsd_mon_state_updater(this_mon->idx, false);
    if(!init_phase) {
        bump_local_timeout(loop, this_mon);
    }
    else {
        ev_timer_stop(loop, w);
        dmn_assert(!this_mon->seen_once);
        this_mon->seen_once = true;
        if(++init_phase_count == num_mons)
            ev_io_stop(loop, helper_read_watcher);
    }
}

static char* num_to_str(const int i) {
    char* out = malloc(64);
    snprintf(out, 64, "%i", i);
    return out;
}

static char* thing_xlate(const char* instr, const char* thing, const unsigned thing_len) {
    char outbuf[1024]; // way more than enough, I'd hope...
    char* out_cur = outbuf;
    while(*instr) {
        if(!strncmp(instr, "%%ITEM%%", 8)) {
            memcpy(out_cur, thing, thing_len);
            out_cur += thing_len;
            instr += 8;
        }
        else {
            *out_cur++ = *instr++;
        }
    }
    *out_cur = '\0';
    return strdup(outbuf);
}

static void send_cmd(const unsigned idx, const mon_t* mon) {
    char** this_args = malloc(mon->svc->num_args * sizeof(char*));

    const unsigned thing_len = strlen(mon->thing);

    for(unsigned i = 0; i < mon->svc->num_args; i++)
        this_args[i] = thing_xlate(mon->svc->args[i], mon->thing, thing_len);

    extmon_cmd_t this_cmd = {
        .idx = idx,
        .timeout = mon->svc->timeout,
        .interval = mon->svc->interval,
        .num_args = mon->svc->num_args,
        .args = (const char**)this_args,
        .desc = mon->desc,
    };

    if(emc_write_command(helper_write_fd, &this_cmd)
        || emc_read_exact(helper_read_fd, "CMD_ACK"))
        log_fatal("plugin_extmon: failed to write command for '%s' to helper!", mon->desc);

    for(unsigned i = 0; i < mon->svc->num_args; i++)
        free(this_args[i]);
    free(this_args);
}

static void spawn_helper(void) {
    int writepipe[2];
    int readpipe[2];
    if(pipe(writepipe))
        log_fatal("plugin_extmon: pipe() failed: %s", dmn_logf_strerror(errno));
    if(pipe(readpipe))
        log_fatal("plugin_extmon: pipe() failed: %s", dmn_logf_strerror(errno));

    helper_pid = fork();
    if(helper_pid == -1)
        log_fatal("plugin_extmon: fork() failed: %s", dmn_logf_strerror(errno));

    // unconditionally unset these for the helper child to ensure nothing can get
    //  screwy on systemd-based systems...
    unsetenv("WATCHDOG_PID");
    unsetenv("WATCHDOG_USEC");
    unsetenv("NOTIFY_SOCKET");

    if(!helper_pid) { // child
        close(writepipe[1]);
        close(readpipe[0]);
        const char* child_read_fdstr = num_to_str(writepipe[0]);
        const char* child_write_fdstr = num_to_str(readpipe[1]);
        execl(helper_path, helper_path, dmn_get_debug() ? "Y" : "N", dmn_get_foreground() ? "F" : "B",
            child_read_fdstr, child_write_fdstr, (const char*)NULL);
        log_fatal("plugin_extmon: execl(%s) failed: %s", helper_path, dmn_logf_strerror(errno));
    }

    // parent;
    dmn_assert(helper_pid);

    close(writepipe[0]);
    close(readpipe[1]);
    helper_write_fd = writepipe[1];
    helper_read_fd = readpipe[0];

    if(emc_write_string(helper_write_fd, "HELO", 4))
        log_fatal("plugin_extmon: failed to write HELO to helper process, helper died immediately?");
    if(emc_read_exact(helper_read_fd, "HELO_ACK"))
        log_fatal("plugin_extmon: failed to read HELO_ACK from helper process, helper died immediately?");

    char cmds_buf[7];
    memcpy(cmds_buf, "CMDS:", 5);
    cmds_buf[5] = num_mons >> 8;
    cmds_buf[6] = num_mons & 0xFF;
    if(emc_write_string(helper_write_fd, cmds_buf, 7))
        log_fatal("plugin_extmon: failed to write command count to helper process");
    if(emc_read_exact(helper_read_fd, "CMDS_ACK"))
        log_fatal("plugin_extmon: failed to read CMDS_ACK from helper process");

    for(unsigned i = 0; i < num_mons; i++)
         send_cmd(i, &mons[i]);

    if(emc_write_string(helper_write_fd, "END_CMDS", 8))
        log_fatal("plugin_extmon: failed to write END_CMDS to helper process");
    if(emc_read_exact(helper_read_fd, "END_CMDS_ACK"))
        log_fatal("plugin_extmon: failed to read END_CMDS_ACK from helper process");

    // done sending stuff, close writepipe and go nonblock on read side for eventloop
    close(helper_write_fd);
    if(unlikely(fcntl(helper_read_fd, F_SETFL, (fcntl(helper_read_fd, F_GETFL, 0)) | O_NONBLOCK) == -1))
        log_fatal("plugin_extmon: Failed to set O_NONBLOCK on pipe: %s", dmn_logf_errno());
}

static bool bad_opt(const char* key, unsigned klen V_UNUSED, const vscf_data_t* d V_UNUSED, void* data V_UNUSED) {
    log_fatal("plugin_extmon: bad global option '%s'", key);
}

void plugin_extmon_load_config(const vscf_data_t* config) {
    if(config) {
        const vscf_data_t* helper_path_cfg = vscf_hash_get_data_byconstkey(config, "helper_path", true);
        if(helper_path_cfg) {
            if(!vscf_is_simple(helper_path_cfg))
                log_fatal("plugin_extmon: config option 'helper_path' must be a simple string");
            helper_path = gdnsd_resolve_path_libexec(vscf_simple_get_data(helper_path_cfg), NULL);
        }
        const vscf_data_t* fail_cfg = vscf_hash_get_data_byconstkey(config, "helper_failure_action", true);
        if(fail_cfg) {
            if(!vscf_is_simple(fail_cfg))
                log_fatal("plugin_extmon: config option 'helper_failure_action' must be a simple string");
            const char* fail_str = vscf_simple_get_data(fail_cfg);
            if(!strcmp(fail_str, "stasis"))
                die_on_helper_failure = false;
            else if(!strcmp(fail_str, "kill_daemon"))
                die_on_helper_failure = true;
            else
                log_fatal("plugin_extmon: config option 'helper_failure_action' must be one of 'stasis' or 'kill_daemon' (you provided '%s')", fail_str);
        }
        vscf_hash_iterate(config, true, bad_opt, NULL);
    }
}

// plugins which don't have a global config stanza (e.g. plugins => { extmon => { ... } }),
//  which is common for monitoring-only, do not get a load_config() call.  So move this
//  final bit to full_config(), which is always called.
void plugin_extmon_full_config(unsigned num_threads V_UNUSED) {
    if(!helper_path)
        helper_path = gdnsd_resolve_path_libexec("gdnsd_extmon_helper", NULL);
}

void plugin_extmon_add_svctype(const char* name, const vscf_data_t* svc_cfg, const unsigned interval, const unsigned timeout) {
    dmn_assert(name); dmn_assert(svc_cfg);

    svcs = realloc(svcs, (num_svcs + 1) * sizeof(svc_t));
    svc_t* this_svc = &svcs[num_svcs++];
    this_svc->name = strdup(name);
    this_svc->timeout = timeout;
    this_svc->interval = interval;

    const vscf_data_t* args_cfg = vscf_hash_get_data_byconstkey(svc_cfg, "cmd", true);
    if(!args_cfg)
        log_fatal("plugin_extmon: service_type '%s': option 'cmd' must be defined!", name);
    this_svc->num_args = vscf_array_get_len(args_cfg);
    if(this_svc->num_args < 1)
        log_fatal("plugin_extmon: service_type '%s': option 'cmd' cannot be an empty array", name);
    this_svc->args = malloc(this_svc->num_args * sizeof(const char*));
    for(unsigned i = 0; i < this_svc->num_args; i++) {
        const vscf_data_t* arg_cfg = vscf_array_get_data(args_cfg, i);
        if(!vscf_is_simple(arg_cfg))
            log_fatal("plugin_extmon: service_type '%s': option 'cmd': all elements must be simple strings", name);
        this_svc->args[i] = strdup(vscf_simple_get_data(arg_cfg));
    }
}

static void add_mon_any(const char* desc, const char* svc_name, const char* thing, const unsigned idx) {
    dmn_assert(desc); dmn_assert(svc_name); dmn_assert(thing);

    mons = realloc(mons, (num_mons + 1) * sizeof(mon_t));
    mon_t* this_mon = &mons[num_mons++];
    this_mon->desc = strdup(desc);
    this_mon->idx = idx;

    this_mon->svc = NULL;
    for(unsigned i = 0; i < num_svcs; i++) {
        if(!strcmp(svcs[i].name, svc_name)) {
            this_mon->svc = &svcs[i];
            break;
        }
    }
    dmn_assert(this_mon->svc);

    this_mon->thing = strdup(thing);
    this_mon->local_timeout = NULL;
    this_mon->seen_once = false;
}

void plugin_extmon_add_mon_addr(const char* desc, const char* svc_name, const char* cname, const dmn_anysin_t* addr V_UNUSED, const unsigned idx) {
    dmn_assert(desc); dmn_assert(svc_name); dmn_assert(cname); dmn_assert(addr);
    add_mon_any(desc, svc_name, cname, idx);
}

void plugin_extmon_add_mon_cname(const char* desc, const char* svc_name, const char* cname, const unsigned idx) {
    dmn_assert(desc); dmn_assert(svc_name); dmn_assert(cname);
    add_mon_any(desc, svc_name, cname, idx);
}

void plugin_extmon_post_privdrop(void) {
    dmn_assert(helper_path);
    if(num_mons)
        spawn_helper();
}

void plugin_extmon_init_monitors(struct ev_loop* mon_loop) {
    if(num_mons) {
        helper_read_watcher = malloc(sizeof(ev_io));
        ev_io_init(helper_read_watcher, helper_read_cb, helper_read_fd, EV_READ);
        ev_set_priority(helper_read_watcher, 2);
        ev_io_start(mon_loop, helper_read_watcher);
        for(unsigned i = 0; i < num_mons; i++) {
            mon_t* this_mon = &mons[i];
            this_mon->local_timeout = malloc(sizeof(ev_timer));
            ev_timer_init(this_mon->local_timeout, local_timeout_cb, 0., 0.);
            this_mon->local_timeout->data = this_mon;
            ev_set_priority(this_mon->local_timeout, 0);
            bump_local_timeout(mon_loop, this_mon);
        }
    }
}

void plugin_extmon_start_monitors(struct ev_loop* mon_loop) {
    if(num_mons && !total_helper_failure_flag) {
        init_phase = false;
        ev_io_start(mon_loop, helper_read_watcher);
        for(unsigned i = 0; i < num_mons; i++)
            bump_local_timeout(mon_loop, &mons[i]);
    }
}
