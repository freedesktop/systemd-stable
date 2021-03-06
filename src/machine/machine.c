/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2011 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <systemd/sd-messages.h>

#include "util.h"
#include "mkdir.h"
#include "hashmap.h"
#include "strv.h"
#include "fileio.h"
#include "special.h"
#include "unit-name.h"
#include "dbus-common.h"
#include "machine.h"

Machine* machine_new(Manager *manager, const char *name) {
        Machine *m;

        assert(manager);
        assert(name);

        m = new0(Machine, 1);
        if (!m)
                return NULL;

        m->name = strdup(name);
        if (!m->name)
                goto fail;

        m->state_file = strappend("/run/systemd/machines/", m->name);
        if (!m->state_file)
                goto fail;

        if (hashmap_put(manager->machines, m->name, m) < 0)
                goto fail;

        m->class = _MACHINE_CLASS_INVALID;
        m->manager = manager;

        return m;

fail:
        free(m->state_file);
        free(m->name);
        free(m);

        return NULL;
}

void machine_free(Machine *m) {
        assert(m);

        if (m->in_gc_queue)
                LIST_REMOVE(Machine, gc_queue, m->manager->machine_gc_queue, m);

        if (m->scope) {
                hashmap_remove(m->manager->machine_units, m->scope);
                free(m->scope);
        }

        free(m->scope_job);

        hashmap_remove(m->manager->machines, m->name);

        if (m->create_message)
                dbus_message_unref(m->create_message);

        free(m->name);
        free(m->state_file);
        free(m->service);
        free(m->root_directory);
        free(m);
}

int machine_save(Machine *m) {
        _cleanup_free_ char *temp_path = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        assert(m);
        assert(m->state_file);

        if (!m->started)
                return 0;

        r = mkdir_safe_label("/run/systemd/machines", 0755, 0, 0);
        if (r < 0)
                goto finish;

        r = fopen_temporary(m->state_file, &f, &temp_path);
        if (r < 0)
                goto finish;

        fchmod(fileno(f), 0644);

        fprintf(f,
                "# This is private data. Do not parse.\n"
                "NAME=%s\n",
                m->name);

        if (m->scope) {
                _cleanup_free_ char *escaped;

                escaped = cescape(m->scope);
                if (!escaped) {
                        r = -ENOMEM;
                        goto finish;
                }

                fprintf(f, "SCOPE=%s\n", escaped);
        }

        if (m->scope_job)
                fprintf(f, "SCOPE_JOB=%s\n", m->scope_job);

        if (m->service) {
                _cleanup_free_ char *escaped;

                escaped = cescape(m->service);
                if (!escaped) {
                        r = -ENOMEM;
                        goto finish;
                }
                fprintf(f, "SERVICE=%s\n", escaped);
        }

        if (m->root_directory) {
                _cleanup_free_ char *escaped;

                escaped = cescape(m->root_directory);
                if (!escaped) {
                        r = -ENOMEM;
                        goto finish;
                }
                fprintf(f, "ROOT=%s\n", escaped);
        }

        if (!sd_id128_equal(m->id, SD_ID128_NULL))
                fprintf(f, "ID=" SD_ID128_FORMAT_STR "\n", SD_ID128_FORMAT_VAL(m->id));

        if (m->leader != 0)
                fprintf(f, "LEADER=%lu\n", (unsigned long) m->leader);

        if (m->class != _MACHINE_CLASS_INVALID)
                fprintf(f, "CLASS=%s\n", machine_class_to_string(m->class));

        if (dual_timestamp_is_set(&m->timestamp))
                fprintf(f,
                        "REALTIME=%llu\n"
                        "MONOTONIC=%llu\n",
                        (unsigned long long) m->timestamp.realtime,
                        (unsigned long long) m->timestamp.monotonic);

        fflush(f);

        if (ferror(f) || rename(temp_path, m->state_file) < 0) {
                r = -errno;
                unlink(m->state_file);
                unlink(temp_path);
        }

finish:
        if (r < 0)
                log_error("Failed to save machine data for %s: %s", m->name, strerror(-r));

        return r;
}

int machine_load(Machine *m) {
        _cleanup_free_ char *realtime = NULL, *monotonic = NULL, *id = NULL, *leader = NULL, *class = NULL;
        int r;

        assert(m);

        r = parse_env_file(m->state_file, NEWLINE,
                           "SCOPE",     &m->scope,
                           "SCOPE_JOB", &m->scope_job,
                           "SERVICE",   &m->service,
                           "ROOT",      &m->root_directory,
                           "ID",        &id,
                           "LEADER",    &leader,
                           "CLASS",     &class,
                           "REALTIME",  &realtime,
                           "MONOTONIC", &monotonic,
                           NULL);
        if (r < 0) {
                if (r == -ENOENT)
                        return 0;

                log_error("Failed to read %s: %s", m->state_file, strerror(-r));
                return r;
        }

        if (id)
                sd_id128_from_string(id, &m->id);

        if (leader)
                parse_pid(leader, &m->leader);

        if (class) {
                MachineClass c;

                c = machine_class_from_string(class);
                if (c >= 0)
                        m->class = c;
        }

        if (realtime) {
                unsigned long long l;
                if (sscanf(realtime, "%llu", &l) > 0)
                        m->timestamp.realtime = l;
        }

        if (monotonic) {
                unsigned long long l;
                if (sscanf(monotonic, "%llu", &l) > 0)
                        m->timestamp.monotonic = l;
        }

        return r;
}

static int machine_start_scope(Machine *m, DBusMessageIter *iter) {
        _cleanup_free_ char *description = NULL;
        DBusError error;
        char *job;
        int r = 0;

        assert(m);

        dbus_error_init(&error);

        if (!m->scope) {
                _cleanup_free_ char *escaped = NULL;
                char *scope;

                escaped = unit_name_escape(m->name);
                if (!escaped)
                        return log_oom();

                scope = strjoin("machine-", escaped, ".scope", NULL);
                if (!scope)
                        return log_oom();

                description = strappend(m->class == MACHINE_VM ? "Virtual Machine " : "Container ", m->name);

                r = manager_start_scope(m->manager, scope, m->leader, SPECIAL_MACHINE_SLICE, description, iter, &error, &job);
                if (r < 0) {
                        log_error("Failed to start machine scope: %s", bus_error(&error, r));
                        dbus_error_free(&error);

                        free(scope);
                        return r;
                } else {
                        m->scope = scope;

                        free(m->scope_job);
                        m->scope_job = job;
                }
        }

        if (m->scope)
                hashmap_put(m->manager->machine_units, m->scope, m);

        return r;
}

int machine_start(Machine *m, DBusMessageIter *iter) {
        int r;

        assert(m);

        if (m->started)
                return 0;

        /* Create cgroup */
        r = machine_start_scope(m, iter);
        if (r < 0)
                return r;

        log_struct(LOG_INFO,
                   MESSAGE_ID(SD_MESSAGE_MACHINE_START),
                   "NAME=%s", m->name,
                   "LEADER=%lu", (unsigned long) m->leader,
                   "MESSAGE=New machine %s.", m->name,
                   NULL);

        if (!dual_timestamp_is_set(&m->timestamp))
                dual_timestamp_get(&m->timestamp);

        m->started = true;

        /* Save new machine data */
        machine_save(m);

        machine_send_signal(m, true);

        return 0;
}

static int machine_stop_scope(Machine *m) {
        DBusError error;
        char *job;
        int r;

        assert(m);

        dbus_error_init(&error);

        if (!m->scope)
                return 0;

        r = manager_stop_unit(m->manager, m->scope, &error, &job);
        if (r < 0) {
                log_error("Failed to stop machine scope: %s", bus_error(&error, r));
                dbus_error_free(&error);
                return r;
        }

        free(m->scope_job);
        m->scope_job = job;

        return 0;
}

int machine_stop(Machine *m) {
        int r = 0, k;
        assert(m);

        if (m->started)
                log_struct(LOG_INFO,
                           MESSAGE_ID(SD_MESSAGE_MACHINE_STOP),
                           "NAME=%s", m->name,
                           "LEADER=%lu", (unsigned long) m->leader,
                           "MESSAGE=Machine %s terminated.", m->name,
                           NULL);

        /* Kill cgroup */
        k = machine_stop_scope(m);
        if (k < 0)
                r = k;

        unlink(m->state_file);
        machine_add_to_gc_queue(m);

        if (m->started)
                machine_send_signal(m, false);

        m->started = false;

        return r;
}

int machine_check_gc(Machine *m, bool drop_not_started) {
        assert(m);

        if (drop_not_started && !m->started)
                return 0;

        if (m->scope_job)
                return 1;

        if (m->scope)
                return manager_unit_is_active(m->manager, m->scope) != 0;

        return 0;
}

void machine_add_to_gc_queue(Machine *m) {
        assert(m);

        if (m->in_gc_queue)
                return;

        LIST_PREPEND(Machine, gc_queue, m->manager->machine_gc_queue, m);
        m->in_gc_queue = true;
}

MachineState machine_get_state(Machine *s) {
        assert(s);

        if (s->scope_job)
                return s->started ? MACHINE_OPENING : MACHINE_CLOSING;

        return MACHINE_RUNNING;
}

int machine_kill(Machine *m, KillWho who, int signo) {
        assert(m);

        if (!m->scope)
                return -ESRCH;

        return manager_kill_unit(m->manager, m->scope, who, signo, NULL);
}

static const char* const machine_class_table[_MACHINE_CLASS_MAX] = {
        [MACHINE_CONTAINER] = "container",
        [MACHINE_VM] = "vm"
};

DEFINE_STRING_TABLE_LOOKUP(machine_class, MachineClass);

static const char* const machine_state_table[_MACHINE_STATE_MAX] = {
        [MACHINE_OPENING] = "opening",
        [MACHINE_RUNNING] = "running",
        [MACHINE_CLOSING] = "closing"
};

DEFINE_STRING_TABLE_LOOKUP(machine_state, MachineState);

static const char* const kill_who_table[_KILL_WHO_MAX] = {
        [KILL_LEADER] = "leader",
        [KILL_ALL] = "all"
};

DEFINE_STRING_TABLE_LOOKUP(kill_who, KillWho);
