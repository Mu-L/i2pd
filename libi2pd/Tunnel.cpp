/*
* Copyright (c) 2013-2025, The PurpleI2P Project
*
* This file is part of Purple i2pd project and licensed under BSD3
*
* See full license text in LICENSE file at top of project tree
*/

#include <string.h>
#include "I2PEndian.h"
#include <random>
#include <thread>
#include <algorithm>
#include <vector>
#include "Crypto.h"
#include "RouterContext.h"
#include "Log.h"
#include "Timestamp.h"
#include "I2NPProtocol.h"
#include "Transports.h"
#include "NetDb.hpp"
#include "Config.h"
#include "Tunnel.h"
#include "TunnelPool.h"
#include "util.h"
#include "ECIESX25519AEADRatchetSession.h"

namespace i2p
{
namespace tunnel
{
	Tunnel::Tunnel (std::shared_ptr<const TunnelConfig> config):
		TunnelBase (config->GetTunnelID (), config->GetNextTunnelID (), config->GetNextIdentHash ()),
		m_Config (config), m_IsShortBuildMessage (false), m_Pool (nullptr),
		m_State (eTunnelStatePending), m_FarEndTransports (i2p::data::RouterInfo::eAllTransports),
		m_IsRecreated (false), m_Latency (UNKNOWN_LATENCY)
	{
	}

	Tunnel::~Tunnel ()
	{
	}

	void Tunnel::Build (uint32_t replyMsgID, std::shared_ptr<OutboundTunnel> outboundTunnel)
	{
		auto numHops = m_Config->GetNumHops ();
		const int numRecords = numHops <= STANDARD_NUM_RECORDS ? STANDARD_NUM_RECORDS : MAX_NUM_RECORDS;
		auto msg = numRecords <= STANDARD_NUM_RECORDS ? NewI2NPShortMessage () : NewI2NPMessage ();
		*msg->GetPayload () = numRecords;
		const size_t recordSize = m_Config->IsShort () ? SHORT_TUNNEL_BUILD_RECORD_SIZE : TUNNEL_BUILD_RECORD_SIZE;
		msg->len += numRecords*recordSize + 1;
		// shuffle records
		std::vector<int> recordIndicies;
		for (int i = 0; i < numRecords; i++) recordIndicies.push_back(i);
		std::shuffle (recordIndicies.begin(), recordIndicies.end(), m_Pool ? m_Pool->GetRng () : std::mt19937(std::random_device()()));

		// create real records
		uint8_t * records = msg->GetPayload () + 1;
		TunnelHopConfig * hop = m_Config->GetFirstHop ();
		int i = 0;
		while (hop)
		{
			uint32_t msgID;
			if (hop->next) // we set replyMsgID for last hop only
				RAND_bytes ((uint8_t *)&msgID, 4);
			else
				msgID = replyMsgID;
			hop->recordIndex = recordIndicies[i]; i++;
			hop->CreateBuildRequestRecord (records, msgID);
			hop = hop->next;
		}
		// fill up fake records with random data
		for (int i = numHops; i < numRecords; i++)
		{
			int idx = recordIndicies[i];
			RAND_bytes (records + idx*recordSize, recordSize);
		}

		// decrypt real records
		hop = m_Config->GetLastHop ()->prev;
		while (hop)
		{
			// decrypt records after current hop
			TunnelHopConfig * hop1 = hop->next;
			while (hop1)
			{
				hop->DecryptRecord (records, hop1->recordIndex);
				hop1 = hop1->next;
			}
			hop = hop->prev;
		}
		msg->FillI2NPMessageHeader (m_Config->IsShort () ? eI2NPShortTunnelBuild : eI2NPVariableTunnelBuild);
		auto s = shared_from_this ();
		msg->onDrop = [s]()
			{
				LogPrint (eLogInfo, "I2NP: Tunnel ", s->GetTunnelID (), " request was not sent");
				s->SetState (i2p::tunnel::eTunnelStateBuildFailed);		
			};
		
		// send message
		if (outboundTunnel)
		{
			if (m_Config->IsShort ())
			{
				auto ident = m_Config->GetFirstHop () ? m_Config->GetFirstHop ()->ident : nullptr;
				if (ident && ident->GetIdentHash () != outboundTunnel->GetEndpointIdentHash ()) // don't encrypt if IBGW = OBEP
				{
					auto msg1 = i2p::garlic::WrapECIESX25519MessageForRouter (msg, ident->GetEncryptionPublicKey ());
					if (msg1) msg = msg1;
				}
			}
			outboundTunnel->SendTunnelDataMsgTo (GetNextIdentHash (), 0, msg);
		}
		else
		{
			if (m_Config->IsShort () && m_Config->GetLastHop () &&
				m_Config->GetLastHop ()->ident->GetIdentHash () != m_Config->GetLastHop ()->nextIdent)
			{
				// add garlic key/tag for reply
				uint8_t key[32];
				uint64_t tag = m_Config->GetLastHop ()->GetGarlicKey (key);
				if (m_Pool && m_Pool->GetLocalDestination ())
					m_Pool->GetLocalDestination ()->SubmitECIESx25519Key (key, tag);
				else
					i2p::context.SubmitECIESx25519Key (key, tag);
			}
			i2p::transport::transports.SendMessage (GetNextIdentHash (), msg);
		}
	}

	bool Tunnel::HandleTunnelBuildResponse (uint8_t * msg, size_t len)
	{
		int num = msg[0];
		LogPrint (eLogDebug, "Tunnel: TunnelBuildResponse ", num, " records.");
		if (num > MAX_NUM_RECORDS)
		{
			LogPrint (eLogError, "Tunnel: Too many records in TunnelBuildResponse", num);
			return false;
		}		
		if (len < num*m_Config->GetRecordSize () + 1)
		{
			LogPrint (eLogError, "Tunnel: TunnelBuildResponse of ", num, " records is too short ", len);
			return false;
		}
		
		TunnelHopConfig * hop = m_Config->GetLastHop ();
		while (hop)
		{
			// decrypt current hop
			if (hop->recordIndex >= 0 && hop->recordIndex < msg[0])
			{
				if (!hop->DecryptBuildResponseRecord (msg + 1))
					return false;
			}
			else
			{
				LogPrint (eLogWarning, "Tunnel: Hop index ", hop->recordIndex, " is out of range");
				return false;
			}

			// decrypt records before current hop
			TunnelHopConfig * hop1 = hop->prev;
			while (hop1)
			{
				auto idx = hop1->recordIndex;
				if (idx >= 0 && idx < num)
					hop->DecryptRecord (msg + 1, idx);
				else
					LogPrint (eLogWarning, "Tunnel: Hop index ", idx, " is out of range");
				hop1 = hop1->prev;
			}
			hop = hop->prev;
		}

		bool established = true;
		size_t numHops = 0;
		hop = m_Config->GetFirstHop ();
		while (hop)
		{
			uint8_t ret = hop->GetRetCode (msg + 1);
			LogPrint (eLogDebug, "Tunnel: Build response ret code=", (int)ret);
			if (hop->ident)
				i2p::data::UpdateRouterProfile (hop->ident->GetIdentHash (),
					[ret](std::shared_ptr<i2p::data::RouterProfile> profile)
					{
						if (profile) profile->TunnelBuildResponse (ret);
					});
			if (ret)
				// if any of participants declined the tunnel is not established
				established = false;
			hop = hop->next;
			numHops++;
		}
		if (established)
		{
			// create tunnel decryptions from layer and iv keys in reverse order
			m_Hops.resize (numHops);
			hop = m_Config->GetLastHop ();
			int i = 0;
			while (hop)
			{
				m_Hops[i].ident = hop->ident;
				m_Hops[i].decryption.SetKeys (hop->layerKey, hop->ivKey);
				hop = hop->prev;
				i++;
			}
			m_IsShortBuildMessage = m_Config->IsShort ();
			m_FarEndTransports = m_Config->GetFarEndTransports ();
			m_Config = nullptr;
		}
		if (established) m_State = eTunnelStateEstablished;
		return established;
	}

	bool Tunnel::LatencyFitsRange(int lowerbound, int upperbound) const
	{
		auto latency = GetMeanLatency();
		return latency >= lowerbound && latency <= upperbound;
	}

	void Tunnel::EncryptTunnelMsg (std::shared_ptr<const I2NPMessage> in, std::shared_ptr<I2NPMessage> out)
	{
		const uint8_t * inPayload = in->GetPayload () + 4;
		uint8_t * outPayload = out->GetPayload () + 4;
		for (auto& it: m_Hops)
		{
			it.decryption.Decrypt (inPayload, outPayload);
			inPayload = outPayload;
		}
	}

	void Tunnel::SendTunnelDataMsg (std::shared_ptr<i2p::I2NPMessage> msg)
	{
		LogPrint (eLogWarning, "Tunnel: Can't send I2NP messages without delivery instructions");
	}

	std::vector<std::shared_ptr<const i2p::data::IdentityEx> > Tunnel::GetPeers () const
	{
		auto peers = GetInvertedPeers ();
		std::reverse (peers.begin (), peers.end ());
		return peers;
	}

	std::vector<std::shared_ptr<const i2p::data::IdentityEx> > Tunnel::GetInvertedPeers () const
	{
		// hops are in inverted order
		std::vector<std::shared_ptr<const i2p::data::IdentityEx> > ret;
		for (const auto& it: m_Hops)
			ret.push_back (it.ident);
		return ret;
	}

	void Tunnel::SetState(TunnelState state)
	{
		m_State = state;
	}

	void Tunnel::VisitTunnelHops(TunnelHopVisitor v)
	{
		// hops are in inverted order, we must return in direct order
		for (auto it = m_Hops.rbegin (); it != m_Hops.rend (); it++)
			v((*it).ident);
	}

	void InboundTunnel::HandleTunnelDataMsg (std::shared_ptr<I2NPMessage>&& msg)
	{
		if (!IsEstablished () && GetState () != eTunnelStateExpiring) 
		{	
			// incoming messages means a tunnel is alive
			SetState (eTunnelStateEstablished); 
			auto pool = GetTunnelPool ();
			if (pool)
			{
				// update LeaseSet
				auto dest = pool->GetLocalDestination ();
				if (dest) dest->SetLeaseSetUpdated (true);
			}	
		}	
		EncryptTunnelMsg (msg, msg);
		msg->from = GetSharedFromThis ();
		m_Endpoint.HandleDecryptedTunnelDataMsg (msg);
	}

	bool InboundTunnel::Recreate ()
	{
		if (!IsRecreated ())
		{
			auto pool = GetTunnelPool ();
			if (pool)
			{
				SetRecreated (true);
				pool->RecreateInboundTunnel (std::static_pointer_cast<InboundTunnel>(shared_from_this ()));
				return true;
			}	
		}
		return false;
	}	
		
	ZeroHopsInboundTunnel::ZeroHopsInboundTunnel ():
		InboundTunnel (std::make_shared<ZeroHopsTunnelConfig> ()),
		m_NumReceivedBytes (0)
	{
	}

	void ZeroHopsInboundTunnel::SendTunnelDataMsg (std::shared_ptr<i2p::I2NPMessage> msg)
	{
		if (msg)
		{
			m_NumReceivedBytes += msg->GetLength ();
			msg->from = GetSharedFromThis ();
			HandleI2NPMessage (msg);
		}
	}

	void OutboundTunnel::SendTunnelDataMsgTo (const uint8_t * gwHash, uint32_t gwTunnel, std::shared_ptr<i2p::I2NPMessage> msg)
	{
		TunnelMessageBlock block;
		if (gwHash)
		{
			block.hash = gwHash;
			if (gwTunnel)
			{
				block.deliveryType = eDeliveryTypeTunnel;
				block.tunnelID = gwTunnel;
			}
			else
				block.deliveryType = eDeliveryTypeRouter;
		}
		else
			block.deliveryType = eDeliveryTypeLocal;
		block.data = msg;

		SendTunnelDataMsgs ({block});
	}

	void OutboundTunnel::SendTunnelDataMsgs (const std::vector<TunnelMessageBlock>& msgs)
	{
		std::unique_lock<std::mutex> l(m_SendMutex);
		for (auto& it : msgs)
			m_Gateway.PutTunnelDataMsg (it);
		m_Gateway.SendBuffer ();
	}

	void OutboundTunnel::HandleTunnelDataMsg (std::shared_ptr<i2p::I2NPMessage>&& tunnelMsg)
	{
		LogPrint (eLogError, "Tunnel: Incoming message for outbound tunnel ", GetTunnelID ());
	}

	bool OutboundTunnel::Recreate ()
	{
		if (!IsRecreated ())
		{
			auto pool = GetTunnelPool ();
			if (pool)
			{
				SetRecreated (true);
				pool->RecreateOutboundTunnel (std::static_pointer_cast<OutboundTunnel>(shared_from_this ()));
				return true;
			}	
		}
		return false;
	}
		
	ZeroHopsOutboundTunnel::ZeroHopsOutboundTunnel ():
		OutboundTunnel (std::make_shared<ZeroHopsTunnelConfig> ()),
		m_NumSentBytes (0)
	{
	}

	void ZeroHopsOutboundTunnel::SendTunnelDataMsgs (const std::vector<TunnelMessageBlock>& msgs)
	{
		for (auto& msg : msgs)
		{
			if (!msg.data) continue;
			m_NumSentBytes += msg.data->GetLength ();
			switch (msg.deliveryType)
			{
				case eDeliveryTypeLocal:
					HandleI2NPMessage (msg.data);
				break;
				case eDeliveryTypeTunnel:
					i2p::transport::transports.SendMessage (msg.hash, i2p::CreateTunnelGatewayMsg (msg.tunnelID, msg.data));
				break;
				case eDeliveryTypeRouter:
					i2p::transport::transports.SendMessage (msg.hash, msg.data);
				break;
				default:
					LogPrint (eLogError, "Tunnel: Unknown delivery type ", (int)msg.deliveryType);
			}
		}
	}

	Tunnels tunnels;

	Tunnels::Tunnels (): m_IsRunning (false), m_Thread (nullptr), m_MaxNumTransitTunnels (DEFAULT_MAX_NUM_TRANSIT_TUNNELS),
		m_TotalNumSuccesiveTunnelCreations (0), m_TotalNumFailedTunnelCreations (0), // for normal average
		m_TunnelCreationSuccessRate (TCSR_START_VALUE), m_TunnelCreationAttemptsNum(0),
		m_Rng(i2p::util::GetMonotonicMicroseconds ()%1000000LL)
	{
	}

	Tunnels::~Tunnels ()
	{
		DeleteTunnelPool(m_ExploratoryPool);
	}

	std::shared_ptr<TunnelBase> Tunnels::GetTunnel (uint32_t tunnelID)
	{
		std::lock_guard<std::mutex> l(m_TunnelsMutex);
		auto it = m_Tunnels.find(tunnelID);
		if (it != m_Tunnels.end ())
			return it->second;
		return nullptr;
	}

	bool Tunnels::AddTunnel (std::shared_ptr<TunnelBase> tunnel)
	{
		if (!tunnel) return false;
		std::lock_guard<std::mutex> l(m_TunnelsMutex);
		return m_Tunnels.emplace (tunnel->GetTunnelID (), tunnel).second;
	}
		
	void Tunnels::RemoveTunnel (uint32_t tunnelID)
	{
		std::lock_guard<std::mutex> l(m_TunnelsMutex);
		m_Tunnels.erase (tunnelID);
	}	
		
	std::shared_ptr<InboundTunnel> Tunnels::GetPendingInboundTunnel (uint32_t replyMsgID)
	{
		return GetPendingTunnel (replyMsgID, m_PendingInboundTunnels);
	}

	std::shared_ptr<OutboundTunnel> Tunnels::GetPendingOutboundTunnel (uint32_t replyMsgID)
	{
		return GetPendingTunnel (replyMsgID, m_PendingOutboundTunnels);
	}

	template<class TTunnel>
	std::shared_ptr<TTunnel> Tunnels::GetPendingTunnel (uint32_t replyMsgID, const std::map<uint32_t, std::shared_ptr<TTunnel> >& pendingTunnels)
	{
		auto it = pendingTunnels.find(replyMsgID);
		if (it != pendingTunnels.end () && it->second->GetState () == eTunnelStatePending)
		{
			it->second->SetState (eTunnelStateBuildReplyReceived);
			return it->second;
		}
		return nullptr;
	}

	std::shared_ptr<InboundTunnel> Tunnels::GetNextInboundTunnel ()
	{
		std::shared_ptr<InboundTunnel> tunnel;
		size_t minReceived = 0;
		for (const auto& it : m_InboundTunnels)
		{
			if (!it->IsEstablished ()) continue;
			if (!tunnel || it->GetNumReceivedBytes () < minReceived)
			{
				tunnel = it;
				minReceived = it->GetNumReceivedBytes ();
			}
		}
		return tunnel;
	}

	std::shared_ptr<OutboundTunnel> Tunnels::GetNextOutboundTunnel ()
	{
		if (m_OutboundTunnels.empty ()) return nullptr;
		uint32_t ind = m_Rng () % m_OutboundTunnels.size (), i = 0;
		std::shared_ptr<OutboundTunnel> tunnel;
		for (const auto& it: m_OutboundTunnels)
		{
			if (it->IsEstablished ())
			{
				tunnel = it;
				i++;
			}
			if (i > ind && tunnel) break;
		}
		return tunnel;
	}

	std::shared_ptr<TunnelPool> Tunnels::CreateTunnelPool (int numInboundHops, 
	    int numOutboundHops, int numInboundTunnels, int numOutboundTunnels, 
	    int inboundVariance, int outboundVariance, bool isHighBandwidth)
	{
		auto pool = std::make_shared<TunnelPool> (numInboundHops, numOutboundHops, 
			numInboundTunnels, numOutboundTunnels, inboundVariance, outboundVariance, isHighBandwidth);
		std::unique_lock<std::mutex> l(m_PoolsMutex);
		m_Pools.push_back (pool);
		return pool;
	}

	void Tunnels::DeleteTunnelPool (std::shared_ptr<TunnelPool> pool)
	{
		if (pool)
		{
			StopTunnelPool (pool);
			{
				std::unique_lock<std::mutex> l(m_PoolsMutex);
				m_Pools.remove (pool);
			}
		}
	}

	void Tunnels::StopTunnelPool (std::shared_ptr<TunnelPool> pool)
	{
		if (pool)
		{
			pool->SetActive (false);
			pool->DetachTunnels ();
		}
	}

	void Tunnels::Start ()
	{
		m_IsRunning = true;
		m_Thread = new std::thread (std::bind (&Tunnels::Run, this));
		m_TransitTunnels.Start ();
	}

	void Tunnels::Stop ()
	{
		m_TransitTunnels.Stop ();
		m_IsRunning = false;
		m_Queue.WakeUp ();
		if (m_Thread)
		{
			m_Thread->join ();
			delete m_Thread;
			m_Thread = 0;
		}
	}

	void Tunnels::Run ()
	{
		i2p::util::SetThreadName("Tunnels");
		std::this_thread::sleep_for (std::chrono::seconds(1)); // wait for other parts are ready

		uint64_t lastTs = 0, lastPoolsTs = 0, lastMemoryPoolTs = 0;
		std::list<std::shared_ptr<I2NPMessage> > msgs;
		while (m_IsRunning)
		{
			try
			{
				if (m_Queue.Wait (1,0)) // 1 sec
				{
					m_Queue.GetWholeQueue (msgs);
					int numMsgs = 0;
					uint32_t prevTunnelID = 0, tunnelID = 0;
					std::shared_ptr<TunnelBase> prevTunnel;
					while (!msgs.empty ())
					{
						auto msg = msgs.front (); msgs.pop_front ();
						if (!msg) continue;
						std::shared_ptr<TunnelBase> tunnel;
						uint8_t typeID = msg->GetTypeID ();
						switch (typeID)
						{
							case eI2NPTunnelData:
							case eI2NPTunnelGateway:
							{
								tunnelID = bufbe32toh (msg->GetPayload ());
								if (tunnelID == prevTunnelID)
									tunnel = prevTunnel;
								else if (prevTunnel)
									prevTunnel->FlushTunnelDataMsgs ();

								if (!tunnel)
									tunnel = GetTunnel (tunnelID);
								if (tunnel)
								{
									if (typeID == eI2NPTunnelData)
										tunnel->HandleTunnelDataMsg (std::move (msg));
									else // tunnel gateway assumed
										HandleTunnelGatewayMsg (tunnel, msg);
								}
								else
									LogPrint (eLogWarning, "Tunnel: Tunnel not found, tunnelID=", tunnelID, " previousTunnelID=", prevTunnelID, " type=", (int)typeID);

								break;
							}
							case eI2NPShortTunnelBuild:
								HandleShortTunnelBuildMsg (msg);
							break;	
							case eI2NPVariableTunnelBuild:
								HandleVariableTunnelBuildMsg (msg);
							break;	
							case eI2NPShortTunnelBuildReply:
								HandleTunnelBuildReplyMsg (msg, true);
							break;
							case eI2NPVariableTunnelBuildReply:
								HandleTunnelBuildReplyMsg (msg, false);
							break;	
							case eI2NPTunnelBuild:
							case eI2NPTunnelBuildReply:
								LogPrint (eLogWarning, "Tunnel: TunnelBuild is too old for ECIES router");
							break;	
							default:
								LogPrint (eLogWarning, "Tunnel: Unexpected message type ", (int) typeID);
						}

						prevTunnelID = tunnelID;
						prevTunnel = tunnel;
						numMsgs++;	
						
						if (msgs.empty ())
						{	
							if (numMsgs < MAX_TUNNEL_MSGS_BATCH_SIZE && !m_Queue.IsEmpty ())
								m_Queue.GetWholeQueue (msgs); // try more
							else if (tunnel)
								tunnel->FlushTunnelDataMsgs (); // otherwise flush last
						}	
					}
				}

				if (i2p::transport::transports.IsOnline())
				{
					uint64_t ts = i2p::util::GetSecondsSinceEpoch ();
					if (ts - lastTs >= TUNNEL_MANAGE_INTERVAL || // manage tunnels every 15 seconds
					    ts + TUNNEL_MANAGE_INTERVAL < lastTs)
					{
						ManageTunnels (ts);
						lastTs = ts;
					}
					if (ts - lastPoolsTs >= TUNNEL_POOLS_MANAGE_INTERVAL || // manage pools every 5 seconds
					    ts + TUNNEL_POOLS_MANAGE_INTERVAL < lastPoolsTs)
					{
						ManageTunnelPools (ts);
						lastPoolsTs = ts;
					}
					if (ts - lastMemoryPoolTs >= TUNNEL_MEMORY_POOL_MANAGE_INTERVAL ||
					    ts + TUNNEL_MEMORY_POOL_MANAGE_INTERVAL < lastMemoryPoolTs) // manage memory pool every 2 minutes
					{
						m_I2NPTunnelEndpointMessagesMemoryPool.CleanUpMt ();
						m_I2NPTunnelMessagesMemoryPool.CleanUpMt ();
						lastMemoryPoolTs = ts;
					}
				}
			}
			catch (std::exception& ex)
			{
				LogPrint (eLogError, "Tunnel: Runtime exception: ", ex.what ());
			}
		}
	}

	void Tunnels::HandleTunnelGatewayMsg (std::shared_ptr<TunnelBase> tunnel, std::shared_ptr<I2NPMessage> msg)
	{
		if (!tunnel)
		{
			LogPrint (eLogError, "Tunnel: Missing tunnel for gateway");
			return;
		}
		const uint8_t * payload = msg->GetPayload ();
		uint16_t len = bufbe16toh(payload + TUNNEL_GATEWAY_HEADER_LENGTH_OFFSET);
		// we make payload as new I2NP message to send
		msg->offset += I2NP_HEADER_SIZE + TUNNEL_GATEWAY_HEADER_SIZE;
		if (msg->offset + len > msg->len)
		{
			LogPrint (eLogError, "Tunnel: Gateway payload ", (int)len, " exceeds message length ", (int)msg->len);
			return;
		}
		msg->len = msg->offset + len;
		auto typeID = msg->GetTypeID ();
		LogPrint (eLogDebug, "Tunnel: Gateway of ", (int) len, " bytes for tunnel ", tunnel->GetTunnelID (), ", msg type ", (int)typeID);

		tunnel->SendTunnelDataMsg (msg);
	}

	void Tunnels::HandleShortTunnelBuildMsg (std::shared_ptr<I2NPMessage> msg)
	{
		if (!msg) return;
		auto tunnel = GetPendingInboundTunnel (msg->GetMsgID()); // replyMsgID
		if (tunnel)
		{
			// endpoint of inbound tunnel
			LogPrint (eLogDebug, "Tunnel: ShortTunnelBuild reply for tunnel ", tunnel->GetTunnelID ());
			if (tunnel->HandleTunnelBuildResponse (msg->GetPayload(), msg->GetPayloadLength()))
			{
				LogPrint (eLogInfo, "Tunnel: Inbound tunnel ", tunnel->GetTunnelID (), " has been created");
				tunnel->SetState (eTunnelStateEstablished);
				AddInboundTunnel (tunnel);
			}
			else
			{
				LogPrint (eLogInfo, "Tunnel: Inbound tunnel ", tunnel->GetTunnelID (), " has been declined");
				tunnel->SetState (eTunnelStateBuildFailed);
			}
			return;
		}
		else
			m_TransitTunnels.PostTransitTunnelBuildMsg (std::move (msg));
	}	

	void Tunnels::HandleVariableTunnelBuildMsg (std::shared_ptr<I2NPMessage> msg)
	{
		auto tunnel = GetPendingInboundTunnel (msg->GetMsgID()); // replyMsgID
		if (tunnel)
		{
			// endpoint of inbound tunnel
			LogPrint (eLogDebug, "Tunnel: VariableTunnelBuild reply for tunnel ", tunnel->GetTunnelID ());
			if (tunnel->HandleTunnelBuildResponse (msg->GetPayload(), msg->GetPayloadLength()))
			{
				LogPrint (eLogInfo, "Tunnel: Inbound tunnel ", tunnel->GetTunnelID (), " has been created");
				tunnel->SetState (eTunnelStateEstablished);
				AddInboundTunnel (tunnel);
			}
			else
			{
				LogPrint (eLogInfo, "Tunnel: Inbound tunnel ", tunnel->GetTunnelID (), " has been declined");
				tunnel->SetState (eTunnelStateBuildFailed);
			}
		}
		else
			m_TransitTunnels.PostTransitTunnelBuildMsg (std::move (msg));
	}	

	void Tunnels::HandleTunnelBuildReplyMsg (std::shared_ptr<I2NPMessage> msg, bool isShort)
	{
		auto tunnel = GetPendingOutboundTunnel (msg->GetMsgID()); // replyMsgID
		if (tunnel)
		{
			// reply for outbound tunnel
			LogPrint (eLogDebug, "Tunnel: TunnelBuildReply for tunnel ", tunnel->GetTunnelID ());
			if (tunnel->HandleTunnelBuildResponse (msg->GetPayload(), msg->GetPayloadLength()))
			{
				LogPrint (eLogInfo, "Tunnel: Outbound tunnel ", tunnel->GetTunnelID (), " has been created");
				tunnel->SetState (eTunnelStateEstablished);
				AddOutboundTunnel (tunnel);
			}
			else
			{
				LogPrint (eLogInfo, "Tunnel: Outbound tunnel ", tunnel->GetTunnelID (), " has been declined");
				tunnel->SetState (eTunnelStateBuildFailed);
			}
		}
		else
			LogPrint (eLogWarning, "Tunnel: Pending tunnel for message ", msg->GetMsgID(), " not found");

	}	
		
	void Tunnels::ManageTunnels (uint64_t ts)
	{
		ManagePendingTunnels (ts);
		std::vector<std::shared_ptr<Tunnel> > tunnelsToRecreate;
		ManageInboundTunnels (ts, tunnelsToRecreate);
		ManageOutboundTunnels (ts, tunnelsToRecreate);
		// rec-create in random order
		if (!tunnelsToRecreate.empty ())
		{	
			if (tunnelsToRecreate.size () > 1)
				std::shuffle (tunnelsToRecreate.begin(), tunnelsToRecreate.end(), m_Rng);
			for (auto& it: tunnelsToRecreate)
				it->Recreate ();
		}	
	}

	void Tunnels::ManagePendingTunnels (uint64_t ts)
	{
		ManagePendingTunnels (m_PendingInboundTunnels, ts);
		ManagePendingTunnels (m_PendingOutboundTunnels, ts);
	}

	template<class PendingTunnels>
	void Tunnels::ManagePendingTunnels (PendingTunnels& pendingTunnels, uint64_t ts)
	{
		// check pending tunnel. delete failed or timeout
		for (auto it = pendingTunnels.begin (); it != pendingTunnels.end ();)
		{
			auto tunnel = it->second;
			switch (tunnel->GetState ())
			{
				case eTunnelStatePending:
					if (ts > tunnel->GetCreationTime () + TUNNEL_CREATION_TIMEOUT ||
					    ts + TUNNEL_CREATION_TIMEOUT < tunnel->GetCreationTime ())
					{
						LogPrint (eLogDebug, "Tunnel: Pending build request ", it->first, " timeout, deleted");
						// update stats
						auto config = tunnel->GetTunnelConfig ();
						if (config)
						{
							auto hop = config->GetFirstHop ();
							while (hop)
							{
								if (hop->ident)
									i2p::data::UpdateRouterProfile (hop->ident->GetIdentHash (),
										[](std::shared_ptr<i2p::data::RouterProfile> profile)
				    					{
											if (profile) profile->TunnelNonReplied ();
										});
								hop = hop->next;
							}
						}
						// delete
						it = pendingTunnels.erase (it);
						FailedTunnelCreation();
					}
					else
						++it;
				break;
				case eTunnelStateBuildFailed:
					LogPrint (eLogDebug, "Tunnel: Pending build request ", it->first, " failed, deleted");
					it = pendingTunnels.erase (it);
					FailedTunnelCreation();
				break;
				case eTunnelStateBuildReplyReceived:
					// intermediate state, will be either established of build failed
					++it;
				break;
				default:
					// success
					it = pendingTunnels.erase (it);
					SuccesiveTunnelCreation();
			}
		}
	}

	void Tunnels::ManageOutboundTunnels (uint64_t ts, std::vector<std::shared_ptr<Tunnel> >& toRecreate)
	{
		for (auto it = m_OutboundTunnels.begin (); it != m_OutboundTunnels.end ();)
		{
			auto tunnel = *it;
			if (tunnel->IsFailed () || ts > tunnel->GetCreationTime () + TUNNEL_EXPIRATION_TIMEOUT ||
			    ts + TUNNEL_EXPIRATION_TIMEOUT < tunnel->GetCreationTime ())
			{
				LogPrint (eLogDebug, "Tunnel: Tunnel with id ", tunnel->GetTunnelID (), " expired or failed");
				auto pool = tunnel->GetTunnelPool ();
				if (pool)
					pool->TunnelExpired (tunnel);
				// we don't have outbound tunnels in m_Tunnels
				it = m_OutboundTunnels.erase (it);
			}
			else
			{
				if (tunnel->IsEstablished ())
				{
					if (!tunnel->IsRecreated () && ts + TUNNEL_RECREATION_THRESHOLD > tunnel->GetCreationTime () + TUNNEL_EXPIRATION_TIMEOUT)
					{
						auto pool = tunnel->GetTunnelPool ();
						// let it die if the tunnel pool has been reconfigured and this is old
						if (pool && tunnel->GetNumHops() == pool->GetNumOutboundHops())
							toRecreate.push_back (tunnel);
					}
					if (ts + TUNNEL_EXPIRATION_THRESHOLD > tunnel->GetCreationTime () + TUNNEL_EXPIRATION_TIMEOUT)
						tunnel->SetState (eTunnelStateExpiring);
				}
				++it;
			}
		}

		if (m_OutboundTunnels.size () < 3)
		{
			// trying to create one more outbound tunnel
			auto inboundTunnel = GetNextInboundTunnel ();
			auto router = i2p::transport::transports.RoutesRestricted() ?
				i2p::transport::transports.GetRestrictedPeer() :
				i2p::data::netdb.GetRandomRouter (i2p::context.GetSharedRouterInfo (), false, true, false); // reachable by us
			if (!inboundTunnel || !router) return;
			LogPrint (eLogDebug, "Tunnel: Creating one hop outbound tunnel");
			CreateTunnel<OutboundTunnel> (
				std::make_shared<TunnelConfig> (std::vector<std::shared_ptr<const i2p::data::IdentityEx> > { router->GetRouterIdentity () },
					inboundTunnel->GetNextTunnelID (), inboundTunnel->GetNextIdentHash (), false), nullptr
			);
		}
	}

	void Tunnels::ManageInboundTunnels (uint64_t ts, std::vector<std::shared_ptr<Tunnel> >& toRecreate)
	{
		for (auto it = m_InboundTunnels.begin (); it != m_InboundTunnels.end ();)
		{
			auto tunnel = *it;
			if (tunnel->IsFailed () || ts > tunnel->GetCreationTime () + TUNNEL_EXPIRATION_TIMEOUT ||
			    ts + TUNNEL_EXPIRATION_TIMEOUT < tunnel->GetCreationTime ())
			{
				LogPrint (eLogDebug, "Tunnel: Tunnel with id ", tunnel->GetTunnelID (), " expired or failed");
				auto pool = tunnel->GetTunnelPool ();
				if (pool)
					pool->TunnelExpired (tunnel);
				RemoveTunnel (tunnel->GetTunnelID ());
				it = m_InboundTunnels.erase (it);
			}
			else
			{
				if (tunnel->IsEstablished ())
				{
					if (!tunnel->IsRecreated () && ts + TUNNEL_RECREATION_THRESHOLD > tunnel->GetCreationTime () + TUNNEL_EXPIRATION_TIMEOUT)
					{
						auto pool = tunnel->GetTunnelPool ();
						// let it die if the tunnel pool was reconfigured and has different number of hops
						if (pool && tunnel->GetNumHops() == pool->GetNumInboundHops())
							toRecreate.push_back (tunnel);
					}

					if (ts + TUNNEL_EXPIRATION_THRESHOLD > tunnel->GetCreationTime () + TUNNEL_EXPIRATION_TIMEOUT)
						tunnel->SetState (eTunnelStateExpiring);
					else // we don't need to cleanup expiring tunnels
						tunnel->Cleanup ();
				}
				it++;
			}
		}

		if (m_InboundTunnels.empty ())
		{
			LogPrint (eLogDebug, "Tunnel: Creating zero hops inbound tunnel");
			CreateZeroHopsInboundTunnel (nullptr);
			CreateZeroHopsOutboundTunnel (nullptr);
			if (!m_ExploratoryPool)
			{
				int ibLen; i2p::config::GetOption("exploratory.inbound.length", ibLen);
				int obLen; i2p::config::GetOption("exploratory.outbound.length", obLen);
				int ibNum; i2p::config::GetOption("exploratory.inbound.quantity", ibNum);
				int obNum; i2p::config::GetOption("exploratory.outbound.quantity", obNum);
				m_ExploratoryPool = CreateTunnelPool (ibLen, obLen, ibNum, obNum, 0, 0, false);
				m_ExploratoryPool->SetLocalDestination (i2p::context.GetSharedDestination ());
			}
			return;
		}

		if (m_OutboundTunnels.empty () || m_InboundTunnels.size () < 3)
		{
			// trying to create one more inbound tunnel
			auto router = i2p::transport::transports.RoutesRestricted() ?
				i2p::transport::transports.GetRestrictedPeer() :
				// should be reachable by us because we send build request directly
				i2p::data::netdb.GetRandomRouter (i2p::context.GetSharedRouterInfo (), false, true, false);
			if (!router) {
				LogPrint (eLogWarning, "Tunnel: Can't find any router, skip creating tunnel");
				return;
			}
			LogPrint (eLogDebug, "Tunnel: Creating one hop inbound tunnel");
			CreateTunnel<InboundTunnel> (
				std::make_shared<TunnelConfig> (std::vector<std::shared_ptr<const i2p::data::IdentityEx> > { router->GetRouterIdentity () }, false), nullptr
			);
		}
	}

	void Tunnels::ManageTunnelPools (uint64_t ts)
	{
		std::unique_lock<std::mutex> l(m_PoolsMutex);
		for (auto& pool : m_Pools)
		{
			if (pool && pool->IsActive ())
				pool->ManageTunnels (ts);
		}
	}

	void Tunnels::PostTunnelData (std::shared_ptr<I2NPMessage> msg)
	{
		if (msg) m_Queue.Put (msg);
	}

	void Tunnels::PostTunnelData (std::list<std::shared_ptr<I2NPMessage> >& msgs)
	{
		m_Queue.Put (msgs);
	}

	template<class TTunnel>
	std::shared_ptr<TTunnel> Tunnels::CreateTunnel (std::shared_ptr<TunnelConfig> config,
		std::shared_ptr<TunnelPool> pool, std::shared_ptr<OutboundTunnel> outboundTunnel)
	{
		auto newTunnel = std::make_shared<TTunnel> (config);
		newTunnel->SetTunnelPool (pool);
		uint32_t replyMsgID;
		RAND_bytes ((uint8_t *)&replyMsgID, 4);
		AddPendingTunnel (replyMsgID, newTunnel);
		newTunnel->Build (replyMsgID, outboundTunnel);
		return newTunnel;
	}

	std::shared_ptr<InboundTunnel> Tunnels::CreateInboundTunnel (std::shared_ptr<TunnelConfig> config,
		std::shared_ptr<TunnelPool> pool, std::shared_ptr<OutboundTunnel> outboundTunnel)
	{
		if (config)
			return CreateTunnel<InboundTunnel>(config, pool, outboundTunnel);
		else
			return CreateZeroHopsInboundTunnel (pool);
	}

	std::shared_ptr<OutboundTunnel> Tunnels::CreateOutboundTunnel (std::shared_ptr<TunnelConfig> config, std::shared_ptr<TunnelPool> pool)
	{
		if (config)
			return CreateTunnel<OutboundTunnel>(config, pool);
		else
			return CreateZeroHopsOutboundTunnel (pool);
	}

	void Tunnels::AddPendingTunnel (uint32_t replyMsgID, std::shared_ptr<InboundTunnel> tunnel)
	{
		m_PendingInboundTunnels[replyMsgID] = tunnel;
	}

	void Tunnels::AddPendingTunnel (uint32_t replyMsgID, std::shared_ptr<OutboundTunnel> tunnel)
	{
		m_PendingOutboundTunnels[replyMsgID] = tunnel;
	}

	void Tunnels::AddOutboundTunnel (std::shared_ptr<OutboundTunnel> newTunnel)
	{
		// we don't need to insert it to m_Tunnels
		m_OutboundTunnels.push_back (newTunnel);
		auto pool = newTunnel->GetTunnelPool ();
		if (pool && pool->IsActive ())
			pool->TunnelCreated (newTunnel);
		else
			newTunnel->SetTunnelPool (nullptr);
	}

	void Tunnels::AddInboundTunnel (std::shared_ptr<InboundTunnel> newTunnel)
	{
		if (AddTunnel (newTunnel))
		{
			m_InboundTunnels.push_back (newTunnel);
			auto pool = newTunnel->GetTunnelPool ();
			if (!pool)
			{
				// build symmetric outbound tunnel
				CreateTunnel<OutboundTunnel> (std::make_shared<TunnelConfig>(newTunnel->GetInvertedPeers (),
						newTunnel->GetNextTunnelID (), newTunnel->GetNextIdentHash (), false), nullptr,
					GetNextOutboundTunnel ());
			}
			else
			{
				if (pool->IsActive ())
					pool->TunnelCreated (newTunnel);
				else
					newTunnel->SetTunnelPool (nullptr);
			}
		}
		else
			LogPrint (eLogError, "Tunnel: Tunnel with id ", newTunnel->GetTunnelID (), " already exists");
	}


	std::shared_ptr<ZeroHopsInboundTunnel> Tunnels::CreateZeroHopsInboundTunnel (std::shared_ptr<TunnelPool> pool)
	{
		auto inboundTunnel = std::make_shared<ZeroHopsInboundTunnel> ();
		inboundTunnel->SetTunnelPool (pool);
		inboundTunnel->SetState (eTunnelStateEstablished);
		m_InboundTunnels.push_back (inboundTunnel);
		AddTunnel (inboundTunnel);
		return inboundTunnel;
	}

	std::shared_ptr<ZeroHopsOutboundTunnel> Tunnels::CreateZeroHopsOutboundTunnel (std::shared_ptr<TunnelPool> pool)
	{
		auto outboundTunnel = std::make_shared<ZeroHopsOutboundTunnel> ();
		outboundTunnel->SetTunnelPool (pool);
		outboundTunnel->SetState (eTunnelStateEstablished);
		m_OutboundTunnels.push_back (outboundTunnel);
		// we don't insert into m_Tunnels
		return outboundTunnel;
	}

	std::shared_ptr<I2NPMessage> Tunnels::NewI2NPTunnelMessage (bool endpoint)
	{
		if (endpoint)
		{
			// should fit two tunnel message + tunnel gateway header, enough for one garlic encrypted streaming packet
			auto msg = m_I2NPTunnelEndpointMessagesMemoryPool.AcquireSharedMt ();
			msg->Align (6);
			msg->offset += TUNNEL_GATEWAY_HEADER_SIZE; // reserve room for TunnelGateway header
			return msg;
		}
		else
		{
			auto msg = m_I2NPTunnelMessagesMemoryPool.AcquireSharedMt ();
			msg->Align (12);
			return msg;
		}
	}

	int Tunnels::GetTransitTunnelsExpirationTimeout ()
	{
		return m_TransitTunnels.GetTransitTunnelsExpirationTimeout ();
	}

	size_t Tunnels::CountTransitTunnels() const
	{
		return m_TransitTunnels.GetNumTransitTunnels ();
	}

	size_t Tunnels::CountInboundTunnels() const
	{
		// TODO: locking
		return m_InboundTunnels.size();
	}

	size_t Tunnels::CountOutboundTunnels() const
	{
		// TODO: locking
		return m_OutboundTunnels.size();
	}

	void Tunnels::SetMaxNumTransitTunnels (uint32_t maxNumTransitTunnels)
	{
		if (maxNumTransitTunnels > 0 && m_MaxNumTransitTunnels != maxNumTransitTunnels)
		{
			LogPrint (eLogDebug, "Tunnel: Max number of transit tunnels set to ", maxNumTransitTunnels);
			m_MaxNumTransitTunnels = maxNumTransitTunnels;
		}
	}
}
}
