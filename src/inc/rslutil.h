#pragma once

#include "rsl.h"

#ifdef RSL_EXPORTS
#define RSL_IMPORT_EXPORT   __declspec( dllexport )
#elif RSL_IMPORTS
#define RSL_IMPORT_EXPORT   __declspec( dllimport )
#else
#define RSL_IMPORT_EXPORT
#endif

namespace RSLib
{

    /*
     * This interface is used by RSLCheckpointUtility for dealing with
     * checkpoint file content.
     */
    class RSL_IMPORT_EXPORT IRSLCheckpointCreator
    {
    public:

        /* Provides checkpoint file content.
         *
         * This method gets called by RSLCheckpointUtility.SaveCheckpoint()
         * just after checkpoint header has been filled. Implemetors should
         * use writer parameter in order to provide the checkpoint file
         * content.
         *
         * PARAMETERS:
         *
         * RSLCheckpointStreamWriter* writer
         *   Stream to be filled with desired content.
         *
         */
        virtual void SaveState(RSLCheckpointStreamWriter* writer) = 0;
    };

    /*
     * Provides a set of utility methods for dealing with checkpoint files.
     */
    class RSL_IMPORT_EXPORT RSLCheckpointUtility
    {
    public:

        /* Creates a checkpoint file where the content its is provided
         * by the checkpointCreator parameter.
         *
         * PARAMETERS:
         *
         * char * directoryName
         *   Directory name where the checkpoint will be placed under.
         *
         * RSLProtocolVersion version
         *   Version of the checkpoint file. This needs to match with
         *   the RSL version used by the application that will read
         *   this checkpoint file.
         *
         * unsigned long long lastExecutedSequenceNumber
         *   This is the sequence number of the last executed command.
         *
         * unsigned int configurationNumber
         *   Configuration number of the current replica set.
         *
         * RSLMemberSet* memberSet
         *   The list of replicas of the current replica set.
         *
         * IRSLCheckpointCreator* checkpointCreator
         *   Object to be called back to fill the checkpoint content.
         *   See IRSLCheckpointCreator interface documentation for details.
         */
        static void SaveCheckpoint(char * directoryName, RSLProtocolVersion version,
            unsigned long long lastExecutedSequenceNumber, unsigned int configurationNumber,
            RSLMemberSet* memberSet, IRSLCheckpointCreator * checkpointCreator);

        // Version must be >= 3. If memberset has non 64 bit memberid, version >= 4
        static bool ChangeReplicaSet(const char* checkpointName, const RSLMemberSet *memberSet);

        static bool MigrateToVersion3(const char* checkpointName, const RSLMemberSet *memberSet);

        static bool MigrateToVersion4(const char* checkpointName, const RSLMemberSet *memberSet);

        /* Gets the replica set from the checkpoint. The checkpoint must be created
         * by a replica running with RSLProtocolVersion >= 3
         */
        static bool GetReplicaSet(const char* checkpointName, RSLMemberSet *memberSet);

        static DWORD GetLatestCheckpoint(const char* directoryName, char*buf, size_t bufSize);

    private:
        RSLCheckpointUtility();
    };

} // namespace RSLib
