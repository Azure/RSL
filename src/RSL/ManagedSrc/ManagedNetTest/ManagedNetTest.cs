using System;
using System.Collections.Generic;
using System.Text;
using System.IO;
using System.Net;
using System.Diagnostics;
using System.Threading;
using ManagedRSLib;

namespace ManagedNetTest
{
    class TestRSLStateMachineProcessor : ManagedRSLStateMachine
    {
        const int SLEEP_TIME = 500;
        static uint g_port = 20000;
        static byte[] g_buf;
        static uint g_maxConfigurationReported = 0;
        static string[] RSLResponseCodeStr = { "RSLSuccess", "RSLFastReadStale", "RSLNotPrimary", "RSLShuttingDown" };

        static bool ReadConfigurationFromFile(ref int configurationNumber, ref int numMembers, ref int[] memberIds)
        {
            try
            {
                string[] lines = File.ReadAllLines("members.txt");
                configurationNumber = Int32.Parse(lines[0]);
                numMembers = Int32.Parse(lines[1]);
                memberIds = new int[numMembers];
                for (int i = 0; i < memberIds.Length; i++)
                {
                    memberIds[i] = Int32.Parse(lines[2 + i]);
                }
            }
            catch (Exception exc)
            {
                Console.WriteLine("Error: " + exc);
                return false;
            }
            return true;
        }

        static bool ReportCurrentConfiguration(uint currentConfigurationNumber)
        {
            try
            {
                File.WriteAllText("CurrentConfig.txt", currentConfigurationNumber.ToString());
            }
            catch (Exception exc)
            {
                Console.WriteLine("Error: " + exc);
                return false;
            }
            return true;
        }

        static void PopulateNode(ManagedRSLNode node, ulong memberId)
        {
            node.HostName = "127.0.0.1";
            //node.Ip = (uint) IPAddress.Parse(node.HostName).Address;
            node.Ip = new IPAddress(0x0100007F);
            node.MemberId = memberId.ToString();
            node.RslPort = (ushort)(g_port + memberId * 1000);
        }

        void Run(ManagedRSLNode[] nodes, ManagedRSLConfigParam cfgParam, ManagedRSLNode selfNode)
        {
            m_score = 0;
            m_outstanding = 0;
            m_size = 10 * 1024 * 1024;
            m_state = new byte[m_size];
            m_clientEnd = false;
            m_isPrimary = false;

            bool res = Initialize(
                cfgParam, 
                nodes, 
                selfNode,
                ManagedRSLProtocolVersion.ManagedRSLProtocolVersion_3, 
                false);
            Debug.Assert(res);

            for (int timer = 0; !m_clientEnd; timer++)
            {
                if (timer >= 1000 / SLEEP_TIME)
                {
                    timer = 0;
                    if (m_isPrimary)
                    {
                        ChangeConfigurationIfNeeded();
                    }
                }
                if (m_isPrimary && m_outstanding < 1)
                {
                    byte[] inc = BitConverter.GetBytes((int)1);
                    SendRequest(inc, this, UInt64.MaxValue);
                }
                Thread.Sleep(SLEEP_TIME);
            }
        }

        void ChangeConfigurationIfNeeded()
        {
            ManagedRSLMemberSet currentMemberSet = new ManagedRSLMemberSet();
            uint configurationNumber = 0;
            GetConfiguration(currentMemberSet, ref configurationNumber);
            uint currentConfigurationNumber = 0;
            byte[] cookie = currentMemberSet.ConfigurationCookie;

            if (cookie != null)
            {
                currentConfigurationNumber = (uint)BitConverter.ToInt32(cookie, 0);
            }

            int desiredConfigurationNumber = 0;
            int numMembersInDesiredConfiguration = 0;
            int[] memberIdsOfDesiredConfiguration = null;
            if (!ReadConfigurationFromFile(ref desiredConfigurationNumber,
                                           ref numMembersInDesiredConfiguration,
                                           ref memberIdsOfDesiredConfiguration))
            {
                return;
            }

            if (currentConfigurationNumber >= desiredConfigurationNumber)
            {
                if (currentConfigurationNumber > g_maxConfigurationReported)
                {
                    if (ReportCurrentConfiguration(currentConfigurationNumber))
                    {
                        g_maxConfigurationReported = currentConfigurationNumber;
                    }
                }
                return;
            }

            Console.WriteLine("New configuration found:");
            ManagedRSLNode[] membersOfNewConfiguration =
                new ManagedRSLNode[numMembersInDesiredConfiguration];
            for (int whichMember = 0; whichMember < numMembersInDesiredConfiguration; ++whichMember)
            {
                ManagedRSLNode node = new ManagedRSLNode();
                membersOfNewConfiguration[whichMember] = node;
                PopulateNode(node, (ulong)memberIdsOfDesiredConfiguration[whichMember]);
                Console.Write("{0} ", node.MemberId);
            }
            Console.WriteLine();
            byte[] configNumberBytes = BitConverter.GetBytes(desiredConfigurationNumber);
            ManagedRSLMemberSet memberSet = new ManagedRSLMemberSet(
                membersOfNewConfiguration,
                configNumberBytes,
                0,
                configNumberBytes.Length);

            ChangeConfiguration(memberSet, this);
        }

        void SendRequest(byte[] data, object cookie, ulong maxSeenSeqNo)
        {
            RSLResponse ec;
            if (maxSeenSeqNo == ulong.MaxValue)
            {
                ec = ReplicateRequest(data, cookie);
            }
            else
            {
                ec = FastReadRequest(maxSeenSeqNo, data, cookie);
            }
            if (ec != RSLResponse.RSLSuccess)
            {
                Console.WriteLine("ReplicateRequest() failed: " + ec);
            }
            lock (this)
            {
                m_outstanding++;
            }
        }

        //
        // RSL State machine implementation
        //

        public override bool LoadState(ManagedRSLCheckpointStream reader)
        {
            if (reader == null)
            {
                m_score = 0;
                return false;
            }
            int totalRead = 0;
            int read = 0;
            while (m_size > totalRead)
            {
                try
                {
                    read = reader.Read(m_state, totalRead, (int)m_size - totalRead);
                    Debug.Assert(read >= 0);
                }
                catch (IOException ioe)
                {
                    Console.WriteLine("Error: " + ioe);
                    return false;
                }
                totalRead += read;
            }
            m_score = BitConverter.ToInt32(m_state, sizeof(Int32));
            return true;
        }

        public override void ExecuteReplicatedRequest(byte[] request, object cookie, ref bool saveState)
        {
            ExecuteRequest(false, request, cookie);
        }

        public override void ExecuteFastReadRequest(byte[] request, object cookie)
        {
            ExecuteRequest(true, request, cookie);
        }

        void ExecuteRequest(bool isFastRead, byte[] request, object cookie)
        {
            if (isFastRead == false)
            {
                int inc = BitConverter.ToInt32(request, 0);
                m_score += inc;
                Console.WriteLine("score: " + m_score);
            }
            if (this == cookie)
            {
                lock (this)
                {
                    Debug.Assert(m_outstanding > 0);
                    m_outstanding--;
                }
            }
        }

        public override void AbortRequest(RSLResponse status, object cookie)
        {
            Console.WriteLine("Request aborted (outstanding={0})", m_outstanding);
            if (this == cookie)
            {
                lock (this)
                {
                    Debug.Assert(m_outstanding > 0);
                    m_outstanding--;
                }
            }
        }

        public override void SaveState(ManagedRSLCheckpointStream writer)
        {
            byte[] score = BitConverter.GetBytes(m_score);
            for (int i = 0; i < score.Length; i++)
            {
                m_state[i] = score[i];
            }
            try
            {
                writer.Write(m_state, 0, m_size);
            }
            catch (IOException ioe)
            {
                Console.WriteLine("Error: " + ioe);
            }
        }

        public override void NotifyPrimaryRecovered()
        {
            Console.WriteLine("primary recovered");
        }

        public override void NotifyStatus(bool isPrimary)
        {
            Console.WriteLine("current primary status is: " + isPrimary);
            m_isPrimary = isPrimary;
        }

        public override void NotifyConfigurationChanged(object cookie)
        {
            Console.WriteLine("Configuration changed!");
        }

        public override void AbortChangeConfiguration(RSLResponse status, object cookie)
        {
            Console.WriteLine("Change configuration aborted!");
            Debug.Assert(false);
        }

        public override void ShutDown(RSLResponse status)
        {
            Console.WriteLine("Shutting down");
            m_clientEnd = true;
        }
        
        public override bool CanBecomePrimary(ref byte[] primaryCookie)
        {
            return true;
        }

        public override bool AcceptMessageFromReplica(ManagedRSLNode node, byte [] primaryCookie)
        {
            return true;
        }

        public override void StateSaved(UInt64 seqNo, String fileName)
        {
            return;
        }

        public override void StateCopied(UInt64 seqNo, String fileName, object cookie)
        {
            return;
        }

        int m_score;
        int m_outstanding;
        int m_size;
        byte[] m_state;
        bool m_clientEnd;
        bool m_isPrimary;

        static void PrintUsage(string errMsg)
        {
            if (errMsg != null)
            {
                Console.WriteLine("Error: " + errMsg);
            }
            Console.WriteLine("Usage: ");
            Console.WriteLine("RSLNetTest <memberId>");
            Console.WriteLine();
        }

        static void Exit(int code)
        {
#if DEBUG
            Debugger.Break();
#endif
            Environment.Exit(code);
        }

        static void Main(string[] args)
        {
            if (args.Length != 1)
            {
                PrintUsage(null);
                Exit(1);
            }
            int id = Int32.Parse(args[0]);
            ManagedRSLStateMachine.Init(@".\debuglogs\" + id);

            ManagedRSLConfigParam cfgParam = new ManagedRSLConfigParam();
            // Initialize the default config param
            cfgParam.NewLeaderGracePeriodSec = 15;
            cfgParam.HeartBeatIntervalSec = 2;
            cfgParam.ElectionDelaySec = 10;
            cfgParam.MaxElectionRandomizeSec = 1;
            cfgParam.InitializeRetryIntervalSec = 1;
            cfgParam.PrepareRetryIntervalSec = 1;
            cfgParam.VoteRetryIntervalSec = 3;
            cfgParam.CPQueryRetryIntervalSec = 5;
            cfgParam.MaxCheckpointIntervalSec = 0;
            cfgParam.JoinMessagesIntervalSec = 1;
            cfgParam.MaxLogLenMB = 1;
            cfgParam.SendTimeoutSec = 5;
            cfgParam.ReceiveTimeoutSec = 5;
            cfgParam.MaxCacheLengthMB = 50;
            cfgParam.MaxVotesInLog = 10000;
            cfgParam.MaxOutstandingPerReplica = 10;
            cfgParam.MaxCheckpoints = 4;
            cfgParam.MaxLogs = 10;
            cfgParam.LogLenRandomize = 20;
            cfgParam.ElectionRandomize = 10;
            cfgParam.FastReadSeqThreshold = 5;
            cfgParam.InMemoryExecutionQueueSizeMB = 10;
            cfgParam.NumReaderThreads = 3;
            cfgParam.WorkingDir = @".\data";

            int configNumber = 0;
            int numReplicas = 0;
            int[] memberIds = null;
            bool res = ReadConfigurationFromFile(ref configNumber, ref numReplicas, ref memberIds);
            Debug.Assert(res);

            // Populate member set
            if (id <= 0)
            {
                PrintUsage("invalid member id!");
                Exit(1);
            }

            int bufSize = 100 * 1024;
            g_buf = new byte[bufSize];
            for (int i = 0; i < bufSize; i++)
            {
                g_buf[i] = (byte)('a' + (i % 26));
            }

            ManagedRSLNode[] nodes = new ManagedRSLNode[numReplicas];
            for (int i = 0; i < numReplicas; i++)
            {
                nodes[i] = new ManagedRSLNode();
                PopulateNode(nodes[i], (uint)memberIds[i]);
            }
            ManagedRSLNode selfNode = new ManagedRSLNode();
            PopulateNode(selfNode, (uint)id);

            // Start replica
            Console.WriteLine("Starting member #" + selfNode.MemberId);
            TestRSLStateMachineProcessor sm = new TestRSLStateMachineProcessor();
            sm.Run(nodes, cfgParam, selfNode);
        }
    }
}
