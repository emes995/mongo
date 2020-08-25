/**
 *    Copyright (C) 2019-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/throttle_cursor.h"

#include "mongo/db/catalog/validate_gen.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/operation_context.h"

namespace mongo {

// Used to change the 'dataSize' passed into DataThrottle::awaitIfNeeded() to be a fixed size of
// 512KB.
MONGO_FAIL_POINT_DEFINE(fixedCursorDataSizeOf512KBForDataThrottle);
MONGO_FAIL_POINT_DEFINE(fixedCursorDataSizeOf2MBForDataThrottle);

SeekableRecordThrottleCursor::SeekableRecordThrottleCursor(OperationContext* opCtx,
                                                           const RecordStore* rs,
                                                           DataThrottle* dataThrottle) {
    _cursor = rs->getCursor(opCtx, /*forward=*/true);
    _dataThrottle = dataThrottle;
}

boost::optional<Record> SeekableRecordThrottleCursor::seekExact(OperationContext* opCtx,
                                                                const RecordId& id) {
    boost::optional<Record> record = _cursor->seekExact(id);
    if (record) {
        const int64_t dataSize = record->data.size() + sizeof(record->id.repr());
        _dataThrottle->awaitIfNeeded(opCtx, dataSize);
    }

    return record;
}

boost::optional<Record> SeekableRecordThrottleCursor::next(OperationContext* opCtx) {
    boost::optional<Record> record = _cursor->next();
    if (record) {
        const int64_t dataSize = record->data.size() + sizeof(record->id.repr());
        _dataThrottle->awaitIfNeeded(opCtx, dataSize);
    }

    return record;
}

SortedDataInterfaceThrottleCursor::SortedDataInterfaceThrottleCursor(OperationContext* opCtx,
                                                                     const IndexAccessMethod* iam,
                                                                     DataThrottle* dataThrottle) {
    _cursor = iam->newCursor(opCtx, /*forward=*/true);
    _dataThrottle = dataThrottle;
}

boost::optional<IndexKeyEntry> SortedDataInterfaceThrottleCursor::seek(
    OperationContext* opCtx, const KeyString::Value& key) {
    boost::optional<IndexKeyEntry> entry = _cursor->seek(key);
    if (entry) {
        const int64_t dataSize = entry->key.objsize() + sizeof(entry->loc.repr());
        _dataThrottle->awaitIfNeeded(opCtx, dataSize);
    }

    return entry;
}

boost::optional<KeyStringEntry> SortedDataInterfaceThrottleCursor::seekForKeyString(
    OperationContext* opCtx, const KeyString::Value& key) {
    boost::optional<KeyStringEntry> entry = _cursor->seekForKeyString(key);
    if (entry) {
        const int64_t dataSize = entry->keyString.getSize() + sizeof(entry->loc.repr());
        _dataThrottle->awaitIfNeeded(opCtx, dataSize);
    }

    return entry;
}

boost::optional<IndexKeyEntry> SortedDataInterfaceThrottleCursor::next(OperationContext* opCtx) {
    boost::optional<IndexKeyEntry> entry = _cursor->next();
    if (entry) {
        const int64_t dataSize = entry->key.objsize() + sizeof(entry->loc.repr());
        _dataThrottle->awaitIfNeeded(opCtx, dataSize);
    }

    return entry;
}

boost::optional<KeyStringEntry> SortedDataInterfaceThrottleCursor::nextKeyString(
    OperationContext* opCtx) {
    boost::optional<KeyStringEntry> entry = _cursor->nextKeyString();
    if (entry) {
        const int64_t dataSize = entry->keyString.getSize() + sizeof(entry->loc.repr());
        _dataThrottle->awaitIfNeeded(opCtx, dataSize);
    }

    return entry;
}

void DataThrottle::awaitIfNeeded(OperationContext* opCtx, const int64_t dataSize) {
    int64_t currentMillis =
        opCtx->getServiceContext()->getFastClockSource()->now().toMillisSinceEpoch();

    // Reset the tracked information as the second has rolled over the starting point.
    if (currentMillis >= _startMillis + 1000) {
        float elapsedTimeSec = static_cast<float>(currentMillis - _startMillis) / 1000;
        float mbProcessed = static_cast<float>(_bytesProcessed + dataSize) / 1024 / 1024;

        // Update how much data we've seen in the last second for CurOp.
        CurOp::get(opCtx)->debug().dataThroughputLastSecond = mbProcessed / elapsedTimeSec;

        _totalMBProcessed += mbProcessed;
        _totalElapsedTimeSec += elapsedTimeSec;

        // Update how much data we've seen throughout the lifetime of the DataThrottle for CurOp.
        CurOp::get(opCtx)->debug().dataThroughputAverage = _totalMBProcessed / _totalElapsedTimeSec;

        _startMillis = currentMillis;
        _bytesProcessed = 0;
    }

    if (MONGO_unlikely(fixedCursorDataSizeOf512KBForDataThrottle.shouldFail())) {
        _bytesProcessed += /* 512KB */ 1024 * 512;
    } else if (MONGO_unlikely(fixedCursorDataSizeOf2MBForDataThrottle.shouldFail())) {
        _bytesProcessed += /* 2MB */ 2 * 1024 * 1024;
    } else {
        _bytesProcessed += dataSize;
    }

    if (_shouldNotThrottle) {
        return;
    }

    // No throttling should take place if 'gMaxValidateMBperSec' is zero.
    uint64_t maxValidateBytesPerSec = gMaxValidateMBperSec.load() * 1024 * 1024;
    if (maxValidateBytesPerSec == 0) {
        return;
    }

    if (_bytesProcessed < maxValidateBytesPerSec) {
        return;
    }

    // Wait a period of time proportional to how much extra data we have read. For example, if we
    // read one 5 MB document and maxValidateBytesPerSec is 1, we should not be waiting until the
    // next 1 second period. We should wait 5 seconds to maintain proper throughput.
    int64_t maxWaitMs = 1000 * std::max(1.0, double(_bytesProcessed) / maxValidateBytesPerSec);

    do {
        int64_t millisToSleep = maxWaitMs - (currentMillis - _startMillis);
        invariant(millisToSleep >= 0);

        opCtx->sleepFor(Milliseconds(millisToSleep));
        currentMillis =
            opCtx->getServiceContext()->getFastClockSource()->now().toMillisSinceEpoch();
    } while (currentMillis < _startMillis + maxWaitMs);
}

}  // namespace mongo
