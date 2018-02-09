using System;
using System.Net;
using System.Net.Sockets;
using System.Collections;
using ManagedRSLib;

namespace RSLStressTest
{
    class Program
    {
        static StateMachine _stateMachine;
        static public System.IO.TextWriter _writer;
        static public System.IO.TextWriter _writerFlag;
        public static bool imPrimary;

        static void Main(string[] args)
        {
            uint lastRequested = 0;
            Hashtable arguments = ParseArguments(args);
            imPrimary = false;

            int id = (int)arguments["id"];
            int targetState = (int)arguments["cases"];
            int numReplicas = (int)arguments["numreplicas"];

            //Create the output stream
            try
            {
                //_writer = new System.IO.StreamWriter((String)arguments["out"], true);
                _writer = Console.Out;
            }
            catch
            {
                System.Environment.Exit(-1);
            }

            //Start up the common infraestructure
            ManagedRSLConfigParam cfg = new ManagedRSLConfigParam();
            cfg.NewLeaderGracePeriodSec = 10;
            cfg.HeartBeatIntervalSec = 2;
            cfg.ElectionDelaySec = 5;
            cfg.MaxElectionRandomizeSec = 1;
            cfg.InitializeRetryIntervalSec = 1;
            cfg.PrepareRetryIntervalSec = 1;
            cfg.VoteRetryIntervalSec = 1;
            cfg.CPQueryRetryIntervalSec = 5;
            cfg.MaxCheckpointIntervalSec = 0;
            cfg.JoinMessagesIntervalSec = 1;
            cfg.MaxLogLenMB = 10;
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
            cfg.WorkingDir = ".\\rslib" + id;

            ManagedRSLNode[] nodes = new ManagedRSLNode[numReplicas];
            for (int i = 0; i < nodes.Length; i++)
            {
                nodes[i] = new ManagedRSLNode();
                nodes[i].MemberId = (i + 1).ToString();
                nodes[i].RslPort = (ushort)(20000 + (1000 * (i+1)));
                nodes[i].HostName = "127.0.0.1";
                nodes[i].Ip = new IPAddress(0x0100007F); // 127.0.0.1
            }
            ManagedRSLNode selfNode = new ManagedRSLNode();
            selfNode.MemberId = id.ToString();
            selfNode.RslPort = (ushort)(20000 + (1000 * id));
            selfNode.HostName = "127.0.0.1";
            selfNode.Ip = new IPAddress(0x0100007F); // 127.0.0.1

            ManagedRSLStateMachine.Init(".\\debuglogs" + id);

            //Create and initialize the state machine
            _stateMachine = new StateMachine((int)arguments["reqsize"]);

            if (_stateMachine.Initialize(cfg, nodes, selfNode, ManagedRSLProtocolVersion.ManagedRSLProtocolVersion_4, false) == false)
            {
                System.Environment.Exit(-1);
            }

            //Wait 3 seconds for the pending request to be executed
            System.Threading.Thread.Sleep(3000);

            _writer.WriteLine("{1} Initial state: {0}", _stateMachine.State, DateTime.Now);
            lastRequested = _stateMachine.State;

            //Execute the test scenario
            while (_stateMachine.State < targetState)
            {
                if (imPrimary)
                {
                    if (_stateMachine.State == lastRequested)
                    {
                        if (_stateMachine.WriteToState((UInt32)_stateMachine.State + 1) == true)
                        {
                            lastRequested = _stateMachine.State + 1;
                            lock (_writer)
                            {
                                _writer.WriteLine("{1} Request: {0}", lastRequested, DateTime.Now);
                            }
                        }
                    }
                }
                else
                {
                    lastRequested = _stateMachine.State;
                }
                _stateMachine.ReadFromState();
                _writer.Flush();
                System.Threading.Thread.Sleep(1);
            }
            _stateMachine.DumpState();
            _writer.Flush();
            try
            {
                _writerFlag = new System.IO.StreamWriter(String.Format("{0}.done", arguments["out"]), true);
                _writerFlag.WriteLine("DONE");
                _writerFlag.Close();
            }
            catch
            {
                System.Environment.Exit(-1);
            }

            _writer.Close();
            System.Threading.Thread.Sleep(int.MaxValue);

        }

        static Hashtable ParseArguments(String[] args)
        {
            Hashtable arguments = new Hashtable();
            for (int idx = 0; idx < args.Length; idx++)
            {
                switch (args[idx])
                {
                    case "/numreplicas":
                        arguments.Add("numreplicas", Int32.Parse(args[++idx]));
                        break;
                    case "/reqsize":
                        arguments.Add("reqsize", Int32.Parse(args[++idx]));
                        break;
                    case "/cases":
                        arguments.Add("cases", Int32.Parse(args[++idx]));
                        break;
                    case "/id":
                        arguments.Add("id", Int32.Parse(args[++idx]));
                        break;
                    case "/out":
                        arguments.Add("out", args[++idx]);
                        break;
                    case "/wait":
                        arguments.Add("wait", args[++idx]);
                        break;
                    default:
                        Usage();
                        break;
                }
            }
            if ((arguments.Count == 0) || (arguments.Count < 4))
            {
                Usage();
            }
            return arguments;
        }

        static void Usage()
        {
            Console.WriteLine("Use: RSLStress.exe /numreplicas <<numReplicas>> /id <<id>> /reqsize <<requestSize>> /cases <<numberOfCases>>");
            Environment.Exit(-1);
        }
    }
}
