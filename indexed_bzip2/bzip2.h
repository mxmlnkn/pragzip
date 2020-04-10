/**
 * Modified version of bzcat.c part of toybox commit 7bf68329eb3b
 * by Rob Landley released under SPDX-0BSD license.
 */

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>


#include "BitReader.hpp"


class BZ2Reader
{
public:
    /* Constants for huffman coding */
    static constexpr int MAX_GROUPS = 6;
    static constexpr int GROUP_SIZE = 50;       /* 64 would have been more efficient */
    static constexpr int MAX_HUFCODE_BITS = 20; /* Longest huffman code allowed */
    static constexpr int MAX_SYMBOLS = 258;     /* 256 literals + RUNA + RUNB */
    static constexpr int SYMBOL_RUNA = 0;
    static constexpr int SYMBOL_RUNB = 1;

    static constexpr size_t IOBUF_SIZE = 4096;
    static constexpr int THREADS = 1;
    static constexpr int CRC32_LOOKUP_TABLE_SIZE = 256;
    /* a small lookup table: raw data -> CRC32 value to speed up CRC calculation */
    static const std::array<uint32_t, CRC32_LOOKUP_TABLE_SIZE> CRC32_TABLE;

private:
    // This is what we know about each huffman coding group
    struct GroupData
    {
        std::array<int, MAX_HUFCODE_BITS + 1> limit;
        std::array<int, MAX_HUFCODE_BITS> base;
        std::array<uint16_t, MAX_SYMBOLS> permute;
        uint8_t minLen;
        uint8_t maxLen;
    };

    struct BurrowsWheelerTransformData
    {
    public:
        void
        prepare();

    public:
        uint32_t origPtr = 0;
        std::array<int, 256> byteCount;

        /* These variables are saved when interrupting decode and are required for resuming */
        int writePos = 0;
        int writeRun = 0;
        int writeCount = 0;
        int writeCurrent = 0;

        uint32_t dataCRC = 0; /* CRC of block as calculated by us */
        uint32_t headerCRC = 0; /* what the block data CRC should be */
        std::vector<uint32_t> dbuf;
    };

    struct BlockHeader
    {
        uint64_t magicBytes;
        bool isRandomized;

        /* First pass decompression data (Huffman and MTF decoding) */

        /**
         * The Mapping table itself is compressed in two parts:
         * huffman_used_map: each bit indicates whether the corresponding range [0...15], [16...31] is present.
         * huffman_used_bitmaps: 0-16 16-bit bitmaps
         * The Huffman map gives 0, 10, 11, 100, 101, ... (8-bit) symbols
         * Instead of storing 2 * 256 bytes ( 0b : A, 10b : B, .. ) for the table, the first part is left out.
         * And for short maps, only the first n-th are actually stored.
         * The second half is also assumed to be ordered, so that we only need to store which symbols are actually
         * present.
         * This however means that the Huffmann table can't be frequency sorted, therefore this is done in a
         * second step / table, the mtfSymbol (move to front) map.
         * This would need 256 bits to store the table in huffman_used_bitmaps.
         * These bits are split in groups of 16 and the presence of each group is encoded in huffman_used_map
         * to save even more bytes.
         * @verbatim
         *  10001000 00000000     # huffman_used_map (bit map)
         *  ^   ^
         *  |   [64,95]
         *  [0...15]
         *  00000000 00100000     # huffman_used_bitmaps[0]
         *  ^          ^    ^
         *  0          10   15
         *          (newline)
         *  00000100 10001001     # huffman_used_bitmaps[1]
         *  ^    ^   ^   ^  ^
         *  64   69  72  76 95
         *       E   H   L  O
         * @endverbatim
         */
        uint16_t huffman_used_map;
        /**
         * mapping table: if some byte values are never used (encoding things
         * like ascii text), the compression code removes the gaps to have fewer
         * symbols to deal with, and writes a sparse bitfield indicating which
         * values were present.  We make a translation table to convert the symbols
         * back to the corresponding bytes.
         */
        std::array<uint8_t, 256> symbolToByte;
        std::array<uint8_t, 256> mtfSymbol;
        unsigned int symbolCount;
        uint16_t huffman_groups; // only actually 15 bit
        /**
         * Every GROUP_SIZE many symbols we switch huffman coding tables.
         * Each group has a selector, which is an index into the huffman coding table arrays.
         *
         * Read in the group selector array, which is stored as MTF encoded
         * bit runs.  (MTF = Move To Front.  Every time a symbol occurs it's moved
         * to the front of the table, so it has a shorter encoding next time.)
         */
        uint16_t selectors_used;

        std::array<char, 32768> selectors;        // nSelectors=15 bits
        std::array<GroupData, MAX_GROUPS> groups; // huffman coding tables
        int groupCount = 0;

        /* Second pass decompression data (burrows-wheeler transform) */
        BurrowsWheelerTransformData bwdata;
    };

public:
    BZ2Reader( std::string filePath ) :
        m_bitReader( filePath )
    {}

    BZ2Reader( int fileDescriptor ) :
        m_bitReader( fileDescriptor )
    {}

    BZ2Reader( const char*  bz2Data,
               const size_t size ) :
        m_bitReader( reinterpret_cast<const uint8_t*>( bz2Data ), size )
    {}

    int
    fileno() const
    {
        return ::fileno( m_bitReader.fp() );
    }

    void
    close()
    {
        m_bitReader.close();
    }

    bool
    closed() const
    {
        return m_bitReader.closed();
    }

    uint32_t
    crc() const
    {
        return m_calculatedStreamCRC;
    }

    bool
    eos() const
    {
        return m_atEndOfStream;
    }

    bool
    eof() const
    {
        return m_atEndOfFile;
    }

    /**
     * @return vectors of block data: offset in file, offset in decoded data
     *         (cumulative size of all prior decoded blocks).
     */
    std::map<size_t, size_t>
    blockOffsets()
    {
        if ( !m_blockToDataOffsetsComplete ) {
            decodeBzip2();
        }

        return m_blockToDataOffsets;
    }

    void
    setBlockOffsets( std::map<size_t, size_t> offsets )
    {
        if ( offsets.size() < 2 ) {
            throw std::invalid_argument( "Block offset map must contain at least one valid block and one EOS block!" );
        }
        m_blockToDataOffsetsComplete = true;
        m_blockToDataOffsets = std::move( offsets );
    }

    size_t
    tell() const
    {
        return m_decodedBytesCount; /* @todo only works for first go through! */
    }

    /**
     * @return number of processed bits of compressed bzip2 input file stream
     * @note Bzip2 is block based and blocks are currently read fully, meaning that the granularity
     *       of the returned position is ~100-900kB. It's only useful for a rough estimate.
     */
    size_t
    tellCompressed() const
    {
        return m_bitReader.tell();
    }

    size_t
    size() const
    {
        if ( !m_blockToDataOffsetsComplete ) {
            throw std::invalid_argument( "Can't get stream size in BZ2 when not finished reading at least once!" );
        }
        return m_blockToDataOffsets.rbegin()->second;
    }

    size_t
    seek( long long int offset,
          int           origin = SEEK_SET );

    /**
     * Undo burrows-wheeler transform on intermediate buffer @ref dbuf to @ref outBuf
     *
     * Burrows-wheeler transform is described at:
     * @see http://dogma.net/markn/articles/bwt/bwt.htm
     * @see http://marknelson.us/1996/09/01/bwt/
     *
     * @return number of actually decoded bytes
     */
    size_t
    decodeStream( size_t nMaxBytesToDecode = std::numeric_limits<size_t>::max() );

    /**
     * The input may be a concatenation of multiple BZip2 files (like produed by pbzip2).
     * This function iterates over those mutliple files and decodes them to the specified output.
     */
    size_t
    decodeBzip2( size_t nMaxBytesToDecode = std::numeric_limits<size_t>::max() )
    {
        size_t nBytesDecoded = 0;
        while ( ( nBytesDecoded < nMaxBytesToDecode ) && !m_bitReader.eof() ) {
            if ( ( m_bitReader.tell() == 0 ) || eos() ) {
                readBzip2Header();
            }
            nBytesDecoded += decodeStream( nMaxBytesToDecode - nBytesDecoded );
        }
        return nBytesDecoded;
    }

    /**
     * @param[out] outputBuffer should at least be large enough to hold @p nBytesToRead bytes
     * @return number of bytes written
     */
    int
    read( const int    outputFileDescriptor,
          char* const  outputBuffer = nullptr,
          const size_t nBytesToRead = std::numeric_limits<size_t>::max() )
    {
        if ( eof() ) {
            return 0;
        }

        m_outputFileDescriptor = outputFileDescriptor;
        m_outputBuffer = outputBuffer;
        m_outputBufferSize = nBytesToRead;
        m_outputBufferWritten = 0;

        const auto nBytes = decodeBzip2( nBytesToRead );

        m_outputFileDescriptor = -1;
        m_outputBuffer = nullptr;
        m_outputBufferSize = 0;
        m_outputBufferWritten = 0;

        return nBytes;
    }

private:
    uint32_t
    getBits( uint8_t nBits )
    {
        return m_bitReader.read( nBits );
    }

    size_t
    flushOutputBuffer( size_t maxBytesToFlush = std::numeric_limits<size_t>::max() );

    BlockHeader
    readBlockHeader();

    /**
     * First pass, read block's symbols into dbuf[dbufCount].
     *
     * This undoes three types of compression: huffman coding, run length encoding,
     * and move to front encoding.  We have to undo all those to know when we've
     * read enough input.
     */
    int
    readBlockData( BlockHeader* header );

    static std::array<uint32_t, CRC32_LOOKUP_TABLE_SIZE>
    createCRC32LookupTable( bool littleEndian = false );

    void
    readBzip2Header();

private:
    BitReader m_bitReader;
    BlockHeader m_lastHeader;

    /* this buffer is needed for decoding */
    std::vector<char> m_decodedBuffer = std::vector<char>( IOBUF_SIZE );
    /* it's strictly increasing during decoding and no previous data in m_decodedBuffer is acced,
     * so we can almost at any position clear m_decodedBuffer and set m_decodedBufferPos to 0, which is done for flushing! */
    size_t m_decodedBufferPos = 0;

    /* Objects related to final output to user. Used by flush */
    int m_outputFileDescriptor = -1;
    char* m_outputBuffer = nullptr;
    size_t m_outputBufferSize = 0; /** write only maximum of this bytes to m_outputBuffer */
    size_t m_outputBufferWritten = 0; /** gets reset on each read call. Return result for @ref read */

    uint8_t m_blockSize100k = 0;
    uint32_t m_streamCRC = 0; /** CRC of stream as last block says */
    uint32_t m_calculatedStreamCRC = 0;
    bool m_blockToDataOffsetsComplete = false;
    size_t m_decodedBytesCount = 0; /** in contrast to m_outputBufferWritten this is the sum over all decodeBuffer calls */
    bool m_atEndOfStream = false;
    bool m_atEndOfFile = false;

    std::map<size_t, size_t> m_blockToDataOffsets;
};


const std::array<uint32_t, BZ2Reader::CRC32_LOOKUP_TABLE_SIZE> BZ2Reader::CRC32_TABLE = createCRC32LookupTable();


size_t
BZ2Reader::seek( long long int offset,
                 int           origin )
{
    if ( !m_blockToDataOffsetsComplete ) {
        decodeBzip2();
    }

    switch ( origin )
    {
    case SEEK_CUR:
        offset = tell() + offset;
        break;
    case SEEK_SET:
        break;
    case SEEK_END:
        offset = size() + offset;
        break;
    }

    if ( static_cast<long long int>( tell() ) == offset ) {
        return offset;
    }

    offset = std::max( 0LL, offset );

    flushOutputBuffer(); // ensure that no old data is left over

    m_atEndOfFile = static_cast<size_t>( offset ) >= size();
    if ( eof() ) {
        return offset;
    }

    /* find offset from map (key and values are sorted, so we can bisect!) */
    const auto blockOffset = std::lower_bound(
        m_blockToDataOffsets.rbegin(), m_blockToDataOffsets.rend(), std::make_pair( 0, offset ),
        [offset] ( std::pair<size_t, size_t> a, std::pair<size_t, size_t> b ) { return a.second > b.second; } );

    if ( ( blockOffset == m_blockToDataOffsets.rend() ) || ( static_cast<size_t>( offset ) < blockOffset->second ) ) {
        throw std::runtime_error( "Could not find block to seek to for given offset" );
    }
    const auto nBytesSeekInBlock = offset - blockOffset->second;

    m_bitReader.seek( blockOffset->first );

    m_lastHeader = readBlockHeader();
    readBlockData( &m_lastHeader );
    m_lastHeader.bwdata.prepare();
    /* no decodeBzip2 necessary because we only seek inside one block! */
    const auto nBytesDecoded = decodeStream( nBytesSeekInBlock );

    if ( nBytesDecoded != nBytesSeekInBlock ) {
        std::stringstream msg;
        msg << "Could not read the required " << nBytesSeekInBlock
        << " to seek in block but only " << nBytesDecoded << "\n";
        throw std::runtime_error( msg.str() );
    }

    return offset;
}


/* Read block header at start of a new compressed data block.  Consists of:
 *
 * 48 bits : Block signature, either pi (data block) or e (EOF block).
 * 32 bits : bw->headerCRC
 * 1  bit  : obsolete feature flag.
 * 24 bits : origPtr (Burrows-wheeler unwind index, only 20 bits ever used)
 * 16 bits : Mapping table index.
 *[16 bits]: symToByte[symTotal] (Mapping table.  For each bit set in mapping
 *           table index above, read another 16 bits of mapping table data.
 *           If correspondig bit is unset, all bits in that mapping table
 *           section are 0.)
 *  3 bits : groupCount (how many huffman tables used to encode, anywhere
 *           from 2 to MAX_GROUPS)
 * variable: hufGroup[groupCount] (MTF encoded huffman table data.)
 */
inline BZ2Reader::BlockHeader
BZ2Reader::readBlockHeader()
{
    BlockHeader header;

    /* note that blocks are NOT byte-aligned! Only the end of the stream has a necessary padding. */
    if ( !m_blockToDataOffsetsComplete ) {
        m_blockToDataOffsets.insert( { m_bitReader.tell(), m_decodedBytesCount } );
    }

    header.magicBytes = ( (uint64_t)getBits( 24 ) << 24 ) | (uint64_t)getBits( 24 );
    header.bwdata.headerCRC = getBits( 32 );
    m_atEndOfStream = header.magicBytes == 0x177245385090 /* bcd(sqrt(pi)) */;
    if ( m_atEndOfStream ) {
        /* EOS block contains CRC for whole stream */
        m_streamCRC = header.bwdata.headerCRC;

        /* read byte padding bits */
        const auto nBitsInByte = m_bitReader.tell() & 7LLU;
        if ( nBitsInByte > 0 ) {
            m_bitReader.read( 8 - nBitsInByte );
        }

        m_atEndOfFile = m_bitReader.eof();
        if ( m_atEndOfFile ) {
            m_blockToDataOffsetsComplete = true;
        }

        if ( m_streamCRC != m_calculatedStreamCRC ) {
            std::stringstream msg;
            msg << "[BZip2 block header] Stream CRC 0x" << std::hex << m_streamCRC
            << " does not match calculated CRC 0x" << m_calculatedStreamCRC;
            throw std::runtime_error( msg.str() );
        }

        return header;
    }

    if ( header.magicBytes != 0x314159265359 /* bcd(pi) */ ) {
        std::stringstream msg;
        msg << "[BZip2 block header] invalid compressed magic 0x" << std::hex << header.magicBytes;
        throw std::domain_error( msg.str() );
    }

    /* simply allocate the maximum of 900kB for the internal block size so we won't run into problem when
     * block sizes changes (e.g. in pbzip2 file). 900kB is nothing in today's age anyways. */
    header.bwdata.dbuf.resize( 900000, 0 );
    header.isRandomized = getBits( 1 );
    if ( header.isRandomized ) {
        throw std::domain_error( "[BZip2 block header] deprecated isRandomized bit is not supported" );
    }

    if ( ( header.bwdata.origPtr = getBits( 24 ) ) > header.bwdata.dbuf.size() ) {
        std::stringstream msg;
        msg << "[BZip2 block header] origPtr " << header.bwdata.origPtr << " is larger than buffer size: " <<
        header.bwdata.dbuf.size();
        throw std::logic_error( msg.str() );
    }

    // mapping table: if some byte values are never used (encoding things
    // like ascii text), the compression code removes the gaps to have fewer
    // symbols to deal with, and writes a sparse bitfield indicating which
    // values were present.  We make a translation table to convert the symbols
    // back to the corresponding bytes.
    header.huffman_groups = getBits( 16 );
    header.symbolCount = 0;
    for ( int i = 0; i < 16; i++ ) {
        if ( header.huffman_groups & ( 1 << ( 15 - i ) ) ) {
            const auto bitmap = getBits( 16 );
            for ( int j = 0; j < 16; j++ ) {
                if ( bitmap & ( 1 << ( 15 - j ) ) ) {
                    header.symbolToByte[header.symbolCount++] = ( 16 * i ) + j;
                }
            }
        }
    }

    // How many different huffman coding groups does this block use?
    header.groupCount = getBits( 3 );
    if ( ( header.groupCount < 2 ) || ( header.groupCount > MAX_GROUPS ) ) {
        std::stringstream msg;
        msg << "[BZip2 block header] Invalid Huffman coding group count " << header.groupCount;
        throw std::logic_error( msg.str() );
    }

    // nSelectors: Every GROUP_SIZE many symbols we switch huffman coding
    // tables.  Each group has a selector, which is an index into the huffman
    // coding table arrays.
    //
    // Read in the group selector array, which is stored as MTF encoded
    // bit runs.  (MTF = Move To Front.  Every time a symbol occurs it's moved
    // to the front of the table, so it has a shorter encoding next time.)
    if ( !( header.selectors_used = getBits( 15 ) ) ) {
        std::stringstream msg;
        msg << "[BZip2 block header] selectors_used " << header.selectors_used << " is invalid";
        throw std::logic_error( msg.str() );
    }
    for ( int i = 0; i < header.groupCount; i++ ) {
        header.mtfSymbol[i] = i;
    }
    for ( int i = 0; i < header.selectors_used; i++ ) {
        int j = 0;
        for ( ; getBits( 1 ); j++ ) {
            if ( j >= header.groupCount ) {
                std::stringstream msg;
                msg << "[BZip2 block header] Could not find zero termination after " << header.groupCount << " bits";
                throw std::domain_error( msg.str() );
            }
        }

        // Decode MTF to get the next selector, and move it to the front.
        const auto uc = header.mtfSymbol[j];
        memmove( header.mtfSymbol.data() + 1, header.mtfSymbol.data(), j );
        header.mtfSymbol[0] = header.selectors[i] = uc;
    }

    // Read the huffman coding tables for each group, which code for symTotal
    // literal symbols, plus two run symbols (RUNA, RUNB)
    const auto symCount = header.symbolCount + 2;
    auto& hh = header.huffman_groups;
    for ( int j = 0; j < header.groupCount; j++ ) {
        // Read lengths
        std::array<uint8_t, MAX_SYMBOLS> length;
        hh = getBits( 5 );
        for ( unsigned int i = 0; i < symCount; i++ ) {
            while ( true ) {
                // !hh || hh > MAX_HUFCODE_BITS in one test.
                if ( MAX_HUFCODE_BITS - 1 < (unsigned)hh - 1 ) {
                    std::stringstream msg;
                    msg << "[BZip2 block header]  start_huffman_length " << hh
                    << " is larger than " << MAX_HUFCODE_BITS << " or zero\n";
                    throw std::logic_error( msg.str() );
                }
                // Grab 2 bits instead of 1 (slightly smaller/faster).  Stop if
                // first bit is 0, otherwise second bit says whether to
                // increment or decrement.
                const auto kk = getBits( 2 );
                if ( kk & 2 ) {
                    hh += 1 - ( ( kk & 1 ) << 1 );
                } else {
                    m_bitReader.m_inbufBitCount++;
                    break;
                }
            }
            length[i] = hh;
        }

        /* Calculate permute[], base[], and limit[] tables from length[].
         *
         * permute[] is the lookup table for converting huffman coded symbols
         * into decoded symbols.  It contains symbol values sorted by length.
         *
         * base[] is the amount to subtract from the value of a huffman symbol
         * of a given length when using permute[].
         *
         * limit[] indicates the largest numerical value a symbol with a given
         * number of bits can have.  It lets us know when to stop reading.
         *
         * To use these, keep reading bits until value <= limit[bitcount] or
         * you've read over 20 bits (error).  Then the decoded symbol
         * equals permute[hufcode_value - base[hufcode_bitcount]].
         */
        const auto hufGroup = &header.groups[j];
        hufGroup->minLen = *std::min_element( length.begin(), length.begin() + symCount );
        hufGroup->maxLen = *std::max_element( length.begin(), length.begin() + symCount );

        // Note that minLen can't be smaller than 1, so we adjust the base
        // and limit array pointers so we're not always wasting the first
        // entry.  We do this again when using them (during symbol decoding).
        const auto base = hufGroup->base.data() - 1;
        const auto limit = hufGroup->limit.data() - 1;

        // zero temp[] and limit[], and calculate permute[]
        int pp = 0;
        std::array<unsigned int, MAX_HUFCODE_BITS + 1> temp;
        for ( int i = hufGroup->minLen; i <= hufGroup->maxLen; i++ ) {
            temp[i] = limit[i] = 0;
            for ( hh = 0; hh < symCount; hh++ ) {
                if ( length[hh] == i ) {
                    hufGroup->permute[pp++] = hh;
                }
            }
        }

        // Count symbols coded for at each bit length
        for ( unsigned int i = 0; i < symCount; i++ ) {
            temp[length[i]]++;
        }

        /* Calculate limit[] (the largest symbol-coding value at each bit
         * length, which is (previous limit<<1)+symbols at this level), and
         * base[] (number of symbols to ignore at each bit length, which is
         * limit minus the cumulative count of symbols coded for already). */
        pp = hh = 0;
        for ( int i = hufGroup->minLen; i < hufGroup->maxLen; i++ ) {
            pp += temp[i];
            limit[i] = pp - 1;
            pp <<= 1;
            base[i + 1] = pp - ( hh += temp[i] );
        }
        limit[hufGroup->maxLen] = pp + temp[hufGroup->maxLen] - 1;
        limit[hufGroup->maxLen + 1] = std::numeric_limits<int>::max();
        base[hufGroup->minLen] = 0;
    }

    return header;
}


inline int
BZ2Reader::readBlockData( BlockHeader* const header )
{
    const GroupData* hufGroup = nullptr;

    // We've finished reading and digesting the block header.  Now read this
    // block's huffman coded symbols from the file and undo the huffman coding
    // and run length encoding, saving the result into dbuf[dbufCount++] = uc

    header->bwdata.byteCount.fill( 0 );
    std::iota( header->mtfSymbol.begin(), header->mtfSymbol.end(), 0 );

    // Loop through compressed symbols.  This is the first "tight inner loop"
    // that needs to be micro-optimized for speed.  (This one fills out dbuf[]
    // linearly, staying in cache more, so isn't as limited by DRAM access.)
    const int* base = nullptr;
    const int* limit = nullptr;
    int dbufCount = 0;
    for ( int ii, jj, hh = 0, runPos = 0, symCount = 0, selector = 0; ; ) {
        // Have we reached the end of this huffman group?
        if ( !( symCount-- ) ) {
            // Determine which huffman coding group to use.
            symCount = GROUP_SIZE - 1;
            if ( selector >= header->selectors_used ) {
                std::stringstream msg;
                msg << "[BZip2 block data] selector " << selector << " out of maximum range " << header->selectors_used;
                throw std::domain_error( msg.str() );
            }
            hufGroup = &header->groups[header->selectors[selector++]];
            base = hufGroup->base.data() - 1;
            limit = hufGroup->limit.data() - 1;
        }

        // Read next huffman-coded symbol (into jj).
        ii = hufGroup->minLen;
        jj = getBits( ii );
        while ( jj > limit[ii] ) {
            // if (ii > hufGroup->maxLen) return RETVAL_DATA_ERROR;
            ii++;

            // Unroll getBits() to avoid a function call when the data's in
            // the buffer already.
            const auto kk =
                m_bitReader.m_inbufBitCount ? ( m_bitReader.m_inbufBits >> --( m_bitReader.m_inbufBitCount ) ) &
                1 : getBits( 1 );
            jj = ( jj << 1 ) | kk;
        }
        // Huffman decode jj into nextSym (with bounds checking)
        jj -= base[ii];

        if ( ii > hufGroup->maxLen ) {
            std::stringstream msg;
            msg << "[BZip2 block data] " << ii << " bigger than max length " << hufGroup->maxLen;
            throw std::domain_error( msg.str() );
        }

        if ( (unsigned)jj >= MAX_SYMBOLS ) {
            std::stringstream msg;
            msg << "[BZip2 block data] " << jj << " larger than max symbols " << MAX_SYMBOLS;
            throw std::domain_error( msg.str() );
        }

        const auto nextSym = hufGroup->permute[jj];

        // If this is a repeated run, loop collecting data
        if ( (unsigned)nextSym <= SYMBOL_RUNB ) {
            // If this is the start of a new run, zero out counter
            if ( !runPos ) {
                runPos = 1;
                hh = 0;
            }

            /* Neat trick that saves 1 symbol: instead of or-ing 0 or 1 at
             * each bit position, add 1 or 2 instead. For example,
             * 1011 is 1<<0 + 1<<1 + 2<<2. 1010 is 2<<0 + 2<<1 + 1<<2.
             * You can make any bit pattern that way using 1 less symbol than
             * the basic or 0/1 method (except all bits 0, which would use no
             * symbols, but a run of length 0 doesn't mean anything in this
             * context). Thus space is saved. */
            hh += ( runPos << nextSym ); // +runPos if RUNA; +2*runPos if RUNB
            runPos <<= 1;
            continue;
        }

        /* When we hit the first non-run symbol after a run, we now know
         * how many times to repeat the last literal, so append that many
         * copies to our buffer of decoded symbols (dbuf) now. (The last
         * literal used is the one at the head of the mtfSymbol array.) */
        if ( runPos ) {
            runPos = 0;
            if ( dbufCount + hh > (int)header->bwdata.dbuf.size() ) {
                std::stringstream msg;
                msg << "[BZip2 block data] dbufCount + hh " << dbufCount + hh
                << " > " << header->bwdata.dbuf.size() << " dbufSize";
                throw std::domain_error( msg.str() );
            }

            const auto uc = header->symbolToByte[header->mtfSymbol[0]];
            header->bwdata.byteCount[uc] += hh;
            while ( hh-- ) {
                header->bwdata.dbuf[dbufCount++] = uc;
            }
        }

        // Is this the terminating symbol?
        if ( nextSym > header->symbolCount ) {
            break;
        }

        /* At this point, the symbol we just decoded indicates a new literal
         * character. Subtract one to get the position in the MTF array
         * at which this literal is currently to be found. (Note that the
         * result can't be -1 or 0, because 0 and 1 are RUNA and RUNB.
         * Another instance of the first symbol in the mtf array, position 0,
         * would have been handled as part of a run.) */
        if ( dbufCount >= (int)header->bwdata.dbuf.size() ) {
            std::stringstream msg;
            msg << "[BZip2 block data] dbufCount " << dbufCount << " > " << header->bwdata.dbuf.size() << " dbufSize";
            throw std::domain_error( msg.str() );
        }
        ii = nextSym - 1;
        auto uc = header->mtfSymbol[ii];
        // On my laptop, unrolling this memmove() into a loop shaves 3.5% off
        // the total running time.
        while ( ii-- ) {
            header->mtfSymbol[ii + 1] = header->mtfSymbol[ii];
        }
        header->mtfSymbol[0] = uc;
        uc = header->symbolToByte[uc];

        // We have our literal byte.  Save it into dbuf.
        header->bwdata.byteCount[uc]++;
        header->bwdata.dbuf[dbufCount++] = uc;
    }

    // Now we know what dbufCount is, do a better sanity check on origPtr.
    if ( header->bwdata.origPtr >= (unsigned int)( header->bwdata.writeCount = dbufCount ) ) {
        std::stringstream msg;
        msg << "[BZip2 block data] origPtr error " << header->bwdata.origPtr;
        throw std::domain_error( msg.str() );
    }

    return 0;
}


inline size_t
BZ2Reader::flushOutputBuffer( size_t maxBytesToFlush )
{
    const auto nBytesToFlush = std::min( m_decodedBufferPos, maxBytesToFlush );
    size_t nBytesFlushed = nBytesToFlush; // default then there is neither output buffer nor file device given

    if ( m_outputFileDescriptor >= 0 ) {
        nBytesFlushed = write( m_outputFileDescriptor, m_decodedBuffer.data(), nBytesToFlush );
    }

    if ( m_outputBuffer != nullptr ) {
        if ( m_outputBufferWritten <= m_outputBufferSize ) {
            nBytesFlushed = std::min( nBytesToFlush, m_outputBufferSize - m_outputBufferWritten );
            std::memcpy( m_outputBuffer + m_outputBufferWritten, m_decodedBuffer.data(), nBytesFlushed );
        } else {
            nBytesFlushed = 0;
        }
    }

    if ( nBytesFlushed > 0 ) {
        m_decodedBytesCount += nBytesFlushed;
        m_outputBufferWritten += nBytesFlushed;
        m_decodedBufferPos -= nBytesFlushed;
        std::memmove( m_decodedBuffer.data(), m_decodedBuffer.data() + nBytesFlushed, m_decodedBufferPos );
    }

    return nBytesFlushed;
}


inline void
BZ2Reader::BurrowsWheelerTransformData::prepare()
{
    // Turn byteCount into cumulative occurrence counts of 0 to n-1.
    for ( size_t i = 0, j = 0; i < byteCount.size(); ++i ) {
        const auto kk = j + byteCount[i];
        byteCount[i] = j;
        j = kk;
    }

    // Use occurrence counts to quickly figure out what order dbuf would be in
    // if we sorted it.
    // Using i as position, j as previous character, hh as current character,
    // and uc as run count.
    for ( int i = 0; i < writeCount; i++ ) {
        const auto uc = static_cast<uint8_t>( dbuf[i] );
        dbuf[byteCount[uc]] |= ( i << 8 );
        byteCount[uc]++;
    }

    dataCRC = 0xFFFFFFFFL;

    /* Decode first byte by hand to initialize "previous" byte. Note that it
     * doesn't get output, and if the first three characters are identical
     * it doesn't qualify as a run (hence uc=255, which will either wrap
     * to 1 or get reset). */
    if ( writeCount > 0 ) {
        writePos = dbuf[origPtr];
        writeCurrent = (unsigned char)writePos;
        writePos >>= 8;
        writeRun = -1;
    }
}


inline size_t
BZ2Reader::decodeStream( const size_t nMaxBytesToDecode )
{
    if ( eof() || ( nMaxBytesToDecode == 0 ) ) {
        return 0;
    }

    int count, pos, current, run, copies, outbyte, previous;
    const auto bw = &m_lastHeader.bwdata;
    // try to flush remnants in output buffer from interrupted last call
    size_t nBytesDecoded = flushOutputBuffer( nMaxBytesToDecode );

    while ( nBytesDecoded < nMaxBytesToDecode ) {
        // If we need to refill dbuf, do it. Only won't be required for resuming interrupted decodations
        if ( bw->writeCount == 0 ) {
            m_lastHeader = readBlockHeader();
            if ( eos() ) {
                bw->writeCount = 0;
                return nBytesDecoded;
            }
            readBlockData( &m_lastHeader );
            m_lastHeader.bwdata.prepare();
        }

        // loop generating output
        count = bw->writeCount;
        pos = bw->writePos;
        current = bw->writeCurrent;
        run = bw->writeRun;
        while ( count > 0 ) {
            // If somebody (like tar) wants a certain number of bytes of
            // data from memory instead of written to a file, humor them.
            if ( nBytesDecoded + m_decodedBufferPos >= nMaxBytesToDecode ) {
                break;
            }
            count--;

            // Follow sequence vector to undo Burrows-Wheeler transform.
            previous = current;
            pos = bw->dbuf[pos];
            current = pos & 0xff;
            pos >>= 8;

            /* Whenever we see 3 consecutive copies of the same byte, the 4th is a repeat count */
            if ( run++ == 3 ) {
                copies = current;
                outbyte = previous;
                current = -1;
            } else {
                copies = 1;
                outbyte = current;
            }

            // Output bytes to buffer, flushing to file if necessary
            while ( copies-- ) {
                if ( m_decodedBufferPos == IOBUF_SIZE ) {
                    nBytesDecoded += flushOutputBuffer( nMaxBytesToDecode - nBytesDecoded );
                }
                m_decodedBuffer[m_decodedBufferPos++] = outbyte;
                bw->dataCRC = ( bw->dataCRC << 8 ) ^ CRC32_TABLE[( bw->dataCRC >> 24 ) ^ outbyte];
            }
            if ( current != previous ) {
                run = 0;
            }
        }

        /* decompression of this block completed successfully */
        if ( count == 0 ) {
            bw->dataCRC = ~( bw->dataCRC );
            m_calculatedStreamCRC = ( ( m_calculatedStreamCRC << 1 ) | ( m_calculatedStreamCRC >> 31 ) ) ^ bw->dataCRC;
            if ( bw->dataCRC != bw->headerCRC ) {
                std::stringstream msg;
                msg << "Calculated CRC " << std::hex << bw->dataCRC << " for block mismatches " << bw->headerCRC;
                throw std::runtime_error( msg.str() );
            }
        }

        bw->writeCount = count;
        /* required for correct data offsets in readBlockHeader */
        nBytesDecoded += flushOutputBuffer( nMaxBytesToDecode - nBytesDecoded );

        /* if we got enough data, save loop state and return */
        if ( nBytesDecoded >= nMaxBytesToDecode ) {
            bw->writePos = pos;
            bw->writeCurrent = current;
            bw->writeRun = run;

            return nBytesDecoded;
        }
    }

    return nBytesDecoded;
}


inline void
BZ2Reader::readBzip2Header()
{
    // Ensure that file starts with "BZh".
    for ( auto i = 0; i < 3; i++ ) {
        const char c = getBits( 8 );
        if ( c != "BZh"[i] ) {
            std::stringstream msg;
            msg << "[BZip2 Header] Input header is not BZip2 magic 'BZh'. Mismatch at bit position "
            << m_bitReader.tell() - 8 << " with " << c << " (0x" << std::hex << (int)c << ")";
            throw std::domain_error( msg.str() );
        }
    }

    // Next byte ascii '1'-'9', indicates block size in units of 100k of
    // uncompressed data. Allocate intermediate buffer for block.
    const auto i = getBits( 8 );
    if ( ( i < '1' ) || ( i > '9' ) ) {
        std::stringstream msg;
        msg << "[BZip2 Header] Blocksize must be one of '0' (" << std::hex << (int)'0' << ") ... '9' (" << (int)'9'
        << ") but is " << i << " (" << (int)i << ")" << std::dec;
        throw std::domain_error( msg.str() );
    }
    m_blockSize100k = i - '0';

    m_calculatedStreamCRC = 0;
}


inline std::array<uint32_t, BZ2Reader::CRC32_LOOKUP_TABLE_SIZE>
BZ2Reader::createCRC32LookupTable( bool littleEndian )
{
    std::array<uint32_t, CRC32_LOOKUP_TABLE_SIZE> table;
    for ( size_t i = 0; i < table.size(); ++i ) {
        uint32_t c = littleEndian ? i : i << 24;
        for ( int j = 0; j < 8; ++j ) {
            if ( littleEndian ) {
                c = ( c & 1 ) ? ( c >> 1 ) ^ 0xEDB88320 : c >> 1;
            } else {
                c = c & 0x80000000 ? ( c << 1 ) ^ 0x04c11db7 : ( c << 1 );
            }
        }
        table[i] = c;
    }
    return table;
}
