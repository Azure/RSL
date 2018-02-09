using System;
using ManagedRSLib;
using System.Diagnostics;

namespace ManagedRSLBasicTest
{
    public class BasicRSLTestMachine : ManagedRSLStateMachine
    {
        int _internalState;

        public ManagedRSLNode Self
        {
            get;
            protected set;
        }

        private bool _IsPrimary = false;
        public bool IsPrimary
        {
            get { return _IsPrimary; }
            set
            {
                if (value)
                {
                    Console.WriteLine("                                           PRIMARY = " + this.Self.MemberId);
                }
                _IsPrimary = value;
            }
        }

        public int InternalState
        {
            get
                {
                    return _internalState;
                }
            set
                {
                    Console.WriteLine(this.Self.MemberId + ".InternalState= " + value);
                    _internalState = value;
                }
        }

        public BasicRSLTestMachine(ManagedRSLNode self)
        {
            this.Self = self;
            this.InternalState = int.MinValue;
        }

        public override bool LoadState(ManagedRSLCheckpointStream stream)
        {
            Console.WriteLine(this.Self.MemberId + ".LoadState");
            if(stream != null)
            {
                int length = (int) stream.Length;
                Byte[] buffer = new byte[length];

                if (stream.Read(buffer, 0, length) != (int) length)
                {
                    return false;
                }

                this.InternalState = System.BitConverter.ToInt32(buffer, 0);

                return true;
            }

            return true;
        }

        ulong lastAppliedDecree = ulong.MinValue;

        public override void ExecuteReplicatedRequest(Byte[] rawRequest, Object cookie, ref Boolean saveState) 
        {
            Console.WriteLine(this.Self.MemberId + ".ExecuteReplicatedRequest");
            int requestedState = System.BitConverter.ToInt32(rawRequest,0);
            if(cookie!=null)
            {
                UserRequest request = (UserRequest) cookie;
                request.Response = GetCurrentSequenceNumber();
            }
            ulong thisDecree = GetCurrentSequenceNumber();
            Debug.Assert(lastAppliedDecree <= thisDecree);
            InternalState = requestedState;
            lastAppliedDecree = thisDecree;
            saveState = false;
        }

        public override void ExecuteFastReadRequest(Byte[] rawRequest, Object cookie)
        {
            Console.WriteLine(this.Self.MemberId + ".ExecuteFastReadRequest");
            int requestedState = System.BitConverter.ToInt32(rawRequest,0);
            if(requestedState != int.MaxValue)
            {
                throw(new ApplicationException("unexpected fast read"));
            }
            UserRequest request = (UserRequest) cookie;
            request.Response = GetCurrentSequenceNumber();
        }

        public override void AbortRequest(RSLResponse status, Object cookie)
        {
            Console.WriteLine(this.Self.MemberId + ".AbortRequest");
        }


        public override void NotifyConfigurationChanged(object cookie)
        {
            Console.WriteLine(this.Self.MemberId + ".NotifyConfigurationChanged: {0}", cookie);
        }

        public override void AbortChangeConfiguration(RSLResponse status, object cookie)
        {
            Console.WriteLine(this.Self.MemberId + ".AbortChangeConfiguration");
        }

        public override void SaveState(ManagedRSLCheckpointStream stream)
        {
            Console.WriteLine(this.Self.MemberId + ".SaveState");
            Byte[] buffer = System.BitConverter.GetBytes(this.InternalState);
            stream.Write(buffer, 0, buffer.Length);
        }

        public Action<BasicRSLTestMachine> OnPrimaryLoss = null;

        public override void NotifyStatus(bool isPrimary) 
        {
            Console.WriteLine(this.Self.MemberId + ".NotifyStatus("+isPrimary+")");
            if (!isPrimary && this.IsPrimary && OnPrimaryLoss != null)
            {
                OnPrimaryLoss(this);
            }

            this.IsPrimary = isPrimary;
        }

        public override void NotifyPrimaryRecovered()
        {
            Console.WriteLine(this.Self.MemberId + ".NotifyPrimaryRecovered");
            this.IsPrimary = true;
        }
        
        public override void ShutDown(RSLResponse status) 
        {
            Console.WriteLine(this.Self.MemberId + ".ShutDown");
            this.lastAppliedDecree = ulong.MinValue;
            this._internalState = int.MinValue;
        }

        public override bool CanBecomePrimary(ref byte[] primaryCookie)
        {
            Console.WriteLine(this.Self.MemberId + ".CanBP");
            return true;
        }

        public string MemberIdToEvict = String.Empty;

        public override bool AcceptMessageFromReplica(ManagedRSLNode node, byte [] primaryCookie)
        {
            Console.WriteLine(this.Self.MemberId + ".AcceptMessageFromReplica("+node.MemberId+")");

            if (String.Equals(node.MemberId, MemberIdToEvict))
            {
                Console.WriteLine(this.Self.MemberId + ".AcceptMessageFromReplica(" + node.MemberId + ")=false --------------------------------------------");
                return false;
            }
            return true;
        }

        public override void StateSaved(UInt64 seqNo, String fileName)
        {
            Console.WriteLine(this.Self.MemberId + ".StateSaved");
            return;
        }

        public override void StateCopied(UInt64 seqNo, String fileName, object cookie)
        {
            Console.WriteLine(this.Self.MemberId + ".StateCopied");
            return;
        }

        public bool ReplicateRequest(UserRequest request)
        {
            return (this.ReplicateRequest(request.RawRequest, request) == RSLResponse.RSLSuccess);
        }
    }
}
