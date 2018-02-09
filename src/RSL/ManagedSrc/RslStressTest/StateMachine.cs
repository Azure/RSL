using System;
using System.Text;
using ManagedRSLib;
using System.Collections;

namespace RSLStressTest
{
    /// <summary>
    /// This is the Replicated state machine for the RSL stress test
    /// </summary>
    public class StateMachine : ManagedRSLStateMachine
    {
        Byte _magicPad;
        Byte[] _reqBuffer;
        ArrayList _state;

        /// <summary>
        /// Constructor, allocate enough space for the request
        /// </summary>
        /// <param name="reqLen"></param>
        public StateMachine(Int32 reqLen)
        {
            if (reqLen >= 4)
            {
                _reqBuffer = new Byte[reqLen];
            }
            _state = new ArrayList();
            _magicPad = BitConverter.GetBytes(0xA5)[0]; //10100101

            //Pad the buffer
            for (int idx = 0; idx < _reqBuffer.Length; idx++)
            {
                _reqBuffer[idx] = _magicPad;
            }
        }

        #region Public Interface
        /// <summary>
        /// Submit a write request
        /// </summary>
        /// <param name="req"></param>
        /// <returns></returns>
        public bool WriteToState(UInt32 req)
        {
            Byte[] request = BitConverter.GetBytes(req);
            Request requestObject;
            Boolean res = false;

            for(int idx=0;idx<request.Length;idx++)
            {
                _reqBuffer[idx] = request[idx];
            }

            requestObject = new Request("Write",req);

            if (ReplicateRequest(_reqBuffer, requestObject) == RSLResponse.RSLSuccess)
            {
                res = true;
            }
            
            return res;
        }

        /// <summary>
        /// Execute a fast read request
        /// </summary>
        /// <returns></returns>
        public bool ReadFromState()
        {
            Request requestObject;

            for (int idx = 0; idx < sizeof(UInt32); idx++)
            {
                _reqBuffer[idx] = _magicPad;
            }

            requestObject = new Request("Read",0);

            FastReadRequest(0, _reqBuffer, requestObject);
            return true;
        }

        /// <summary>
        /// Dumps the state to the log file
        /// </summary>
        public void DumpState()
        {
            foreach (UInt32 i in _state)
            {
                Program._writer.WriteLine("State: {0}", i);
                Program._writer.Flush();
            }
        }
        #endregion

        #region ManagedRSLStateMachine
        /// <summary>
        /// This method is called by the RSL engine at startup, read the last saved checkpoint
        /// </summary>
        /// <param name="reader"></param>
        public override bool LoadState(ManagedRSLCheckpointStream stream)
        {
            if (stream != null)
            {
                Byte[] buffer = new byte[4];
                
                while(stream.Read(buffer, 0, 4) != 0)
                {
                    _state.Add(System.BitConverter.ToUInt32(buffer,0));
                }
            }
            return true;
        }

        /// <summary>
        /// Saves the current state in a checkpoint file
        /// </summary>
        /// <param name="writer"></param>
        public override void SaveState(ManagedRSLCheckpointStream stream)
        {
            foreach (UInt32 i in _state)
            {
                Byte[] buffer = System.BitConverter.GetBytes(i);
                stream.Write(buffer, 0, buffer.Length);
            }
        }

        /// <summary>
        /// When a command is approved the request is sent to this method by the RSL engine
        /// We will use the first 4 bytes as the new integer to add to our state and
        /// verify the magic padding for the rest of the packet
        /// </summary>
        /// <param name="isFastRead"></param>
        /// <param name="rawRequest"></param>
        /// <param name="len"></param>
        /// <param name="cookie"></param>
        public override void ExecuteReplicatedRequest(Byte[] rawRequest, Object cookie, ref Boolean saveState)
        {
            saveState = false;
            UInt32 userState = System.BitConverter.ToUInt32(rawRequest, 0);
            Request requestObject;

            //Check the magic padding
            for (int idx = 4; idx < _reqBuffer.Length; idx++)
            {
                if (_reqBuffer[idx] != _magicPad)
                {
                    //This packet is incorrect, trow an exception
                    throw new ApplicationException("Incorrect magic padding reading request");
                }
            }

            //If this is a write request then add the new element to our state
            _state.Add(userState);

            if (cookie != null)
            {
                requestObject = (Request)cookie;
                requestObject.Process(userState);
            }
        }
        
        public override void ExecuteFastReadRequest(Byte[] rawRequest, Object cookie)
        {
            UInt32 userState = System.BitConverter.ToUInt32(rawRequest, 0);
            Request requestObject;

            //Check the magic padding
            for (int idx = 4; idx < _reqBuffer.Length; idx++)
            {
                if (_reqBuffer[idx] != _magicPad)
                {
                    //This packet is incorrect, trow an exception
                    throw new ApplicationException("Incorrect magic padding reading request");
                }
            }

            if (userState != 0xa5a5a5a5)
            {
                //Incorrect fast read signature
                throw new ApplicationException("Incorrect fast read signature");
            }
            userState = State;
            requestObject = (Request)cookie;
            requestObject.Process(userState);
        }
        
        /// <summary>
        /// This is called when the request can't be executed anymore because this
        /// replica is not a primary anymore
        /// </summary>
        /// <param name="status"></param>
        /// <param name="cookie"></param>
        public override void AbortRequest(RSLResponse status, Object cookie)
        {
            Request requestObject;
            if (cookie != null)
            {
                requestObject = (Request)cookie;
                requestObject.Cancel();
            }
        }

        /// <summary>
        /// This method is called when I become a primary
        /// </summary>
        /// <param name="isPrimary"></param>
        public override void NotifyStatus(bool isPrimary)
        {
            lock (Program._writer)
            {
                try
                {
                    if (isPrimary)
                    {
                        Program._writer.WriteLine("I'm primary");
                        Program._writer.Flush();
                        Program.imPrimary = true;
                    }
                    else
                    {
                        Program._writer.WriteLine("I'm no longer primary");
                        Program._writer.Flush();
                        Program.imPrimary = false;
                    }
                }
                catch
                {
                }
            }
        }

        public override void NotifyConfigurationChanged(object cookie)
        {
            Program._writer.WriteLine("Configuration has changed!");
            Program._writer.Flush();
        }

        public override void AbortChangeConfiguration(RSLResponse status, object cookie)
        {
            Program._writer.WriteLine("Configuration change aborted (" + status + ")!");
            Program._writer.Flush();
        }

        public override void NotifyPrimaryRecovered() {}

        /// <summary>
        /// This method is called when RSL decides that is time to shut
        /// this replica down
        /// </summary>
        /// <param name="status"></param>
        public override void ShutDown(RSLResponse status)
        {
            System.Environment.Exit(255);
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

        #endregion

        #region Properties
        public UInt32 State
        {
            get
            {
                if (_state.Count > 0)
                {
                    return (UInt32)_state[_state.Count - 1];
                }
                else
                {
                    return 0;
                }
            }
        }
        #endregion
    }
}
