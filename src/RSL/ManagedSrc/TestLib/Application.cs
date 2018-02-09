using System;
using System.Net;
using System.Net.Sockets;
using System.Diagnostics;
using ManagedRSLib;
using System.Runtime.InteropServices;
using System.Collections.Generic;
using System.Threading;

namespace ManagedRSLBasicTest
{
    /// <summary>
    /// Test application for the managed RSL lib
    /// </summary>
    public class Application
    {
        public List<BasicRSLTestMachine> _allReplicaSet = null;
        public List<BasicRSLTestMachine>  _replicaSet = null;
        public List<BasicRSLTestMachine>  _pausedReplicas = null;
        List<UserRequest> _pendingRequests;

        public BasicRSLTestMachine PrimaryReplica
        {
            get
            {
                foreach (BasicRSLTestMachine replica in _allReplicaSet)
                {
                    if (replica.IsPrimary)
                    {
                        return replica;
                    }
                }
                return null;
            }
        }
        public List<BasicRSLTestMachine> SecondaryReplicas
        {
            get
            {
                List<BasicRSLTestMachine> replicas = new List<BasicRSLTestMachine>(_allReplicaSet);
                replicas.Remove(this.PrimaryReplica);
                return replicas;
            }
        }

        public Application()
            : this(true)
        {
        }

        public Application(bool cleanup)
        {
            if (cleanup)
            {
                CleanUp();
            }

            _pendingRequests = new List<UserRequest>();
            _pausedReplicas = new List<BasicRSLTestMachine>();
            _allReplicaSet = new List<BasicRSLTestMachine>();
        }

        public void Unload()
        {
            BasicRSLTestMachine.Unload();
        }

        public void CleanUp()
        {
            Console.WriteLine("Cleaning up");

            foreach (String dir in new string[] { RSLWorkingDir, RSLDebugLogsDir })
            {
                if (System.IO.Directory.Exists(dir))
                {
                    System.IO.Directory.Delete(dir, true);
                }
            }
        }

        public static void OnNotification(NotificationLevel level, NotificationLogID logId, string title, string message)
        {
            Console.WriteLine("{0} {1} {2} {3}. ==================================================================", level, logId, title, message);
        }

        private static string RSLWorkingDir = "RSLModel";
        private static string RSLDebugLogsDir = "debuglogs";

        private static ManagedRSLConfigParam cfg;

        /// <summary>
        /// Starts the configured number of replicas, this function will create
        /// the necessery files that are needed for the aplicaton to run
        /// </summary>
        /// <param name="numberOfReplicas">numberOfReplicas</param>
        /// <returns></returns>
        public int StartUp(int numberOfReplicas)
        {
            cfg = new ManagedRSLConfigParam();
            cfg.NewLeaderGracePeriodSec = 10;
            cfg.HeartBeatIntervalSec = 2;
            cfg.ElectionDelaySec = 5;
            cfg.MaxElectionRandomizeSec = 1;
            cfg.InitializeRetryIntervalSec = 1;
            cfg.PrepareRetryIntervalSec = 1;
            cfg.VoteRetryIntervalSec = 1;
            cfg.CPQueryRetryIntervalSec = 5;
            cfg.MaxCheckpointIntervalSec = 0;
            cfg.MaxLogLenMB = 100;
            cfg.SendTimeoutSec = 10;
            cfg.ReceiveTimeoutSec = 10;
            cfg.MaxCacheLengthMB = 50;
            cfg.MaxVotesInLog = 1000;
            cfg.MaxOutstandingPerReplica = 10;
            cfg.MaxCheckpoints = 2;
            cfg.MaxLogs = 5;
            cfg.LogLenRandomize = 20;
            cfg.ElectionRandomize = 10;
            cfg.FastReadSeqThreshold = 5;
            cfg.InMemoryExecutionQueueSizeMB = 10;
            cfg.NumReaderThreads = 5;
            cfg.MaxMessageSizeMB = 1;
            cfg.WorkingDir = RSLWorkingDir;

            ManagedRSLNode[] nodes = new ManagedRSLNode[numberOfReplicas];
            for (int i = 0; i < nodes.Length; i++)
            {
                nodes[i] = new ManagedRSLNode();
                nodes[i].MemberId = (i + 1).ToString();
                nodes[i].RslPort = (ushort)(5000 + (100 * (i+1)));
                nodes[i].HostName = "127.0.0.1";
                nodes[i].Ip = new IPAddress(0x0100007F); // 127.0.0.1
            }

            int startedReplicas = 0;

            if(_replicaSet==null)
            {
                _replicaSet = new List<BasicRSLTestMachine>();

                ManagedRSLStateMachine.NotificationsCallback = OnNotification;

                ManagedRSLStateMachine.Init(".\\" + RSLDebugLogsDir);
                for (int i = 0; i < nodes.Length; i++)
                {
                    BasicRSLTestMachine replica = new BasicRSLTestMachine(nodes[i]);
                    try
                    {
                        bool res = replica.Initialize(
                            cfg, 
                            nodes[i], 
                            ManagedRSLProtocolVersion.ManagedRSLProtocolVersion_4, 
                            false);
                        Debug.Assert(res);
                        startedReplicas++;
                        _replicaSet.Add(replica);
                        _allReplicaSet.Add(replica);
                    }
                    catch(Exception e)
                    {
                        if (e is NullReferenceException || e is System.Runtime.InteropServices.SEHException)
                        {
                            throw;
                        }
                    } //catch
                } // for

                Console.WriteLine("Bootstrapping");
                byte[] buf = new byte[] { 1 };
                ManagedRSLMemberSet memberSet = new ManagedRSLMemberSet(nodes, buf, 0, 1);

                foreach (BasicRSLTestMachine replica in _allReplicaSet)
                {
                    RSLResponse resp = replica.Bootstrap(memberSet, 10);
                    Console.WriteLine(resp);
                }
            }
            else
            {
                startedReplicas = _replicaSet.Count;
            }

            return startedReplicas;
        } //StartUp

        public void Pause()
        {
            Pause(_replicaSet[_replicaSet.Count - 1].Self.MemberId);
        }

        /// <summary>
        /// Pauses a replica
        /// </summary>
        public void Pause(string replicaId)
        {
            BasicRSLTestMachine replica = null;

            foreach (BasicRSLTestMachine replicaI in _replicaSet)
            {
                if (String.Equals(replicaI.Self.MemberId, replicaId))
                {
                    replica = replicaI;
                    break;
                }
            }
            if (replica == null)
            {
                throw new KeyNotFoundException("replica " + replicaId);
            }
            Console.WriteLine("found replica " + replicaId);
            _replicaSet.Remove(replica);
            replica.Pause();
            _pausedReplicas.Add(replica);
            Console.WriteLine("PAUSED replica " + replicaId);
            System.Threading.Thread.Sleep(10000);
            Console.WriteLine("continue after PAUSED replica " + replicaId);
        }

        public void Resume()
        {
            Pause(_pausedReplicas[_pausedReplicas.Count - 1].Self.MemberId);
        }

        /// <summary>
        /// Resumes a replica
        /// </summary>
        public void Resume(string replicaId)
        {
            BasicRSLTestMachine replica = null;

            foreach (BasicRSLTestMachine replicaI in _pausedReplicas)
            {
                if (String.Equals(replicaI.Self.MemberId, replicaId))
                {
                    replica = replicaI;
                    break;
                }
            }
            if (replica == null)
            {
                throw new KeyNotFoundException("replica " + replicaId);
            }

            _pausedReplicas.Remove(replica);
            replica.Resume();
            _replicaSet.Add(replica);
        }

        /// <summary>
        /// Sends a replicate request command to the replicas
        /// </summary>
        /// <param name="newState"></param>
        /// <returns></returns>
        public bool ReplicateCommand(int newState)
        {
            DateTime timeOut = DateTime.Now.AddSeconds(30);
            UserRequest request = new UserRequest(newState);
            int primaryServers = 0;
            bool res = false;

            if (_replicaSet.Count > _pausedReplicas.Count)
            {
                do
                {
                    for (int i = 0; i < _replicaSet.Count; i++)
                    {
                        if (_replicaSet[i].ReplicateRequest(request))
                        {
                            primaryServers++;

                            if (!_replicaSet[i].IsPrimary)
                            {
                                throw new ApplicationException("The request was successful for somebopdy not Primary (" + _replicaSet[i].Self.MemberId + ")");
                            }

                            _pendingRequests.Add(request);
                            if (primaryServers > 1)
                            {
                                throw new ApplicationException("There are more than one primary servers");
                            }
                        }
                    }
                    System.Threading.Thread.Sleep(1);
                } while ((primaryServers == 0) && (DateTime.Now < timeOut));
                if (primaryServers == 1)
                {
                    res = true;
                }
            }
            
            return res;
        }


        /// <summary>
        /// Executes a fast read on every replica ans waits for all the requests to be
        /// executed, maxSeqNumber is the minimum secuence number that is required for 
        /// this replica to be up to date to in order to satisfy the fast read request
        /// </summary>
        /// <param name="maxSeqNumber"></param>
        /// <returns></returns>
        public bool ExecuteFastRead(int secuence)
        {
            UserRequest[] requests = new UserRequest[_replicaSet.Count];
            DateTime timeOut = DateTime.Now.AddSeconds(10);
            System.Byte[] rawRequest = null;
            int replicatedTo = 0;
            UInt64 maxSeqNum;

            for(int i =0; i < _replicaSet.Count; i++)
            {
                UInt64 currentSN = _replicaSet[i].GetCurrentSequenceNumber();
                
                rawRequest = System.BitConverter.GetBytes(int.MaxValue);
                requests[i] = new UserRequest(int.MaxValue);

                if (currentSN == 0)
                {
                    return true;
                }

                if(secuence<=0)
                {
                    maxSeqNum = 0;
                }
                else if(secuence==1)
                {
                    maxSeqNum = currentSN;
                }
                else
                {
                    maxSeqNum = UInt64.MaxValue;
                }
                
                if(_replicaSet[i].FastReadRequest(maxSeqNum, rawRequest, requests[i])==RSLResponse.RSLFastReadStale)
                {
                    if( currentSN < maxSeqNum)
                    {
                        return true;
                    }
                    else
                    {
                        return false;
                    }
                }
            } //do
            
            do
            {
                replicatedTo = 0;
                foreach(UserRequest request in requests)
                {
                    if(request.Response!=System.UInt64.MinValue)
                    {
                        replicatedTo++;
                    }
                }
                System.Threading.Thread.Sleep(1);
            } while((replicatedTo!=_replicaSet.Count)&&(DateTime.Now < timeOut));

            if(replicatedTo==_replicaSet.Count)
            {
                return true;
            }
            
            return false;
        } //ExecuteFastRead

        /// <summary>
        /// Get the internal replica state
        /// </summary>
        /// <returns></returns>
        public int GetState()
        {
            int res = 0;

            SyncStateMachine();

            if (_allReplicaSet != null)
            {
                for (int i = 0; i < _allReplicaSet.Count; i++)
                {
                    res += _allReplicaSet[i].InternalState * ((int)System.Math.Pow(10, i));
                }
            }
            else
            {
                res = -1;
            }
            return res;
        }

        /// <summary>
        /// Get the number of stable replicas in the system
        /// </summary>
        /// <returns></returns>
        public int GetStableReplicas()
        {
            return SyncStateMachine();
        }

        /// <summary>
        /// Syncs the state machine
        /// </summary>
        /// <returns></returns>
        private int SyncStateMachine()
        {
            int stableReplicas = 0;
            int replicatedReq = 0;
            UInt64 seqNum = 0;

            if (_replicaSet.Count > _pausedReplicas.Count)
            {
                //Wait for all the pending requests
                while (replicatedReq != _pendingRequests.Count)
                {
                    foreach (UserRequest request in _pendingRequests)
                    {
                        if (request.Response != System.UInt64.MinValue)
                        {
                            if (seqNum < request.Response)
                            {
                                seqNum = request.Response;
                            }
                            replicatedReq++;
                        }
                    }
                    System.Threading.Thread.Sleep(1);
                }
                _pendingRequests.Clear();

                //Get the max secuence number
                if (seqNum == 0)
                {
                    foreach (BasicRSLTestMachine replica in _replicaSet)
                    {
                        if (replica.GetCurrentSequenceNumber() > seqNum)
                        {
                            seqNum = replica.GetCurrentSequenceNumber() - 1;
                        }
                    }
                }

                //Wait for the replicas to get stable
                while (stableReplicas != _replicaSet.Count)
                {
                    stableReplicas = 0;
                    foreach (BasicRSLTestMachine replica in _replicaSet)
                    {
                        if (replica.GetCurrentSequenceNumber() > seqNum)
                        {
                            stableReplicas++;
                        }
                    }
                    System.Threading.Thread.Sleep(1);
                }
            }
            else
            {
                stableReplicas = _replicaSet.Count;
            }

            return stableReplicas;
        }

        public void WaitForPrimary()
        {
            while (this.PrimaryReplica == null)
            {
                Thread.Sleep(500);
            }
        }

        public void FailoverAndWaitForPrimary()
        {
            BasicRSLTestMachine prim = this.PrimaryReplica;
            EvictMember(prim.Self.MemberId);

            while (this.PrimaryReplica == prim)
            {
                Thread.Sleep(100);
            }
            WaitForPrimary();
            EvictMember(null);
        }

        public void EvictMember(string evictedId)
        {
            foreach (BasicRSLTestMachine member in _allReplicaSet)
            {
                if (!String.Equals(member.Self.MemberId, evictedId))
                {
                    member.MemberIdToEvict = evictedId;
                }
            }
            Console.WriteLine("Evicted {0}", evictedId);
        }

        public void UnloadReplica(BasicRSLTestMachine replica)
        {
            this._allReplicaSet.Remove(replica);
            this._pausedReplicas.Remove(replica);
            this._replicaSet.Remove(replica);
            Console.WriteLine("{0} unloading  ------------------", replica.Self.MemberId);
            replica.UnloadThisOne();
            Console.WriteLine("{0} unloaded -----------------------", replica.Self.MemberId);
        }

        public void ReloadReplica(BasicRSLTestMachine replica)
        {
            Console.WriteLine("{0} reloading  ------------------", replica.Self.MemberId);

            bool res = replica.Initialize(
                cfg,
                replica.Self,
                ManagedRSLProtocolVersion.ManagedRSLProtocolVersion_4,
                false);

            Debug.Assert(res);

            Console.WriteLine("{0} reloaded -----------------------", replica.Self.MemberId);
            this._allReplicaSet.Add(replica);
            this._replicaSet.Add(replica);
        }

        public void WaitForAllReplicasInternalState(int state)
        {
            while (true)
            {
                bool allgood = true;
                foreach (BasicRSLTestMachine repl in _allReplicaSet)
                {
                    if (repl.InternalState != state)
                    {
                        allgood = false;
                        break;
                    }
                }
                if (allgood)
                {
                    return;
                }
                Thread.Sleep(100);
            }
        }

    }
}
