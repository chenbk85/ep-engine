/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2014 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "config.h"
#include "ep_engine.h"
#include "connmap.h"
#include "dcp-backfill-manager.h"
#include "dcp-backfill.h"
#include "dcp-producer.h"

class BackfillManagerTask : public GlobalTask {
public:
    BackfillManagerTask(EventuallyPersistentEngine* e, connection_t c,
                        const Priority &p, double sleeptime = 1,
                        bool shutdown = false)
        : GlobalTask(e, p, sleeptime, shutdown), conn(c) {}

    bool run();

    std::string getDescription();

private:
    connection_t conn;

    static const size_t sleepTime;
};

const size_t BackfillManagerTask::sleepTime = 1;

bool BackfillManagerTask::run() {
    DcpProducer* producer = static_cast<DcpProducer*>(conn.get());
    if (producer->doDisconnect()) {
        return false;
    }

    backfill_status_t status = producer->getBackfillManager()->backfill();
    if (status == backfill_finished) {
        return false;
    } else if (status == backfill_snooze) {
        snooze(sleepTime);
    }

    if (engine->getEpStats().isShutdown) {
        return false;
    }

    return true;
}

std::string BackfillManagerTask::getDescription() {
    std::stringstream ss;
    ss << "Backfilling items for " << conn->getName();
    return ss.str();
}

BackfillManager::BackfillManager(EventuallyPersistentEngine* e, connection_t c)
    : engine(e), conn(c), taskId(0) {

    Configuration& config = e->getConfiguration();

    scanBuffer.bytesRead.store(0);
    scanBuffer.itemsRead.store(0);
    scanBuffer.maxBytes = config.getDcpScanByteLimit();
    scanBuffer.maxItems = config.getDcpScanItemLimit();
}

BackfillManager::~BackfillManager() {
    LockHolder lh(lock);
    ExecutorPool::get()->cancel(taskId);
    while (!backfills.empty()) {
        DCPBackfill* backfill = backfills.front();
        backfills.pop();
        backfill->cancel();
        delete backfill;
    }
}

void BackfillManager::schedule(stream_t stream, uint64_t start, uint64_t end) {
    LockHolder lh(lock);
    backfills.push(new DCPBackfill(engine, stream, start, end));

    if (taskId != 0) {
        ExecutorPool::get()->wake(taskId);
        return;
    }

    ExTask task = new BackfillManagerTask(engine, conn,
                                          Priority::BackfillTaskPriority);
    taskId = ExecutorPool::get()->schedule(task, NONIO_TASK_IDX);
}

bool BackfillManager::bytesRead(uint32_t bytes) {
    if (scanBuffer.itemsRead.load() >= scanBuffer.maxItems) {
        return false;
    }

    // Always allow an item to be backfilled if the scan buffer is empty,
    // otherwise check to see if there is room for the item.
    uint32_t oldVal = 0;
    if (!scanBuffer.bytesRead.compare_exchange_strong(oldVal, bytes)) {
        if (!addIfLessThanMax(scanBuffer.bytesRead, bytes, scanBuffer.maxBytes)) {
            return false;
        }
    }

    scanBuffer.itemsRead.fetch_add(1);
    return true;
}

backfill_status_t BackfillManager::backfill() {
    if (engine->getEpStore()->isMemoryUsageTooHigh()) {
        LOG(EXTENSION_LOG_INFO, "DCP backfilling task for connection %s "
            "temporarily suspended because the current memory usage is too "
            "high", conn->getName().c_str());
        return backfill_snooze;
    }

    LockHolder lh(lock);
    DCPBackfill* backfill = backfills.front();
    lh.unlock();

    backfill_status_t status = backfill->run();

    scanBuffer.bytesRead.store(0);
    scanBuffer.itemsRead.store(0);

    lh.lock();
    backfills.pop();
    if (status == backfill_success) {
        backfills.push(backfill);
    } else if (status == backfill_finished) {
        delete backfill;
        if (backfills.empty()) {
            taskId = 0;
            return backfill_finished;
        }
    } else if (status == backfill_snooze) {
        backfills.push(backfill);
        return backfill_snooze;
    } else {
        abort();
    }

    return backfill_success;
}

bool BackfillManager::addIfLessThanMax(AtomicValue<uint32_t>& val,
                                       uint32_t incr, uint32_t max) {
    do {
        uint32_t oldVal = val.load();
        uint32_t newVal = oldVal + incr;

        if (newVal > max) {
            return false;
        }

        if (val.compare_exchange_strong(oldVal, newVal)) {
            return true;
        }
    } while (true);
}