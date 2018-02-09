## version 1.2.0.0: VotePayload feature

This change allows the secondaries to indicate a small payload (64bits) on each vote they accept. This is a very low footprint and high resolution channel from secondaries to the primary that allows the application at the secondaries to communicate the application at the primary aside with their health.
###Use case: 
A secondary can indicate a particular state to the primary. For example 
- "I am about to be stopped"
- "I am taking a checkpoint right now"
- "I am serving read-only requests and my load is X"
- "I have successfully applied (not only voted on) decree number x"

Note that the format of the payload is opaque to RSL, so the application can decide it completely, and it is up to the application to interpret its meaning.

To set the payload, a secondary can call stateMachine.SetVotePayload(unsigned long long somePayload) at any time, and from that point on, RSL will inject that payload into every vote that secondary accepts. Application can even call SetVotePayload from inside the AcceptMessageFromReplica callback.

To read the last payload provided by all replicas, the primary can get the cluster health, and that will include the last vote payload seen from each replica.

It is also important to note that the payload history is not tracked anywhere. Only the last payload for each replica is kept by the primary, so the application should not take a dependency on not losing any of those payloads. 

### How to consume it?

This is an opt-in feature. When initializing RSL, the application will need to indicate RSLProtocolVersion_6 as the version to run RSL with.

### IMPORTANT NOTE:
This is a change in RSL protocol, which means before any replica starts producing or consuming these messages, the whole cluster must be running the NEW code.
