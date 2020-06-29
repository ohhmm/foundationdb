/*
 * SpecialKeySpace.actor.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2020 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#if defined(NO_INTELLISENSE) && !defined(FDBCLIENT_SPECIALKEYSPACE_ACTOR_G_H)
#define FDBCLIENT_SPECIALKEYSPACE_ACTOR_G_H
#include "fdbclient/SpecialKeySpace.actor.g.h"
#elif !defined(FDBCLIENT_SPECIALKEYSPACE_ACTOR_H)
#define FDBCLIENT_SPECIALKEYSPACE_ACTOR_H

#include "flow/flow.h"
#include "flow/Arena.h"
#include "fdbclient/FDBTypes.h"
#include "fdbclient/KeyRangeMap.h"
#include "fdbclient/ReadYourWrites.h"
#include "flow/actorcompiler.h" // This must be the last #include.

class SpecialKeyRangeReadImpl {
public:
	// Each derived class only needs to implement this simple version of getRange
	virtual Future<Standalone<RangeResultRef>> getRange(ReadYourWritesTransaction* ryw, KeyRangeRef kr) const = 0;

	explicit SpecialKeyRangeReadImpl(KeyRangeRef kr) : range(kr) {}
	KeyRangeRef getKeyRange() const { return range; }
	// true if the getRange call can emit more than one rpc calls,
	// we cache the results to keep consistency in the same getrange lifetime
	// TODO : give this function a more descriptive name
	virtual bool isAsync() const { return false; }

	virtual ~SpecialKeyRangeReadImpl() {}

protected:
	KeyRange range; // underlying key range for this function
};

class ManagementAPIError {
public:
	static std::string toJsonString(bool retriable, std::string command, std::string msg) {
		json_spirit::mObject errorObj;
		errorObj["Retriable"] = retriable;
		errorObj["Command"] = command;
		errorObj["Message"] = msg;
		return json_spirit::write_string(json_spirit::mValue(errorObj), json_spirit::Output_options::raw_utf8);
	}

private:
	ManagementAPIError(){};
};

class SpecialKeyRangeRWImpl : public SpecialKeyRangeReadImpl {
public:
	virtual void set(ReadYourWritesTransaction* ryw, const KeyRef& key, const ValueRef& value) = 0;
	virtual void clear(ReadYourWritesTransaction* ryw, const KeyRangeRef& range) = 0;
	virtual void clear(ReadYourWritesTransaction* ryw, const KeyRef& key) = 0;
	virtual Future<Optional<std::string>> commit(
	    ReadYourWritesTransaction* ryw) = 0; // all delayed async operations of writes in special-key-space

	explicit SpecialKeyRangeRWImpl(KeyRangeRef kr) : SpecialKeyRangeReadImpl(kr) {}
	KeyRangeRef getKeyRange() const { return range; }

	virtual ~SpecialKeyRangeRWImpl() {}

protected:
	KeyRange range;
};

class SpecialKeyRangeAsyncImpl : public SpecialKeyRangeReadImpl {
public:
	explicit SpecialKeyRangeAsyncImpl(KeyRangeRef kr) : SpecialKeyRangeReadImpl(kr) {}

	Future<Standalone<RangeResultRef>> getRange(ReadYourWritesTransaction* ryw, KeyRangeRef kr) const = 0;

	// calling with a cache object to have consistent results if we need to call rpc
	Future<Standalone<RangeResultRef>> getRange(ReadYourWritesTransaction* ryw, KeyRangeRef kr,
	                                            Optional<Standalone<RangeResultRef>>* cache) const {
		return getRangeAsyncActor(this, ryw, kr, cache);
	}

	bool isAsync() const override { return true; }

	ACTOR static Future<Standalone<RangeResultRef>> getRangeAsyncActor(const SpecialKeyRangeReadImpl* skrAyncImpl,
	                                                                   ReadYourWritesTransaction* ryw, KeyRangeRef kr,
	                                                                   Optional<Standalone<RangeResultRef>>* cache) {
		ASSERT(skrAyncImpl->getKeyRange().contains(kr));
		ASSERT(cache != nullptr);
		if (!cache->present()) {
			// For simplicity, every time we need to cache, we read the whole range
			// Although sometimes the range can be narrowed,
			// there is not a general way to do it in complicated scenarios
			Standalone<RangeResultRef> result_ = wait(skrAyncImpl->getRange(ryw, skrAyncImpl->getKeyRange()));
			*cache = result_;
		}
		const auto& allResults = cache->get();
		int start = 0, end = allResults.size();
		while (start < allResults.size() && allResults[start].key < kr.begin) ++start;
		while (end > 0 && allResults[end - 1].key >= kr.end) --end;
		if (start < end) {
			Standalone<RangeResultRef> result = RangeResultRef(allResults.slice(start, end), false);
			result.arena().dependsOn(allResults.arena());
			return result;
		} else
			return Standalone<RangeResultRef>();
	}
};

class SpecialKeySpace {
public:
	enum class MODULE {
		CLUSTERFILEPATH,
		CONNECTIONSTRING,
		FAILURE, // A single key space contains a json string which describes the last failure in special-key-space
		MANAGEMENT, // Management-API
		METRICS, // data-distribution metrics
		TESTONLY, // only used by correctness tests
		TRANSACTION, // transaction related info, conflicting keys, read/write conflict range
		STATUSJSON,
		UNKNOWN, // default value for all unregistered range
		WORKERINTERFACE,
	};

	SpecialKeySpace(KeyRef spaceStartKey = Key(), KeyRef spaceEndKey = normalKeys.end, bool testOnly = true)
	  : range(KeyRangeRef(spaceStartKey, spaceEndKey)), readImpls(nullptr, spaceEndKey),
	    writeImpls(nullptr, spaceEndKey),
	    modules(testOnly ? SpecialKeySpace::MODULE::TESTONLY : SpecialKeySpace::MODULE::UNKNOWN, spaceEndKey) {
		// Default begin of KeyRangeMap is Key(), insert the range to update start key
		readImpls.insert(range, nullptr);
		writeImpls.insert(range, nullptr);
		if (!testOnly) modulesBoundaryInit(); // testOnly is used in the correctness workload
	}

	Future<Optional<Value>> get(ReadYourWritesTransaction* ryw, const Key& key);

	Future<Standalone<RangeResultRef>> getRange(ReadYourWritesTransaction* ryw, KeySelector begin, KeySelector end,
	                                            GetRangeLimits limits, bool reverse = false);

	Future<Void> commit(ReadYourWritesTransaction* ryw);

	void set(ReadYourWritesTransaction* ryw, const KeyRef& key, const ValueRef& value) {
		auto impl = writeImpls[key];
		if (impl == nullptr) throw special_keys_no_module_found(); // TODO : change the error type here
		return impl->set(ryw, key, value);
	}
	// void clear(ReadYourWritesTransaction* ryw, const KeyRangeRef& range );
	void clear(ReadYourWritesTransaction* ryw, const KeyRef& key) {
		auto impl = writeImpls[key];
		if (impl == nullptr) throw special_keys_no_module_found(); // TODO : change the error type here
		return impl->clear(ryw, key);
	}
	// TODO : do we need to move it to .cpp file
	void registerKeyRange(SpecialKeySpace::MODULE module, const KeyRangeRef& kr, SpecialKeyRangeReadImpl* impl,
	                      bool rw = false) {
		// module boundary check
		if (module == SpecialKeySpace::MODULE::TESTONLY)
			ASSERT(normalKeys.contains(kr))
		else
			ASSERT(moduleToBoundary.at(module).contains(kr));
		// make sure the registered range is not overlapping with existing ones
		// Note: kr.end should not be the same as another range's begin, although it should work even they are the same
		for (auto iter = readImpls.rangeContaining(kr.begin); true; ++iter) {
			ASSERT(iter->value() == nullptr);
			if (iter == readImpls.rangeContaining(kr.end))
				break; // Note: relax the condition that the end can be another range's start, if needed
		}
		readImpls.insert(kr, impl);
		// if rw, it means the module can do both read and write
		if (rw) {
			// since write impls are always subset of read impls,
			// no need to check overlapped registration
			auto rwImpl = dynamic_cast<SpecialKeyRangeRWImpl*>(impl);
			ASSERT(rwImpl);
			writeImpls.insert(kr, rwImpl);
		}
	}

	KeyRangeMap<SpecialKeyRangeReadImpl*>& getReadImpls() { return readImpls; }
	KeyRangeMap<SpecialKeyRangeRWImpl*>& getRWImpls() { return writeImpls; }
	KeyRangeMap<SpecialKeySpace::MODULE>& getModules() { return modules; }
	KeyRangeRef getKeyRange() const { return range; }

private:
	ACTOR static Future<Optional<Value>> getActor(SpecialKeySpace* sks, ReadYourWritesTransaction* ryw, KeyRef key);

	ACTOR static Future<Standalone<RangeResultRef>> checkRYWValid(SpecialKeySpace* sks, ReadYourWritesTransaction* ryw,
	                                                              KeySelector begin, KeySelector end,
	                                                              GetRangeLimits limits, bool reverse);
	ACTOR static Future<Standalone<RangeResultRef>> getRangeAggregationActor(SpecialKeySpace* sks,
	                                                                         ReadYourWritesTransaction* ryw,
	                                                                         KeySelector begin, KeySelector end,
	                                                                         GetRangeLimits limits, bool reverse);

	KeyRangeMap<SpecialKeyRangeReadImpl*> readImpls;
	KeyRangeMap<SpecialKeySpace::MODULE> modules;
	KeyRangeMap<SpecialKeyRangeRWImpl*> writeImpls;
	KeyRange range; // key space range, (\xff\xff, \xff\xff\xff\xf) in prod and (, \xff) in test

	static std::unordered_map<SpecialKeySpace::MODULE, KeyRange> moduleToBoundary;

	// Initialize module boundaries, used to handle cross_module_read
	void modulesBoundaryInit() {
		for (const auto& pair : moduleToBoundary) {
			ASSERT(range.contains(pair.second));
			// Make sure the module is not overlapping with any registered read modules
			// Note: same like ranges, one module's end cannot be another module's start, relax the condition if needed
			ASSERT(modules.rangeContaining(pair.second.begin) == modules.rangeContaining(pair.second.end) &&
			       modules[pair.second.begin] == SpecialKeySpace::MODULE::UNKNOWN);
			modules.insert(pair.second, pair.first);
			// Note: Due to underlying implementation, the insertion here is important to make cross_module_read being
			// handled correctly
			readImpls.insert(pair.second, nullptr);
			writeImpls.insert(pair.second, nullptr);
		}
	}
};

// Use special key prefix "\xff\xff/transaction/conflicting_keys/<some_key>",
// to retrieve keys which caused latest not_committed(conflicting with another transaction) error.
// The returned key value pairs are interpretted as :
// prefix/<key1> : '1' - any keys equal or larger than this key are (probably) conflicting keys
// prefix/<key2> : '0' - any keys equal or larger than this key are (definitely) not conflicting keys
// Currently, the conflicting keyranges returned are original read_conflict_ranges or union of them.
class ConflictingKeysImpl : public SpecialKeyRangeReadImpl {
public:
	explicit ConflictingKeysImpl(KeyRangeRef kr);
	Future<Standalone<RangeResultRef>> getRange(ReadYourWritesTransaction* ryw, KeyRangeRef kr) const override;
};

class ReadConflictRangeImpl : public SpecialKeyRangeReadImpl {
public:
	explicit ReadConflictRangeImpl(KeyRangeRef kr);
	Future<Standalone<RangeResultRef>> getRange(ReadYourWritesTransaction* ryw, KeyRangeRef kr) const override;
};

class WriteConflictRangeImpl : public SpecialKeyRangeReadImpl {
public:
	explicit WriteConflictRangeImpl(KeyRangeRef kr);
	Future<Standalone<RangeResultRef>> getRange(ReadYourWritesTransaction* ryw, KeyRangeRef kr) const override;
};

class DDStatsRangeImpl : public SpecialKeyRangeAsyncImpl {
public:
	explicit DDStatsRangeImpl(KeyRangeRef kr);
	Future<Standalone<RangeResultRef>> getRange(ReadYourWritesTransaction* ryw, KeyRangeRef kr) const override;
};

class ExcludeServersRangeImpl : public SpecialKeyRangeRWImpl {
public:
	explicit ExcludeServersRangeImpl(KeyRangeRef kr);
	Future<Standalone<RangeResultRef>> getRange(ReadYourWritesTransaction* ryw, KeyRangeRef kr) const override;
	void set(ReadYourWritesTransaction* ryw, const KeyRef& key, const ValueRef& value) override;
	void clear(ReadYourWritesTransaction* ryw, const KeyRangeRef& range) override;
	void clear(ReadYourWritesTransaction* ryw, const KeyRef& key) override;
	Future<Optional<std::string>> commit(ReadYourWritesTransaction* ryw) override;
};

#include "flow/unactorcompiler.h"
#endif
