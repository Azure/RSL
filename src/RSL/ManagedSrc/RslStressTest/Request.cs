using System;
using System.Collections;
using System.Text;

namespace RSLStressTest
{
    class Request
    {
        UInt32 _request;
        String _requestType;
        DateTime _requested;
        DateTime _processed;

        public Request(String type,UInt32 request)
        {
            _request = request;
            _requestType = type;
            _requested = DateTime.Now;
        }

        public void Process(UInt32 req)
        {
            _processed = DateTime.Now;
            lock (Program._writer)
            {
                Program._writer.WriteLine("{3} {0}: {2} Time: {1}", _requestType, _processed.Subtract(_requested).TotalMilliseconds, req,DateTime.Now);
                Program._writer.Flush();
            }
        }

        public void Cancel()
        {            
            lock (Program._writer)
            {
                Program._writer.WriteLine("Cancel {0}", _request);
                Program._writer.Flush();
            }
        }

        #region Properties
        public DateTime Requested
        {
            get
            {
                return _requested;
            }
        }

        public DateTime Processed
        {
            get
            {
                return _processed;
            }
        }

        public String Type
        {
            get
            {
                return _requestType;
            }
        }
        #endregion
    }
}
