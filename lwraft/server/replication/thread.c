/*
 * Copyright © 2012-2015 VMware, Inc.  All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the “License”); you may not
 * use this file except in compliance with the License.  You may obtain a copy
 * of the License at http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an “AS IS” BASIS, without
 * warranties or conditions of any kind, EITHER EXPRESS OR IMPLIED.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */



/*
 * Module Name: Replication thread
 *
 * Filename: thread.c
 *
 * Abstract:
 *  The directory server replication algorithm is an implementation of the Raft paper
 *  (referred as "the paper" in the source file comments) for replicating LDAP entry changes (Add/Modify/Delete/Moddn):
 *  Diego Ongaro and John Ousterhout, “In Search of an Understandable Consensus Algorithm (Extended Version)”, May 20, 2014
 *  The Raft protocol replaces (or as an alternative to) vmdird multi-master replication algorithm.
 *  See the functional specification for detail: https://wiki.eng.vmware.com/Lightwave/Raft#Project_Milestone
 */

#include "includes.h"

extern DWORD AttrListToEntry(PVDIR_SCHEMA_CTX, PSTR, PSTR*, PVDIR_ENTRY);
extern DWORD VmDirAddModSingleAttributeReplace(PVDIR_OPERATION, PCSTR, PCSTR, PVDIR_BERVALUE);
extern DWORD VmDirCloneStackOperation(PVDIR_OPERATION, PVDIR_OPERATION, VDIR_OPERATION_TYPE, ber_tag_t, PVDIR_SCHEMA_CTX);
extern int VmDirEntryAttrValueNormalize(PVDIR_ENTRY, BOOLEAN);
extern DWORD VmDirSyncCounterReset(PVMDIR_SYNCHRONIZE_COUNTER pSyncCounter, int syncValue);
extern DWORD VmDirConditionBroadcast(PVMDIR_COND pCondition);

int VmDirRaftCommitHook(VOID);

static DWORD _VmDirAppendEntriesRpc(PVMDIR_SERVER_CONTEXT *ppServer, PVMDIR_PEER_PROXY pProxySelf, int);
static DWORD _VmDirRequestVoteRpc(PVMDIR_SERVER_CONTEXT *pServer, PVMDIR_PEER_PROXY pProxySelf);
static DWORD _VmDirReplicationThrFun(PVOID);
static DWORD _VmDirRaftVoteSchdThread();
static DWORD _VmDirStartProxies(VOID);
static DWORD _VmDirRpcConnect(PVMDIR_SERVER_CONTEXT *ppServer, PVMDIR_PEER_PROXY pProxySelf);
static VOID _VmDirApplyLogsUpto(UINT64 indexToApply);
static DWORD _VmDirApplyLog(unsigned long long);
static VOID _VmDirWaitPeerThreadsShutdown();
static DWORD _VmDirDeleteRaftProxy(char *dn_norm);
static VOID _VmDirRemovePeerInLock(PCSTR pHostname);
static VOID _VmDirWaitLogCommitDone();

PVMDIR_MUTEX gRaftStateMutex = NULL;

PVMDIR_COND gRaftRequestPendingCond = NULL;
PVMDIR_COND gRaftRequestVoteCond = NULL;
PVMDIR_COND gRaftAppendEntryReachConsensusCond = NULL;
PVMDIR_COND gGotVoteResultCond = NULL;
PVMDIR_COND gPeersReadyCond = NULL;

VDIR_RAFT_STATE gRaftState = {0};
PVDIR_RAFT_LOG gEntries = NULL;
VDIR_RAFT_LOG gLogEntry = {0};

static char gNewPartner[VMDIR_MAX_LDAP_URI_LEN] = {0};

VOID VmDirNewPartner(PCSTR *hostname)
{
    memcpy(gNewPartner, hostname, strlen((char *)hostname));
}

DWORD
InitializeReplicationThread(
    void)
{
    DWORD               dwError = 0;
    PVDIR_THREAD_INFO   pThrInfo = NULL;

    dwError = VmDirAllocateMutex(&gRaftStateMutex);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocateCondition(&gRaftRequestPendingCond);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocateCondition(&gPeersReadyCond);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocateCondition(&gRaftRequestVoteCond);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocateCondition(&gRaftAppendEntryReachConsensusCond);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocateCondition(&gGotVoteResultCond);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocateMemory( sizeof(*pThrInfo), (PVOID*)&pThrInfo);
    BAIL_ON_VMDIR_ERROR(dwError);

    if (gVmdirGlobals.dwRaftElectionTimeoutMS < 10 ||
        gVmdirGlobals.dwRaftPingIntervalMS < 20 ||
        gVmdirGlobals.dwRaftElectionTimeoutMS <= (gVmdirGlobals.dwRaftPingIntervalMS << 1))
    {
        dwError = LDAP_OPERATIONS_ERROR;
        VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL,
          "InitializeReplicationThread: Raft parameter Error %d: %s must be greater than twice of %s",
          dwError, "RaftElectionTimeoutMS", "RaftPingIntervalMS");
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    VMDIR_LOG_INFO( VMDIR_LOG_MASK_ALL, "InitializeReplicationThread Raft parameters: %s=%d, %s=%d",
      "RaftElectionTimeoutMS", gVmdirGlobals.dwRaftElectionTimeoutMS,
      "RaftPingIntervalMS", gVmdirGlobals.dwRaftPingIntervalMS);

    VmDirSrvThrInit(
                &pThrInfo,
                gVmdirGlobals.replAgrsMutex,       // alternative mutex
                gVmdirGlobals.replAgrsCondition,   // alternative cond
                TRUE);


    dwError = VmDirCreateThread( &pThrInfo->tid, FALSE, _VmDirReplicationThrFun, pThrInfo);
    BAIL_ON_VMDIR_ERROR(dwError);

    VmDirSrvThrAdd(pThrInfo);

cleanup:

    return dwError;

error:

    if (pThrInfo)
    {
        VmDirSrvThrFree(pThrInfo);
    }

    goto cleanup;
}

/*
 * This thread detect ping recieve timeout (not get ping from leader for a peroid of time of time of time),
 * and start a vote when this occur or start an new vote if the request vote got a split vote.
 */
static
DWORD
_VmDirRaftVoteSchdThread()
{
    int dwError = 0;

    gRaftState.role = VDIR_RAFT_ROLE_FOLLOWER;
    UINT64 waitTime = gVmdirGlobals.dwRaftElectionTimeoutMS;
    BOOLEAN bLock = FALSE;

    VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "_VmDirRaftVoteSchdThread: started.");

    while(1)
    {
        UINT64 now = {0};
        int term = 0;
        int waitConsensus = WAIT_CONSENSUS_TIMEOUT_MS;
        BOOLEAN bWaitTimeout = FALSE;

        if (VmDirdState() == VMDIRD_STATE_SHUTDOWN)
        {
            goto done;
        }

        //VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "_VmDirRaftVoteSchdThread wait %llu ms", waitTime);
        VMDIR_LOCK_MUTEX(bLock, gRaftStateMutex);

        VmDirConditionTimedWait(gRaftRequestVoteCond, gRaftStateMutex, waitTime);

        if (VmDirdState() == VMDIRD_STATE_SHUTDOWN)
        {
            goto done;
        }

        if (gRaftState.clusterSize < 2)
        {
            VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);
            continue;
        }

        if (gRaftState.role == VDIR_RAFT_ROLE_CANDIDATE)
        {
            //Split votes
            goto startVote;
        } else if (gRaftState.role == VDIR_RAFT_ROLE_FOLLOWER)
        {
            UINT64 waitTimeRemain = 0;

            now = VmDirGetTimeInMilliSec();
            waitTimeRemain = now - gRaftState.lastPingRecvTime;
            if (waitTimeRemain >= gVmdirGlobals.dwRaftElectionTimeoutMS)
            {
                //Hasn't recieved ping for an duration of gVmdirGlobals.dwRaftElectionTimeoutMS - switch to candidate
                gRaftState.role = VDIR_RAFT_ROLE_CANDIDATE;
                gRaftState.rpcSent = TRUE;
                goto startVote;
            } else
            {
                VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);
                continue;
            }
        } else 
        {
            //Server is a leader
            waitTime = gVmdirGlobals.dwRaftElectionTimeoutMS;
            VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);
            continue;
        }

startVote:
        // Stay in gRaftStateMutex
        do
        {
            dwError = 0;
            if (_VmDirPeersIdleInLock() < (gRaftState.clusterSize/2))
            {
                 //Wait gPeersReadyCond only if not enough peers are Ready
                 dwError = VmDirConditionTimedWait(gPeersReadyCond, gRaftStateMutex, WAIT_PEERS_READY_MS);
            }
        } while (dwError == ETIMEDOUT);

        if (dwError)
        {
            //waitTime or other error during wait for gPeersReadyCond.
            VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);
            if (dwError != ETIMEDOUT)
            {
                VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "_VmDirRaftVoteSchdThread: wait gPeersReadyCond error: %d", dwError);
            }
            continue;
        }

        if (gRaftState.rpcSent)
        {
            //Increment term only when at least one request vote rpc was sent out with
            //   the previous requtest vote to avoid waisting term numbers.
            gRaftState.currentTerm++;
            VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);
            _VmDirUpdateRaftPsState(gRaftState.currentTerm, TRUE, 0, NULL, 0);
            VMDIR_LOCK_MUTEX(bLock, gRaftStateMutex);
        }

        if (gRaftState.votedFor.lberbv_len > 0)
        {   
            VmDirFreeBervalContent(&gRaftState.votedFor);
        }
  
        gRaftState.disallowUpdates = TRUE;
        gRaftState.votedForTerm = 0;
        gRaftState.rpcSent = FALSE;
        term = gRaftState.currentTerm;
        gRaftState.voteConsensusCnt = 1; //vote for self
        gRaftState.voteDeniedCnt = 0;
        gRaftState.voteConsenusuTerm = term;
        gRaftState.cmd = ExecReqestVote;

        bWaitTimeout = FALSE;
        VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "_VmDirRaftVoteSchdThread: wait vote result; role %d term %d",
                       gRaftState.role, gRaftState.currentTerm); 

        //Now invoke paralle RPC calls to all (available) peers
        //VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "_VmDirRaftVoteSchdThread: calling _VmDirRaftExec role %d term %d ...",
        //               gRaftState.role, gRaftState.currentTerm);
        VmDirConditionBroadcast(gRaftRequestPendingCond);

        //Wait for (majority of) peer threads to complete their RRC calls.
        VmDirConditionTimedWait(gGotVoteResultCond, gRaftStateMutex, waitConsensus);
        gRaftState.cmd = ExecNone;

        if (bWaitTimeout)
        {
            VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "_VmDirRaftVoteSchdThread: wait vote result waitTime in %d sec; role %d term %d",
                           WAIT_CONSENSUS_TIMEOUT_MS, gRaftState.role, gRaftState.currentTerm);
        }

        //Now evalute vote outcome
        if (gRaftState.role == VDIR_RAFT_ROLE_CANDIDATE)
        {
           if (gRaftState.currentTerm == gRaftState.voteConsenusuTerm &&
               gRaftState.voteConsensusCnt >= (gRaftState.clusterSize/2 + 1))
           {
               // Verify that the server got majority votes.
               gRaftState.role = VDIR_RAFT_ROLE_LEADER;
               gRaftState.lastPingRecvTime = 0; //This would invoke immediate Pings by proxy threads.
               VmDirConditionBroadcast(gRaftRequestPendingCond);
               VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);

               _VmDirWaitLogCommitDone();
               gRaftState.disallowUpdates = FALSE;

               waitTime = gVmdirGlobals.dwRaftElectionTimeoutMS;
               continue;
           } else
           {
                //Stay in candidate - split vote; wait at least 150ms.
                waitTime = (UINT64)(rand()%WAIT_REELECTION_RAND_MS + 150);
           }
        } else
        {
            // Become follower
            UINT64 waitTimeRemain = VmDirGetTimeInMilliSec() - gRaftState.lastPingRecvTime;

            waitTime = waitTimeRemain < gVmdirGlobals.dwRaftElectionTimeoutMS? waitTimeRemain:gVmdirGlobals.dwRaftElectionTimeoutMS;
        }
        VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);
    }

done:
    VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);
    VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "_VmDirRaftVoteSchdThread: shutdown completed.");
    return dwError;
}

/*
 * Logs from lastApplied to HighesIndex may or maynot committed though the highest committed
 * log must be (persisted) in this server per the committing and voting algorithm.
 * Those logs should be indirectly (or comfirmed to be) committed (replicated to half or more peers)
 * as the paper secion 5.4.2 explained.
 * The implementation will have the new leader to wait and check those logs being committed via Raft Ping
 * and then apply them locally before acceping new (external) LDAP update requests.
 */
static
VOID
_VmDirWaitLogCommitDone()
{
    BOOLEAN bLock = FALSE;
    PVMDIR_PEER_PROXY pProxy = NULL;
    int matchIdxCnt = 0;
    UINT64 idxToApply = 0;

    while(1)
    {
       if (VmDirdState() == VMDIRD_STATE_SHUTDOWN ||
           gRaftState.lastApplied == gRaftState.lastLogIndex)
       {
           goto done;
       }

       VMDIR_LOCK_MUTEX(bLock, gRaftStateMutex);
       matchIdxCnt = 0;
       for(pProxy=gRaftState.proxies; pProxy; pProxy=pProxy->pNext)
       {
           if (pProxy->matchIndex > gRaftState.lastApplied)
           {
               matchIdxCnt++;
           }
       }

       idxToApply = 0;
       if (matchIdxCnt >= (gRaftState.clusterSize >> 1))
       {
           //Found at least one committed log index
           for(pProxy=gRaftState.proxies; pProxy; pProxy=pProxy->pNext)
           {
               if (pProxy->matchIndex > gRaftState.lastApplied &&
                   (idxToApply == 0 || pProxy->matchIndex < idxToApply))
               {
                   //locate the lowest index that is committed.
                   idxToApply = pProxy->matchIndex;
                   break;
               }
           }
       }
       VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);

       if (idxToApply == 0)
       {
           //No progress to replicate uncommtted logs.
           //Wait Ping to replicate them.
           VmDirSleep(500);
       } else
       {
           //_VmDirApplyLogsUpto will advance gRaftState.lastApplied and gRaftState.commitIndex
           _VmDirApplyLogsUpto(idxToApply);
       }
    }

done:
    VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "_VmDirWaitLogCommitDone: lastLogIndex %llu", gRaftState.lastLogIndex);
    return;
}

static
DWORD
VmDirRaftPeerThread(void *ctx)
{
    BOOLEAN bLock = FALSE;
    UINT64 pingTimeout = gVmdirGlobals.dwRaftPingIntervalMS;
    UINT64 prevPingTime = {0};
    UINT64 now = {0};
    int cmd = ExecNone;
    PVMDIR_SERVER_CONTEXT pServer = NULL;
    DWORD dwError = 0;
    PVMDIR_PEER_PROXY pProxySelf = (PVMDIR_PEER_PROXY)ctx;
    PSTR pPeerHostName = pProxySelf->raftPeerHostname;

    dwError=_VmDirRpcConnect(&pServer, pProxySelf);
    BAIL_ON_VMDIR_ERROR( dwError );

    while(1)
    {
        if (VmDirdState() == VMDIRD_STATE_SHUTDOWN || pProxySelf->isDeleted)
        {
            goto cleanup;
        }

        //VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "VmDirRaftPeerThread: wait RequestPendingCond (peer %s); role %d term %d",
        //    pPeerHostName, gRaftState.role, gRaftState.currentTerm);

        VMDIR_LOCK_MUTEX(bLock, gRaftStateMutex);
        pProxySelf->proxy_state = RPC_IDLE;
        if (_VmDirPeersIdleInLock() >= (gRaftState.clusterSize/2))
        {
            VmDirConditionSignal(gPeersReadyCond);
        }
        VmDirConditionTimedWait(gRaftRequestPendingCond, gRaftStateMutex, pingTimeout);
        pProxySelf->proxy_state = RPC_BUSY;
        VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);

appendEntriesRepeat:
        if (VmDirdState() == VMDIRD_STATE_SHUTDOWN || pProxySelf->isDeleted)
        {
            goto cleanup;
        }

        //VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "VmDirRaftPeerThread: exit RequestPendingCond (peer %s); role %d term %d",
        //    pPeerHostName, gRaftState.role, gRaftState.currentTerm);
        VMDIR_LOCK_MUTEX(bLock, gRaftStateMutex);
        cmd = gRaftState.cmd;
        if (gRaftState.role == VDIR_RAFT_ROLE_LEADER)
        {
            now = VmDirGetTimeInMilliSec();
            if (cmd == ExecNone)
            {
                //VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "VmDirRaftPeerThread: current gRaftState.cmd %d (peer %s); role %d term %d",
                //    gRaftState.cmd, pPeerHostName, gRaftState.role, gRaftState.currentTerm);
                if ((now - prevPingTime) >= gVmdirGlobals.dwRaftPingIntervalMS)
                {
                    prevPingTime = now;
                    pingTimeout = gVmdirGlobals.dwRaftPingIntervalMS;
                    cmd = ExecPing;
                    //VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "VmDirRaftPeerThread: will ExecPing to peer %s; role %d term %d",
                    //    pPeerHostName, gRaftState.role, gRaftState.currentTerm);
                } else
                {
                    pingTimeout = now - prevPingTime;
                    VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);
                    continue;
                }
            } else
            {
                // AppendEntriesRpc - reset ping time timeout
                pingTimeout = gVmdirGlobals.dwRaftPingIntervalMS;
                prevPingTime = now;
            }
        } else
        {
            //Reset prevPingTime so that ping will be sent immediately switching to leader.
            prevPingTime = 0;
        }
        VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);

        //VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "VmDirRaftPeerThread: to exe gRaftState.cmd %d (peer %s); role %d term %d",
        //    gRaftState.cmd, pPeerHostName, gRaftState.role, gRaftState.currentTerm);
        switch(cmd)
        {
            case ExecReqestVote:
                 _VmDirRequestVoteRpc(&pServer, pProxySelf);
                 break;
            case ExecAppendEntries:
                 _VmDirAppendEntriesRpc(&pServer, pProxySelf, ExecAppendEntries);
                 break;
            case ExecPing:
                 _VmDirAppendEntriesRpc(&pServer, pProxySelf, ExecPing);
                 break;
            case ExecNone:
                 continue;
            default:
                 VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "VmDirRaftPeerThread request type unknown %d", gRaftState.cmd);
                 continue;
        }

        VMDIR_LOCK_MUTEX(bLock, gRaftStateMutex);
        if (gRaftState.role == VDIR_RAFT_ROLE_LEADER &&
            gRaftState.cmd == ExecAppendEntries &&
            gEntries && gEntries->index > pProxySelf->matchIndex)
        {
            //During AppendEntriesRpc, new AppendEntries is initiated,
            //  (e.g. other peers proceed faster than this peer), and the completed RPC
            //  needs to replicate the new gEntries without waiting.
            VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);
            goto appendEntriesRepeat; 
        }
        VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);
    }
    VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "VmDirRaftPeerThread: thread for peer %s shutdown completed", pPeerHostName);

cleanup:
    if (pServer)
    {
        VmDirCloseServer( pServer);
    }
    pProxySelf->isDeleted = TRUE;
    pProxySelf->tid = 0;
    return dwError;

error:
    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "VmDirRaftPeerThread: thread for peer %s error %d", pPeerHostName, dwError);
    goto cleanup;
}

static
VOID
_VmDirRemovePeerInLock(PCSTR pHostname)
{
    PVMDIR_PEER_PROXY curProxy = NULL;

    for(curProxy=gRaftState.proxies; curProxy; curProxy=curProxy->pNext)
    {
        if (VmDirStringCompareA(curProxy->raftPeerHostname, pHostname, FALSE)==0)
        {
            break;
        }
    }
    if (curProxy)
    {
        if (curProxy->isDeleted == FALSE)
        {
            curProxy->isDeleted = TRUE;
            gRaftState.clusterSize--;
        }
    }
}

static
DWORD
_VmDirNewPeerProxyInLock(PCSTR pHostname, VDIR_RAFT_PROXY_STATE state)
{
    DWORD dwError = 0;
    PVMDIR_PEER_PROXY pPp = NULL;
    PVMDIR_PEER_PROXY curProxy = NULL;

    for(curProxy=gRaftState.proxies; curProxy; curProxy=curProxy->pNext)
    {
        if (VmDirStringCompareA(curProxy->raftPeerHostname, pHostname, FALSE)==0)
        {
            break;
        }
    }

    if (curProxy && !curProxy->isDeleted)
    {
       //Peer exists already.
       goto cleanup;
    }

    if (curProxy && curProxy->isDeleted)
    {
        //Add the previously deleted back.
        pPp = curProxy;
    } else
    {
        dwError = VmDirAllocateMemory( sizeof(VMDIR_PEER_PROXY), (PVOID*)&pPp);
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    strcpy(pPp->raftPeerHostname, pHostname);
    if (gRaftState.proxies == NULL)
    {
        gRaftState.proxies = pPp;
    } else if (pPp->isDeleted)
    {
       pPp->isDeleted = FALSE;
    } else
    {
        pPp->pNext = gRaftState.proxies;
        gRaftState.proxies = pPp;
    }

    //If the proxy is in promo process, then it Will be set to RpcIdle when receving a vote request from the peer.
    pPp->proxy_state = state;

    dwError = VmDirCreateThread(&pPp->tid, TRUE, VmDirRaftPeerThread, (PVOID)pPp);
    BAIL_ON_VMDIR_ERROR(dwError);

    VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "VmDirNewPeerProxy: added new peer proxy for host %s", pHostname);
cleanup:
    return dwError;

error:
    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "VmDirNewPeerProxy: added new peer proxy for host %s error %d", pHostname, dwError);
    goto cleanup;
}

static
DWORD
_VmDirReplicationThrFun(
    PVOID   pArg
    )
{
    int dwError = 0;
    BOOLEAN bInReplAgrsLock = FALSE;
    BOOLEAN bInReplCycleDoneLock = FALSE;
    PVDIR_THREAD_INFO pRaftVoteSchdThreadInfo = NULL;
    BOOLEAN bGlobalsLoaded = FALSE;
    PSTR pszLocalErrorMsg = NULL;
    BOOLEAN bSignaledConnThreads = FALSE;

    dwError = _VmDirRaftLoadGlobals(&pszLocalErrorMsg);
    if (dwError == 0)
    {
        bGlobalsLoaded = TRUE;
    }

    if (!bGlobalsLoaded)
    {
        VMDIR_SAFE_FREE_MEMORY(pszLocalErrorMsg);
        dwError = 0;

        VMDIR_LOG_INFO( VMDIR_LOG_MASK_ALL, "_VmDirReplicationThrFun: waiting for promoting ...");
        //server has not complete vdcpromo, wait signal triggered by vdcpromo
        VMDIR_LOCK_MUTEX(bInReplAgrsLock, gVmdirGlobals.replAgrsMutex);
        dwError = VmDirConditionWait( gVmdirGlobals.replAgrsCondition, gVmdirGlobals.replAgrsMutex );
        BAIL_ON_VMDIR_ERROR( dwError);
        VMDIR_UNLOCK_MUTEX(bInReplAgrsLock, gVmdirGlobals.replAgrsMutex);

        if (gNewPartner[0] != '\0')
        {
            dwError=VmDirFirstReplicationCycle(gNewPartner);
            BAIL_ON_VMDIR_ERROR( dwError );

            VMDIR_LOG_INFO( VMDIR_LOG_MASK_ALL, "_VmDirReplicationThrFun: complete vdcpromo from partner %s.", gNewPartner);
        } else
        {
            VMDIR_LOG_INFO( VMDIR_LOG_MASK_ALL, "_VmDirReplicationThrFun: complete vdcpromo as first server.");
        }
    }

    //Wake up LDAP connection threads
    VMDIR_LOCK_MUTEX(bInReplCycleDoneLock, gVmdirGlobals.replCycleDoneMutex);
    VmDirConditionSignal(gVmdirGlobals.replCycleDoneCondition);
    VMDIR_UNLOCK_MUTEX(bInReplCycleDoneLock, gVmdirGlobals.replCycleDoneMutex);

    if (!bGlobalsLoaded)
    {
        //Wait until vdcpromo has completed adding the DC to cluster.
        int retryCnt = 0;
        while(TRUE)
        {
            dwError = _VmDirRaftLoadGlobals(&pszLocalErrorMsg);
            if (dwError == 0 || retryCnt++ > 10)
            {
                break;
            }
            VMDIR_SAFE_FREE_MEMORY(pszLocalErrorMsg);
            VmDirSleep(3000);
        }
    }

    BAIL_ON_VMDIR_ERROR_WITH_MSG(dwError, (pszLocalErrorMsg), "_VmDirReplicationThrFun: _VmDirRaftLoadGlobals");

    dwError = _VmDirLoadRaftState();
    BAIL_ON_VMDIR_ERROR( dwError);

    dwError = _VmDirStartProxies();
    BAIL_ON_VMDIR_ERROR( dwError);

    dwError = VmDirAllocateMemory( sizeof(*pRaftVoteSchdThreadInfo), (PVOID*)&pRaftVoteSchdThreadInfo);
    BAIL_ON_VMDIR_ERROR(dwError);

    VmDirSrvThrInit( &pRaftVoteSchdThreadInfo, gVmdirGlobals.replAgrsMutex, gVmdirGlobals.replAgrsCondition, TRUE);
    dwError = VmDirCreateThread( &pRaftVoteSchdThreadInfo->tid, TRUE, _VmDirRaftVoteSchdThread, pRaftVoteSchdThreadInfo);
    BAIL_ON_VMDIR_ERROR(dwError);

    VmDirSrvThrAdd(pRaftVoteSchdThreadInfo);

    VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "_VmDirReplicationThrFun: started.");

    while (1)
    {
        if (!bSignaledConnThreads && VmDirdState() == VMDIRD_STATE_NORMAL)
        {
            //Wake up LDAP connection threads - signal again, connection threads may
            // have missed the signal.
            VMDIR_LOCK_MUTEX(bInReplCycleDoneLock, gVmdirGlobals.replCycleDoneMutex);
            VmDirConditionSignal(gVmdirGlobals.replCycleDoneCondition);
            VMDIR_UNLOCK_MUTEX(bInReplCycleDoneLock, gVmdirGlobals.replCycleDoneMutex);
            bSignaledConnThreads = TRUE;
        }
        if (VmDirdState() == VMDIRD_STATE_SHUTDOWN)
        {
            BOOLEAN bLock = FALSE;
            VMDIR_LOCK_MUTEX(bLock, gRaftStateMutex);
            VmDirConditionBroadcast(gRaftRequestPendingCond);
            VmDirConditionBroadcast(gPeersReadyCond);
            VmDirConditionSignal(gRaftAppendEntryReachConsensusCond);
            VmDirConditionSignal(gRaftRequestVoteCond);
            VmDirConditionSignal(gGotVoteResultCond);
            VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);
            _VmDirWaitPeerThreadsShutdown();
            goto cleanup;
        }
        VmDirSleep( 3000 );
    }
    VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "_VmDirReplicationThrFun: shutdown completed.");

cleanup:
    VMDIR_UNLOCK_MUTEX(bInReplAgrsLock, gVmdirGlobals.replAgrsMutex);
    VMDIR_UNLOCK_MUTEX(bInReplCycleDoneLock, gVmdirGlobals.replCycleDoneMutex);
    VMDIR_SAFE_FREE_MEMORY(pszLocalErrorMsg);

    return 0;

error:
    VmDirdStateSet( VMDIRD_STATE_FAILURE );
    if (pRaftVoteSchdThreadInfo)
    {
         VmDirSrvThrFree(pRaftVoteSchdThreadInfo);
    }
    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL,
                    "_VmDirReplicationThrFun: Replication has failed with unrecoverable error %d", dwError);
    goto cleanup;
}

DWORD
VmDirGetReplCycleCounter(
    VOID
    )
{
    BOOLEAN bInLock = FALSE;
    DWORD   dwCount = 0;

    VMDIR_LOCK_MUTEX(bInLock, gVmdirGlobals.replCycleDoneMutex);
    dwCount = gVmdirGlobals.dwReplCycleCounter;
    VMDIR_UNLOCK_MUTEX(bInLock, gVmdirGlobals.replCycleDoneMutex);

    return dwCount;
}

static
VOID
_VmDirWaitPeerThreadsShutdown()
{
    PVMDIR_PEER_PROXY pPeerProxy = NULL;
    VMDIR_THREAD tid = 0;
    BOOLEAN bLock = FALSE;

    pPeerProxy = gRaftState.proxies;
    while (pPeerProxy)
    {
        PVMDIR_PEER_PROXY pNext = pPeerProxy->pNext;
        VMDIR_LOCK_MUTEX(bLock, gRaftStateMutex);
        if (pPeerProxy->tid == 0)
        {
            pPeerProxy = pNext;
            VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);
            continue;
        }
        tid = pPeerProxy->tid;
        VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);
        VmDirThreadJoin(&tid, NULL);
        pPeerProxy = pNext;
    }
}

static
DWORD
_VmDirRequestVoteRpc(PVMDIR_SERVER_CONTEXT *ppServer, PVMDIR_PEER_PROXY pProxySelf)
{
    DWORD dwError = 0;
    REQUEST_VOTE_ARGS reqVoteArgs = {0};
    PVMDIR_SERVER_CONTEXT pServer = *ppServer;
    PCSTR pPeerHostName = pProxySelf->raftPeerHostname;
    BOOLEAN bLock = FALSE;
    BOOLEAN waitSignaled = FALSE;

    VMDIR_LOCK_MUTEX(bLock, gRaftStateMutex);
    reqVoteArgs.candidateId = gRaftState.hostname.lberbv_val;
    reqVoteArgs.term = gRaftState.currentTerm;
    reqVoteArgs.lastLogIndex = gRaftState.lastLogIndex;
    reqVoteArgs.lastLogTerm = gRaftState.lastLogTerm;
    VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);

    dwError = VmDirRaftRequestVote(pServer, reqVoteArgs.term, reqVoteArgs.candidateId,
                                  (UINT32)reqVoteArgs.lastLogIndex, reqVoteArgs.lastLogTerm,
                                  &reqVoteArgs.currentTerm, &reqVoteArgs.voteGranted);
    if (dwError)
    {
        if (dwError == rpc_s_connect_rejected || dwError == rpc_s_connect_timed_out ||
            dwError == rpc_s_cannot_connect || dwError == rpc_s_connection_closed)
        {
            VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "_VmDirRequestVoteRpc: not connected or disconnected peer %s", pPeerHostName);
            pProxySelf->proxy_state = RPC_DISCONN;
            _VmDirRpcConnect(ppServer, pProxySelf);
            goto done;
        } else if (dwError == rpc_s_auth_method)
        {
             VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "_VmDirRequestVoteRpc: rpc_s_auth_method peer %s", pPeerHostName);
             pProxySelf->proxy_state = RPC_DISCONN;
             _VmDirRpcConnect(ppServer, pProxySelf);
             goto done;
        }

        if(gRaftState.role != VDIR_RAFT_ROLE_CANDIDATE)
        {
             goto done;
        }

        VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "_VmDirRequestVoteRpc: RPC call VmDirRaftRequestVote failed to peer %s error %d",
                        pPeerHostName, dwError);
        goto done;
    }

    gRaftState.rpcSent = TRUE;
    bLock = FALSE;
    VMDIR_LOCK_MUTEX(bLock, gRaftStateMutex);
    if (reqVoteArgs.currentTerm > gRaftState.currentTerm)
    {
        /* The vote must have been denied in this case.
         * The peer may also has its log index larger than mine in which case this server should
         * start a new vote the sooner the better.  However, we can't tell it is the case,
         * thus simply treat it as a split vote. We may improve the vote efficiency if we have an
         * additional a OUT parameter to tell this condition.
         */
        gRaftState.currentTerm = reqVoteArgs.currentTerm;
        gRaftState.voteDeniedCnt++;

        VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);
        _VmDirUpdateRaftPsState(reqVoteArgs.currentTerm, FALSE, 0, NULL, 0);

        VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "_VmDirRequestVoteRpc: peer (%s) term %d > current term, change role to follower",
                       pPeerHostName, reqVoteArgs.currentTerm);
        goto done;
    }

    if (gRaftState.role != VDIR_RAFT_ROLE_CANDIDATE)
    {
        VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL,
          "_VmDirRequestVoteRpc: server role changed to %d; term %d; peer: (%s)(term %d)",
          gRaftState.role, gRaftState.currentTerm, pPeerHostName, reqVoteArgs.currentTerm);
        goto done;
    }

    if (reqVoteArgs.voteGranted != 0)
    {
        VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL,
           "_VmDirRequestVoteRpc: vote denied from peer %s role %d term %d",
          pPeerHostName, gRaftState.role, gRaftState.currentTerm);
        gRaftState.voteDeniedCnt++;
        if (reqVoteArgs.voteGranted == 2)
        {
            //Peer has a larger highest logIndex, switch to follower,
            // so don't send request vote anymore for this term.
            gRaftState.role = VDIR_RAFT_ROLE_FOLLOWER;
        }
        //Denied due to split vote or any other reasons - stay as a candidate.
        goto done;
    }

    if (gRaftState.currentTerm == gRaftState.voteConsenusuTerm)
    {
        gRaftState.voteConsensusCnt++;
    }

    if (gRaftState.voteConsensusCnt >= (gRaftState.clusterSize/2 + 1))
    {
        //got majority votes, wake up _VmDirRaftVoteSchdThread which will change role to leader.
        VmDirConditionSignal(gGotVoteResultCond);

        VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL,
          "_VmDirRequestVoteRpc: vote granted from majority consensusCount %d for term %d; become leader in term %d",
          gRaftState.voteConsensusCnt, gRaftState.voteConsenusuTerm, gRaftState.currentTerm);
        goto done;
    } else
    {
        waitSignaled = TRUE;
        VmDirConditionSignal(gGotVoteResultCond);
    }
    
    VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "_VmDirRequestVoteRpc: vote granted from peer %s voteConsensusCnt %d (term %d)",
                   pPeerHostName, gRaftState.voteConsensusCnt, gRaftState.voteConsenusuTerm);

done:
    if (!waitSignaled && (gRaftState.voteConsensusCnt - 1 + gRaftState.voteDeniedCnt) >= _VmDirPeersConnectedInLock())
    {
        //Also wakeup vote scheduler if all available peers received votes.
        VmDirConditionSignal(gGotVoteResultCond);
    }
    VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);
    return 0;
}

static
DWORD
_VmDirAppendEntriesRpc(PVMDIR_SERVER_CONTEXT *ppServer, PVMDIR_PEER_PROXY pProxySelf, int cmd)
{
    DWORD dwError = 0;
    APPEND_ENTRIES_ARGS args = {0};
    PCSTR pPeerHostName = pProxySelf->raftPeerHostname;
    PVMDIR_SERVER_CONTEXT pServer = *ppServer;
    BOOLEAN bLock = FALSE;
    unsigned long long startLogIndex = 0;
    VDIR_RAFT_LOG curChgLog = {0};
    VDIR_RAFT_LOG preChgLog = {0};

    VMDIR_LOCK_MUTEX(bLock, gRaftStateMutex);
    if (gRaftState.role != VDIR_RAFT_ROLE_LEADER)
    {
        goto cleanup;
    }

    args.leader = gRaftState.hostname.lberbv_val;
    if (cmd == ExecAppendEntries)
    {
        if (gEntries == NULL || gEntries->packRaftLog.lberbv_len == 0)
        {
            //caller has given up and removed gEntries.
            VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "_VmDirAppendEntriesRpc ExecAppendEntries has no entries to send");
            goto cleanup;
        }
        args.entriesSize = gEntries->packRaftLog.lberbv_len;
        dwError = VmDirAllocateAndCopyMemory(gEntries->packRaftLog.lberbv_val, args.entriesSize, (PVOID*)&args.entries);
        BAIL_ON_VMDIR_ERROR(dwError);

        startLogIndex = gEntries->index;
        VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);
        dwError = _VmDirGetPrevLogArgs(&args.preLogIndex, &args.preLogTerm, gEntries->index - 1, __LINE__);
        BAIL_ON_VMDIR_ERROR(dwError);
    } else if (cmd == ExecPing)
    {
        //This is a Ping
        args.entriesSize = 0;
        args.entries = NULL;
        startLogIndex = gRaftState.lastLogIndex;
        VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);
        dwError = _VmDirGetPrevLogArgs(&args.preLogIndex, &args.preLogTerm, startLogIndex, __LINE__);
        BAIL_ON_VMDIR_ERROR(dwError);
    } else
    {
         assert(0);
    }

ReplicateLog:
    VMDIR_LOCK_MUTEX(bLock, gRaftStateMutex);
    args.term = gRaftState.currentTerm;
    args.leaderCommit = gRaftState.commitIndex;
    VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);

    //VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "_VmDirAppendEntriesRpc: call with startLogIndex %llu preLogIndex %llu for peer %s",
    //               startLogIndex, args.preLogIndex, pPeerHostName);
 
    dwError = VmDirRaftAppendEntries(pServer, args.term, args.leader,
                                  (UINT32)args.preLogIndex, args.preLogTerm,
                                   args.leaderCommit,
                                   args.entriesSize, args.entries,
                                   &args.currentTerm, &args.status);
    if (dwError)
    {
        if (dwError == rpc_s_connect_rejected || dwError == rpc_s_connect_timed_out ||
            dwError == rpc_s_cannot_connect || dwError == rpc_s_connection_closed)
        {
            pProxySelf->proxy_state = RPC_DISCONN;
            VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "_VmDirAppendEntriesRpc: not connected or disconnected peer %s", pPeerHostName);
            _VmDirRpcConnect(ppServer, pProxySelf);
        } else if (dwError == rpc_s_auth_method)
        {
            pProxySelf->proxy_state = RPC_DISCONN;
            VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "_VmDirAppendEntriesRpc: rpc_s_auth_method peer %s", pPeerHostName);
            _VmDirRpcConnect(ppServer, pProxySelf);
        } else if (dwError == VMDIR_ERROR_UNWILLING_TO_PERFORM)
        {
             //Peer may be in process of starting up
            VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "_VmDirAppendEntriesRpc: peer %s not ready to serve", pPeerHostName);
            VmDirSleep(2000);
        } else
        {
            VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "_VmDirAppendEntriesRpc: RPC call failed to peer %s error %d",
                        pPeerHostName, dwError);
            VmDirSleep(1000);
        }
        goto cleanup;
    }

    VMDIR_LOCK_MUTEX(bLock, gRaftStateMutex);
    if (gRaftState.role != VDIR_RAFT_ROLE_LEADER || gRaftState.currentTerm != args.term)
    {
        //Other RPC calls or events changed the server term, role or state, forfeit the current RPC call result.
        VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL,
          "_VmDirAppendEntriesRpc: server state changed after RPC call; server role %d term %d cmd %d; peer: (%s) term %d",
          gRaftState.role, gRaftState.currentTerm, gRaftState.cmd, pPeerHostName, args.currentTerm);
        goto cleanup;
    }

    if (args.currentTerm > gRaftState.currentTerm)
    {
        //Remote has higher term, swtich to follower.
        int term = 0;
        gRaftState.role = VDIR_RAFT_ROLE_FOLLOWER;
        gRaftState.lastPingRecvTime = VmDirGetTimeInMilliSec(); //Set for request vote timeout.
        gRaftState.currentTerm = args.currentTerm;
        term = gRaftState.currentTerm;
        VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);

        dwError = _VmDirUpdateRaftPsState(term, FALSE, 0, NULL, 0);
        BAIL_ON_VMDIR_ERROR(dwError);

        VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "_VmDirAppendEntriesRpc: peer (%s) term %d > current term, change role to follower",
                       pPeerHostName, args.currentTerm);
        goto cleanup;
    }

    if (args.status != 0)
    {
        //Remote doesn't contain log with preLogIndex, try a lower preLogIndex
        if (args.preLogIndex == 0)
        {
           dwError = LDAP_OPERATIONS_ERROR;
           VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "_VmDirAppendEntriesRpc invalid remote server (%s) which has no log >= 0 error %d",
                           pPeerHostName, dwError);
           goto cleanup;
        }
        VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);

        //Fetch the prev log downward
        VMDIR_SAFE_FREE_MEMORY(args.entries);
        args.entries = NULL;
        args.entriesSize = 0;

        _VmDirChgLogFree(&preChgLog);

        dwError = _VmDirFetchLogEntry(args.preLogIndex, &preChgLog, __LINE__);
        BAIL_ON_VMDIR_ERROR(dwError);

        if (preChgLog.index == 0)
        {
            dwError = LDAP_OPERATIONS_ERROR;
            VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "_VmDirAppendEntriesRpc invalid local server missing prev logIndex %llu error %d",
                            args.preLogIndex, dwError);
        }

        args.entriesSize = preChgLog.packRaftLog.lberbv_len;
        dwError = VmDirAllocateAndCopyMemory(preChgLog.packRaftLog.lberbv_val, args.entriesSize, (PVOID*)&args.entries);
        BAIL_ON_VMDIR_ERROR(dwError);

        dwError = _VmDirGetPrevLogArgs(&args.preLogIndex, &args.preLogTerm, preChgLog.index - 1, __LINE__);
        BAIL_ON_VMDIR_ERROR(dwError);

        goto ReplicateLog;
    }

    //The peer has confirmed the matching preLogIndex.
    if (args.entries)
    {
        _VmDirChgLogFree(&curChgLog);
        curChgLog.packRaftLog.lberbv_len = args.entriesSize;
        dwError = VmDirAllocateAndCopyMemory(args.entries, args.entriesSize, (PVOID*)&curChgLog.packRaftLog.lberbv_val);
        BAIL_ON_VMDIR_ERROR(dwError);
        curChgLog.packRaftLog.bOwnBvVal = TRUE;

        dwError = _VmDirUnpackLogEntry(&curChgLog);
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    if(curChgLog.index > pProxySelf->matchIndex)
    {
        pProxySelf->matchIndex = curChgLog.index;
    }

    if (args.preLogIndex > pProxySelf->matchIndex)
    {
        pProxySelf->matchIndex = args.preLogIndex;
    }

    if (cmd == ExecAppendEntries)
    {
        if (curChgLog.index > 0 && gEntries && curChgLog.index == gEntries->index)
        {
            //This is AppendEntries with uncommitted log in args.entries. Now the gap has closed or no gap.
            int consensusCnt = 0;

            pProxySelf->bLogReplicated = TRUE;

            consensusCnt = _VmDirGetAppendEntriesConsensusCountInLock();
            if (consensusCnt >= (gRaftState.clusterSize/2 + 1))
            {
                 VmDirConditionSignal(gRaftAppendEntryReachConsensusCond);
            }
            //VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL,
            //        "_VmDirAppendEntriesRpc: got consent from %s current logIdx %llu term %d",
            //        pPeerHostName, gEntries->index, gRaftState.currentTerm);
            goto cleanup;
        }
    } else
    {
        //This is a ping
        if (args.preLogIndex == startLogIndex)
        {
            /* Now the peer is now in sycn. */

            /*
            VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL,
                       "_VmDirAppendEntriesRpc: peer %s in sync or closed gap with starting logIndex %llu term %d",
                       pPeerHostName, startLogIndex, gRaftState.currentTerm);
            */
            goto cleanup;
        }
    }

    if (args.preLogIndex >= startLogIndex)
    {
        goto cleanup;
    }

    /*
    VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL,
            "_VmDirAppendEntriesRpc: need to catchup logs for peer %s StartLogIndex %llu preLogIndex %llu entries %s",
            pPeerHostName, startLogIndex, args.preLogIndex, args.entries?"not null":"null");
    */

    /*
     * For Ping or AppendEntries with gap exists.
     * Will fetch and replicate logs upward until gap closed.
     */

    VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);

    _VmDirChgLogFree(&preChgLog);

    dwError = _VmDirGetNextLog(args.preLogIndex + 1, startLogIndex, &preChgLog,  __LINE__);
    if (preChgLog.index == 0)
    {
       if (cmd == ExecAppendEntries)
       {
           //Log with startLogIndex is uncommitted
           goto cleanup;
       }

       dwError =  LDAP_OPERATIONS_ERROR;
       VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL,
                       "_VmDirAppendEntriesRpc: fail to close gap for peer %s StartLogIndex %llu preLogIndex %llu cmd %d",
                      pPeerHostName, startLogIndex, args.preLogIndex, cmd);
       goto error;
    }

    args.preLogIndex = preChgLog.index;
    args.preLogTerm = preChgLog.term;

    VMDIR_SAFE_FREE_MEMORY(args.entries);
    args.entries = NULL;
    args.entriesSize = 0;

    _VmDirChgLogFree(&curChgLog);
    dwError = _VmDirGetNextLog(args.preLogIndex+1, startLogIndex, &curChgLog,  __LINE__);
    BAIL_ON_VMDIR_ERROR(dwError);

    if (curChgLog.index == 0)
    {
        //Reached startLogIndex which has an un-committed log or ping with gap closed.
        goto ReplicateLog;
    }

    //Gap has yet to close.
    args.entriesSize = curChgLog.packRaftLog.lberbv_len;
    dwError = VmDirAllocateAndCopyMemory(curChgLog.packRaftLog.lberbv_val, args.entriesSize, (PVOID*)&args.entries);
    BAIL_ON_VMDIR_ERROR(dwError);

    goto ReplicateLog;

cleanup:
    VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);
    VMDIR_SAFE_FREE_MEMORY(args.entries);
    args.entries = NULL;
    args.entriesSize = 0;
    _VmDirChgLogFree(&preChgLog);
    _VmDirChgLogFree(&curChgLog);
/*
    if (dwError == 0)
    {
        VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL,
            "_VmDirAppendEntriesRpc: rpc call complete for peer %s; startLogIdx %llu", pPeerHostName, startLogIndex);
    }
*/
    return dwError;

error:
    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "_VmDirAppendEntriesRpc: error rpc call peer %s, error %d", pPeerHostName, dwError);
    goto cleanup;
}

//Search internally to get all peer computer hosts and start the peer threads.
static
DWORD
_VmDirStartProxies(
    VOID
)
{
    DWORD dwError = ERROR_SUCCESS;
    VDIR_BERVALUE dcContainerDN = VDIR_BERVALUE_INIT;
    PSTR pHostname = NULL;
    PSTR pszName = NULL;
    VDIR_BERVALUE dcContainerDNrdn = VDIR_BERVALUE_INIT;
    VDIR_ENTRY_ARRAY entryArray = {0};
    int i = 0;
    BOOLEAN bLock = FALSE;

    VmDirGetParentDN(&(gVmdirServerGlobals.dcAccountDN), &dcContainerDN);
    if (dcContainerDN.lberbv.bv_len == 0)
    {
        dwError = LDAP_OPERATIONS_ERROR;
        BAIL_ON_VMDIR_ERROR( dwError );
    }

    dwError = VmDirSimpleEqualFilterInternalSearch(dcContainerDN.lberbv.bv_val,
                    LDAP_SCOPE_ONE, ATTR_OBJECT_CLASS, OC_COMPUTER, &entryArray);
    BAIL_ON_VMDIR_ERROR(dwError);

    VMDIR_LOCK_MUTEX(bLock, gRaftStateMutex);
    gRaftState.clusterSize = 1;
    for (i = 0; i < entryArray.iSize; i++)
    {
        dwError = VmDirNormalizeDNWrapper( &(entryArray.pEntry[i].dn));
        BAIL_ON_VMDIR_ERROR(dwError);
        if (VmDirStringCompareA(entryArray.pEntry[i].dn.bvnorm_val,
                                gVmdirServerGlobals.dcAccountDN.bvnorm_val, FALSE) == 0)
        {
            //The server is self.
            continue;
        }
        VmDirFreeBervalContent(&dcContainerDNrdn);
        dwError = VmDirGetRdn(&entryArray.pEntry[i].dn, &dcContainerDNrdn);  
        BAIL_ON_VMDIR_ERROR(dwError);

        VMDIR_SAFE_FREE_STRINGA(pHostname);
        VMDIR_SAFE_FREE_STRINGA(pszName);

        dwError = VmDirRdnToNameValue(&dcContainerDNrdn, &pszName, &pHostname);
        BAIL_ON_VMDIR_ERROR(dwError);

        dwError = _VmDirNewPeerProxyInLock(pHostname, RPC_IDLE);
        BAIL_ON_VMDIR_ERROR(dwError);

        gRaftState.clusterSize++;
    }

cleanup:
    VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);
    VmDirFreeEntryArrayContent(&entryArray);
    VmDirFreeBervalContent(&dcContainerDN);
    VmDirFreeBervalContent(&dcContainerDNrdn);
    VMDIR_SAFE_FREE_STRINGA(pHostname);
    VMDIR_SAFE_FREE_STRINGA(pszName);
    return dwError;

error:
    goto cleanup;
}

DWORD
_VmDirRequestVoteGetReply(UINT32 term, char *candidateId, unsigned long long lastLogIndex,
    UINT32 lastLogTerm, UINT32 *currentTerm, UINT32 *voteGranted)
{
    DWORD dwError = 0;
    UINT32 iVoteGranted = 1;
    UINT32 iVotedForTerm = 0;
    BOOLEAN bLock = FALSE;
    int oldterm = 0;
    VDIR_BERVALUE bvVotedFor = {0};

    *voteGranted = 1; //Default to denied with reason split vote or other than larger highest logIndex of mine.
    
    if (!gRaftState.initialized)
    {
        //Don't participate in vote if this server has not been initialized.
        VmDirSleep(WAIT_REELECTION_RAND_MS << 1); //Pause a little so that peers won't waste term sequence numbers.
        dwError = VMDIR_ERROR_UNWILLING_TO_PERFORM;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    VMDIR_LOCK_MUTEX(bLock, gRaftStateMutex);
    oldterm = gRaftState.currentTerm;

    if (term < gRaftState.currentTerm || gRaftState.lastLogTerm > lastLogTerm ||
        (gRaftState.lastLogTerm == lastLogTerm && gRaftState.lastLogIndex > lastLogIndex) ||
        (gRaftState.role == VDIR_RAFT_ROLE_LEADER && term == gRaftState.currentTerm))
    {
	//My term is larger than the requester, or my highest log/term are larger, then deny the vote.
        *currentTerm = gRaftState.currentTerm;
        if (gRaftState.lastLogTerm > lastLogTerm ||
            (gRaftState.lastLogTerm == lastLogTerm && gRaftState.lastLogIndex > lastLogIndex))
        {
            *voteGranted = 2;
        }
        goto cleanup;
    }

    if (gRaftState.votedFor.lberbv_len > 0 &&
        VmDirStringCompareA(gRaftState.votedFor.lberbv_val, candidateId, FALSE) != 0)
    {
        //I have voted for a different remote in the same term, then deny the vote.
        *currentTerm = gRaftState.currentTerm;
        goto cleanup;
    }

    //Grant the vote.
    iVoteGranted = 0;

    if (gRaftState.votedFor.lberbv_len == 0)
    {
       //Remeber (persist) the candidate I have voted and the associated term.
       dwError = VmDirAllocateBerValueAVsnprintf(&gRaftState.votedFor, "%s", candidateId);
       BAIL_ON_VMDIR_ERROR(dwError);

       iVotedForTerm = gRaftState.votedForTerm = term;
       dwError = VmDirBervalContentDup(&gRaftState.votedFor, &bvVotedFor);
       BAIL_ON_VMDIR_ERROR(dwError);
    }

    *currentTerm = gRaftState.currentTerm = term;
    *voteGranted = iVoteGranted;
    VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);

    _VmDirUpdateRaftPsState(term, iVotedForTerm > 0, iVotedForTerm, &bvVotedFor, 0);

    VMDIR_LOCK_MUTEX(bLock, gRaftStateMutex);
    if  (gRaftState.role == VDIR_RAFT_ROLE_LEADER)
    {
        //Switch to follower from leader
        gRaftState.role = VDIR_RAFT_ROLE_FOLLOWER;
        gRaftState.lastPingRecvTime = VmDirGetTimeInMilliSec(); //Set for request vote timeout.
        VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL,
          "_VmDirRequestVoteGetReply: role changed to follower; candidate %s term %d; server role %d term %d granted code %d",
          candidateId, term, gRaftState.role, *currentTerm, iVoteGranted);
    } else
    {
        VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL,
          "_VmDirRequestVoteGetReply: candidateId %s term %d lastLogTerm %d; server term %d (old term %d) role %d granted code %d",
          candidateId, term, lastLogTerm, gRaftState.currentTerm, oldterm, gRaftState.role, iVoteGranted);
    }

cleanup:
    VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);
    VmDirFreeBervalContent(&bvVotedFor);
    return dwError;

error:
    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL,
          "_VmDirRequestVoteGetReply: error %d candidateId %s term %d lastLogTerm %d; server term %d (old term %d) role %d granted code %d error %d",
          dwError, candidateId, term, lastLogTerm, gRaftState.currentTerm, oldterm, gRaftState.role, iVoteGranted);
    goto cleanup;
}

DWORD
_VmDirAppendEntriesGetReply(
    UINT32 term,
    char *leader,
    unsigned long long preLogIndex,
    UINT32 preLogTerm,
    unsigned long long leaderCommit,
    int entrySize,
    char *entries,
    UINT32 *currentTerm,
    UINT32 *status
    )
{
    DWORD dwError = 0;
    BOOLEAN bLock = FALSE;
    int oldterm = 0;
    unsigned long long indexToApply = 0;
    BOOLEAN bLogFound = FALSE;
    BOOLEAN bTermMatch = FALSE;
    VDIR_RAFT_LOG chgLog = {0};
    static int logCnt = 0;
    static time_t prevLogTime = {0};
    time_t now = {0};
    BOOLEAN bLeaderChanged = FALSE;

    *status = 1;

    if (!gRaftState.initialized || !_VmDirRaftPeerIsReady(leader))
    {
        //Don't try to replicate if this server is not initialized or the peer thread is not ready.
        dwError = VMDIR_ERROR_UNWILLING_TO_PERFORM;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    if (gRaftState.leader.lberbv.bv_len == 0 || 
        VmDirStringCompareA(gRaftState.leader.lberbv.bv_val, leader, FALSE) !=0 )
    {
        bLeaderChanged = TRUE;
    }

    VMDIR_LOCK_MUTEX(bLock, gRaftStateMutex);

    if (bLeaderChanged)
    {
        VmDirFreeBervalContent(&gRaftState.leader);
        dwError = VmDirAllocateBerValueAVsnprintf(&gRaftState.leader, "%s", leader);
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    oldterm = gRaftState.currentTerm;

    if (gRaftState.currentTerm > term)
    {
        //Tell remote to switch to follower
        *currentTerm = gRaftState.currentTerm;
        goto cleanup;
    }

    //Switch to follower if not, or keep as the follower role
    gRaftState.role = VDIR_RAFT_ROLE_FOLLOWER;
    *currentTerm = gRaftState.currentTerm = term;
    gRaftState.lastPingRecvTime = VmDirGetTimeInMilliSec(); 
    VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);

    if (term != oldterm)
    {
        _VmDirUpdateRaftPsState(term, FALSE, 0, NULL, 0);
    }

    if (preLogIndex == 0)
    {
        //No any log yet, considered a match
        bLogFound = bTermMatch = TRUE;
    } else
    {
        dwError = _VmDirLogLookup(preLogIndex, preLogTerm, &bLogFound, &bTermMatch);
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    if (!bLogFound || !bTermMatch)
    {
        //Tell remote to decrement preLogIndex, and provides an older logEntry.
        goto cleanup;
    }

    //Now preLogIndex found locally and has a macthing term - delete all logs above preLogIndex+1,
    //  those logs are uncommitted, and were replicated from old leaders.
    dwError = _VmDirDeleteAllLogs(preLogIndex+1);
    BAIL_ON_VMDIR_ERROR(dwError);

    if (entrySize > 0)
    {
        chgLog.packRaftLog.lberbv_len = entrySize;
        dwError = VmDirAllocateAndCopyMemory(entries, entrySize, (PVOID*)&chgLog.packRaftLog.lberbv_val);
        BAIL_ON_VMDIR_ERROR(dwError);
        chgLog.packRaftLog.bOwnBvVal = TRUE;

        dwError = _VmDirUnpackLogEntry(&chgLog);
        BAIL_ON_VMDIR_ERROR(dwError);

        dwError = _VmDirPersistLog(&chgLog);
        BAIL_ON_VMDIR_ERROR(dwError);

        VMDIR_LOCK_MUTEX(bLock, gRaftStateMutex);
        gRaftState.lastLogIndex = chgLog.index;
        gRaftState.lastLogTerm = chgLog.term;
        indexToApply = gRaftState.commitIndex = VMDIR_MIN(leaderCommit, gRaftState.lastLogIndex);
        VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);
    } else
    {
        VMDIR_LOCK_MUTEX(bLock, gRaftStateMutex);
        indexToApply = gRaftState.commitIndex = VMDIR_MIN(leaderCommit, preLogIndex);
        VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);
    }

    if (indexToApply > 0)
    {
        _VmDirApplyLogsUpto(indexToApply);
    }

    *status = 0;

cleanup:
    VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);

    if (dwError == 0 && logCnt++ % 10 == 0)
    {
        now = time(&now);
        if ((now - prevLogTime) > 30)
        {
            prevLogTime = now;
            //Log ping or appendEntries not more than every 10 calls or 30 seconds
            VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL,
              "_VmDirAppendEntriesGetReply: entrySize %d; leader %s term %d leaderCommit %llu preLogIndex %llu preLogTerm %d; server term %d (old term %d) role %d status %d",
              entrySize, leader, term, leaderCommit, preLogIndex, preLogTerm, *currentTerm, oldterm, gRaftState.role, *status);
        }
    }
    _VmDirChgLogFree(&chgLog);
    return dwError;

error:
     VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL,
            "_VmDirAppendEntriesGetReply: entrySize %d; error %d leader %s term %d leaderCommit %llu preLogIndex %llu preLogTerm %d; server term %d (old term %d) role %d status %d gRaftStateInitialized %s",
            entrySize, dwError, leader, term, leaderCommit, preLogIndex, preLogTerm, *currentTerm, oldterm, gRaftState.role, *status, gRaftState.initialized?"Yes":"No");
    goto cleanup;
}

//Only one thread can enter VmDirRaftCommitHook - it is serialzied by the MDB write transaction mutex.
int VmDirRaftCommitHook()
{
    int dwError = 0;
    BOOLEAN bLock = FALSE;
    int retryCnt = 0;

    if (gLogEntry.index == 0)
    {
       /*
        * This transaciton has no log entry to replicate:
        * 1. If the server is a leader, then it may need to update Raft PS state.
        *    in such case, just commit it locally.
        * 2. If the server is a candidate or follower, always allow it to commit transaction
        *    for Raft state and local log entry.
        */
        goto cleanup;
    }

    VMDIR_LOCK_MUTEX(bLock, gRaftStateMutex);
    if (gRaftState.clusterSize < 2)
    {
        //This is a standalone server
        gRaftState.commitIndex = gRaftState.lastApplied = gLogEntry.index; 
        gRaftState.commitIndexTerm = gLogEntry.term;
        _VmDirChgLogFree(&gLogEntry);
        goto cleanup;
    }

    if (gRaftState.role != VDIR_RAFT_ROLE_LEADER)
    {
        dwError = VMDIR_ERROR_UNWILLING_TO_PERFORM;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    retryCnt = 0;
    do
    {
        if (_VmDirPeersIdleInLock() < (gRaftState.clusterSize/2))
        {
             //Wait only if not enough in Ready.
             dwError = VmDirConditionTimedWait(gPeersReadyCond, gRaftStateMutex, WAIT_PEERS_READY_MS);
        }
        if (dwError == ETIMEDOUT && retryCnt++ > 5)
        {
            dwError = VMDIR_ERROR_UNWILLING_TO_PERFORM;
            BAIL_ON_VMDIR_ERROR(dwError);
        }
    } while (dwError == ETIMEDOUT);

    BAIL_ON_VMDIR_ERROR(dwError);

    //Now invoke paralle RPC calls to all (available) peers
    VmDirConditionBroadcast(gRaftRequestPendingCond);

    gRaftState.cmd = ExecAppendEntries;
    //gEntries is accessed by proxy threads.
    gEntries = &gLogEntry;
    _VmDirClearProxyLogReplicatedInLock();

    //Wait for majority peers to replicate the log.
    //VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "VmDirRaftCommitHook: wait gRaftAppendEntryReachConsensusCond; role %d term %d",
    //               gRaftState.role, gRaftState.currentTerm);

    VmDirConditionTimedWait(gRaftAppendEntryReachConsensusCond, gRaftStateMutex, WAIT_CONSENSUS_TIMEOUT_MS);

    if (_VmDirGetAppendEntriesConsensusCountInLock() < (gRaftState.clusterSize/2 + 1))
    {
        //Check ConsensusCount again since it may be waken up by shutdown;
        dwError = VMDIR_ERROR_UNWILLING_TO_PERFORM;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    //The log entry is committed,
    gRaftState.cmd = ExecNone;
    gRaftState.commitIndex = gRaftState.lastApplied = gRaftState.lastLogIndex = gLogEntry.index;
    gRaftState.lastLogTerm = gRaftState.commitIndexTerm = gLogEntry.term;

    //VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "VmDirRaftCommitHook: succeeded; server role %d term %d lastApplied %llu",
    //    gRaftState.role, gRaftState.currentTerm, gRaftState.lastApplied);

cleanup:
    _VmDirChgLogFree(&gLogEntry);
    gEntries = NULL;
    VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);
    return dwError;

error:
    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "VmDirRaftCommitHook: error %d", dwError);
    goto cleanup;
}

//This create chglog for the LDAP Add right becore calling pfnBETxnCommit, At this poiont,
//  all validations of the LDAP add have completed, and the all changes associated with
//  the LDAP Add (including indices creation) have been applied to the MDB backends
DWORD VmDirAddRaftPreCommit(PVDIR_ENTRY pEntry, PVDIR_OPERATION pAddOp)
{
    DWORD dwError = 0;
    char *p = NULL;
    BOOLEAN bLock = FALSE;
    VDIR_BERVALUE encodedEntry = VDIR_BERVALUE_INIT;

    if (pEntry->eId < NEW_ENTRY_EID_PREFIX)
    {
       //Don't create log for entry with system assigned eid.
       goto cleanup;
    }

    if ((p=VmDirStringCaseStrA(pEntry->dn.bvnorm_val, RAFT_CONTEXT_DN)) &&
         VmDirStringCompareA(p, RAFT_CONTEXT_DN, FALSE)==0)
    {
        //Don't create log entry for raft context entry.
        if (pEntry->eId >= NEW_ENTRY_EID_PREFIX)
        {
            //don't use raft assigned eid;
            pEntry->eId = 0;
        }
        goto cleanup;
    }

    VMDIR_LOCK_MUTEX(bLock, gRaftStateMutex);
    if (gRaftState.clusterSize >= 2 && gRaftState.role != VDIR_RAFT_ROLE_LEADER)
    {
        dwError = VMDIR_ERROR_UNWILLING_TO_PERFORM;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    gLogEntry.index = gRaftState.commitIndex + 1;
    gLogEntry.term = gRaftState.currentTerm;
    gLogEntry.entryId = pEntry->eId;
    gLogEntry.requestCode = LDAP_REQ_ADD;

    dwError = VmDirEncodeEntry( pEntry, &encodedEntry );
    BAIL_ON_VMDIR_ERROR(dwError);

    gLogEntry.chglog.lberbv_len = encodedEntry.lberbv.bv_len;
    gLogEntry.chglog.lberbv_val = encodedEntry.lberbv.bv_val;
    gLogEntry.chglog.bOwnBvVal = TRUE;

    dwError = _VmDirPackLogEntry(&gLogEntry);
    BAIL_ON_VMDIR_ERROR(dwError);

    VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);

    dwError = VmDirAddRaftEntry(pEntry->pSchemaCtx, &gLogEntry, pAddOp);
    BAIL_ON_VMDIR_ERROR(dwError);

cleanup:
    VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);
    return dwError;

error:
    _VmDirChgLogFree(&gLogEntry);
    goto cleanup;
}

DWORD VmDirModifyRaftPreCommit(
    PVDIR_SCHEMA_CTX pSchemaCtx,
    ENTRYID entryId,
    char *dn,
    PVDIR_MODIFICATION pmods,
    PVDIR_OPERATION pModifyOp)
{
    DWORD dwError = 0;
    char *p = NULL;
    BOOLEAN bLock = FALSE;
    VDIR_BERVALUE encodedMods = {0};

    if ((p=VmDirStringCaseStrA(dn, RAFT_CONTEXT_DN)) &&
        VmDirStringCompareA(p, RAFT_CONTEXT_DN, FALSE)==0)
    {
        goto cleanup;
    }

    VMDIR_LOCK_MUTEX(bLock, gRaftStateMutex);
    if (gRaftState.clusterSize >= 2 && gRaftState.role != VDIR_RAFT_ROLE_LEADER)
    {
        dwError = VMDIR_ERROR_UNWILLING_TO_PERFORM;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    dwError = VmDirEncodeMods(pmods, &encodedMods);
    BAIL_ON_VMDIR_ERROR(dwError);

    gLogEntry.index = gRaftState.commitIndex + 1;
    gLogEntry.term = gRaftState.currentTerm;
    gLogEntry.entryId = entryId;
    gLogEntry.requestCode = LDAP_REQ_MODIFY;
    gLogEntry.chglog.lberbv_len = encodedMods.lberbv.bv_len;
    gLogEntry.chglog.lberbv_val = encodedMods.lberbv.bv_val;
    gLogEntry.chglog.bOwnBvVal = TRUE;
    encodedMods.lberbv.bv_val = NULL;
    encodedMods.lberbv.bv_len = 0;
    dwError = _VmDirPackLogEntry(&gLogEntry);
    BAIL_ON_VMDIR_ERROR(dwError);

    VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);

    dwError = VmDirAddRaftEntry(pSchemaCtx, &gLogEntry, pModifyOp);
    BAIL_ON_VMDIR_ERROR(dwError);

cleanup:
    VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);
    return dwError;

error:
    _VmDirChgLogFree(&gLogEntry);
    VmDirFreeBervalContent(&encodedMods);
    goto cleanup;
}

DWORD VmDirDeleteRaftPreCommit(
    PVDIR_SCHEMA_CTX pSchemaCtx,
    EntryId eid,
    char *dn,
    PVDIR_OPERATION pDeleteOp)
{
    DWORD dwError = 0;
    char *p = NULL;
    BOOLEAN bLock = FALSE;

    if ((p=VmDirStringCaseStrA(dn, RAFT_CONTEXT_DN)) &&
        VmDirStringCompareA(p, RAFT_CONTEXT_DN, FALSE)==0)
    {
        goto cleanup;
    }

    dwError = _VmDirDeleteRaftProxy(dn);
    BAIL_ON_VMDIR_ERROR(dwError);

    VMDIR_LOCK_MUTEX(bLock, gRaftStateMutex);
    if (gRaftState.clusterSize >= 2 && gRaftState.role != VDIR_RAFT_ROLE_LEADER)
    {
        dwError = VMDIR_ERROR_UNWILLING_TO_PERFORM;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    gLogEntry.index = gRaftState.commitIndex + 1;
    gLogEntry.term = gRaftState.currentTerm;
    gLogEntry.entryId = eid;
    gLogEntry.requestCode = LDAP_REQ_DELETE;
    gLogEntry.chglog.lberbv_len = 0;
    gLogEntry.chglog.lberbv_val = NULL;
    dwError = _VmDirPackLogEntry(&gLogEntry);
    BAIL_ON_VMDIR_ERROR(dwError);

    VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);

    dwError = VmDirAddRaftEntry(pSchemaCtx, &gLogEntry, pDeleteOp);
    BAIL_ON_VMDIR_ERROR(dwError);

cleanup:
    VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);
    return dwError;

error:
    _VmDirChgLogFree(&gLogEntry);
    goto cleanup;
}

DWORD
VmDirAddRaftProxy(PVDIR_ENTRY pEntry)
{
    DWORD dwError = 0;
    VDIR_BERVALUE peerRdn = VDIR_BERVALUE_INIT;
    VDIR_BERVALUE dcContainerDN = VDIR_BERVALUE_INIT;
    PSTR pHostname = NULL;
    PSTR  pszName = NULL;
    BOOLEAN bLock = FALSE;

    if (pEntry->dn.bvnorm_val == NULL ||
        gVmdirServerGlobals.dcAccountDN.bvnorm_val == NULL ||
        pEntry->pdn.bvnorm_len == 0 ||
        VmDirStringCompareA(pEntry->dn.bvnorm_val, gVmdirServerGlobals.dcAccountDN.bvnorm_val, FALSE) == 0)
    {
        //Don't add server as peer if this server has not been configured yet or the server is self
        goto cleanup;
    }

    VmDirGetParentDN(&(gVmdirServerGlobals.dcAccountDN), &dcContainerDN);
    if (dcContainerDN.lberbv.bv_len == 0)
    {
        dwError = LDAP_OPERATIONS_ERROR;
        BAIL_ON_VMDIR_ERROR( dwError );
    }

    if (VmDirStringCompareA(pEntry->pdn.bvnorm_val, dcContainerDN.bvnorm_val, FALSE) != 0)
    {
       goto cleanup;
    }

    dwError = VmDirGetRdn(&pEntry->dn, &peerRdn);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirRdnToNameValue(&peerRdn, &pszName, &pHostname);
    BAIL_ON_VMDIR_ERROR(dwError);

    if (VmDirStringCompareA(gRaftState.hostname.lberbv.bv_val, pHostname, FALSE)==0)
    {
        goto cleanup;
    }

    VMDIR_LOCK_MUTEX(bLock, gRaftStateMutex);
    dwError = _VmDirNewPeerProxyInLock(pHostname, PENDING_ADD);
    VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);
    BAIL_ON_VMDIR_ERROR(dwError);

cleanup:
    VmDirFreeBervalContent(&dcContainerDN);
    VmDirFreeBervalContent(&peerRdn);
    VMDIR_SAFE_FREE_MEMORY(pHostname);
    VMDIR_SAFE_FREE_MEMORY(pszName);
    return dwError;
error:
    goto cleanup;
}

static
DWORD
_VmDirDeleteRaftProxy(char *dn_norm)
{
    DWORD dwError = 0;
    VDIR_BERVALUE peerRdn = VDIR_BERVALUE_INIT;
    VDIR_BERVALUE dcContainerDN = VDIR_BERVALUE_INIT;
    VDIR_BERVALUE parentDN = VDIR_BERVALUE_INIT;
    VDIR_BERVALUE bvDN = VDIR_BERVALUE_INIT;
    PSTR pHostname = NULL;
    PSTR  pszName = NULL;
    BOOLEAN bLock = FALSE;

    if (dn_norm == NULL)
    {
        dwError = VMDIR_ERROR_INVALID_PARAMETER;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    bvDN.lberbv.bv_val = dn_norm;
    bvDN.lberbv.bv_len = VmDirStringLenA(dn_norm);

    dwError =  VmDirGetParentDN(&bvDN, &parentDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    if (VmDirStringCompareA(dn_norm, gVmdirServerGlobals.dcAccountDN.bvnorm_val, FALSE) == 0)
    {
        /* 
         * Deleting a Raft leader should follow the procedure:
         *  1. Shutdown the server which is the raft leader
         *  2. Send deleting account LDAP operation for this account toward the new Raft leader (or to be standalone server).
         *  3. Shutdown the server with the account deleted above (better to rename the data.mdb once it is shutdown).
         */
        dwError = VMDIR_ERROR_UNWILLING_TO_PERFORM;
        VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "VmDirDeleteRaftProxy: deleting account with raft leader is not allowed; error %d", dwError);
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    dwError = VmDirGetParentDN(&(gVmdirServerGlobals.dcAccountDN), &dcContainerDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    if (dcContainerDN.lberbv.bv_len == 0)
    {
        dwError = LDAP_OPERATIONS_ERROR;
        BAIL_ON_VMDIR_ERROR( dwError );
    }

    if (VmDirStringCompareA(parentDN.lberbv.bv_val, dcContainerDN.bvnorm_val, FALSE) != 0)
    {
       //Not a machine account
       goto cleanup;
    }

    dwError = VmDirGetRdn(&bvDN, &peerRdn);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirRdnToNameValue(&peerRdn, &pszName, &pHostname);
    BAIL_ON_VMDIR_ERROR(dwError);

    VMDIR_LOCK_MUTEX(bLock, gRaftStateMutex);
    _VmDirRemovePeerInLock(pHostname);
    VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);

cleanup:
    VmDirFreeBervalContent(&dcContainerDN);
    VmDirFreeBervalContent(&peerRdn);
    VmDirFreeBervalContent(&parentDN);
    VMDIR_SAFE_FREE_MEMORY(pHostname);
    VMDIR_SAFE_FREE_MEMORY(pszName);
    return dwError;

error:
    goto cleanup;
}

static
DWORD
_VmDirRpcConnect(PVMDIR_SERVER_CONTEXT *ppServer, PVMDIR_PEER_PROXY pProxySelf)
{
    DWORD dwError = 0;
    PVMDIR_SERVER_CONTEXT pServer = NULL;
    PSTR pszDcAccountPwd = NULL;
    PCSTR pPeerHostName = pProxySelf->raftPeerHostname;
    UINT32 remoteServerState = 0;
    BOOLEAN bLock = FALSE;
    static int logCnt = 0;

    assert(ppServer);

    if (*ppServer)
    {
        VmDirCloseServer(*ppServer);
        *ppServer = NULL;
    }

    while(VmDirdState() != VMDIRD_STATE_SHUTDOWN && pProxySelf->isDeleted == FALSE)
    {
       VMDIR_SAFE_FREE_MEMORY(pszDcAccountPwd);
       dwError = VmDirReadDCAccountPassword(&pszDcAccountPwd);
       BAIL_ON_VMDIR_ERROR( dwError );

       if (pServer)
       {
           VmDirCloseServer( pServer);
           pServer = NULL;
       }
       dwError = VmDirOpenServerA(pPeerHostName, gVmdirServerGlobals.dcAccountUPN.lberbv_val,
                              NULL, pszDcAccountPwd, 0, NULL, &pServer);
       if (dwError == rpc_s_connect_rejected || dwError == rpc_s_connect_timed_out ||
           dwError == rpc_s_cannot_connect || dwError == rpc_s_connection_closed ||
           dwError == rpc_s_auth_method)
       {
          if (logCnt++ % 10 == 0)
          {
              VMDIR_LOG_WARNING(VMDIR_LOG_MASK_ALL,
                                "_VmDirRpcConnect: not connected or authorization error %d host %s, will retry to connect.",
                                dwError, pPeerHostName);
          }
          VmDirSleep(gVmdirGlobals.dwRaftPingIntervalMS>>1);
          continue;
       } else if (dwError != 0)
       {
           if (logCnt++ % 5 == 0)
           {
               VMDIR_LOG_WARNING(VMDIR_LOG_MASK_ALL, "_VmDirRpcConnect: VmDirOpenServerA error %d host %s",
                          dwError, pPeerHostName);
           }
           goto error;
       }

       //Try to make an rpc call.
       dwError = VmDirGetState(pServer, &remoteServerState);
       if (dwError != 0)
       {
           VmDirCloseServer( pServer);
           pServer = NULL;
           if (logCnt++ % 5 == 0)
           {
               VMDIR_LOG_WARNING(VMDIR_LOG_MASK_ALL, "_VmDirRpcConnect: cannot get server state for host %s, will retry.",
                                 pPeerHostName);
           }
           VmDirSleep(3000);
           continue;
       }

       VMDIR_LOCK_MUTEX(bLock, gRaftStateMutex);
       if (pProxySelf->proxy_state == PENDING_ADD)
       {
           pProxySelf->proxy_state = RPC_BUSY;
           gRaftState.clusterSize++;
       }
       VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);

       VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "_VmDirRpcConnect: RPC to %s established.", pPeerHostName);
       *ppServer = pServer;
       pServer = NULL;
       break;
    }

cleanup:
    VMDIR_SAFE_FREE_STRINGA(pszDcAccountPwd);
    return dwError;

error:
    if (pServer)
    {
        VmDirCloseServer( pServer);
    }
    goto cleanup;
}

static
VOID
_VmDirApplyLogsUpto(UINT64 indexToApply)
{
     UINT64 logIdx = 0;
     UINT64 logIdxStart = gRaftState.lastApplied + 1;

     //VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "_VmDirApplyLogsUpto: logIdxStart %llu upto logIdx %llu", logIdxStart, indexToApply);
     for (logIdx = logIdxStart; logIdx <= indexToApply; logIdx++)
     {
         _VmDirApplyLog(logIdx);
     }
}

static
DWORD
_VmDirApplyLog(unsigned long long indexToApply)
{
    DWORD dwError = 0;
    VDIR_RAFT_LOG logEntry = {0};
    VDIR_ENTRY entry = {0};
    PVDIR_SCHEMA_CTX pSchemaCtx = NULL;
    VDIR_OPERATION ldapOp = {0};
    VDIR_OPERATION modOp = {0};
    char logEntryDn[RAFT_CONTEXT_DN_MAX_LEN] = {0};
    VDIR_BERVALUE bvIndexApplied = VDIR_BERVALUE_INIT;
    PSTR pszLocalErrorMsg = NULL;
    char logIndexStr[VMDIR_MAX_I64_ASCII_STR_LEN] = {0};
    char opStr[RAFT_CONTEXT_DN_MAX_LEN] = {0};
    BOOLEAN bLock = FALSE;
    BOOLEAN bHasTxn = FALSE; 
    int iPostCommitPluginRtn = 0;

    VMDIR_LOCK_MUTEX(bLock, gRaftStateMutex);
    if (indexToApply <= gRaftState.lastApplied)
    {
        //already applied.
        VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);
        VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "_VmDirApplyLog: ignore allready applied LogIndex %llu lastApplied %llu",
                     indexToApply, gRaftState.lastApplied);
        goto cleanup;
    }
    VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);

    dwError = _VmDirFetchLogEntry(indexToApply, &logEntry, __LINE__);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirSchemaCtxAcquire( &pSchemaCtx );
    BAIL_ON_VMDIR_ERROR(dwError);

    if (logEntry.requestCode == LDAP_REQ_ADD)
    {
        strcpy(opStr, "Add");
        entry.encodedEntrySize = logEntry.chglog.lberbv_len;
        dwError = VmDirAllocateAndCopyMemory(logEntry.chglog.lberbv_val, entry.encodedEntrySize,
                                             (PVOID *)&entry.encodedEntry);
        BAIL_ON_VMDIR_ERROR(dwError);

        dwError = VmDirDecodeEntry(pSchemaCtx, &entry);
        BAIL_ON_VMDIR_ERROR_WITH_MSG( dwError, (pszLocalErrorMsg), "VmDirDecodeEntry with logIndx %llu", indexToApply);
    
        entry.eId = logEntry.entryId;

        dwError = VmDirGetParentDN(&entry.dn, &entry.pdn );
        BAIL_ON_VMDIR_ERROR_WITH_MSG( dwError, (pszLocalErrorMsg), "VmDirGetParentDN with logIndx %llu", indexToApply);
    
        dwError = VmDirInitStackOperation( &ldapOp, VDIR_OPERATION_TYPE_REPL, LDAP_REQ_ADD, pSchemaCtx );
        BAIL_ON_VMDIR_ERROR(dwError);

        ldapOp.request.addReq.pEntry = &entry;
    
        ldapOp.pBEIF = VmDirBackendSelect(NULL);
        assert(ldapOp.pBEIF);
    
        dwError = VmDirEntryAttrValueNormalize(&entry, FALSE /*all attributes*/);
        BAIL_ON_VMDIR_ERROR(dwError);
    
        dwError = ldapOp.pBEIF->pfnBETxnBegin(ldapOp.pBECtx, VDIR_BACKEND_TXN_WRITE);
        BAIL_ON_VMDIR_ERROR(dwError);
        bHasTxn = TRUE;
    
        dwError = ldapOp.pBEIF->pfnBEEntryAdd(ldapOp.pBECtx, &entry);
        BAIL_ON_VMDIR_ERROR_WITH_MSG( dwError, (pszLocalErrorMsg),
              "pfnBEEntryAdd  %s from logIndx %llu", entry.dn.lberbv_val, indexToApply);
    } else if (logEntry.requestCode == LDAP_REQ_MODIFY)
    {
        strcpy(opStr, "Modify");
        ModifyReq*   modReq = NULL;
        BOOLEAN bDnModified = FALSE;

        dwError = VmDirInitStackOperation( &ldapOp, VDIR_OPERATION_TYPE_REPL, LDAP_REQ_MODIFY, pSchemaCtx );
        BAIL_ON_VMDIR_ERROR(dwError);

        ldapOp.pBEIF = VmDirBackendSelect(NULL);
        assert(ldapOp.pBEIF);

        modReq = &(ldapOp.request.modifyReq);

        dwError = VmDirDecodeMods(pSchemaCtx, logEntry.chglog.lberbv_val, &modReq->mods);
        BAIL_ON_VMDIR_ERROR_WITH_MSG( dwError, (pszLocalErrorMsg), "VmDirDecodeMods from logIndx %llu", indexToApply);

        dwError = ldapOp.pBEIF->pfnBETxnBegin(ldapOp.pBECtx, VDIR_BACKEND_TXN_WRITE);
        BAIL_ON_VMDIR_ERROR(dwError);
        bHasTxn = TRUE;

        dwError = ldapOp.pBEIF->pfnBEIdToEntry(ldapOp.pBECtx, pSchemaCtx,
                                               logEntry.entryId, &entry, VDIR_BACKEND_ENTRY_LOCK_WRITE);
        BAIL_ON_VMDIR_ERROR_WITH_MSG( dwError, (pszLocalErrorMsg), "pfnBEIdToEntry from logIndx %llu", indexToApply);

        dwError = VmDirBervalContentDup(&entry.dn, &modReq->dn);
        BAIL_ON_VMDIR_ERROR(dwError);

        // Apply modify operations to the current entry (in pack format)
        dwError = VmDirApplyModsToEntryStruct(pSchemaCtx, modReq, &entry, &bDnModified, &pszLocalErrorMsg );
        BAIL_ON_VMDIR_ERROR_WITH_MSG( dwError, pszLocalErrorMsg, "ApplyModsToEntryStruct (%s)", pszLocalErrorMsg);

        dwError = ldapOp.pBEIF->pfnBEEntryModify(ldapOp.pBECtx, modReq->mods, &entry);
        BAIL_ON_VMDIR_ERROR_WITH_MSG( dwError, pszLocalErrorMsg, "BEEntryModify, (%s)",
                                      VDIR_SAFE_STRING(ldapOp.pBEErrorMsg));
    } else if (logEntry.requestCode == LDAP_REQ_DELETE)
    {
        strcpy(opStr, "Delete");
        DeleteReq *delReq = NULL;
        ModifyReq *modReq = NULL;

        dwError = VmDirInitStackOperation( &ldapOp, VDIR_OPERATION_TYPE_REPL, LDAP_REQ_DELETE, pSchemaCtx );
        BAIL_ON_VMDIR_ERROR(dwError);

        ldapOp.pBEIF = VmDirBackendSelect(NULL);
        assert(ldapOp.pBEIF);

        dwError = ldapOp.pBEIF->pfnBETxnBegin(ldapOp.pBECtx, VDIR_BACKEND_TXN_WRITE);
        BAIL_ON_VMDIR_ERROR(dwError);
        bHasTxn = TRUE;

        dwError = ldapOp.pBEIF->pfnBEIdToEntry(ldapOp.pBECtx, pSchemaCtx,
                                               logEntry.entryId, &entry, VDIR_BACKEND_ENTRY_LOCK_WRITE);
        BAIL_ON_VMDIR_ERROR_WITH_MSG(dwError, (pszLocalErrorMsg),
                                     "Delete entry pfnBEIdToEntry eId %llu", logEntry.entryId);

        dwError = VmDirEntryUnpack(&entry);
        BAIL_ON_VMDIR_ERROR_WITH_MSG( dwError, (pszLocalErrorMsg), "Delete entry VmDirEntryUnpack");

        delReq = &(ldapOp.request.deleteReq);
        modReq = &(ldapOp.request.modifyReq);

        dwError = VmDirBervalContentDup(&(entry.dn), &(delReq->dn));
        BAIL_ON_VMDIR_ERROR(dwError);

        dwError = VmDirNormalizeDN( &(delReq->dn), pSchemaCtx );
        BAIL_ON_VMDIR_ERROR_WITH_MSG(dwError, pszLocalErrorMsg, "Delete entry, normalizationDN");

        dwError = VmDirNormalizeMods(pSchemaCtx, modReq->mods, &pszLocalErrorMsg );
        BAIL_ON_VMDIR_ERROR(dwError);

        dwError = VmDirGetParentDN(&entry.dn, &entry.pdn );
        BAIL_ON_VMDIR_ERROR_WITH_MSG(dwError, pszLocalErrorMsg, "Delete entry; get ParentDn");

        dwError = GenerateDeleteAttrsMods( &ldapOp, &entry );
        BAIL_ON_VMDIR_ERROR_WITH_MSG(dwError, pszLocalErrorMsg, "GenerateDeleteAttrsMods");

        dwError = VmDirNormalizeMods(pSchemaCtx, modReq->mods, &pszLocalErrorMsg );
        BAIL_ON_VMDIR_ERROR(dwError);

        dwError = VmDirApplyModsToEntryStruct(pSchemaCtx, modReq, &entry, NULL, &pszLocalErrorMsg );
        BAIL_ON_VMDIR_ERROR_WITH_MSG(dwError, pszLocalErrorMsg, "Delete entry VmDirApplyModsToEntryStruct");

        dwError = ldapOp.pBEIF->pfnBEEntryDelete(ldapOp.pBECtx, modReq->mods, &entry );
        BAIL_ON_VMDIR_ERROR_WITH_MSG( dwError, (pszLocalErrorMsg), "pfnBEEntryDelete from logIndx %llu", indexToApply);

        dwError = DeleteRefAttributesValue(&ldapOp, &(entry.dn));
        BAIL_ON_VMDIR_ERROR_WITH_MSG(dwError, (pszLocalErrorMsg),
                                     "DeleteRefAttributesValue from logIndx %llu", indexToApply);

    } else
    {
        dwError = LDAP_OPERATIONS_ERROR;
        BAIL_ON_VMDIR_ERROR_WITH_MSG( dwError, pszLocalErrorMsg, "Invalid log format");
    }

    dwError = VmDirCloneStackOperation(&ldapOp, &modOp, VDIR_OPERATION_TYPE_INTERNAL, LDAP_REQ_MODIFY, NULL);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirStringPrintFA(logEntryDn, sizeof(logEntryDn),  "%s=%llu,%s",
                    ATTR_CN, logEntry.index, RAFT_LOGS_CONTAINER_DN);
    BAIL_ON_VMDIR_ERROR(dwError);
    
    modOp.reqDn.lberbv.bv_val = logEntryDn;
    modOp.reqDn.lberbv.bv_len = VmDirStringLenA(logEntryDn);
    
    dwError = VmDirStringPrintFA(logIndexStr, sizeof(logIndexStr), "%llu", indexToApply);
    BAIL_ON_VMDIR_ERROR(dwError);

    bvIndexApplied.lberbv.bv_val  = logIndexStr;
    bvIndexApplied.lberbv.bv_len = VmDirStringLenA(logIndexStr);

    dwError = VmDirAddModSingleAttributeReplace(&modOp, RAFT_PERSIST_STATE_DN,
                                                ATTR_RAFT_LAST_APPLIED, &bvIndexApplied);
    BAIL_ON_VMDIR_ERROR_WITH_MSG( dwError, (pszLocalErrorMsg),
          "VmDirAddModSingleAttributeReplace mode on %s = %llu ", ATTR_RAFT_LAST_APPLIED, logIndexStr);

    dwError = VmDirInternalModifyEntry(&modOp);
    BAIL_ON_VMDIR_ERROR_WITH_MSG( dwError, (pszLocalErrorMsg),
          "VmDirInternalModifyEntry mod on %s = %llu ", ATTR_RAFT_LAST_APPLIED, logIndexStr);  

    dwError = ldapOp.pBEIF->pfnBETxnCommit(ldapOp.pBECtx);
    BAIL_ON_VMDIR_ERROR(dwError);

    if (logEntry.requestCode == LDAP_REQ_ADD)
    {
        dwError = VmDirEntryUnpack(&entry);
        BAIL_ON_VMDIR_ERROR_WITH_MSG(dwError, pszLocalErrorMsg, "VmDirEntryUnpack)");

        dwError = VmDirSchemaEntryPreAdd(&ldapOp, &entry);
        BAIL_ON_VMDIR_ERROR_WITH_MSG(dwError, pszLocalErrorMsg, "SchemaEntryPreAdd");

        iPostCommitPluginRtn = VmDirExecutePostAddCommitPlugins(&ldapOp, &entry, dwError);


        if (iPostCommitPluginRtn != LDAP_SUCCESS && iPostCommitPluginRtn != ldapOp.ldapResult.errCode)
        {
            VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "_VmDirApplyLog: ADD, VdirExecutePostAddCommitPlugins %s - code(%d)",
                    entry.dn.lberbv_val, iPostCommitPluginRtn);
        }
    } else if (logEntry.requestCode == LDAP_REQ_MODIFY)
    {
        dwError = VmDirSchemaModMutexAcquire(&ldapOp);
        BAIL_ON_VMDIR_ERROR_WITH_MSG(dwError, pszLocalErrorMsg, "Lock schema mod mutex");

        iPostCommitPluginRtn = VmDirExecutePostModifyCommitPlugins(&ldapOp, &entry, dwError);

        if ( iPostCommitPluginRtn != LDAP_SUCCESS && iPostCommitPluginRtn != ldapOp.ldapResult.errCode)
        {
            VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "_VmDirApplyLog: MODIFY: VdirExecutePostModifyCommitPlugins %s - code(%d)",
                      entry.dn.lberbv_val, iPostCommitPluginRtn);
        }
    }

    VMDIR_LOCK_MUTEX(bLock, gRaftStateMutex);
    gRaftState.lastApplied = indexToApply;
    if ( gRaftState.commitIndex < logEntry.index)
    {
         gRaftState.commitIndex = logEntry.index;
         gRaftState.commitIndexTerm = logEntry.term;
    }
    VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);

    if (logEntry.requestCode == LDAP_REQ_ADD)
    {
        dwError = VmDirAddRaftProxy(&entry);
        BAIL_ON_VMDIR_ERROR_WITH_MSG( dwError, (pszLocalErrorMsg), "VmDirAddRaftProxy %s",
                                 VDIR_SAFE_STRING(entry.dn.lberbv.bv_val)); 
        
    } else if (logEntry.requestCode == LDAP_REQ_DELETE)
    {
        dwError = _VmDirDeleteRaftProxy(BERVAL_NORM_VAL(entry.dn));
        BAIL_ON_VMDIR_ERROR_WITH_MSG( dwError, (pszLocalErrorMsg), "VmDirDeleteRaftProxy %s",
                                 VDIR_SAFE_STRING(entry.dn.lberbv.bv_val));
    }

    VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "_VmDirApplyLog: applied log: %s %s %s", logEntryDn, opStr, entry.dn.lberbv.bv_val);

cleanup:
    if (modOp.pBECtx)
    {
        modOp.pBECtx->pBEPrivate = NULL; //Make sure that calls commit/abort only once.
    }
    VmDirFreeOperationContent(&modOp);
    (VOID)VmDirSchemaModMutexRelease(&ldapOp);
    ldapOp.request.addReq.pEntry = NULL;
    VmDirFreeOperationContent(&ldapOp);
    VmDirFreeEntryContent(&entry);
    if (pSchemaCtx)
    {
        VmDirSchemaCtxRelease(pSchemaCtx);
    }
    VMDIR_SAFE_FREE_MEMORY(pszLocalErrorMsg);
    _VmDirChgLogFree(&logEntry);
    return dwError;

error:
    if (bHasTxn)
    {
        ldapOp.pBEIF->pfnBETxnAbort(ldapOp.pBECtx);
    }
    VMDIR_LOG_ERROR(VMDIR_LOG_MASK_ALL, "_VmDirApplyLog: %s error %d", VDIR_SAFE_STRING(pszLocalErrorMsg), dwError);
    goto cleanup;
}

/* Currently assume each MDB transaction have only one LDAP Add
 * To support more than one LDAP Add, a (static) major id might bitOR into pEntryId
 *  - like VmDirRaftLogIndexToCommit
 * The major id would be reset for each MDB transaction.
 * To support this, raftLog needs to pack multiple LDAP Add(s) into one log.
 */
VOID
VmDirRaftNextNewEntryId(ENTRYID *pEntryId)
{
    *pEntryId = NEW_ENTRY_EID_PREFIX | (gRaftState.commitIndex + 1);
}

/*
 * This function is used to create ObjectSid for each new entry to add.
 * Since it is called before a MDB txn_begin, it might be called more than once
 * with the same gRaftState.commitIndex (which is unique within a mdb transaction)
 */
UINT64 VmDirRaftLogIndexToCommit()
{
    static UINT64 prevIdx = 0;
    static UINT64 idxMajor = 0;
    UINT64 commitIndex = 0;
    BOOLEAN bLock = FALSE;
 
    VMDIR_LOCK_MUTEX(bLock, gRaftStateMutex); 
    if (gRaftState.commitIndex == prevIdx)
    {
        idxMajor++;
    } else
    {
        prevIdx = gRaftState.commitIndex;
        idxMajor = 0;
    }
    commitIndex = (gRaftState.commitIndex + 1) | (idxMajor << 32);
    VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);
    
    return commitIndex;
}

BOOLEAN
VmDirRaftDisallowUpdates(PCSTR caller)
{
    if (gRaftState.role == VDIR_RAFT_ROLE_LEADER && gRaftState.disallowUpdates)
    {
        //For information only, so not in the mutex.
        VMDIR_LOG_INFO(VMDIR_LOG_MASK_ALL, "VmDirRaftDisallowUpdates: disallowed %s during leader transition.", caller);
    }
    return gRaftState.disallowUpdates;
}

/*
 * Set ppszLeader to raft leader's server name or NULL if the server
 * itself is a leader, the server is a candidate, or the server
 * have not received Ping yet after switching to follower
 */
DWORD
VmDirRaftGetLeader(PSTR *ppszLeader)
{
    BOOLEAN bLock = FALSE;
    PSTR pszLeader = NULL;
    DWORD dwError = 0;

    VMDIR_LOCK_MUTEX(bLock, gRaftStateMutex);
    if (gRaftState.clusterSize >= 2 &&
        gRaftState.role == VDIR_RAFT_ROLE_FOLLOWER &&
        gRaftState.leader.lberbv_len > 0)
    {
       dwError = VmDirAllocateStringAVsnprintf(&pszLeader, "%s", gRaftState.leader.lberbv_val);
       BAIL_ON_VMDIR_ERROR(dwError);
    }
    VMDIR_UNLOCK_MUTEX(bLock, gRaftStateMutex);

    *ppszLeader = pszLeader;

cleanup:
    return dwError;

error:
    goto cleanup;
}

BOOLEAN
VmDirRaftNeedReferral(PCSTR pszReqDn)
{
    char *p = NULL;
    BOOLEAN bNeedReferral = FALSE;

    if ((p=VmDirStringCaseStrA(pszReqDn, RAFT_CONTEXT_DN)) &&
         VmDirStringCompareA(p, RAFT_CONTEXT_DN, FALSE)==0)
    {
        //Don't offer referral for Raft states or logs.
        goto done;
    }

    if (pszReqDn == NULL || pszReqDn[0] == '\0')
    {
        //Search for DseRoot, don't no need for referral
        goto done;
    }

    bNeedReferral = (gRaftState.role == VDIR_RAFT_ROLE_FOLLOWER);

done:
    return bNeedReferral;
}

