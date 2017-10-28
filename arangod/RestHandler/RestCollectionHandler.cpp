////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2017 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#include "RestCollectionHandler.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/ServerState.h"
#include "Rest/HttpRequest.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/PhysicalCollection.h"
#include "StorageEngine/StorageEngine.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/OperationOptions.h"
#include "Utils/SingleCollectionTransaction.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/Methods/Collections.h"

#include <velocypack/Builder.h>
#include <velocypack/Collection.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::rest;

RestCollectionHandler::RestCollectionHandler(GeneralRequest* request,
                                             GeneralResponse* response)
    : RestVocbaseBaseHandler(request, response) {}

RestStatus RestCollectionHandler::execute() {
  switch (_request->requestType()) {
    case rest::RequestType::GET:
      handleCommandGet();
      break;
    case rest::RequestType::POST:
      handleCommandPost();
      break;
    case rest::RequestType::PUT:
      handleCommandPut();
      break;
    case rest::RequestType::DELETE_REQ:
      handleCommandDelete();
      break;
    default:
      generateError(rest::ResponseCode::METHOD_NOT_ALLOWED,
                    TRI_ERROR_HTTP_METHOD_NOT_ALLOWED);
  }

  return RestStatus::DONE;
}

void RestCollectionHandler::handleCommandGet() {
  std::vector<std::string> suffixes = _request->decodedSuffixes();
  VPackBuilder builder;

  // /_api/collection
  if (suffixes.empty()) {
    bool excludeSystem = _request->parsedValue("excludeSystem", false);

    builder.openArray();
    methods::Collections::enumerate(_vocbase, [&](LogicalCollection* coll) {
      if (!excludeSystem || !coll->isSystem()) {
        collectionRepresentation(builder, coll, /*showProperties*/ false,
                                 /*showFigures*/ false, /*showCount*/ false,
                                 /*aggregateCount*/ false);
      }
    });
    builder.close();
    generateOk(rest::ResponseCode::OK, builder.slice());
    return;
  }

  std::string const name = suffixes[0];
  // /_api/collection/<name>
  if (suffixes.size() == 1) {
    Result found = methods::Collections::lookup(
        _vocbase, name, [&](LogicalCollection* coll) {
          collectionRepresentation(builder, coll, /*showProperties*/ false,
                                   /*showFigures*/ false, /*showCount*/ false,
                                   /*aggregateCount*/ false);
        });
    if (found.ok()) {
      generateOk(rest::ResponseCode::OK, builder);
    } else {
      generateError(found);
    }
    return;
  }

  if (suffixes.size() > 2) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "expect GET /_api/collection/<collection-name>/<method>");
    return;
  }

  std::string const sub = suffixes[1];
  bool skipGenerate = false;
  Result found = methods::Collections::lookup(
      _vocbase, name, [&](LogicalCollection* coll) {
        if (sub == "checksum") {
          // /_api/collection/<identifier>/checksum
          bool withRevisions = _request->parsedValue("withRevisions", false);
          bool withData = _request->parsedValue("withData", false);

          ChecksumResult result = coll->checksum(withRevisions, withData);
          if (result.ok()) {
            VPackObjectBuilder obj(&builder, true);
            obj->add("checksum", result.slice().get("checksum"));
            obj->add("revision", result.slice().get("revision"));
            collectionRepresentation(builder, coll, /*showProperties*/ false,
                                     /*showFigures*/ false, /*showCount*/ false,
                                     /*aggregateCount*/ false);
          } else {
            skipGenerate = true;
            this->generateError(result);
          }
        } else if (sub == "figures") {
          // /_api/collection/<identifier>/figures
          collectionRepresentation(builder, coll, /*showProperties*/ true,
                                   /*showFigures*/ true, /*showCount*/ true,
                                   /*aggregateCount*/ false);
        } else if (sub == "count") {
          // /_api/collection/<identifier>/count
          bool details = _request->parsedValue("details", false);
          collectionRepresentation(builder, coll, /*showProperties*/ true,
                                   /*showFigures*/ false, /*showCount*/ true,
                                   /*aggregateCount*/ !details);
        } else if (sub == "properties") {
          // /_api/collection/<identifier>/count
          bool details = _request->parsedValue("details", false);
          collectionRepresentation(builder, coll, /*showProperties*/ true,
                                   /*showFigures*/ false, /*showCount*/ false,
                                   /*aggregateCount*/ details);
        } else if (sub == "revision") {
          // /_api/collection/<identifier>/count
          VPackObjectBuilder obj(&builder, true);
          collectionRepresentation(builder, coll, /*showProperties*/ true,
                                   /*showFigures*/ false, /*showCount*/ false,
                                   /*aggregateCount*/ false);
          auto ctx = transaction::StandaloneContext::Create(_vocbase);
          SingleCollectionTransaction trx(ctx, coll->cid(),
                                          AccessMode::Type::READ);
          Result res = trx.begin();
          if (res.fail()) {
            THROW_ARANGO_EXCEPTION(res);
          }
          obj->add("revision",
                   VPackValue(StringUtils::itoa(coll->revision(&trx))));

        } else if (sub == "shards") {
          // /_api/collection/<identifier>/shards
          if (!ServerState::instance()->isRunningInCluster()) {
            skipGenerate = true; // must be internal error for tests
            this->generateError(Result(TRI_ERROR_INTERNAL));
            return;
          }

          collectionRepresentation(builder, coll, /*showProperties*/ true,
                                   /*showFigures*/ false, /*showCount*/ false,
                                   /*aggregateCount*/ false);
          auto shards = ClusterInfo::instance()->getShardList(coll->name());
          VPackArrayBuilder arr(&builder, "shards", true);
          for (ShardID const& shard : *shards) {
            arr->add(VPackValue(shard));
          }

        } else {
          skipGenerate = true;
          this->generateError(
              rest::ResponseCode::NOT_FOUND, TRI_ERROR_HTTP_NOT_FOUND,
              "expecting one of the resources 'checksum', 'count', "
              "'figures', 'properties', 'revision', 'shards'");
        }
      });

  if (skipGenerate) {
    return;
  }
  if (found.ok()) {
    generateOk(rest::ResponseCode::OK, builder);
    _response->setHeader("location", _request->requestPath());
  } else {
    generateError(found);
  }
}

// create a collection
void RestCollectionHandler::handleCommandPost() {
  bool parseSuccess = true;
  std::shared_ptr<VPackBuilder> parsedBody = parseVelocyPackBody(parseSuccess);
  if (!parseSuccess) {
    // error message generated in parseVelocyPackBody
    return;
  }
  VPackSlice const body = parsedBody->slice();
  VPackSlice name;
  if (!body.isObject() || !(name = body.get("name")).isString() ||
      name.getStringLength() == 0) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_ARANGO_ILLEGAL_NAME);
    return;
  }

  auto cluster =
      application_features::ApplicationServer::getFeature<ClusterFeature>(
          "Cluster");
  bool waitsForSync = cluster->createWaitsForSyncReplication();
  waitsForSync = VelocyPackHelper::getBooleanValue(body, "body", waitsForSync);

  TRI_col_type_e type = TRI_col_type_e::TRI_COL_TYPE_DOCUMENT;
  VPackSlice typeSlice = body.get("type");
  if ((typeSlice.isString() && (typeSlice.compareString("edge") == 0 ||
                                typeSlice.compareString("3") == 0)) ||
      (typeSlice.isNumber() &&
       typeSlice.getUInt() == TRI_col_type_e::TRI_COL_TYPE_EDGE)) {
    type = TRI_col_type_e::TRI_COL_TYPE_EDGE;
  }

  // for some "security" have a white-list of allowed parameters
  VPackBuilder filtered = VPackCollection::keep(
      parsedBody->slice(),
      std::unordered_set<std::string>{
          "doCompact", "isSystem", "id", "isVolatile", "journalSize",
          "indexBuckets", "keyOptions", "waitForSync", "cacheEnabled",
          "shardKeys", "numberOfShards", "distributeShardsLike", "avoidServers",
          "isSmart", "smartGraphAttribute", "replicationFactor", "servers"});
  VPackSlice const parameters = filtered.slice();

  // now we can create the collection
  VPackBuilder builder;
  Result res = methods::Collections::create(
      _vocbase, name.copyString(), type, parameters, waitsForSync,
      [&](LogicalCollection* coll) {
        collectionRepresentation(builder, coll, /*showProperties*/ true,
                                 /*showFigures*/ false, /*showCount*/ false,
                                 /*aggregateCount*/ false);
      });
  if (res.ok()) {
    generateOk(rest::ResponseCode::OK, builder);
  } else {
    generateError(res);
  }
}

void RestCollectionHandler::handleCommandPut() {
  std::vector<std::string> suffixes = _request->decodedSuffixes();
  if (suffixes.size() != 2) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "expected PUT /_api/collection/<collection-name>/<action>");
    return;
  }
  bool parseSuccess = true;
  std::shared_ptr<VPackBuilder> parsedBody = parseVelocyPackBody(parseSuccess);
  if (!parseSuccess) {
    // error message generated in parseVelocyPackBody
    return;
  }
  VPackSlice body = parsedBody->slice();
  if (!body.isObject()) {
    body = VPackSlice::emptyObjectSlice();
  }

  std::string const name = suffixes[0];
  std::string const sub = suffixes[1];
  Result res;
  VPackBuilder builder;
  Result found = methods::Collections::lookup(
      _vocbase, name, [&](LogicalCollection* coll) {
        VPackObjectBuilder obj(&builder, true);

        if (sub == "load") {
          res = methods::Collections::load(_vocbase, coll);
          if (res.ok()) {
            bool cc = VelocyPackHelper::getBooleanValue(body, "count", true);
            collectionRepresentation(builder, coll, /*showProperties*/ false,
                                     /*showFigures*/ false, /*showCount*/ cc,
                                     /*aggregateCount*/ false);
          }
        } else if (sub == "unload") {
          bool flush = _request->parsedValue("flush", false);
          if (flush &&
              coll->status() ==
                  TRI_vocbase_col_status_e::TRI_VOC_COL_STATUS_LOADED) {
            EngineSelectorFeature::ENGINE->flushWal(false, false, false);
          }

          res = methods::Collections::unload(_vocbase, coll);
          if (res.ok()) {
            collectionRepresentation(builder, coll, /*showProperties*/ false,
                                     /*showFigures*/ false, /*showCount*/ false,
                                     /*aggregateCount*/ false);
          }
        } else if (sub == "truncate") {
          OperationOptions opts;
          opts.waitForSync = _request->parsedValue("waitForSync", false);
          opts.isSynchronousReplicationFrom = _request->value("isSynchronousReplication");

          auto ctx = transaction::StandaloneContext::Create(_vocbase);
          SingleCollectionTransaction trx(ctx, coll->cid(),
                                          AccessMode::Type::EXCLUSIVE);
          res = trx.begin();
          if (res.ok()) {
            OperationResult result = trx.truncate(coll->name(), opts);
            if (!result.successful()) {
              LOG_TOPIC(ERR, Logger::FIXME) << "truncate failed [" << result.code << "] " << result.errorMessage;
            }
            res = trx.finish(result.code);
          }
          if (res.ok()) {
            collectionRepresentation(builder, coll, /*showProperties*/ false,
                                     /*showFigures*/ false, /*showCount*/ false,
                                     /*aggregateCount*/ false);
          }

        } else if (sub == "properties") {
          std::vector<std::string> keep = {"doCompact", "journalSize",
            "waitForSync", "indexBuckets", "replicationFactor", "cacheEnabled"};
          VPackBuilder props = VPackCollection::keep(body, keep);
          
          res = methods::Collections::updateProperties(coll, props.slice());
          if (res.ok()) {
            collectionRepresentation(builder, coll, /*showProperties*/ true,
                                     /*showFigures*/ false, /*showCount*/ false,
                                     /*aggregateCount*/ false);
          }

        } else if (sub == "rename") {
          VPackSlice const name = body.get("name");
          if (!name.isString()) {
            res = Result(TRI_ERROR_ARANGO_ILLEGAL_NAME, "name is empty");
            return;
          }

          res = methods::Collections::rename(coll, name.copyString(), false);
          if (res.ok()) {
            collectionRepresentation(builder, coll, /*showProperties*/ false,
                                     /*showFigures*/ false, /*showCount*/ false,
                                     /*aggregateCount*/ false);
          }

        } else if (sub == "rotate") {
          
          auto ctx = transaction::StandaloneContext::Create(_vocbase);
          SingleCollectionTransaction trx(ctx, coll->cid(), AccessMode::Type::READ);
          res = trx.begin();
          if (res.ok()) {
            res.reset(coll->getPhysical()->rotateActiveJournal());
          }
          
        } else if (sub == "loadIndexesIntoMemory") {
          res = methods::Collections::warmup(_vocbase, coll);
          VPackObjectBuilder obj(&builder, true);
          obj->add("result", VPackValue(res.ok()));
        } else {
          res.reset(TRI_ERROR_HTTP_NOT_FOUND,
                    "expecting one of the actions 'load', 'unload', 'truncate',"
                    " 'properties', 'rename', 'loadIndexesIntoMemory'");
        }
      });

  if (found.fail()) {
    generateError(found);
  } else if (res.ok()) {
    generateOk(rest::ResponseCode::OK, builder);
    _response->setHeader("location", _request->requestPath());
  } else {
    generateError(res);
  }
}

void RestCollectionHandler::handleCommandDelete() {
  std::vector<std::string> suffixes = _request->decodedSuffixes();
  if (suffixes.size() != 1) {
    generateError(rest::ResponseCode::BAD, TRI_ERROR_HTTP_BAD_PARAMETER,
                  "expected DELETE /_api/collection/<collection-name>");
    return;
  }

  std::string const name = suffixes[0];
  bool allowDropSystem = _request->parsedValue("isSystem", false);

  VPackBuilder builder;
  Result res;
  Result found = methods::Collections::lookup(
      _vocbase, name, [&](LogicalCollection* coll) {
        std::string cid = coll->cid_as_string();
        VPackObjectBuilder obj(&builder, true);
        obj->add("id", VPackValue(cid));
        res = methods::Collections::drop(_vocbase, coll, allowDropSystem, -1.0);
      });
  if (found.fail()) {
    generateError(found);
  } else if (res.fail()) {
    generateError(res);
  } else {
    generateOk(rest::ResponseCode::OK, builder);
  }
}

void RestCollectionHandler::collectionRepresentation(
    VPackBuilder& builder, LogicalCollection* coll, bool showProperties,
    bool showFigures, bool showCount, bool aggregateCount) {
  bool wasOpen = builder.isOpenObject();
  if (!wasOpen) {
    builder.openObject();
  }

  // `methods::Collections::properties` will filter these out
  builder.add("id", VPackValue(coll->cid_as_string()));
  builder.add("name", VPackValue(coll->name()));
  builder.add("status", VPackValue(coll->status()));
  builder.add("type", VPackValue(coll->type()));
  if (!showProperties) {
    builder.add("isSystem", VPackValue(coll->isSystem()));
    builder.add("globallyUniqueId", VPackValue(coll->globallyUniqueId()));
  } else {
    Result res = methods::Collections::properties(coll, builder);
    if (res.fail()) {
      THROW_ARANGO_EXCEPTION(res);
    }
  }

  if (showFigures) {
    auto figures = coll->figures();
    builder.add("figures", figures->slice());
  }

  if (showCount) {
    auto ctx = transaction::StandaloneContext::Create(_vocbase);
    SingleCollectionTransaction trx(ctx, coll->cid(), AccessMode::Type::READ);
    Result res = trx.begin();
    if (res.fail()) {
      THROW_ARANGO_EXCEPTION(res);
    }
    OperationResult opRes = trx.count(coll->name(), aggregateCount);
    trx.finish(opRes.code);
    if (!opRes.successful()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(opRes.code, opRes.errorMessage);
    }
    builder.add("count", opRes.slice());
  }

  if (!wasOpen) {
    builder.close();
  }
}
