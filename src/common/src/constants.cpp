#include "constants.h"

using namespace RSLibImpl;

UInt8 Constants::BITMASKS[Constants::BITS_PER_BYTE]= 
    {0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80};

UInt8 Constants::END_DOC_TERM[Constants::END_DOC_TERM_LENGTH + 1] = 
    "\1\2\3EndDoc\3\2\1";

UInt8 Constants::DOCID_PREFIX[Constants::DOCID_PREFIX_LENGTH + 1] =
    "\1\2\3DocId\3\2\1";

UInt8 Constants::DELETED_DOC_TERM[Constants::DELETED_DOC_TERM_LENGTH + 1] = 
    "\1\2\3DeletedDoc\3\2\1";

UInt8 Constants::META_PREFIX[Constants::META_PREFIX_LENGTH + 1] = 
    "\2\3\4Meta\4\3\2";

char Constants::SECONDARY_INDEX_FILE_EXTENSION[MAX_PATH] = ".sec";

UInt8 Constants::SR_TERM[Constants::SR_TERM_LENGTH + 1] = 
    "STATICRANK";

char Constants::SR_TERM_EXT[MAX_PATH] = ".srindex";

