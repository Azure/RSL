using System;
using ManagedRSLBasicTest;
using System.Threading;
using System.Diagnostics;

namespace ManagedRSLApp
{
    /// <summary>
    /// Summary description for Class1.
    /// </summary>
    class Class1
    {
        /// <summary>
        /// The main entry point for the application.
        /// </summary>
        [STAThread]
        static void Main(string[] args)
        {
            DateTime maxLoopTime;
            
            Test testToRun = TestToRun();

            if ((testToRun & Test.FailoverStress) != Test.None)
            {
                maxLoopTime = DateTime.UtcNow + LoopTime;

                do
                {
                    Console.WriteLine("Test FailoverStress in main AppDomain:");
                    AppDomain.CurrentDomain.SetData("cleanup", DoCleanup());
                    DoTestFailover();

                    Console.WriteLine("Test FailoverStress in first AppDomain:");
                    AppDomain appd = AppDomain.CreateDomain("NewAppDomain");
                    appd.SetData("cleanup", false);
                    appd.DoCallBack(DoTestFailover);
                } while (DateTime.UtcNow < maxLoopTime);
            }

            if ( (testToRun & Test.UnloadStress) != Test.None)
            {
                maxLoopTime = DateTime.UtcNow + LoopTime;

                bool cleanup = DoCleanup();

                do
                {
                    Console.WriteLine("Test UnloadStress in AppDomain:");
                    AppDomain appd = AppDomain.CreateDomain("NewAppDomain");
                    appd.SetData("cleanup", cleanup);
                    appd.DoCallBack(DoTestUnload);
                    cleanup = false;

                    Console.WriteLine("Test UnloadStress in AppDomain:");
                    appd = AppDomain.CreateDomain("NewAppDomain");
                    appd.SetData("cleanup", cleanup);
                    appd.DoCallBack(DoTestUnload);
                } while (DateTime.UtcNow < maxLoopTime);
            }

            if ((testToRun & Test.BadSecondary) != Test.None)
            {
                maxLoopTime = DateTime.UtcNow + LoopTime;

                bool cleanup = DoCleanup();

                do
                {
                    Console.WriteLine("Test in AppDomain:");
                    AppDomain appd = AppDomain.CreateDomain("NewAppDomain");
                    appd.SetData("cleanup", cleanup);
                    appd.DoCallBack(DoTestBadSecondary);
                    cleanup = false;

                    Console.WriteLine("Test in AppDomain:");
                    appd = AppDomain.CreateDomain("NewAppDomain");
                    appd.SetData("cleanup", cleanup);
                    appd.DoCallBack(DoTestBadSecondary);
                } while (DateTime.UtcNow < maxLoopTime);
            }

            if ((testToRun & Test.NoActivityBadSecondary) != Test.None)
            {
                maxLoopTime = DateTime.UtcNow + LoopTime;

                bool cleanup = DoCleanup();

                do
                {
                    Console.WriteLine("Test NoActivityBadSecondary in AppDomain:");
                    AppDomain appd = AppDomain.CreateDomain("NewAppDomain");
                    appd.SetData("cleanup", cleanup);
                    appd.DoCallBack(DoTestNoActivityBadSecondary);
                    cleanup = false;

                    Console.WriteLine("Test NoActivityBadSecondary in AppDomain:");
                    appd = AppDomain.CreateDomain("NewAppDomain");
                    appd.SetData("cleanup", cleanup);
                    appd.DoCallBack(DoTestNoActivityBadSecondary);
                } while (DateTime.UtcNow < maxLoopTime);
            }

            Environment.Exit(0);
        }

        private static bool DoCleanup()
        {
            return !Environment.CommandLine.Contains("/nocleanup");
        }

        [Flags]
        enum Test
        {
            None=0,
            FailoverStress=1,
            UnloadStress=2,
            BadSecondary = 4,
            NoActivityBadSecondary = 8,
            All = FailoverStress + UnloadStress + BadSecondary + NoActivityBadSecondary,
        }

        private static Test TestToRun()
        {
            Test test = Test.None;

            if (Environment.CommandLine.Contains("/FailoverStress"))
            {
                test |= Test.FailoverStress;
            }

            if (Environment.CommandLine.Contains("/UnloadStress"))
            {
                test |= Test.UnloadStress;
            }

            if (Environment.CommandLine.Contains("/BadSecondary"))
            {
                test |= Test.BadSecondary;
            }

            if (Environment.CommandLine.Contains("/NoActivityBadSecondary"))
            {
                test |= Test.NoActivityBadSecondary;
            }

            if (Environment.CommandLine.Contains("/infiniteloop"))
            {
                LoopTime = TimeSpan.MaxValue;
            }

            if (Environment.CommandLine.Contains("/All"))
            {
                LoopTime = default(TimeSpan);
                test = Test.All;
            }

            if (test == Test.None)
            {
                Console.WriteLine("{0} [/infiniteloop] <testsToRun>", Environment.GetCommandLineArgs()[0]);
                Console.WriteLine("Where <testsToRun> is optional (and this help will show) or it is a sequence of:");
                Console.WriteLine("/FailoverStress");
                Console.WriteLine("/UnloadStress");
                Console.WriteLine("/BadSecondary");
                Console.WriteLine("/NoActivityBadSecondary");
                Console.WriteLine("/All");
                Environment.Exit(0);
            }

            if (LoopTime == default(TimeSpan))
            {
                LoopTime = TimeSpan.FromMinutes(2);
            }

            return test;
        }

        private static TimeSpan LoopTime = default(TimeSpan);

        static void DoTestFailover()
        {
            Console.WriteLine("FailoverTest");

            bool cleanup = (bool)AppDomain.CurrentDomain.GetData("cleanup");
            Application app = new Application(cleanup);
            Console.WriteLine("  Starting");
            app.StartUp(25);
            app.WaitForPrimary();

            DateTime maxTime = DateTime.UtcNow + TimeSpan.FromSeconds(60);

            int n = 1;
            int nFailures=0;
            BasicRSLTestMachine evicted = null;

            Thread thr = new Thread(() =>
                {
                    while (DateTime.UtcNow<maxTime)
                    {
                        BasicRSLTestMachine prim = app.PrimaryReplica;

                        if (n % 10 == 0)
                        {
                            if (evicted == null)
                            {
                                app.EvictMember(prim.Self.MemberId);
                                evicted = prim;
                                ThreadPool.QueueUserWorkItem((object o) =>
                                {
                                    BasicRSLTestMachine oldPrim = (BasicRSLTestMachine)o;
                                    while (app.PrimaryReplica == oldPrim)
                                    {
                                        Thread.Sleep(100);
                                    }
                                    nFailures = 0;
                                    app.EvictMember(null);
                                    evicted = null;
                                }, prim);

                                continue;
                            }
                        }

                        if (prim != evicted || nFailures < 100)
                        {
                            n++;

                            if (!prim.ReplicateRequest(new UserRequest(n)))
                            {
                                nFailures++;
                                Console.WriteLine("{0} couldnt replicate {1}", prim.Self.MemberId, n);
                                app.WaitForPrimary();
                            }
                        }
                    }
                });
            thr.Start();
            thr.Join();
            while (evicted != null)
            {
                Thread.Sleep(100);
            }
            Console.WriteLine("==================================== DONE ==================================");

            Console.WriteLine("  Unloading...");
            app.Unload();
            Console.WriteLine("  Unloaded!");
        }

        static void DoTestBadSecondary()
        {
            Console.WriteLine("BadSecondary");

            bool cleanup = (bool)AppDomain.CurrentDomain.GetData("cleanup");
            Application app = new Application(cleanup);
            Console.WriteLine("  Starting");
            app.StartUp(3);
            app.WaitForPrimary();

            int maxState = app.PrimaryReplica.InternalState;
            if (cleanup)
            {
                maxState = 0;
            }

            for (int i = 0; i < 100; i++)
            {
                maxState++;
                Console.WriteLine("  PassDecrees:" + app.ReplicateCommand(maxState));
            }
            //Console.WriteLine("  Waiting...");
            //app.WaitForAllReplicasInternalState(maxState);
            app.UnloadReplica(app.SecondaryReplicas[0]);
            for (int i = 0; i < 100; i++)
            {
                maxState++;
                Console.WriteLine("  PassDecrees:" + app.ReplicateCommand(maxState));
            }
            Console.WriteLine("  Waiting..."); 
            app.WaitForAllReplicasInternalState(maxState);

            Console.WriteLine("  Unloading...");
            app.Unload();
            Console.WriteLine("  Unloaded!");
        }

        static void DoTestNoActivityBadSecondary()
        {
            Console.WriteLine("BadNoActivitySecondary");

            bool cleanup = (bool)AppDomain.CurrentDomain.GetData("cleanup");
            Application app = new Application(cleanup);
            Console.WriteLine("  Starting");
            app.StartUp(5);
            app.WaitForPrimary();

            int maxState = app.PrimaryReplica.InternalState;
            if (cleanup)
            {
                maxState = 0;
            }

            bool cycle = false;

            cycle = true;

            Thread thr = new Thread( ()=>
            {
                BasicRSLTestMachine oldReplica = app.PrimaryReplica;

                while (cycle)
                {
                    if (oldReplica.IsPrimary)
                    {
                        oldReplica = app.SecondaryReplicas[0];
                    }

                    app.UnloadReplica(oldReplica);
                    Thread.Sleep(5000);
                    app.ReloadReplica(oldReplica);
                }
            });

            thr.Start();

            app.UnloadReplica(app.SecondaryReplicas[0]);

            for (int i = 0; i < 100; i++)
            {
                app.PrimaryReplica.OnPrimaryLoss = new Action<BasicRSLTestMachine>((p) =>
                    {
                        Debug.Assert(false, "We should not lose primariness here (" + p.Self + ")");
                    });

                for (int j = 0; j < 5; j++)
                {
                    maxState++;
                    app.ReplicateCommand(maxState);

                    Thread.Sleep(5000);
                }

                for (int j = 0; j < 5; j++)
                {
                    maxState++;
                    app.ReplicateCommand(maxState);

                    Thread.Sleep(5000);
                }
                app.PrimaryReplica.OnPrimaryLoss = null;

                Console.WriteLine("failing over...");
                app.FailoverAndWaitForPrimary();
            }

            cycle = false;
            thr.Join();

            Console.WriteLine("  Waiting...");
            app.WaitForAllReplicasInternalState(maxState);

            Console.WriteLine("  Unloading...");
            app.Unload();
            Console.WriteLine("  Unloaded!");
        }

        static void DoTestUnload()
        {
            Console.WriteLine("UnloadTest");

            bool cleanup = (bool)AppDomain.CurrentDomain.GetData("cleanup");
            Application app = new Application(cleanup);
            Console.WriteLine("  Starting");
            app.StartUp(25);
            app.WaitForPrimary();

            for (int i = 0; i < 10; i++)
            {
                Console.WriteLine("  PassDecrees:" + app.ReplicateCommand(i));
            }

            Console.WriteLine("  Unloading...");
            app.Unload();
            Console.WriteLine("  Unloaded!");
        }
    }
}
