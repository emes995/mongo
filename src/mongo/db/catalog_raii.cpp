/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/catalog_raii.h"

#include "mongo/db/catalog/catalog_helper.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_uuid_mismatch.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/repl/collection_utils.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(setAutoGetCollectionWait);
MONGO_FAIL_POINT_DEFINE(hangBeforeAutoGetCollectionLockFreeShardedStateAccess);

/**
 * Performs some sanity checks on the collection and database.
 */
void verifyDbAndCollection(OperationContext* opCtx,
                           LockMode modeColl,
                           const NamespaceStringOrUUID& nsOrUUID,
                           const NamespaceString& resolvedNss,
                           CollectionPtr& coll,
                           Database* db,
                           bool verifyWriteEligible) {
    invariant(!nsOrUUID.uuid() || coll,
              str::stream() << "Collection for " << resolvedNss.ns()
                            << " disappeared after successfully resolving " << nsOrUUID.toString());

    invariant(!nsOrUUID.uuid() || db,
              str::stream() << "Database for " << resolvedNss.ns()
                            << " disappeared after successfully resolving " << nsOrUUID.toString());

    // In most cases we expect modifications for system.views to upgrade MODE_IX to MODE_X before
    // taking the lock. One exception is a query by UUID of system.views in a transaction. Usual
    // queries of system.views (by name, not UUID) within a transaction are rejected. However, if
    // the query is by UUID we can't determine whether the namespace is actually system.views until
    // we take the lock here. So we have this one last assertion.
    uassert(51070,
            "Modifications to system.views must take an exclusive lock",
            !resolvedNss.isSystemDotViews() || modeColl != MODE_IX);

    if (!db || !coll) {
        return;
    }

    // Verify that we are using the latest instance if we intend to perform writes.
    if (feature_flags::gPointInTimeCatalogLookups.isEnabledAndIgnoreFCV() && verifyWriteEligible) {
        auto latest = CollectionCatalog::latest(opCtx);
        if (!latest->containsCollection(opCtx, coll)) {
            throwWriteConflictException(str::stream()
                                        << "Unable to write to collection '" << coll->ns()
                                        << "' due to catalog changes; please "
                                           "retry the operation");
        }
        if (opCtx->recoveryUnit()->isActive()) {
            const auto mySnapshot = opCtx->recoveryUnit()->getPointInTimeReadTimestamp(opCtx);
            if (mySnapshot && *mySnapshot < coll->getMinimumValidSnapshot()) {
                throwWriteConflictException(str::stream()
                                            << "Unable to write to collection '" << coll->ns()
                                            << "' due to snapshot timestamp " << *mySnapshot
                                            << " being older than collection minimum "
                                            << *coll->getMinimumValidSnapshot()
                                            << "; please retry the operation");
            }
        }
    }

    // If we are in a transaction, we cannot yield and wait when there are pending catalog
    // changes. Instead, we must return an error in such situations. We ignore this restriction
    // for the oplog, since it never has pending catalog changes.
    if (opCtx->inMultiDocumentTransaction() && resolvedNss != NamespaceString::kRsOplogNamespace) {
        if (auto minSnapshot = coll->getMinimumVisibleSnapshot()) {
            auto mySnapshot =
                opCtx->recoveryUnit()->getPointInTimeReadTimestamp(opCtx).get_value_or(
                    opCtx->recoveryUnit()->getCatalogConflictingTimestamp());

            uassert(
                ErrorCodes::SnapshotUnavailable,
                str::stream() << "Unable to read from a snapshot due to pending collection catalog "
                                 "changes; please retry the operation. Snapshot timestamp is "
                              << mySnapshot.toString() << ". Collection minimum is "
                              << minSnapshot->toString(),
                mySnapshot.isNull() || mySnapshot >= minSnapshot.value());
        }
    }
}

/**
 * Defines sorting order for NamespaceStrings based on what their ResourceId would be for locking.
 */
struct ResourceIdNssComparator {
    bool operator()(const NamespaceString& lhs, const NamespaceString& rhs) const {
        return ResourceId(RESOURCE_COLLECTION, lhs) < ResourceId(RESOURCE_COLLECTION, rhs);
    }
};

/**
 * Fills the input 'collLocks' with CollectionLocks, acquiring locks on namespaces 'nsOrUUID' and
 * 'secondaryNssOrUUIDs' in ResourceId(RESOURCE_COLLECTION, nss) order.
 *
 * The namespaces will be resolved, the locks acquired, and then the namespaces will be checked for
 * changes in case there is a race with rename and a UUID no longer matches the locked namespace.
 *
 * Handles duplicate namespaces across 'nsOrUUID' and 'secondaryNssOrUUIDs'. Only one lock will be
 * taken on each namespace.
 */
void acquireCollectionLocksInResourceIdOrder(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nsOrUUID,
    LockMode modeColl,
    Date_t deadline,
    const std::vector<NamespaceStringOrUUID>& secondaryNssOrUUIDs,
    std::vector<CollectionNamespaceOrUUIDLock>* collLocks) {
    invariant(collLocks->empty());
    auto catalog = CollectionCatalog::get(opCtx);

    // Use a set so that we can easily dedupe namespaces to avoid locking the same collection twice.
    std::set<NamespaceString, ResourceIdNssComparator> temp;
    std::set<NamespaceString, ResourceIdNssComparator> verifyTemp;
    do {
        // Clear the data structures when/if we loop more than once.
        collLocks->clear();
        temp.clear();
        verifyTemp.clear();

        // Create a single set with all the resolved namespaces sorted by ascending
        // ResourceId(RESOURCE_COLLECTION, nss).
        temp.insert(catalog->resolveNamespaceStringOrUUID(opCtx, nsOrUUID));
        for (const auto& secondaryNssOrUUID : secondaryNssOrUUIDs) {
            invariant(secondaryNssOrUUID.db() == nsOrUUID.db(),
                      str::stream()
                          << "Unable to acquire locks for collections across different databases ("
                          << secondaryNssOrUUID << " vs " << nsOrUUID << ")");
            temp.insert(catalog->resolveNamespaceStringOrUUID(opCtx, secondaryNssOrUUID));
        }

        // Acquire all of the locks in order. And clear the 'catalog' because the locks will access
        // a fresher one internally.
        catalog = nullptr;
        for (auto& nss : temp) {
            collLocks->emplace_back(opCtx, nss, modeColl, deadline);
        }

        // Check that the namespaces have NOT changed after acquiring locks. It's possible to race
        // with a rename collection when the given NamespaceStringOrUUID is a UUID, and consequently
        // fail to lock the correct namespace.
        //
        // The catalog reference must be refreshed to see the latest Collection data. Otherwise we
        // won't see any concurrent DDL/catalog operations.
        auto catalog = CollectionCatalog::get(opCtx);
        verifyTemp.insert(catalog->resolveNamespaceStringOrUUID(opCtx, nsOrUUID));
        for (const auto& secondaryNssOrUUID : secondaryNssOrUUIDs) {
            verifyTemp.insert(catalog->resolveNamespaceStringOrUUID(opCtx, secondaryNssOrUUID));
        }
    } while (temp != verifyTemp);
}

}  // namespace

AutoGetDb::AutoGetDb(OperationContext* opCtx,
                     const DatabaseName& dbName,
                     LockMode mode,
                     Date_t deadline)
    : AutoGetDb(opCtx, dbName, mode, deadline, [] {
          Lock::GlobalLockSkipOptions options;
          return options;
      }()) {}

AutoGetDb::AutoGetDb(OperationContext* opCtx,
                     const DatabaseName& dbName,
                     LockMode mode,
                     Date_t deadline,
                     Lock::DBLockSkipOptions options)
    : _dbName(dbName), _dbLock(opCtx, dbName, mode, deadline, std::move(options)), _db([&] {
          auto databaseHolder = DatabaseHolder::get(opCtx);
          return databaseHolder->getDb(opCtx, dbName);
      }()) {
    // The 'primary' database must be version checked for sharding.
    // TODO SERVER-63706 Pass dbName directly
    catalog_helper::assertMatchingDbVersion(opCtx, _dbName.toStringWithTenantId());
}

Database* AutoGetDb::ensureDbExists(OperationContext* opCtx) {
    if (_db) {
        return _db;
    }

    auto databaseHolder = DatabaseHolder::get(opCtx);
    _db = databaseHolder->openDb(opCtx, _dbName, nullptr);

    catalog_helper::assertMatchingDbVersion(opCtx, _dbName.toStringWithTenantId());

    return _db;
}

Database* AutoGetDb::refreshDbReferenceIfNull(OperationContext* opCtx) {
    if (!_db) {
        auto databaseHolder = DatabaseHolder::get(opCtx);
        _db = databaseHolder->getDb(opCtx, _dbName);
        catalog_helper::assertMatchingDbVersion(opCtx, _dbName.toStringWithTenantId());
    }
    return _db;
}


CollectionNamespaceOrUUIDLock::CollectionNamespaceOrUUIDLock(OperationContext* opCtx,
                                                             const NamespaceStringOrUUID& nsOrUUID,
                                                             LockMode mode,
                                                             Date_t deadline)
    : _lock([opCtx, &nsOrUUID, mode, deadline] {
          if (auto& ns = nsOrUUID.nss()) {
              return Lock::CollectionLock{opCtx, *ns, mode, deadline};
          }

          auto resolveNs = [opCtx, &nsOrUUID] {
              return CollectionCatalog::get(opCtx)->resolveNamespaceStringOrUUID(opCtx, nsOrUUID);
          };

          // We cannot be sure that the namespace we lock matches the UUID given because we resolve
          // the namespace from the UUID without the safety of a lock. Therefore, we will continue
          // to re-lock until the namespace we resolve from the UUID before and after taking the
          // lock is the same.
          while (true) {
              auto ns = resolveNs();
              Lock::CollectionLock lock{opCtx, ns, mode, deadline};
              if (ns == resolveNs()) {
                  return lock;
              }
          }
      }()) {}

AutoGetCollection::AutoGetCollection(OperationContext* opCtx,
                                     const NamespaceStringOrUUID& nsOrUUID,
                                     LockMode modeColl,
                                     Options options)
    : AutoGetCollection(opCtx,
                        nsOrUUID,
                        modeColl,
                        std::move(options),
                        /*verifyWriteEligible=*/modeColl != MODE_IS) {}

AutoGetCollection::AutoGetCollection(OperationContext* opCtx,
                                     const NamespaceStringOrUUID& nsOrUUID,
                                     LockMode modeColl,
                                     Options options,
                                     ForReadTag reader)
    : AutoGetCollection(
          opCtx, nsOrUUID, modeColl, std::move(options), /*verifyWriteEligible=*/false) {}

AutoGetCollection::AutoGetCollection(OperationContext* opCtx,
                                     const NamespaceStringOrUUID& nsOrUUID,
                                     LockMode modeColl,
                                     Options options,
                                     bool verifyWriteEligible)
    : _autoDb([&] {
          auto& deadline = options._deadline;

          invariant(!opCtx->isLockFreeReadsOp());

          // Acquire the global/RSTL and all the database locks (may or may not be multiple
          // databases).

          Lock::DBLockSkipOptions dbLockOptions;
          dbLockOptions.skipRSTLLock = [&] {
              const auto& maybeNss = nsOrUUID.nss();

              if (maybeNss) {
                  const auto& nss = *maybeNss;
                  return repl::canCollectionSkipRSTLLockAcquisition(nss);
              }
              return false;
          }();
          dbLockOptions.skipFlowControlTicket = [&nsOrUUID] {
              const auto& maybeNss = nsOrUUID.nss();

              if (maybeNss) {
                  const auto& nss = *maybeNss;
                  bool notReplicated = !nss.isReplicated();
                  // TODO: Improve comment
                  //
                  // If the 'opCtx' is in a multi document transaction, pure reads on the
                  // transaction session collections would acquire the global lock in the IX mode
                  // and acquire a flow control ticket.
                  bool isTransactionCollection =
                      nss == NamespaceString::kSessionTransactionsTableNamespace ||
                      nss == NamespaceString::kTransactionCoordinatorsNamespace;
                  return notReplicated || isTransactionCollection;
              }
              return false;
          }();
          // TODO SERVER-67817 Use NamespaceStringOrUUID::db() instead.
          return AutoGetDb(opCtx,
                           nsOrUUID.nss() ? nsOrUUID.nss()->dbName() : *nsOrUUID.dbName(),
                           isSharedLockMode(modeColl) ? MODE_IS : MODE_IX,
                           deadline,
                           std::move(dbLockOptions));
      }()) {

    auto& viewMode = options._viewMode;
    auto& deadline = options._deadline;
    auto& secondaryNssOrUUIDs = options._secondaryNssOrUUIDs;

    // Out of an abundance of caution, force operations to acquire new snapshots after
    // acquiring exclusive collection locks. Operations that hold MODE_X locks make an
    // assumption that all writes are visible in their snapshot and no new writes will commit.
    // This may not be the case if an operation already has a snapshot open before acquiring an
    // exclusive lock.
    if (modeColl == MODE_X) {
        invariant(!opCtx->recoveryUnit()->isActive(),
                  str::stream() << "Snapshot opened before acquiring X lock for " << nsOrUUID);
    }

    // Acquire the collection locks. If there's only one lock, then it can simply be taken. If
    // there are many, however, the locks must be taken in _ascending_ ResourceId order to avoid
    // deadlocks across threads.
    if (secondaryNssOrUUIDs.empty()) {
        uassertStatusOK(nsOrUUID.isNssValid());
        _collLocks.emplace_back(opCtx, nsOrUUID, modeColl, deadline);
    } else {
        acquireCollectionLocksInResourceIdOrder(
            opCtx, nsOrUUID, modeColl, deadline, secondaryNssOrUUIDs, &_collLocks);
    }

    // Wait for a configured amount of time after acquiring locks if the failpoint is enabled
    setAutoGetCollectionWait.execute(
        [&](const BSONObj& data) { sleepFor(Milliseconds(data["waitForMillis"].numberInt())); });

    auto catalog = CollectionCatalog::get(opCtx);
    auto databaseHolder = DatabaseHolder::get(opCtx);

    // Check that the collections are all safe to use.
    _resolvedNss = catalog->resolveNamespaceStringOrUUID(opCtx, nsOrUUID);
    _coll = catalog->lookupCollectionByNamespace(opCtx, _resolvedNss);

    if (_coll) {
        // It is possible for an operation to have created the database and collection after this
        // AutoGetCollection initialized its AutoGetDb, but before it has performed the collection
        // lookup. Thus, it is possible for AutoGetDb to hold nullptr while _coll is a valid
        // pointer. This would be unexpected, as for a collection to exist the database must exist.
        // We ensure the database reference is valid by refreshing it.
        _autoDb.refreshDbReferenceIfNull(opCtx);
    }

    checkCollectionUUIDMismatch(opCtx, _resolvedNss, _coll, options._expectedUUID);
    verifyDbAndCollection(
        opCtx, modeColl, nsOrUUID, _resolvedNss, _coll, _autoDb.getDb(), verifyWriteEligible);
    for (auto& secondaryNssOrUUID : secondaryNssOrUUIDs) {
        auto secondaryResolvedNss =
            catalog->resolveNamespaceStringOrUUID(opCtx, secondaryNssOrUUID);
        auto secondaryColl = catalog->lookupCollectionByNamespace(opCtx, secondaryResolvedNss);
        auto secondaryDbName = secondaryNssOrUUID.dbName() ? secondaryNssOrUUID.dbName()
                                                           : secondaryNssOrUUID.nss()->dbName();
        verifyDbAndCollection(opCtx,
                              MODE_IS,
                              secondaryNssOrUUID,
                              secondaryResolvedNss,
                              secondaryColl,
                              databaseHolder->getDb(opCtx, *secondaryDbName),
                              verifyWriteEligible);
    }

    if (_coll) {
        // Fetch and store the sharding collection description data needed for use during the
        // operation. The shardVersion will be checked later if the shard filtering metadata is
        // fetched, ensuring both that the collection description info used here and the routing
        // table are consistent with the read request's shardVersion.
        //
        // Note: sharding versioning for an operation has no concept of multiple collections.
        auto scopedCss = CollectionShardingState::acquire(opCtx, _resolvedNss);
        scopedCss->checkShardVersionOrThrow(opCtx);

        auto collDesc = scopedCss->getCollectionDescription(opCtx);
        if (collDesc.isSharded()) {
            _coll.setShardKeyPattern(collDesc.getKeyPattern());
        }

        return;
    }

    const auto receivedShardVersion{
        OperationShardingState::get(opCtx).getShardVersion(_resolvedNss)};

    if (!options._expectedUUID) {
        // We only need to look up a view if an expected collection UUID was not provided. If this
        // namespace were a view, the collection UUID mismatch check would have failed above.
        if ((_view = catalog->lookupView(opCtx, _resolvedNss))) {
            uassert(ErrorCodes::CommandNotSupportedOnView,
                    str::stream() << "Taking " << _resolvedNss.ns()
                                  << " lock for timeseries is not allowed",
                    viewMode == auto_get_collection::ViewMode::kViewsPermitted ||
                        !_view->timeseries());

            uassert(ErrorCodes::CommandNotSupportedOnView,
                    str::stream() << "Namespace " << _resolvedNss.ns()
                                  << " is a view, not a collection",
                    viewMode == auto_get_collection::ViewMode::kViewsPermitted);

            uassert(StaleConfigInfo(_resolvedNss,
                                    *receivedShardVersion,
                                    ShardVersion::UNSHARDED() /* wantedVersion */,
                                    ShardingState::get(opCtx)->shardId()),
                    str::stream() << "Namespace " << _resolvedNss
                                  << " is a view therefore the shard "
                                  << "version attached to the request must be unset or UNSHARDED",
                    !receivedShardVersion || *receivedShardVersion == ShardVersion::UNSHARDED());

            return;
        }
    }

    // There is neither a collection nor a view for the namespace, so if we reached to this point
    // there are the following possibilities depending on the received shard version:
    //   1. ShardVersion::UNSHARDED: The request comes from a router and the operation entails the
    //      implicit creation of an unsharded collection. We can continue.
    //   2. ShardVersion::IGNORED: The request comes from a router that broadcasted the same to all
    //      shards, but this shard doesn't own any chunks for the collection. We can continue.
    //   3. boost::none: The request comes from client directly connected to the shard. We can
    //      continue.
    //   4. Any other value: The request comes from a stale router on a collection or a view which
    //      was deleted time ago (or the user manually deleted it from from underneath of sharding).
    //      We return a stale config error so that the router recovers.

    uassert(StaleConfigInfo(_resolvedNss,
                            *receivedShardVersion,
                            boost::none /* wantedVersion */,
                            ShardingState::get(opCtx)->shardId()),
            str::stream() << "No metadata for namespace " << _resolvedNss << " therefore the shard "
                          << "version attached to the request must be unset, UNSHARDED or IGNORED",
            !receivedShardVersion || *receivedShardVersion == ShardVersion::UNSHARDED() ||
                *receivedShardVersion == ShardVersion::IGNORED());
}

Collection* AutoGetCollection::getWritableCollection(OperationContext* opCtx) {
    invariant(_collLocks.size() == 1);

    // Acquire writable instance if not already available
    if (!_writableColl) {

        auto catalog = CollectionCatalog::get(opCtx);
        _writableColl = catalog->lookupCollectionByNamespaceForMetadataWrite(opCtx, _resolvedNss);
        // Makes the internal CollectionPtr Yieldable and resets the writable Collection when
        // the write unit of work finishes so we re-fetches and re-clones the Collection if a
        // new write unit of work is opened.
        opCtx->recoveryUnit()->registerChange(
            [this, opCtx](boost::optional<Timestamp> commitTime) {
                _coll =
                    CollectionPtr(opCtx, _coll.get(), LookupCollectionForYieldRestore(_coll->ns()));
                _writableColl = nullptr;
            },
            [this, originalCollection = _coll.get(), opCtx]() {
                _coll = CollectionPtr(opCtx,
                                      originalCollection,
                                      LookupCollectionForYieldRestore(originalCollection->ns()));
                _writableColl = nullptr;
            });

        // Set to writable collection. We are no longer yieldable.
        _coll = _writableColl;
    }
    return _writableColl;
}

AutoGetCollectionLockFree::AutoGetCollectionLockFree(OperationContext* opCtx,
                                                     const NamespaceStringOrUUID& nsOrUUID,
                                                     RestoreFromYieldFn restoreFromYield,
                                                     Options options)
    : _lockFreeReadsBlock(opCtx),
      _globalLock(opCtx, MODE_IS, options._deadline, Lock::InterruptBehavior::kThrow, [] {
          Lock::GlobalLockSkipOptions options;
          options.skipRSTLLock = true;
          return options;
      }()) {

    auto& viewMode = options._viewMode;

    // Wait for a configured amount of time after acquiring locks if the failpoint is enabled
    setAutoGetCollectionWait.execute(
        [&](const BSONObj& data) { sleepFor(Milliseconds(data["waitForMillis"].numberInt())); });

    auto catalog = CollectionCatalog::get(opCtx);
    _resolvedNss = catalog->resolveNamespaceStringOrUUID(opCtx, nsOrUUID);
    _collection = catalog->lookupCollectionByNamespaceForRead(opCtx, _resolvedNss);

    // When we restore from yield on this CollectionPtr we will update _collection above and use its
    // new pointer in the CollectionPtr
    _collectionPtr = CollectionPtr(
        opCtx,
        _collection.get(),
        [this, restoreFromYield = std::move(restoreFromYield)](OperationContext* opCtx, UUID uuid) {
            restoreFromYield(_collection, opCtx, uuid);
            return _collection.get();
        });

    // Check that the sharding database version matches our read.
    // Note: this must always be checked, regardless of whether the collection exists, so that the
    // dbVersion of this node or the caller gets updated quickly in case either is stale.
    catalog_helper::assertMatchingDbVersion(opCtx, _resolvedNss.db());

    checkCollectionUUIDMismatch(opCtx, _resolvedNss, _collectionPtr, options._expectedUUID);

    hangBeforeAutoGetCollectionLockFreeShardedStateAccess.executeIf(
        [&](auto&) { hangBeforeAutoGetCollectionLockFreeShardedStateAccess.pauseWhileSet(opCtx); },
        [&](const BSONObj& data) {
            return opCtx->getLogicalSessionId() &&
                opCtx->getLogicalSessionId()->getId() == UUID::fromCDR(data["lsid"].uuid());
        });

    if (_collection) {
        // Fetch and store the sharding collection description data needed for use during the
        // operation. The shardVersion will be checked later if the shard filtering metadata is
        // fetched, ensuring both that the collection description info fetched here and the routing
        // table are consistent with the read request's shardVersion.
        auto scopedCss = CollectionShardingState::acquire(opCtx, _collection->ns());
        auto collDesc = scopedCss->getCollectionDescription(opCtx);
        if (collDesc.isSharded()) {
            _collectionPtr.setShardKeyPattern(collDesc.getKeyPattern());
        }

        // If the collection exists, there is no need to check for views.
        return;
    }

    invariant(!options._expectedUUID);
    _view = catalog->lookupView(opCtx, _resolvedNss);
    uassert(ErrorCodes::CommandNotSupportedOnView,
            str::stream() << "Taking " << _resolvedNss.ns()
                          << " lock for timeseries is not allowed",
            !_view || viewMode == auto_get_collection::ViewMode::kViewsPermitted ||
                !_view->timeseries());
    uassert(ErrorCodes::CommandNotSupportedOnView,
            str::stream() << "Namespace " << _resolvedNss.ns() << " is a view, not a collection",
            !_view || viewMode == auto_get_collection::ViewMode::kViewsPermitted);
    if (_view) {
        // We are about to succeed setup as a view. No LockFree state was setup so do not mark the
        // OperationContext as LFR.
        _lockFreeReadsBlock.reset();
    }
}

AutoGetCollectionMaybeLockFree::AutoGetCollectionMaybeLockFree(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nsOrUUID,
    LockMode modeColl,
    auto_get_collection::ViewMode viewMode,
    Date_t deadline) {
    if (opCtx->isLockFreeReadsOp()) {
        _autoGetLockFree.emplace(
            opCtx,
            nsOrUUID,
            [](std::shared_ptr<const Collection>& collection, OperationContext* opCtx, UUID uuid) {
                LOGV2_FATAL(5342700,
                            "This is a nested lock helper and there was an attempt to "
                            "yield locks, which should be impossible");
            },
            AutoGetCollectionLockFree::Options{}.viewMode(viewMode).deadline(deadline));
    } else {
        _autoGet.emplace(opCtx,
                         nsOrUUID,
                         modeColl,
                         AutoGetCollection::Options{}.viewMode(viewMode).deadline(deadline));
    }
}

struct CollectionWriter::SharedImpl {
    SharedImpl(CollectionWriter* parent) : _parent(parent) {}

    CollectionWriter* _parent;
    std::function<Collection*()> _writableCollectionInitializer;
};

CollectionWriter::CollectionWriter(OperationContext* opCtx, const UUID& uuid)
    : _collection(&_storedCollection),
      _managed(true),
      _sharedImpl(std::make_shared<SharedImpl>(this)) {

    _storedCollection = CollectionCatalog::get(opCtx)->lookupCollectionByUUID(opCtx, uuid);
    _sharedImpl->_writableCollectionInitializer = [opCtx, uuid]() {
        return CollectionCatalog::get(opCtx)->lookupCollectionByUUIDForMetadataWrite(opCtx, uuid);
    };
}

CollectionWriter::CollectionWriter(OperationContext* opCtx, const NamespaceString& nss)
    : _collection(&_storedCollection),
      _managed(true),
      _sharedImpl(std::make_shared<SharedImpl>(this)) {
    _storedCollection = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);
    _sharedImpl->_writableCollectionInitializer = [opCtx, nss]() {
        return CollectionCatalog::get(opCtx)->lookupCollectionByNamespaceForMetadataWrite(opCtx,
                                                                                          nss);
    };
}

CollectionWriter::CollectionWriter(OperationContext* opCtx, AutoGetCollection& autoCollection)
    : _collection(&autoCollection.getCollection()),
      _managed(true),
      _sharedImpl(std::make_shared<SharedImpl>(this)) {
    _sharedImpl->_writableCollectionInitializer = [&autoCollection, opCtx]() {
        return autoCollection.getWritableCollection(opCtx);
    };
}

CollectionWriter::CollectionWriter(Collection* writableCollection)
    : _collection(&_storedCollection),
      _storedCollection(writableCollection),
      _writableCollection(writableCollection),
      _managed(false) {}

CollectionWriter::~CollectionWriter() {
    // Notify shared state that this instance is destroyed
    if (_sharedImpl) {
        _sharedImpl->_parent = nullptr;
    }
}

Collection* CollectionWriter::getWritableCollection(OperationContext* opCtx) {
    // Acquire writable instance lazily if not already available
    if (!_writableCollection) {
        _writableCollection = _sharedImpl->_writableCollectionInitializer();

        // If we are using our stored Collection then we are not managed by an AutoGetCollection and
        // we need to manage lifetime here.
        if (_managed) {
            bool usingStoredCollection = *_collection == _storedCollection;
            auto rollbackCollection =
                usingStoredCollection ? std::move(_storedCollection) : CollectionPtr();

            // Resets the writable Collection when the write unit of work finishes so we re-fetch
            // and re-clone the Collection if a new write unit of work is opened. Holds the back
            // pointer to the CollectionWriter explicitly so we can detect if the instance is
            // already destroyed.
            opCtx->recoveryUnit()->registerChange(
                [shared = _sharedImpl](boost::optional<Timestamp>) {
                    if (shared->_parent)
                        shared->_parent->_writableCollection = nullptr;
                },
                [shared = _sharedImpl,
                 rollbackCollection = std::move(rollbackCollection)]() mutable {
                    if (shared->_parent) {
                        shared->_parent->_storedCollection = std::move(rollbackCollection);
                        shared->_parent->_writableCollection = nullptr;
                    }
                });
            if (usingStoredCollection) {
                _storedCollection = _writableCollection;
            }
        }
    }
    return _writableCollection;
}

LockMode fixLockModeForSystemDotViewsChanges(const NamespaceString& nss, LockMode mode) {
    return nss.isSystemDotViews() ? MODE_X : mode;
}

ReadSourceScope::ReadSourceScope(OperationContext* opCtx,
                                 RecoveryUnit::ReadSource readSource,
                                 boost::optional<Timestamp> provided)
    : _opCtx(opCtx), _originalReadSource(opCtx->recoveryUnit()->getTimestampReadSource()) {
    // Abandoning the snapshot is unsafe when the snapshot is managed by a lock free read helper.
    invariant(!_opCtx->isLockFreeReadsOp());

    if (_originalReadSource == RecoveryUnit::ReadSource::kProvided) {
        _originalReadTimestamp = *_opCtx->recoveryUnit()->getPointInTimeReadTimestamp(_opCtx);
    }

    _opCtx->recoveryUnit()->abandonSnapshot();
    _opCtx->recoveryUnit()->setTimestampReadSource(readSource, provided);
}

ReadSourceScope::~ReadSourceScope() {
    // Abandoning the snapshot is unsafe when the snapshot is managed by a lock free read helper.
    invariant(!_opCtx->isLockFreeReadsOp());

    _opCtx->recoveryUnit()->abandonSnapshot();
    if (_originalReadSource == RecoveryUnit::ReadSource::kProvided) {
        _opCtx->recoveryUnit()->setTimestampReadSource(_originalReadSource, _originalReadTimestamp);
    } else {
        _opCtx->recoveryUnit()->setTimestampReadSource(_originalReadSource);
    }
}

AutoGetOplog::AutoGetOplog(OperationContext* opCtx, OplogAccessMode mode, Date_t deadline)
    : _shouldNotConflictWithSecondaryBatchApplicationBlock(opCtx->lockState()) {
    auto lockMode = (mode == OplogAccessMode::kRead) ? MODE_IS : MODE_IX;
    if (mode == OplogAccessMode::kLogOp) {
        // Invariant that global lock is already held for kLogOp mode.
        invariant(opCtx->lockState()->isWriteLocked());
    } else {
        _globalLock.emplace(opCtx, lockMode, deadline, Lock::InterruptBehavior::kThrow);
    }

    _oplogInfo = LocalOplogInfo::get(opCtx);
    _oplog = &_oplogInfo->getCollection();
}


AutoGetChangeCollection::AutoGetChangeCollection(OperationContext* opCtx,
                                                 AutoGetChangeCollection::AccessMode mode,
                                                 boost::optional<TenantId> tenantId,
                                                 Date_t deadline) {
    if (mode == AccessMode::kWriteInOplogContext) {
        // The global lock must already be held.
        invariant(opCtx->lockState()->isWriteLocked());
    }

    if (mode != AccessMode::kRead) {
        // TODO SERVER-66715 avoid taking 'AutoGetCollection' and remove
        // 'AllowLockAcquisitionOnTimestampedUnitOfWork'.
        _allowLockAcquisitionTsWuow.emplace(opCtx->lockState());
    }

    _coll.emplace(opCtx,
                  NamespaceString::makeChangeCollectionNSS(tenantId),
                  mode == AccessMode::kRead ? MODE_IS : MODE_IX,
                  AutoGetCollection::Options{}.deadline(deadline));
}

const Collection* AutoGetChangeCollection::operator->() const {
    return _coll ? _coll->getCollection().get() : nullptr;
}

const CollectionPtr& AutoGetChangeCollection::operator*() const {
    return _coll->getCollection();
}

AutoGetChangeCollection::operator bool() const {
    return _coll && _coll->getCollection().get();
}


}  // namespace mongo
