/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "storaged"

#include <stdint.h>
#include <time.h>

#include <string>
#include <unordered_map>

#include <android/content/pm/IPackageManagerNative.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/macros.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>
#include <android-base/stringprintf.h>
#include <binder/IServiceManager.h>
#include <log/log_event_list.h>

#include "storaged.h"
#include "storaged_uid_monitor.h"

using namespace android;
using namespace android::base;
using namespace android::content::pm;

static bool refresh_uid_names;

std::unordered_map<uint32_t, struct uid_info> uid_monitor::get_uid_io_stats()
{
    std::unique_ptr<lock_t> lock(new lock_t(&um_lock));
    return get_uid_io_stats_locked();
};

/* return true on parse success and false on failure */
bool uid_info::parse_uid_io_stats(std::string&& s)
{
    std::vector<std::string> fields = Split(s, " ");
    if (fields.size() < 11 ||
        !ParseUint(fields[0],  &uid) ||
        !ParseUint(fields[1],  &io[FOREGROUND].rchar) ||
        !ParseUint(fields[2],  &io[FOREGROUND].wchar) ||
        !ParseUint(fields[3],  &io[FOREGROUND].read_bytes) ||
        !ParseUint(fields[4],  &io[FOREGROUND].write_bytes) ||
        !ParseUint(fields[5],  &io[BACKGROUND].rchar) ||
        !ParseUint(fields[6],  &io[BACKGROUND].wchar) ||
        !ParseUint(fields[7],  &io[BACKGROUND].read_bytes) ||
        !ParseUint(fields[8],  &io[BACKGROUND].write_bytes) ||
        !ParseUint(fields[9],  &io[FOREGROUND].fsync) ||
        !ParseUint(fields[10], &io[BACKGROUND].fsync)) {
        LOG_TO(SYSTEM, WARNING) << "Invalid I/O stats: \""
                                << s << "\"";
        return false;
    }
    return true;
}

/* return true on parse success and false on failure */
bool task_info::parse_task_io_stats(std::string&& s)
{
    std::vector<std::string> fields = Split(s, ",");
    if (fields.size() < 13 ||
        !ParseInt(fields[2],  &pid) ||
        !ParseUint(fields[3],  &io[FOREGROUND].rchar) ||
        !ParseUint(fields[4],  &io[FOREGROUND].wchar) ||
        !ParseUint(fields[5],  &io[FOREGROUND].read_bytes) ||
        !ParseUint(fields[6],  &io[FOREGROUND].write_bytes) ||
        !ParseUint(fields[7],  &io[BACKGROUND].rchar) ||
        !ParseUint(fields[8],  &io[BACKGROUND].wchar) ||
        !ParseUint(fields[9],  &io[BACKGROUND].read_bytes) ||
        !ParseUint(fields[10], &io[BACKGROUND].write_bytes) ||
        !ParseUint(fields[11], &io[FOREGROUND].fsync) ||
        !ParseUint(fields[12], &io[BACKGROUND].fsync)) {
        LOG_TO(SYSTEM, WARNING) << "Invalid I/O stats: \""
                                << s << "\"";
        return false;
    }
    comm = fields[1];
    return true;
}

bool io_usage::is_zero() const
{
    for (int i = 0; i < IO_TYPES; i++) {
        for (int j = 0; j < UID_STATS; j++) {
            for (int k = 0; k < CHARGER_STATS; k++) {
                if (bytes[i][j][k])
                    return false;
            }
        }
    }
    return true;
}

static void get_uid_names(const vector<int>& uids, const vector<std::string*>& uid_names)
{
    sp<IServiceManager> sm = defaultServiceManager();
    if (sm == NULL) {
        LOG_TO(SYSTEM, ERROR) << "defaultServiceManager failed";
        return;
    }

    sp<IBinder> binder = sm->getService(String16("package_native"));
    if (binder == NULL) {
        LOG_TO(SYSTEM, ERROR) << "getService package_native failed";
        return;
    }

    sp<IPackageManagerNative> package_mgr = interface_cast<IPackageManagerNative>(binder);
    std::vector<std::string> names;
    binder::Status status = package_mgr->getNamesForUids(uids, &names);
    if (!status.isOk()) {
        LOG_TO(SYSTEM, ERROR) << "package_native::getNamesForUids failed: "
                              << status.exceptionMessage();
        return;
    }

    for (uint32_t i = 0; i < uid_names.size(); i++) {
        if (!names[i].empty()) {
            *uid_names[i] = names[i];
        }
    }

    refresh_uid_names = false;
}

std::unordered_map<uint32_t, struct uid_info> uid_monitor::get_uid_io_stats_locked()
{
    std::unordered_map<uint32_t, struct uid_info> uid_io_stats;
    std::string buffer;
    if (!ReadFileToString(UID_IO_STATS_PATH, &buffer)) {
        PLOG_TO(SYSTEM, ERROR) << UID_IO_STATS_PATH << ": ReadFileToString failed";
        return uid_io_stats;
    }

    std::vector<std::string> io_stats = Split(std::move(buffer), "\n");
    struct uid_info u;
    vector<int> uids;
    vector<std::string*> uid_names;

    for (uint32_t i = 0; i < io_stats.size(); i++) {
        if (io_stats[i].empty()) {
            continue;
        }

        if (io_stats[i].compare(0, 4, "task")) {
            if (!u.parse_uid_io_stats(std::move(io_stats[i])))
                continue;
            uid_io_stats[u.uid] = u;
            uid_io_stats[u.uid].name = std::to_string(u.uid);
            uids.push_back(u.uid);
            uid_names.push_back(&uid_io_stats[u.uid].name);
            if (last_uid_io_stats.find(u.uid) == last_uid_io_stats.end()) {
                refresh_uid_names = true;
            } else {
                uid_io_stats[u.uid].name = last_uid_io_stats[u.uid].name;
            }
        } else {
            struct task_info t;
            if (!t.parse_task_io_stats(std::move(io_stats[i])))
                continue;
            uid_io_stats[u.uid].tasks[t.pid] = t;
        }
    }

    if (!uids.empty() && refresh_uid_names) {
        get_uid_names(uids, uid_names);
    }

    return uid_io_stats;
}

static const int MAX_UID_RECORDS_SIZE = 1000 * 48; // 1000 uids in 48 hours

static inline int records_size(
    const std::map<uint64_t, struct uid_records>& curr_records)
{
    int count = 0;
    for (auto const& it : curr_records) {
        count += it.second.entries.size();
    }
    return count;
}

void uid_monitor::add_records_locked(uint64_t curr_ts)
{
    // remove records more than 5 days old
    if (curr_ts > 5 * DAY_TO_SEC) {
        auto it = records.lower_bound(curr_ts - 5 * DAY_TO_SEC);
        records.erase(records.begin(), it);
    }

    struct uid_records new_records;
    for (const auto& p : curr_io_stats) {
        struct uid_record record = {};
        record.name = p.first;
        if (!p.second.uid_ios.is_zero()) {
            record.ios.uid_ios = p.second.uid_ios;
            for (const auto& p_task : p.second.task_ios) {
                if (!p_task.second.is_zero())
                    record.ios.task_ios[p_task.first] = p_task.second;
            }
            new_records.entries.push_back(record);
        }
    }

    curr_io_stats.clear();
    new_records.start_ts = start_ts;
    start_ts = curr_ts;

    if (new_records.entries.empty())
      return;

    // make some room for new records
    int overflow = records_size(records) +
        new_records.entries.size() - MAX_UID_RECORDS_SIZE;
    while (overflow > 0 && records.size() > 0) {
        auto del_it = records.begin();
        overflow -= del_it->second.entries.size();
        records.erase(records.begin());
    }

    records[curr_ts] = new_records;
}

std::map<uint64_t, struct uid_records> uid_monitor::dump(
    double hours, uint64_t threshold, bool force_report)
{
    if (force_report) {
        report();
    }

    std::unique_ptr<lock_t> lock(new lock_t(&um_lock));

    std::map<uint64_t, struct uid_records> dump_records;
    uint64_t first_ts = 0;

    if (hours != 0) {
        first_ts = time(NULL) - hours * HOUR_TO_SEC;
    }

    for (auto it = records.lower_bound(first_ts); it != records.end(); ++it) {
        const std::vector<struct uid_record>& recs = it->second.entries;
        struct uid_records filtered;

        for (const auto& rec : recs) {
            const io_usage& uid_usage = rec.ios.uid_ios;
            if (uid_usage.bytes[READ][FOREGROUND][CHARGER_ON] +
                uid_usage.bytes[READ][FOREGROUND][CHARGER_OFF] +
                uid_usage.bytes[READ][BACKGROUND][CHARGER_ON] +
                uid_usage.bytes[READ][BACKGROUND][CHARGER_OFF] +
                uid_usage.bytes[WRITE][FOREGROUND][CHARGER_ON] +
                uid_usage.bytes[WRITE][FOREGROUND][CHARGER_OFF] +
                uid_usage.bytes[WRITE][BACKGROUND][CHARGER_ON] +
                uid_usage.bytes[WRITE][BACKGROUND][CHARGER_OFF] > threshold) {
                filtered.entries.push_back(rec);
            }
        }

        if (filtered.entries.empty())
            continue;

        filtered.start_ts = it->second.start_ts;
        dump_records.insert(
            std::pair<uint64_t, struct uid_records>(it->first, filtered));
    }

    return dump_records;
}

void uid_monitor::update_curr_io_stats_locked()
{
    std::unordered_map<uint32_t, struct uid_info> uid_io_stats =
        get_uid_io_stats_locked();
    if (uid_io_stats.empty()) {
        return;
    }

    for (const auto& it : uid_io_stats) {
        const struct uid_info& uid = it.second;

        if (curr_io_stats.find(uid.name) == curr_io_stats.end()) {
          curr_io_stats[uid.name] = {};
        }

        struct uid_io_usage& usage = curr_io_stats[uid.name];
        int64_t fg_rd_delta = uid.io[FOREGROUND].read_bytes -
            last_uid_io_stats[uid.uid].io[FOREGROUND].read_bytes;
        int64_t bg_rd_delta = uid.io[BACKGROUND].read_bytes -
            last_uid_io_stats[uid.uid].io[BACKGROUND].read_bytes;
        int64_t fg_wr_delta = uid.io[FOREGROUND].write_bytes -
            last_uid_io_stats[uid.uid].io[FOREGROUND].write_bytes;
        int64_t bg_wr_delta = uid.io[BACKGROUND].write_bytes -
            last_uid_io_stats[uid.uid].io[BACKGROUND].write_bytes;

        usage.uid_ios.bytes[READ][FOREGROUND][charger_stat] +=
            (fg_rd_delta < 0) ? 0 : fg_rd_delta;
        usage.uid_ios.bytes[READ][BACKGROUND][charger_stat] +=
            (bg_rd_delta < 0) ? 0 : bg_rd_delta;
        usage.uid_ios.bytes[WRITE][FOREGROUND][charger_stat] +=
            (fg_wr_delta < 0) ? 0 : fg_wr_delta;
        usage.uid_ios.bytes[WRITE][BACKGROUND][charger_stat] +=
            (bg_wr_delta < 0) ? 0 : bg_wr_delta;

        for (const auto& task_it : uid.tasks) {
            const struct task_info& task = task_it.second;
            const pid_t pid = task_it.first;
            const std::string& comm = task_it.second.comm;
            int64_t task_fg_rd_delta = task.io[FOREGROUND].read_bytes -
                last_uid_io_stats[uid.uid].tasks[pid].io[FOREGROUND].read_bytes;
            int64_t task_bg_rd_delta = task.io[BACKGROUND].read_bytes -
                last_uid_io_stats[uid.uid].tasks[pid].io[BACKGROUND].read_bytes;
            int64_t task_fg_wr_delta = task.io[FOREGROUND].write_bytes -
                last_uid_io_stats[uid.uid].tasks[pid].io[FOREGROUND].write_bytes;
            int64_t task_bg_wr_delta = task.io[BACKGROUND].write_bytes -
                last_uid_io_stats[uid.uid].tasks[pid].io[BACKGROUND].write_bytes;

            struct io_usage& task_usage = usage.task_ios[comm];
            task_usage.bytes[READ][FOREGROUND][charger_stat] +=
                (task_fg_rd_delta < 0) ? 0 : task_fg_rd_delta;
            task_usage.bytes[READ][BACKGROUND][charger_stat] +=
                (task_bg_rd_delta < 0) ? 0 : task_bg_rd_delta;
            task_usage.bytes[WRITE][FOREGROUND][charger_stat] +=
                (task_fg_wr_delta < 0) ? 0 : task_fg_wr_delta;
            task_usage.bytes[WRITE][BACKGROUND][charger_stat] +=
                (task_bg_wr_delta < 0) ? 0 : task_bg_wr_delta;
        }
    }

    last_uid_io_stats = uid_io_stats;
}

void uid_monitor::report()
{
    std::unique_ptr<lock_t> lock(new lock_t(&um_lock));

    update_curr_io_stats_locked();
    add_records_locked(time(NULL));
}

void uid_monitor::set_charger_state(charger_stat_t stat)
{
    std::unique_ptr<lock_t> lock(new lock_t(&um_lock));

    if (charger_stat == stat) {
        return;
    }

    update_curr_io_stats_locked();
    charger_stat = stat;
}

void uid_monitor::init(charger_stat_t stat)
{
    charger_stat = stat;
    start_ts = time(NULL);
    last_uid_io_stats = get_uid_io_stats();
}

uid_monitor::uid_monitor()
{
    sem_init(&um_lock, 0, 1);
}

uid_monitor::~uid_monitor()
{
    sem_destroy(&um_lock);
}
