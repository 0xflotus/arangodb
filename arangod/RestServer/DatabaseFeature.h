////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016 ArangoDB GmbH, Cologne, Germany
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
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#ifndef APPLICATION_FEATURES_DATABASE_FEATURE_H
#define APPLICATION_FEATURES_DATABASE_FEATURE_H 1

#include "ApplicationFeatures/ApplicationFeature.h"
#include "Basics/DataProtector.h"
#include "Basics/Mutex.h"
#include "Basics/Thread.h"

struct TRI_vocbase_t;
struct TRI_server_t;

namespace arangodb {
class DatabaseManagerThread;

namespace aql {
class QueryRegistry;
}

/// @brief databases list structure
struct DatabasesLists {
  std::unordered_map<std::string, TRI_vocbase_t*> _databases;
  std::unordered_map<std::string, TRI_vocbase_t*> _coordinatorDatabases;
  std::unordered_set<TRI_vocbase_t*> _droppedDatabases;
};

class DatabaseManagerThread : public Thread {
 public:
  DatabaseManagerThread(DatabaseManagerThread const&) = delete;
  DatabaseManagerThread& operator=(DatabaseManagerThread const&) = delete;

  DatabaseManagerThread(); 
  ~DatabaseManagerThread();

  void run() override;
};

class DatabaseFeature final : public application_features::ApplicationFeature {
 friend class DatabaseManagerThread;

 public:
  static DatabaseFeature* DATABASE;

 public:
  explicit DatabaseFeature(application_features::ApplicationServer* server);
  ~DatabaseFeature();

 public:
  void collectOptions(std::shared_ptr<options::ProgramOptions>) override final;
  void validateOptions(std::shared_ptr<options::ProgramOptions>) override final;
  void prepare() override final;
  void start() override final;
  void unprepare() override final;

 public:
  TRI_vocbase_t* systemDatabase() const { return _vocbase; }
  bool ignoreDatafileErrors() const { return _ignoreDatafileErrors; }
  bool isInitiallyEmpty() const { return _isInitiallyEmpty; }
  bool checkVersion() const { return _checkVersion; }
  bool forceSyncProperties() const { return _forceSyncProperties; }
  void forceSyncProperties(bool value) { _forceSyncProperties = value; }
  bool waitForSync() const { return _defaultWaitForSync; }
  uint64_t maximalJournalSize() const { return _maximalJournalSize; }

  void disableReplicationApplier() { _replicationApplier = false; }
  void disableCompactor() { _disableCompactor = true; }
  void enableCheckVersion() { _checkVersion = true; }
  void enableUpgrade() { _upgrade = true; }
 
 public:
  static TRI_server_t* SERVER;
 
 private:
  void openDatabases();
  void closeDatabases();
  void updateContexts();
  void shutdownCompactor();

  /// @brief create base app directory
  int createBaseApplicationDirectory(std::string const& appPath, std::string const& type);
  
  /// @brief create app subdirectory for a database
  int createApplicationDirectory(std::string const& name, std::string const& basePath);

  /// @brief iterate over all databases in the databases directory and open them
  int iterateDatabases();

  /// @brief close all opened databases
  void closeOpenDatabases();

  /// @brief close all dropped databases
  void closeDroppedDatabases();

 private:
  uint64_t _maximalJournalSize;
  bool _defaultWaitForSync;
  bool _forceSyncProperties;
  bool _ignoreDatafileErrors;
  bool _throwCollectionNotLoadedError;

  std::unique_ptr<TRI_server_t> _server; // TODO
  TRI_vocbase_t* _vocbase;
  std::atomic<arangodb::aql::QueryRegistry*> _queryRegistry; // TODO
  DatabaseManagerThread* _databaseManager;

  std::atomic<DatabasesLists*> _databasesLists; // TODO
  // TODO: Make this again a template once everybody has gcc >= 4.9.2
  // arangodb::basics::DataProtector<64>
  arangodb::basics::DataProtector _databasesProtector;
  arangodb::Mutex _databasesMutex;

  bool _isInitiallyEmpty;
  bool _replicationApplier;
  bool _disableCompactor;
  bool _checkVersion;
  bool _iterateMarkersOnOpen;
  bool _upgrade;

 public:
  static uint32_t const DefaultIndexBuckets;
};
}

#endif
