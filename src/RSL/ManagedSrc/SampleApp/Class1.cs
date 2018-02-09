using System;
using ManagedRSLBasicTest;

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
            Console.WriteLine("Test in main AppDomain:");
            DoTest();

            Console.WriteLine("Test in first AppDomain:"); 
            AppDomain appd = AppDomain.CreateDomain("NewAppDomain");
            appd.DoCallBack(DoTest);

            Console.WriteLine("Test in second AppDomain:"); 
            appd = AppDomain.CreateDomain("NewAppDomain2");
            appd.DoCallBack(DoTest);

            Environment.Exit(0);
        }

        static void DoTest()
        {
            Application app = new Application();
            app.StartUp(3);
            Console.WriteLine("  ExecuteFastRead:" + app.ExecuteFastRead(0));
            Console.WriteLine("  ReplicateCommand:" + app.ReplicateCommand(5));
            Console.WriteLine("  ExecuteFastRead:" + app.ExecuteFastRead(1));
            Console.WriteLine("  ReplicateCommand:" + app.ReplicateCommand(3));
            Console.WriteLine("  ExecuteFastRead:" + app.ExecuteFastRead(2));
            Console.WriteLine("  Unloading...");
            app.Unload();
            Console.WriteLine("  Unloaded!");
        }
    }
}
