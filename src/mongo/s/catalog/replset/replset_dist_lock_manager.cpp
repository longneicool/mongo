/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/replset/replset_dist_lock_manager.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/s/catalog/dist_lock_catalog.h"
#include "mongo/s/type_locks.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/timer.h"

namespace mongo {

    using std::string;
    using std::unique_ptr;
    using stdx::chrono::milliseconds;

    ReplSetDistLockManager::ReplSetDistLockManager(StringData processID,
                                                   unique_ptr<DistLockCatalog> catalog,
                                                   milliseconds pingInterval):
        _processID(processID.toString()),
        _catalog(std::move(catalog)),
        _pingInterval(pingInterval) {
    }

    ReplSetDistLockManager::~ReplSetDistLockManager() = default;

    void ReplSetDistLockManager::startUp() {
        _execThread = stdx::make_unique<stdx::thread>(&ReplSetDistLockManager::doTask, this);
    }

    void ReplSetDistLockManager::shutDown() {
        {
            stdx::lock_guard<stdx::mutex> sl(_mutex);
            _isShutDown = true;
            _shutDownCV.notify_all();
        }

        // Don't grab _mutex, otherwise will deadlock trying to join. Safe to read
        // _execThread since it is modified only at statrUp().
        if (_execThread && _execThread->joinable()) {
            _execThread->join();
            _execThread.reset();
        }

        auto status = _catalog->stopPing(_processID);
        if (!status.isOK()) {
            warning() << "error encountered while cleaning up distributed ping entry for "
                      << _processID << causedBy(status);
        }
    }

    bool ReplSetDistLockManager::isShutDown() {
        stdx::lock_guard<stdx::mutex> sl(_mutex);
        return _isShutDown;
    }

    void ReplSetDistLockManager::doTask() {
        while (!isShutDown()) {
            _catalog->ping(_processID, Date_t::now());

            std::deque<DistLockHandle> toUnlockBatch;
            {
                stdx::unique_lock<stdx::mutex> ul(_mutex);
                toUnlockBatch.swap(_unlockList);
            }

            for (const auto& toUnlock : toUnlockBatch) {
                auto unlockStatus = _catalog->unlock(toUnlock);

                if (!unlockStatus.isOK()) {
                    warning() << "Failed to unlock lock with " << LocksType::lockID()
                              << ": " << toUnlock << causedBy(unlockStatus);
                    queueUnlock(toUnlock);
                }

                if (isShutDown()) {
                    return;
                }
            }

            stdx::unique_lock<stdx::mutex> ul(_mutex);
            _shutDownCV.wait_for(ul, _pingInterval);
        }
    }

    StatusWith<DistLockManager::ScopedDistLock> ReplSetDistLockManager::lock(
            StringData name,
            StringData whyMessage,
            milliseconds waitFor,
            milliseconds lockTryInterval) {
        Timer timer;
        Timer msgTimer;

        while (waitFor <= milliseconds::zero() || milliseconds(timer.millis()) < waitFor) {
            OID lockSessionID = OID::gen();
            string who = str::stream() << _processID << ":" << getThreadName();
            auto lockResult = _catalog->grabLock(name,
                                                 lockSessionID,
                                                 who,
                                                 _processID,
                                                 Date_t::now(),
                                                 whyMessage);

            auto status = lockResult.getStatus();

            if (status.isOK()) {
                // Lock is acquired since findAndModify was able to successfully modify
                // the lock document.
                return ScopedDistLock(lockSessionID, this);
            }

            if (status != ErrorCodes::LockStateChangeFailed) {
                // An error occurred but the write might have actually been applied on the
                // other side. Schedule an unlock to clean it up just in case.
                queueUnlock(lockSessionID);
                return status;
            }

            // TODO: implement lock overtaking here.

            if (waitFor == milliseconds::zero()) {
                break;
            }

            // Periodically message for debugging reasons
            if (msgTimer.seconds() > 10) {
                LOG(0) << "waited " << timer.seconds() << "s for distributed lock " << name
                       << " for " << whyMessage;

                msgTimer.reset();
            }

            milliseconds timeRemaining =
                    std::max(milliseconds::zero(), waitFor - milliseconds(timer.millis()));
            sleepFor(std::min(lockTryInterval, timeRemaining));
        }

        return {ErrorCodes::LockBusy, str::stream() << "timed out waiting for " << name};
    }

    void ReplSetDistLockManager::unlock(const DistLockHandle& lockSessionID) {
        auto unlockStatus = _catalog->unlock(lockSessionID);

        if (!unlockStatus.isOK()) {
            queueUnlock(lockSessionID);
        }
    }

    Status ReplSetDistLockManager::checkStatus(const DistLockHandle& lockHandle) {
        auto lockStatus = _catalog->getLockByTS(lockHandle);

        if (!lockStatus.isOK()) {
            return lockStatus.getStatus();
        }

        auto lockDoc = lockStatus.getValue();
        if (!lockDoc.isValid(nullptr)) {
            return {ErrorCodes::LockNotFound, "lock owner changed"};
        }

        return Status::OK();
    }

    void ReplSetDistLockManager::queueUnlock(const DistLockHandle& lockSessionID) {
        stdx::unique_lock<stdx::mutex> ul(_mutex);
        _unlockList.push_back(lockSessionID);
    }
}
