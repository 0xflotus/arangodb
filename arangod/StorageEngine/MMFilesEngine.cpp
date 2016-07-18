////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "MMFilesEngine.h"
#include "Basics/FileUtils.h"
#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/files.h"
#include "RestServer/DatabasePathFeature.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "VocBase/server.h"
#include "VocBase/vocbase.h"

#ifdef ARANGODB_ENABLE_ROCKSDB
#include "Indexes/RocksDBIndex.h"
#endif

using namespace arangodb;

std::string const MMFilesEngine::EngineName("mmfiles");

/// @brief extract the numeric part from a filename
static uint64_t GetNumericFilenamePart(char const* filename) {
  char const* pos = strrchr(filename, '-');

  if (pos == nullptr) {
    return 0;
  }

  return basics::StringUtils::uint64(pos + 1);
}

/// @brief compare two filenames, based on the numeric part contained in
/// the filename. this is used to sort database filenames on startup
static bool DatabaseIdStringComparator(std::string const& lhs, std::string const& rhs) {
  return GetNumericFilenamePart(lhs.c_str()) < GetNumericFilenamePart(rhs.c_str());
}

// create the storage engine
MMFilesEngine::MMFilesEngine(application_features::ApplicationServer* server)
    : StorageEngine(server, "mmfilesEngine"), 
      _iterateMarkersOnOpen(true),
      _isUpgrade(false) {
}

MMFilesEngine::~MMFilesEngine() {
}

// add the storage engine's specifc options to the global list of options
void MMFilesEngine::collectOptions(std::shared_ptr<options::ProgramOptions>) {
}
  
// validate the storage engine's specific options
void MMFilesEngine::validateOptions(std::shared_ptr<options::ProgramOptions>) {
}

// preparation phase for storage engine. can be used for internal setup.
// the storage engine must not start any threads here or write any files
void MMFilesEngine::prepare() {
  TRI_ASSERT(EngineSelectorFeature::ENGINE = this);

  LOG(INFO) << "MMFilesEngine::prepare()";
 
  // get base path from DatabaseServerFeature 
  auto database = application_features::ApplicationServer::getFeature<DatabasePathFeature>("DatabasePath");
  _basePath = database->directory();
  TRI_ASSERT(!_basePath.empty());

  _databasePath = basics::FileUtils::buildFilename(_basePath, "databases") + TRI_DIR_SEPARATOR_CHAR;
}

// start the engine. now it's allowed to start engine-specific threads,
// write files etc.
void MMFilesEngine::start() {
  TRI_ASSERT(EngineSelectorFeature::ENGINE = this);
  
  LOG(INFO) << "MMFilesEngine::start()";

  // test if the "databases" directory is present and writable
  verifyDirectories();
  
  int res = TRI_ERROR_NO_ERROR;
  
  // get names of all databases
  std::vector<std::string> names(getDatabaseNames());

  if (names.empty()) {
    // no databases found, i.e. there is no system database!
    // create a database for the system database
    res = createDatabaseDirectory(TRI_NewTickServer(), TRI_VOC_SYSTEM_DATABASE);
    _iterateMarkersOnOpen = false;
  }

  if (res != TRI_ERROR_NO_ERROR) {
    LOG(ERR) << "unable to initialize databases: " << TRI_errno_string(res);
    THROW_ARANGO_EXCEPTION(res);
  }

  // ...........................................................................
  // open and scan all databases
  // ...........................................................................

  // scan all databases
  res = openDatabases();

  if (res != TRI_ERROR_NO_ERROR) {
    LOG(ERR) << "could not iterate over all databases: "  << TRI_errno_string(res);
    THROW_ARANGO_EXCEPTION(res);
  }
}

// stop the storage engine. this can be used to flush all data to disk,
// shutdown threads etc. it is guaranteed that there will be no read and
// write requests to the storage engine after this call
void MMFilesEngine::stop() {
  TRI_ASSERT(EngineSelectorFeature::ENGINE = this);
  
  LOG(INFO) << "MMFilesEngine::stop()";
}

// fill the Builder object with an array of databases that were detected
// by the storage engine. this method must sort out databases that were not
// fully created (see "createDatabase" below). called at server start only
void MMFilesEngine::getDatabases(arangodb::velocypack::Builder& result) {
}

// fill the Builder object with an array of collections (and their corresponding
// indexes) that were detected by the storage engine. called at server start only
void MMFilesEngine::getCollectionsAndIndexes(arangodb::velocypack::Builder& result) {
}

// determine the maximum revision id previously handed out by the storage
// engine. this value is used as a lower bound for further HLC values handed out by
// the server. called at server start only, after getDatabases() and getCollectionsAndIndexes()
uint64_t MMFilesEngine::getMaxRevision() {
  return 0; // TODO
}

// asks the storage engine to create a database as specified in the VPack
// Slice object and persist the creation info. It is guaranteed by the server that 
// no other active database with the same name and id exists when this function
// is called. If this operation fails somewhere in the middle, the storage 
// engine is required to fully clean up the creation and throw only then, 
// so that subsequent database creation requests will not fail.
// the WAL entry for the database creation will be written *after* the call
// to "createDatabase" returns
void MMFilesEngine::createDatabase(TRI_voc_tick_t id, arangodb::velocypack::Slice const& data) {
}

// asks the storage engine to drop the specified database and persist the 
// deletion info. Note that physical deletion of the database data must not 
// be carried out by this call, as there may
// still be readers of the database's data. It is recommended that this operation
// only sets a deletion flag for the database but let's an async task perform
// the actual deletion. The async task can later call the callback function to 
// check whether the physical deletion of the database is possible.
// the WAL entry for database deletion will be written *after* the call
// to "dropDatabase" returns
void MMFilesEngine::dropDatabase(TRI_voc_tick_t id, std::function<bool()> const& canRemovePhysically) {
}

// asks the storage engine to create a collection as specified in the VPack
// Slice object and persist the creation info. It is guaranteed by the server 
// that no other active collection with the same name and id exists in the same
// database when this function is called. If this operation fails somewhere in 
// the middle, the storage engine is required to fully clean up the creation 
// and throw only then, so that subsequent collection creation requests will not fail.
// the WAL entry for the collection creation will be written *after* the call
// to "createCollection" returns
void MMFilesEngine::createCollection(TRI_voc_tick_t databaseId, TRI_voc_cid_t id,
                                     arangodb::velocypack::Slice const& data) {
}

// asks the storage engine to drop the specified collection and persist the 
// deletion info. Note that physical deletion of the collection data must not 
// be carried out by this call, as there may
// still be readers of the collection's data. It is recommended that this operation
// only sets a deletion flag for the collection but let's an async task perform
// the actual deletion.
// the WAL entry for collection deletion will be written *after* the call
// to "dropCollection" returns
void MMFilesEngine::dropCollection(TRI_voc_tick_t databaseId, TRI_voc_cid_t id, 
                                   std::function<bool()> const& canRemovePhysically) {
}

// asks the storage engine to rename the collection as specified in the VPack
// Slice object and persist the renaming info. It is guaranteed by the server 
// that no other active collection with the same name and id exists in the same
// database when this function is called. If this operation fails somewhere in 
// the middle, the storage engine is required to fully revert the rename operation
// and throw only then, so that subsequent collection creation/rename requests will 
// not fail. the WAL entry for the rename will be written *after* the call
// to "renameCollection" returns
void MMFilesEngine::renameCollection(TRI_voc_tick_t databaseId, TRI_voc_cid_t id,
                                     arangodb::velocypack::Slice const& data) {
}

// asks the storage engine to change properties of the collection as specified in 
// the VPack Slice object and persist them. If this operation fails 
// somewhere in the middle, the storage engine is required to fully revert the 
// property changes and throw only then, so that subsequent operations will not fail.
// the WAL entry for the propery change will be written *after* the call
// to "changeCollection" returns
void MMFilesEngine::changeCollection(TRI_voc_tick_t databaseId, TRI_voc_cid_t id,
                                     arangodb::velocypack::Slice const& data) {
}

// asks the storage engine to create an index as specified in the VPack
// Slice object and persist the creation info. The database id, collection id 
// and index data are passed in the Slice object. Note that this function
// is not responsible for inserting the individual documents into the index.
// If this operation fails somewhere in the middle, the storage engine is required 
// to fully clean up the creation and throw only then, so that subsequent index 
// creation requests will not fail.
// the WAL entry for the index creation will be written *after* the call
// to "createIndex" returns
void MMFilesEngine::createIndex(TRI_voc_tick_t databaseId, TRI_voc_cid_t collectionId,
                                TRI_idx_iid_t id, arangodb::velocypack::Slice const& data) {
}

// asks the storage engine to drop the specified index and persist the deletion 
// info. Note that physical deletion of the index must not be carried out by this call, 
// as there may still be users of the index. It is recommended that this operation
// only sets a deletion flag for the index but let's an async task perform
// the actual deletion.
// the WAL entry for index deletion will be written *after* the call
// to "dropIndex" returns
void MMFilesEngine::dropIndex(TRI_voc_tick_t databaseId, TRI_voc_cid_t collectionId,
                              TRI_idx_iid_t id) {
}

// iterate all documents of the underlying collection
// this is called when a collection is openend, and all its documents need to be added to
// indexes etc.
void MMFilesEngine::iterateDocuments(TRI_voc_tick_t databaseId, TRI_voc_cid_t collectionId,
                                     std::function<void(arangodb::velocypack::Slice const&)> const& cb) {
}

// adds a document to the storage engine
// this will be called by the WAL collector when surviving documents are being moved
// into the storage engine's realm
void MMFilesEngine::addDocumentRevision(TRI_voc_tick_t databaseId, TRI_voc_cid_t collectionId,
                                        arangodb::velocypack::Slice const& document) {
}

// removes a document from the storage engine
// this will be called by the WAL collector when non-surviving documents are being removed
// from the storage engine's realm
void MMFilesEngine::removeDocumentRevision(TRI_voc_tick_t databaseId, TRI_voc_cid_t collectionId,
                                           arangodb::velocypack::Slice const& document) {
}
  
void MMFilesEngine::verifyDirectories() {
  if (!TRI_IsDirectory(_basePath.c_str())) {
    LOG(ERR) << "database path '" << _basePath << "' is not a directory";

    THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DATADIR_INVALID);
  }

  if (!TRI_IsWritable(_basePath.c_str())) {
    // database directory is not writable for the current user... bad luck
    LOG(ERR) << "database directory '" << _basePath
             << "' is not writable for current user";

    THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DATADIR_NOT_WRITABLE);
  }

  // ...........................................................................
  // verify existence of "databases" subdirectory
  // ...........................................................................

  if (!TRI_IsDirectory(_databasePath.c_str())) {
    long systemError;
    std::string errorMessage;
    int res = TRI_CreateDirectory(_databasePath.c_str(), systemError, errorMessage);

    if (res != TRI_ERROR_NO_ERROR) {
      LOG(ERR) << "unable to create database directory '"
               << _databasePath << "': " << errorMessage;

      THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DATADIR_NOT_WRITABLE);
    }
  }

  if (!TRI_IsWritable(_databasePath.c_str())) {
    LOG(ERR) << "database directory '" << _databasePath << "' is not writable";

    THROW_ARANGO_EXCEPTION(TRI_ERROR_ARANGO_DATADIR_NOT_WRITABLE);
  }
}

/// @brief get the names of all databases 
std::vector<std::string> MMFilesEngine::getDatabaseNames() const {
  std::vector<std::string> databases;

  for (auto const& name : TRI_FilesDirectory(_databasePath.c_str())) {
    TRI_ASSERT(!name.empty());

    if (!basics::StringUtils::isPrefix(name, "database-")) {
      // found some other file
      continue;
    }

    // found a database name
    std::string const dname(arangodb::basics::FileUtils::buildFilename(_databasePath, name));

    if (TRI_IsDirectory(dname.c_str())) {
      databases.emplace_back(name);
    }
  }

  // sort by id
  std::sort(databases.begin(), databases.end(), DatabaseIdStringComparator);

  return databases;
}

/// @brief create a new database directory 
int MMFilesEngine::createDatabaseDirectory(TRI_voc_tick_t id,
                                           std::string const& name) {
  std::string const dirname = databaseDirectory(id);

  // use a temporary directory first. otherwise, if creation fails, the server
  // might be left with an empty database directory at restart, and abort.

  std::string const tmpname(dirname + ".tmp");

  if (TRI_IsDirectory(tmpname.c_str())) {
    TRI_RemoveDirectory(tmpname.c_str());
  }

  std::string errorMessage;
  long systemError;

  int res = TRI_CreateDirectory(tmpname.c_str(), systemError, errorMessage);

  if (res != TRI_ERROR_NO_ERROR) {
    if (res != TRI_ERROR_FILE_EXISTS) {
      LOG(ERR) << "failed to create database directory: " << errorMessage;
    }
    return res;
  }

  TRI_IF_FAILURE("CreateDatabase::tempDirectory") { return TRI_ERROR_DEBUG; }

  std::string const tmpfile(
      arangodb::basics::FileUtils::buildFilename(tmpname, ".tmp"));
  res = TRI_WriteFile(tmpfile.c_str(), "", 0);

  TRI_IF_FAILURE("CreateDatabase::tempFile") { return TRI_ERROR_DEBUG; }

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_RemoveDirectory(tmpname.c_str());
    return res;
  }

  // finally rename
  res = TRI_RenameFile(tmpname.c_str(), dirname.c_str());

  TRI_IF_FAILURE("CreateDatabase::renameDirectory") { return TRI_ERROR_DEBUG; }

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_RemoveDirectory(tmpname.c_str());  // clean up
    return res;
  }

  // now everything is valid

  res = saveDatabaseParameters(id, name, false);

  if (res != TRI_ERROR_NO_ERROR) {
    return res;
  }

  // finally remove the .tmp file
  {
    std::string const tmpfile(arangodb::basics::FileUtils::buildFilename(dirname, ".tmp"));
    TRI_UnlinkFile(tmpfile.c_str());
  }

  return TRI_ERROR_NO_ERROR;
}

/// @brief save a parameter.json file for a database
int MMFilesEngine::saveDatabaseParameters(TRI_voc_tick_t id, 
                                          std::string const& name,
                                          bool deleted) {
  TRI_ASSERT(id > 0);
  TRI_ASSERT(!name.empty());

  VPackBuilder builder = databaseToVelocyPack(id, name, deleted);
  std::string const file = parametersFile(id);

  if (!arangodb::basics::VelocyPackHelper::velocyPackToFile(
          file.c_str(), builder.slice(), true)) {
    LOG(ERR) << "cannot save database information in file '" << file << "'";
    return TRI_ERROR_INTERNAL;
  }

  return TRI_ERROR_NO_ERROR;
}
  
VPackBuilder MMFilesEngine::databaseToVelocyPack(TRI_voc_tick_t id, 
                                                 std::string const& name, 
                                                 bool deleted) const {
  TRI_ASSERT(id > 0);
  TRI_ASSERT(!name.empty());

  VPackBuilder builder;
  builder.openObject();
  builder.add("id", VPackValue(std::to_string(id)));
  builder.add("name", VPackValue(name));
  builder.add("deleted", VPackValue(deleted));
  builder.close();

  return builder;
}

std::string MMFilesEngine::databaseDirectory(TRI_voc_tick_t id) const {
  return _databasePath + "database-" + std::to_string(id);
}

std::string MMFilesEngine::parametersFile(TRI_voc_tick_t id) const {
  return basics::FileUtils::buildFilename(databaseDirectory(id), TRI_VOC_PARAMETER_FILE);
}

/// @brief iterate over all databases in the databases directory and open them
int MMFilesEngine::openDatabases() {
  if (_iterateMarkersOnOpen) {
    LOG(WARN) << "no shutdown info found. scanning datafiles for last tick...";
  }

  // open databases in defined order
  std::vector<std::string> files = TRI_FilesDirectory(_databasePath.c_str());
  std::sort(files.begin(), files.end(), DatabaseIdStringComparator);

  for (auto const& name : files) {
    TRI_ASSERT(!name.empty());
    
    TRI_voc_tick_t id = GetNumericFilenamePart(name.c_str());

    if (id == 0) {
      // invalid id
      continue;
    }

    // construct and validate path
    std::string const directory(basics::FileUtils::buildFilename(_databasePath, name));

    if (!TRI_IsDirectory(directory.c_str())) {
      continue;
    }

    if (!basics::StringUtils::isPrefix(name, "database-") ||
        basics::StringUtils::isSuffix(name, ".tmp")) {
      LOG_TOPIC(TRACE, Logger::DATAFILES) << "ignoring file '" << name << "'";
      continue;
    }

    // we have a directory...

    if (!TRI_IsWritable(directory.c_str())) {
      // the database directory we found is not writable for the current user
      // this can cause serious trouble so we will abort the server start if we
      // encounter this situation
      LOG(ERR) << "database directory '" << directory
               << "' is not writable for current user";

      return TRI_ERROR_ARANGO_DATADIR_NOT_WRITABLE;
    }

    // we have a writable directory...
    std::string const tmpfile(basics::FileUtils::buildFilename(directory, ".tmp"));

    if (TRI_ExistsFile(tmpfile.c_str())) {
      // still a temporary... must ignore
      LOG(TRACE) << "ignoring temporary directory '" << tmpfile << "'";
      continue;
    }

    // a valid database directory

    // now read data from parameter.json file
    std::string const file = parametersFile(id);

    if (!TRI_ExistsFile(file.c_str())) {
      // no parameter.json file
      
      if (TRI_FilesDirectory(directory.c_str()).empty()) {
        // directory is otherwise empty, continue!
        LOG(WARN) << "ignoring empty database directory '" << directory
                  << "' without parameters file";
        continue;
      } 
        
      // abort
      LOG(ERR) << "database directory '" << directory
               << "' does not contain parameters file or parameters file cannot be read";
      return TRI_ERROR_ARANGO_ILLEGAL_PARAMETER_FILE;
    }

    LOG(DEBUG) << "reading database parameters from file '" << file << "'";
    std::shared_ptr<VPackBuilder> builder;
    try {
      builder = arangodb::basics::VelocyPackHelper::velocyPackFromFile(file);
    } catch (...) {
      LOG(ERR) << "database directory '" << directory
               << "' does not contain a valid parameters file";

      // abort
      return TRI_ERROR_ARANGO_ILLEGAL_PARAMETER_FILE;
    }

    VPackSlice parameters = builder->slice();
    std::string const parametersString = parameters.toJson();

    LOG(DEBUG) << "database parameters: " << parametersString;
      
    VPackSlice idSlice = parameters.get("id");
    
    if (!idSlice.isString() ||
        id != static_cast<TRI_voc_tick_t>(basics::StringUtils::uint64(idSlice.copyString()))) {
      LOG(ERR) << "database directory '" << directory
               << "' does not contain a valid parameters file";
      return TRI_ERROR_ARANGO_ILLEGAL_PARAMETER_FILE;
    }
    
    if (arangodb::basics::VelocyPackHelper::getBooleanValue(parameters, "deleted", false)) {
      // database is deleted, skip it!
      LOG(DEBUG) << "found dropped database in directory '" << directory << "'";
      LOG(DEBUG) << "removing superfluous database directory '" << directory << "'";

#ifdef ARANGODB_ENABLE_ROCKSDB
      // delete persistent indexes for this database
      TRI_voc_tick_t id = static_cast<TRI_voc_tick_t>(
          basics::StringUtils::uint64(idSlice.copyString()));
      RocksDBFeature::dropDatabase(id);
#endif

      TRI_RemoveDirectory(directory.c_str());
      continue;
    }

    VPackSlice nameSlice = parameters.get("name");

    if (!nameSlice.isString()) {
      LOG(ERR) << "database directory '" << directory << "' does not contain a valid parameters file";

      return TRI_ERROR_ARANGO_ILLEGAL_PARAMETER_FILE;
    }

    std::string const databaseName = nameSlice.copyString();

    // use defaults

    // .........................................................................
    // open the database and scan collections in it
    // .........................................................................
/*
    // try to open this database
    TRI_vocbase_t* vocbase = TRI_OpenVocBase(
        server, databaseDirectory.c_str(), id, databaseName.c_str(),
        _isUpgrade, _iterateMarkersOnOpen);

    if (vocbase == nullptr) {
      // grab last error
      int res = TRI_errno();

      if (res == TRI_ERROR_NO_ERROR) {
        // but we must have an error...
        res = TRI_ERROR_INTERNAL;
      }

      LOG(ERR) << "could not process database directory '" << directory
               << "' for database '" << name << "': " << TRI_errno_string(res);
      return res;
    }
*/
  }

  return TRI_ERROR_NO_ERROR;
}
