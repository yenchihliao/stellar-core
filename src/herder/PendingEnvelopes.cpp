﻿#include "PendingEnvelopes.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "herder/HerderImpl.h"
#include "herder/HerderPersistence.h"
#include "herder/HerderUtils.h"
#include "herder/TxSetFrame.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/OverlayManager.h"
#include "scp/QuorumSetUtils.h"
#include "scp/Slot.h"
#include "util/Logging.h"
#include <unordered_set>
#include <xdrpp/marshal.h>

using namespace std;

#define QSET_CACHE_SIZE 10000
#define TXSET_CACHE_SIZE 10000

namespace stellar
{

PendingEnvelopes::PendingEnvelopes(Application& app, HerderImpl& herder)
	: mApp(app)
	, mHerder(herder)
	, mQsetCache(QSET_CACHE_SIZE)
	, mTxSetFetcher(
		  app, [](Peer::pointer peer, Hash hash) { peer->sendGetTxSet(hash); })
	, mQuorumSetFetcher(app, [](Peer::pointer peer,
								Hash hash) { peer->sendGetQuorumSet(hash); })
	, mTxSetCache(TXSET_CACHE_SIZE)
	, mRebuildQuorum(true)
	, mQuorumTracker(mHerder.getSCP())
	, mProcessedCount(
		  app.getMetrics().NewCounter({"scp", "pending", "processed"}))
	, mDiscardedCount(
		  app.getMetrics().NewCounter({"scp", "pending", "discarded"}))
	, mFetchingCount(
		  app.getMetrics().NewCounter({"scp", "pending", "fetching"}))
	, mReadyCount(app.getMetrics().NewCounter({"scp", "pending", "ready"}))
	, mFetchDuration(app.getMetrics().NewTimer({"scp", "fetch", "duration"}))
{
}

PendingEnvelopes::~PendingEnvelopes()
{
}

void
PendingEnvelopes::peerDoesntHave(MessageType type, Hash const& itemID,
								 Peer::pointer peer)
{
	switch (type)
	{
	case TX_SET:
		mTxSetFetcher.doesntHave(itemID, peer);
		break;
	case SCP_QUORUMSET:
		mQuorumSetFetcher.doesntHave(itemID, peer);
		break;
	default:
		CLOG(INFO, "Herder") << "Unknown Type in peerDoesntHave: " << type;
		break;
	}
}

SCPQuorumSetPtr
PendingEnvelopes::getKnownQSet(Hash const& hash, bool touch)
{
	SCPQuorumSetPtr res;
	auto it = mKnownQSets.find(hash);
	if (it != mKnownQSets.end())
	{
		res = it->second.lock();
	}

	// refresh the cache for this key
	if (res && touch)
	{
		mQsetCache.put(hash, res);
	}
	return res;
}

SCPQuorumSetPtr
PendingEnvelopes::putQSet(Hash const& qSetHash, SCPQuorumSet const& qSet)
{
	CLOG(TRACE, "Herder") << "Add SCPQSet " << hexAbbrev(qSetHash);
	SCPQuorumSetPtr res;
	assert(isQuorumSetSane(qSet, false));
	res = getKnownQSet(qSetHash, true);
	if (!res)
	{
		res = std::make_shared<SCPQuorumSet>(qSet);
		mKnownQSets[qSetHash] = res;
		mQsetCache.put(qSetHash, res);
	}
	return res;
}

void
PendingEnvelopes::addSCPQuorumSet(Hash const& hash, SCPQuorumSet const& q)
{
	putQSet(hash, q);
	mQuorumSetFetcher.recv(hash);
}

bool
PendingEnvelopes::recvSCPQuorumSet(Hash const& hash, SCPQuorumSet const& q)
{
	CLOG(TRACE, "Herder") << "Got SCPQSet " << hexAbbrev(hash);

	auto lastSeenSlotIndex = mQuorumSetFetcher.getLastSeenSlotIndex(hash);
	if (lastSeenSlotIndex == 0)
	{
		return false;
	}

	bool res = isQuorumSetSane(q, false);
	if (res)
	{
		addSCPQuorumSet(hash, q);
	}
	else
	{
		discardSCPEnvelopesWithQSet(hash);
	}
	return res;
}

void
PendingEnvelopes::discardSCPEnvelopesWithQSet(Hash const& hash)
{
	CLOG(TRACE, "Herder") << "Discarding SCP Envelopes with SCPQSet "
						  << hexAbbrev(hash);

	auto envelopes = mQuorumSetFetcher.fetchingFor(hash);
	for (auto& envelope : envelopes)
		discardSCPEnvelope(envelope);
}

void
PendingEnvelopes::updateMetrics()
{
	int64 processed = 0;
	int64 discarded = 0;
	int64 fetching = 0;
	int64 ready = 0;

	for (auto const& s : mEnvelopes)
	{
		auto& v = s.second;
		processed += v.mProcessedEnvelopes.size();
		discarded += v.mDiscardedEnvelopes.size();
		fetching += v.mFetchingEnvelopes.size();
		ready += v.mReadyEnvelopes.size();
	}
	mProcessedCount.set_count(processed);
	mDiscardedCount.set_count(discarded);
	mFetchingCount.set_count(fetching);
	mReadyCount.set_count(ready);
}

TxSetFramePtr
PendingEnvelopes::putTxSet(Hash const& hash, uint64 slot, TxSetFramePtr txset)
{
	auto res = getKnownTxSet(hash, slot, true);
	if (!res)
	{
		res = txset;
		mKnownTxSets[hash] = res;
		mTxSetCache.put(hash, std::make_pair(slot, res));
	}
	return res;
}

// tries to find a txset in memory, setting touch also touches the LRU,
// extending the lifetime of the result *and* updating the slot number
// to a greater value if needed
TxSetFramePtr
PendingEnvelopes::getKnownTxSet(Hash const& hash, uint64 slot, bool touch)
{
	// slot is only used when `touch` is set
	assert(touch || (slot == 0));
	TxSetFramePtr res;
	auto it = mKnownTxSets.find(hash);
	if (it != mKnownTxSets.end())
	{
		res = it->second.lock();
	}

	// refresh the cache for this key
	if (res && touch)
	{
		bool update = true;
		if (mTxSetCache.exists(hash))
		{
			auto& v = mTxSetCache.get(hash);
			update = (slot > v.first);
		}
		if (update)
		{
			mTxSetCache.put(hash, std::make_pair(slot, res));
		}
	}
	return res;
}

void
PendingEnvelopes::addTxSet(Hash const& hash, uint64 lastSeenSlotIndex,
						   TxSetFramePtr txset)
{
	CLOG(TRACE, "Herder") << "Add TxSet " << hexAbbrev(hash);

	putTxSet(hash, lastSeenSlotIndex, txset);
	mTxSetFetcher.recv(hash);
}

bool
PendingEnvelopes::recvTxSet(Hash const& hash, TxSetFramePtr txset)
{
	CLOG(TRACE, "Herder") << "Got TxSet " << hexAbbrev(hash);

	auto lastSeenSlotIndex = mTxSetFetcher.getLastSeenSlotIndex(hash);
	if (lastSeenSlotIndex == 0)
	{
		return false;
	}

	addTxSet(hash, lastSeenSlotIndex, txset);
	return true;
}

bool
PendingEnvelopes::isNodeDefinitelyInQuorum(NodeID const& node)
{
	if (mRebuildQuorum)
	{
		rebuildQuorumTrackerState();
		mRebuildQuorum = false;
	}
	return mQuorumTracker.isNodeDefinitelyInQuorum(node);
}

// called from Peer and when an Item tracker completes
Herder::EnvelopeStatus
PendingEnvelopes::recvSCPEnvelope(SCPEnvelope const& envelope)
{
	auto const& nodeID = envelope.statement.nodeID;
	if (!isNodeDefinitelyInQuorum(nodeID))
	{
		CLOG(DEBUG, "Herder")
			<< "Dropping envelope from "
			<< mApp.getConfig().toShortString(nodeID) << " (not in quorum)";
		return Herder::ENVELOPE_STATUS_DISCARDED;
	}

	// did we discard this envelope?
	// do we already have this envelope?
	// do we have the qset
	// do we have the txset

	try
	{
		if (isDiscarded(envelope))
		{
			return Herder::ENVELOPE_STATUS_DISCARDED;
		}

		touchFetchCache(envelope);

		auto& envs = mEnvelopes[envelope.statement.slotIndex];
		auto& fetching = envs.mFetchingEnvelopes;
		auto& processed = envs.mProcessedEnvelopes;

		auto fetchIt = fetching.find(envelope);

		if (fetchIt == fetching.end())
		{ // we aren't fetching this envelope
			if (processed.find(envelope) == processed.end())
			{ // we haven't seen this envelope before
				// insert it into the fetching set
				fetchIt =
					fetching.emplace(envelope, std::chrono::steady_clock::now())
						.first;
				startFetch(envelope);
				updateMetrics();
			}
			else
			{
				// we already have this one
				return Herder::ENVELOPE_STATUS_PROCESSED;
			}
		}

		// we are fetching this envelope
		// check if we are done fetching it
		if (isFullyFetched(envelope))
		{
			std::chrono::nanoseconds durationNano =
				std::chrono::steady_clock::now() - fetchIt->second;
			mFetchDuration.Update(durationNano);
			CLOG(TRACE, "Perf")
				<< "Herder fetched for "
				<< hexAbbrev(sha256(xdr::xdr_to_opaque(envelope))) << " in "
				<< std::chrono::duration<double>(durationNano).count()
				<< " seconds";

			// move the item from fetching to processed
			processed.emplace(envelope);
			fetching.erase(fetchIt);

			envelopeReady(envelope);
			updateMetrics();
			return Herder::ENVELOPE_STATUS_READY;
		} // else just keep waiting for it to come in

		return Herder::ENVELOPE_STATUS_FETCHING;
	}
	catch (xdr::xdr_runtime_error& e)
	{
		CLOG(TRACE, "Herder")
			<< "PendingEnvelopes::recvSCPEnvelope got corrupt message: "
			<< e.what();
		return Herder::ENVELOPE_STATUS_DISCARDED;
	}
}

void
PendingEnvelopes::discardSCPEnvelope(SCPEnvelope const& envelope)
{
	try
	{
		auto& envs = mEnvelopes[envelope.statement.slotIndex];
		auto& discardedSet = envs.mDiscardedEnvelopes;
		auto r = discardedSet.insert(envelope);

		if (!r.second)
		{
			return;
		}

		envs.mFetchingEnvelopes.erase(envelope);

		stopFetch(envelope);
	}
	catch (xdr::xdr_runtime_error& e)
	{
		CLOG(TRACE, "Herder")
			<< "PendingEnvelopes::discardSCPEnvelope got corrupt message: "
			<< e.what();
	}
	updateMetrics();
}

bool
PendingEnvelopes::isDiscarded(SCPEnvelope const& envelope) const
{
	auto envelopes = mEnvelopes.find(envelope.statement.slotIndex);
	if (envelopes == mEnvelopes.end())
	{
		return false;
	}

	auto& discardedSet = envelopes->second.mDiscardedEnvelopes;
	auto discarded = discardedSet.find(envelope);
	return discarded != discardedSet.end();
}

void
PendingEnvelopes::cleanKnownData()
{
	auto it = mKnownQSets.begin();
	while (it != mKnownQSets.end())
	{
		if (it->second.expired())
		{
			it = mKnownQSets.erase(it);
		}
		else
		{
			++it;
		}
	}
	auto it2 = mKnownTxSets.begin();
	while (it2 != mKnownTxSets.end())
	{
		if (it2->second.expired())
		{
			it2 = mKnownTxSets.erase(it2);
		}
		else
		{
			++it2;
		}
	}
}

#ifdef BUILD_TESTS
void
PendingEnvelopes::clearQSetCache()
{
	mQsetCache.clear();
	mKnownQSets.clear();
}
#endif

void
PendingEnvelopes::envelopeReady(SCPEnvelope const& envelope)
{
	CLOG(TRACE, "Herder") << "Envelope ready i:" << envelope.statement.slotIndex
						  << " t:" << envelope.statement.pledges.type();

	StellarMessage msg;
	msg.type(SCP_MESSAGE);
	msg.envelope() = envelope;
	mApp.getOverlayManager().broadcastMessage(msg);

	auto envW = mHerder.getHerderSCPDriver().wrapEnvelope(envelope);
	mEnvelopes[envelope.statement.slotIndex].mReadyEnvelopes.push_back(envW);
}

bool
PendingEnvelopes::isFullyFetched(SCPEnvelope const& envelope)
{
	if (!getKnownQSet(
			Slot::getCompanionQuorumSetHashFromStatement(envelope.statement),
			false))
	{
		return false;
	}

	auto txSetHashes = getTxSetHashes(envelope);
	return std::all_of(std::begin(txSetHashes), std::end(txSetHashes),
					   [&](Hash const& txSetHash) {
						   return getKnownTxSet(txSetHash, 0, false);
					   });
}

void
PendingEnvelopes::startFetch(SCPEnvelope const& envelope)
{
	Hash h = Slot::getCompanionQuorumSetHashFromStatement(envelope.statement);

	if (!getKnownQSet(h, false))
	{
		mQuorumSetFetcher.fetch(h, envelope);
	}

	for (auto const& h2 : getTxSetHashes(envelope))
	{
		if (!getKnownTxSet(h2, 0, false))
		{
			mTxSetFetcher.fetch(h2, envelope);
		}
	}

	CLOG(TRACE, "Herder") << "StartFetch i:" << envelope.statement.slotIndex
						  << " t:" << envelope.statement.pledges.type();
}

void
PendingEnvelopes::stopFetch(SCPEnvelope const& envelope)
{
	Hash h = Slot::getCompanionQuorumSetHashFromStatement(envelope.statement);
	mQuorumSetFetcher.stopFetch(h, envelope);

	for (auto const& h2 : getTxSetHashes(envelope))
	{
		mTxSetFetcher.stopFetch(h2, envelope);
	}

	CLOG(TRACE, "Herder") << "StopFetch i:" << envelope.statement.slotIndex
						  << " t:" << envelope.statement.pledges.type();
}

void
PendingEnvelopes::touchFetchCache(SCPEnvelope const& envelope)
{
	auto qsetHash =
		Slot::getCompanionQuorumSetHashFromStatement(envelope.statement);
	getKnownQSet(qsetHash, true);

	for (auto const& h : getTxSetHashes(envelope))
	{
		getKnownTxSet(h, envelope.statement.slotIndex, true);
	}
}

SCPEnvelopeWrapperPtr
PendingEnvelopes::pop(uint64 slotIndex)
{
	auto it = mEnvelopes.begin();
	while (it != mEnvelopes.end() && slotIndex >= it->first)
	{
		auto& v = it->second.mReadyEnvelopes;
		if (v.size() != 0)
		{
			auto ret = v.back();
			v.pop_back();

			updateMetrics();
			return ret;
		}
		it++;
	}
	return nullptr;
}

vector<uint64>
PendingEnvelopes::readySlots()
{
	vector<uint64> result;
	for (auto const& entry : mEnvelopes)
	{
		if (!entry.second.mReadyEnvelopes.empty())
			result.push_back(entry.first);
	}
	return result;
}

void
PendingEnvelopes::eraseBelow(uint64 slotIndex)
{
	for (auto iter = mEnvelopes.begin(); iter != mEnvelopes.end();)
	{
		if (iter->first < slotIndex)
		{
			iter = mEnvelopes.erase(iter);
		}
		else
			break;
	}

	// 0 is special mark for data that we do not know the slot index
	// it is used for state loaded from database
	mTxSetCache.erase_if([&](TxSetFramCacheItem const& i) {
		return i.first != 0 && i.first < slotIndex;
	});

	updateMetrics();
}

void
PendingEnvelopes::slotClosed(uint64 slotIndex)
{
	// force recomputing the transitive quorum
	mRebuildQuorum = true;

	// stop processing envelopes & downloads for the slot falling off the
	// window
	auto maxSlots = mApp.getConfig().MAX_SLOTS_TO_REMEMBER;
	if (slotIndex > maxSlots)
	{
		slotIndex -= maxSlots;

		mEnvelopes.erase(slotIndex);

		mTxSetFetcher.stopFetchingBelow(slotIndex + 1);
		mQuorumSetFetcher.stopFetchingBelow(slotIndex + 1);

		mTxSetCache.erase_if(
			[&](TxSetFramCacheItem const& i) { return i.first == slotIndex; });
	}

	cleanKnownData();
	updateMetrics();
}

TxSetFramePtr
PendingEnvelopes::getTxSet(Hash const& hash)
{
	return getKnownTxSet(hash, 0, false);
}

SCPQuorumSetPtr
PendingEnvelopes::getQSet(Hash const& hash)
{
	auto qset = getKnownQSet(hash, false);
	if (qset)
	{
		return qset;
	}
	// if it was not known, see if we can find it somewhere else
	auto& scp = mHerder.getSCP();
	if (hash == scp.getLocalNode()->getQuorumSetHash())
	{
		qset = make_shared<SCPQuorumSet>(scp.getLocalQuorumSet());
	}
	else
	{
		auto& db = mApp.getDatabase();
		qset = HerderPersistence::getQuorumSet(db, db.getSession(), hash);
	}
	if (qset)
	{
		qset = putQSet(hash, *qset);
	}
	return qset;
}

Json::Value
PendingEnvelopes::getJsonInfo(size_t limit)
{
	Json::Value ret;

	updateMetrics();

	auto& scp = mHerder.getSCP();
	{
		auto it = mEnvelopes.rbegin();
		size_t l = limit;
		while (it != mEnvelopes.rend() && l-- != 0)
		{
			if (it->second.mFetchingEnvelopes.size() != 0)
			{
				Json::Value& slot = ret[std::to_string(it->first)]["fetching"];
				for (auto const& kv : it->second.mFetchingEnvelopes)
				{
					slot.append(scp.envToStr(kv.first));
				}
			}
			if (it->second.mReadyEnvelopes.size() != 0)
			{
				Json::Value& slot = ret[std::to_string(it->first)]["pending"];
				for (auto const& e : it->second.mReadyEnvelopes)
				{
					slot.append(scp.envToStr(e->getEnvelope()));
				}
			}
			it++;
		}
	}
	return ret;
}

void
PendingEnvelopes::rebuildQuorumTrackerState()
{
	// rebuild quorum information using data sources starting with the
	// freshest source
	mQuorumTracker.rebuild([&](NodeID const& id) -> SCPQuorumSetPtr {
		SCPQuorumSetPtr res;
		if (id == mHerder.getSCP().getLocalNodeID())
		{
			res = getQSet(mHerder.getSCP().getLocalNode()->getQuorumSetHash());
		}
		else
		{
			auto m = mHerder.getSCP().getLatestMessage(id);
			if (m != nullptr)
			{
				auto h =
					Slot::getCompanionQuorumSetHashFromStatement(m->statement);
				res = getQSet(h);
			}
			if (res == nullptr)
			{
				// see if we had some information for that node
				auto& db = mApp.getDatabase();
				auto h = HerderPersistence::getNodeQuorumSet(
					db, db.getSession(), id);
				if (h)
				{
					res = getQSet(*h);
				}
			}
		}
		return res;
	});
}

QuorumTracker::QuorumMap const&
PendingEnvelopes::getCurrentlyTrackedQuorum() const
{
	return mQuorumTracker.getQuorum();
}

void
PendingEnvelopes::envelopeProcessed(SCPEnvelope const& env)
{
	auto const& st = env.statement;
	auto const& id = st.nodeID;

	auto h = Slot::getCompanionQuorumSetHashFromStatement(st);

	SCPQuorumSetPtr qset = getQSet(h);
	if (!mQuorumTracker.expand(id, qset))
	{
		// could not expand quorum, queue up a rebuild
		mRebuildQuorum = true;
	}
}
}
