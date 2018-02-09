#pragma once
#include "basic_types.h"
#include "logging.h"
#define NOMINMAX

namespace RSLibImpl
{

//This struct includes definition of all the global constants that are 
//used throughout. If there are constants specific to one class those should
//go with that class.
struct Constants
{
    //Bytes per Page
    static const UInt16 PAGE_SIZE = 4096;

    static const UInt8 LOG_BITS_PER_BYTE = 3;
    static const UInt8 BITS_PER_BYTE = (1 << LOG_BITS_PER_BYTE);

    //Number of bits in each location for meta data is stored in 4 bits
    static const UInt8 BITS_PER_LOCATION = 4;

    //The term importance (see fileformats.h)
    static const UInt8 BITS_TERM_IMPORTANCE = 2;

    //Total number of bits devoted to term meta data. 
    static const UInt8 BITS_TERM_META_DATA = 
        BITS_TERM_IMPORTANCE + BITS_PER_LOCATION; 

    static const UInt8 LOG_BYTES_PER_WORD = 3;
    static const UInt8 BYTES_PER_WORD = (1 << LOG_BYTES_PER_WORD);

    static const UInt8 LOG_BITS_PER_WORD =
        (LOG_BITS_PER_BYTE + LOG_BYTES_PER_WORD);
    static const UInt8 BITS_PER_WORD = (1 << LOG_BITS_PER_WORD);

    static const UInt8 BLOCKS_PER_PAGE = 54;

    //The high order bits
    static const UInt8 UPPER_NIBBLE_MASK = 0xF0;

    //The low order bits (always little endian)
    static const UInt8 LOWER_NIBBLE_MASK = 0x0F;

    //Half byte
    static const UInt8 NIBBLE_SIZE = BITS_PER_BYTE >> 1;

    //How many bytes per block of skip
    static const UInt8 LOG_BLOCK_SIZE = 6;
    static const UInt8 BLOCK_SIZE = (1 << LOG_BLOCK_SIZE);

    //How many bytes of data on a page
    static const UInt16 DATA_BYTES_PER_PAGE = BLOCKS_PER_PAGE*BLOCK_SIZE;

    //The size of the word to store inline. 
    static const UInt8 INLINE_TERM_SIZE = 14;

    //max size of term
    static const UInt8 MAX_TERM_SIZE = 0xFF;

    //Bits for meta data per posting
    static const UInt8 DEFAULT_META_BITS_PER_POSTING = 2;

    static const UInt64 MIN_LOC = 0Ui64;
    static const UInt64 MAX_LOC = (0Ui64 - 1Ui64);

    //Number of document records per subdoc file entry
    static const UInt8 SUBDOC_ENTRY_SPAN = 4;

    //First missing skip 
    static const UInt32 FIRST_MISSING_SKIP = 26;

    //Last missing skip
    static const UInt32 LAST_MISSING_SKIP = 27;

    //Index Partitions by Static Score
    static const UInt8 SPLITS_SCORE = 1;

    //Index Partitions by Document
    static const UInt8 SPLITS_DOC = 1;

    //Index Partitions by Word
    static const UInt8 SPLITS_WORD = 1;

    //End document term specification
    static const UInt8 END_DOC_TERM_LENGTH = 12;
    static UInt8 END_DOC_TERM[END_DOC_TERM_LENGTH + 1];

    //DocID term prefix
    static const UInt8 DOCID_PREFIX_LENGTH = 11;
    static UInt8 DOCID_PREFIX[DOCID_PREFIX_LENGTH + 1];

    //Deleted document term specification
    static const UInt8 DELETED_DOC_TERM_LENGTH = 16;
    static UInt8 DELETED_DOC_TERM[DELETED_DOC_TERM_LENGTH + 1];

    //Table to lookup bits TODO - find out if const can be used here.
    static UInt8 BITMASKS[BITS_PER_BYTE];

    //Max results for a query.
    static const UInt8 MAX_QUERY_RESULTS = 8;

    //Max terms in a query
    static const unsigned int MAX_QUERY_TERMS = 16;

    //from file formats (we should change that?)
    static const UInt8 DOCID_SIZE = 6;

    //Escape value in subIndex
    static const UInt8 SUBINDEX_ESCAPE = 0x1b;

    //Common Prefix for meta words, we can make it much
    //shorter if we want to. 
    static const UInt8 META_PREFIX_LENGTH = 10;
    static UInt8 META_PREFIX[META_PREFIX_LENGTH + 1];
    
    enum TermImportance
    {
        NormalTermImportance = 0,
        AboveNormalTermImportance,
        BelowNormalTermImportance
    };

    static char SECONDARY_INDEX_FILE_EXTENSION[MAX_PATH];

    static const UInt8 MAX_RANGE_PRECISION = 64;

    //The term for static rank range queries
    static const UInt8 SR_TERM_LENGTH = 10;
    static UInt8 SR_TERM[SR_TERM_LENGTH + 1];
    static char SR_TERM_EXT[MAX_PATH]; //Extension of file created
    static const UInt8 SR_TERM_BASE = 2; //Base of the range encoding
    static const UInt8 SR_TERM_PRECISION = 8;//Precision of range encoding
    static const UInt8 SR_TERM_IMA_SIZE = 20; //Log size of InMemoryAccumulator

    //Normalizing number of documents per index
    static const UInt64 NORMALIZED_DOCUMENTS_IN_INDEX = 1000000000;

    static const UInt32 EXPECTED_UNIQUE_WORDS_PER_DOCUMENT = 20;
    static const UInt32 EXPECTED_TOTAL_WORDS_PER_DOCUMENT = 600;
};

} // namespace RSLibImpl

