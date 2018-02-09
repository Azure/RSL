using System;

namespace ManagedRSLBasicTest
{
    public class UserRequest
    {
        System.UInt64 _response;
        int _request;

        public System.UInt64 Response
        {
            get
            {
                return _response;
            }
            set
            {
                _response = value;
            }
        }

        public int Request
        {
            get
            {
                return _request;
            }
        }

        public UserRequest(int request)
        {
            _request = request;
            _response = System.UInt64.MinValue;
        }

        public Byte[] RawRequest
        {
            get
            {
                return System.BitConverter.GetBytes(_request);
            }
        }
    }
}
