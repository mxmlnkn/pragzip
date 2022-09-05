/**
 * @file This is an alternative lookup table to check the precode histogram for validity.
 *       It crams all necessary counts into 24 bits in order to not only have a partial LUT but a complete one,
 *       to save a branch for possibly valid cases.
 *       The bits were shaved off by specially accounting for overflows when adding up partial histograms.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

#include <BitManipulation.hpp>

#include "SingleLUT.hpp"


namespace pragzip::PrecodeCheck::SingleCompressedLUT
{
using pragzip::PrecodeCheck::SingleLUT::Histogram;
using pragzip::PrecodeCheck::SingleLUT::HISTOGRAM_TO_LOOK_UP_BITS;
using pragzip::PrecodeCheck::SingleLUT::PRECODE_X4_TO_HISTOGRAM_LUT;
using pragzip::PrecodeCheck::SingleLUT::PRECODE_HISTOGRAM_VALID_LUT;
using pragzip::PrecodeCheck::SingleLUT::POWER_OF_TWO_SPECIAL_CASES;
using pragzip::PrecodeCheck::SingleLUT::VariableLengthPackedHistogram::OVERFLOW_MEMBER_OFFSET;
using pragzip::PrecodeCheck::SingleLUT::VariableLengthPackedHistogram::OVERFLOW_BITS_MASK;


constexpr auto COMPRESSED_PRECODE_HISTOGRAM_CHUNK_COUNT = 4U;
constexpr auto COMPRESSED_PRECODE_HISTOGRAM_INDEX_BITS =
    requiredBits( COMPRESSED_PRECODE_HISTOGRAM_CHUNK_COUNT * sizeof( uint64_t ) * CHAR_BIT );

static const auto COMPRESSED_PRECODE_HISTOGRAM_VALID_LUT_DICT =
    [] ()
    {
        constexpr auto CHUNK_COUNT = COMPRESSED_PRECODE_HISTOGRAM_CHUNK_COUNT;
        constexpr auto LUT_SIZE = PRECODE_HISTOGRAM_VALID_LUT.size();
        using ChunkedValues = std::array<uint64_t, CHUNK_COUNT>;
        static_assert( sizeof( ChunkedValues ) == sizeof( ChunkedValues::value_type ) * CHUNK_COUNT );

        std::map<ChunkedValues, uint16_t> valueToKey;
        std::vector<uint8_t> dictionary;

        using Address = uint8_t;
        using CompressedLUT = std::array<Address, LUT_SIZE / CHUNK_COUNT>;
        CompressedLUT compressedLUT{};

        for ( size_t i = 0; i < LUT_SIZE; i += CHUNK_COUNT ) {
            ChunkedValues chunkedValues{};
            for ( size_t j = 0; j < CHUNK_COUNT; ++j ) {
                chunkedValues[j] = PRECODE_HISTOGRAM_VALID_LUT[i + j];
            }

            const auto [match, wasInserted] = valueToKey.try_emplace( chunkedValues, valueToKey.size() );
            if ( wasInserted ) {
                for ( const auto bitMask : chunkedValues ) {
                    for ( size_t j = 0; j < sizeof( bitMask ) * CHAR_BIT; ++j ) {
                        dictionary.push_back( static_cast<uint8_t>( ( bitMask >> j ) & 1ULL ) );
                    }
                }
            }
            compressedLUT[i / CHUNK_COUNT] = match->second;
        }

        assert( valueToKey.size() < std::numeric_limits<CompressedLUT::value_type>::max() );

        return std::make_tuple( compressedLUT, dictionary );
    }();


/**
 * @note Requires 4 (precode count) + 57 (maximum precode count * 3) bits to check for validity.
 *       Get all 57 bits at once to avoid a data dependency on the precode count. Note that this is only
 *       possible assuming a 64-bit gzip footer, else, this could be a wrong transformation because it wouldn't
 *       be able to find very small deflate blocks close to the end of the file. because they trigger an EOF.
 *       Note that such very small blocks would normally be Fixed Huffman decoding anyway.
 */
[[nodiscard]] constexpr pragzip::Error
checkPrecode( const uint64_t next4Bits,
              const uint64_t next57Bits )
{
    constexpr auto PRECODE_BITS = pragzip::deflate::PRECODE_BITS;
    const auto codeLengthCount = 4 + next4Bits;
    const auto precodeBits = next57Bits & nLowestBitsSet<uint64_t>( codeLengthCount * PRECODE_BITS );

    constexpr auto PRECODES_PER_CHUNK = 4U;
    constexpr auto CACHED_BITS = PRECODE_BITS * PRECODES_PER_CHUNK;
    constexpr auto CHUNK_COUNT = ceilDiv( pragzip::deflate::MAX_PRECODE_COUNT, PRECODES_PER_CHUNK );
    static_assert( CACHED_BITS == 12 );
    static_assert( CHUNK_COUNT == 5 );

    Histogram bitLengthFrequencies{ 0 };
    Histogram overflowsInSum{ 0 };
    Histogram overflowsInLUT{ 0 };

    for ( size_t chunk = 0; chunk < CHUNK_COUNT; ++chunk ) {
        auto precodeChunk = precodeBits >> ( chunk * CACHED_BITS );
        /* The last requires no bit masking because @ref next57Bits is already sufficiently masked.
         * This branch will hopefully get unrolled, else it could hinder performance. */
        if ( chunk != CHUNK_COUNT - 1 ) {
            precodeChunk &= nLowestBitsSet<uint64_t, CACHED_BITS>();
        }

        const auto partialHistogram = PRECODE_X4_TO_HISTOGRAM_LUT[precodeChunk];

        /**
         * Account for overflows over the storage boundaries during addition.
         *  - Addition in lowest bits: 0+0 -> 0, 0+1 -> 1, 1+0 -> 1, 1+1 -> 0 (+ carry bit)
         *                             <=> bitwise xor ^ (also sometimes called carryless addition)
         *  - If there is a carry-over (overflow) from a lower bit, then these results will be inverted.
         *    We can check for that with another xor, wich also acts as a bit-wise inequality comparison,
         *    setting the resulting bit only to 1 if both source bits are different.
         *    This result needs to be masked to the bits of interest but that can be done last to reduce instructions.
         */
        const auto carrylessSum = bitLengthFrequencies ^ partialHistogram;
        bitLengthFrequencies = bitLengthFrequencies + partialHistogram;
        overflowsInSum |= carrylessSum ^ bitLengthFrequencies;
        overflowsInLUT |= partialHistogram;
    }

    /* Ignore non-zero and overflow counts for lookup. */
    const auto histogramToLookUp = ( bitLengthFrequencies >> 5 )
                                   & nLowestBitsSet<Histogram>( HISTOGRAM_TO_LOOK_UP_BITS );
    const auto nonZeroCount = bitLengthFrequencies & nLowestBitsSet<Histogram>( 5 );
    if ( UNLIKELY( POWER_OF_TWO_SPECIAL_CASES[nonZeroCount] == histogramToLookUp ) ) [[unlikely]] {
        return pragzip::Error::NONE;
    }

    if ( ( ( overflowsInSum & OVERFLOW_BITS_MASK ) != 0 )
         || ( ( overflowsInLUT & ( ~Histogram( 0 ) << OVERFLOW_MEMBER_OFFSET ) ) != 0 ) ) {
        return pragzip::Error::INVALID_CODE_LENGTHS;
    }

    const auto& [histogramLUT, validLUT] = COMPRESSED_PRECODE_HISTOGRAM_VALID_LUT_DICT;
    constexpr auto INDEX_BITS = COMPRESSED_PRECODE_HISTOGRAM_INDEX_BITS;
    const auto elementIndex = ( histogramToLookUp >> INDEX_BITS )
                              & nLowestBitsSet<Histogram>( HISTOGRAM_TO_LOOK_UP_BITS - INDEX_BITS );
    const auto validIndex = ( histogramLUT[elementIndex] << INDEX_BITS )
                            + ( histogramToLookUp & nLowestBitsSet<uint64_t>( INDEX_BITS ) );
    if ( LIKELY( ( validLUT[validIndex] ) == 0 ) ) [[unlikely]] {
        /* This also handles the case of all being zero, which in the other version returns EMPTY_ALPHABET!
         * Some might also not be bloating but simply invalid, we cannot differentiate that but it can be
         * helpful for tests to have different errors. For actual usage comparison with NONE is sufficient. */
        return pragzip::Error::BLOATING_HUFFMAN_CODING;
    }

    return pragzip::Error::NONE;
}
}  // namespace pragzip::PrecodeCheck::SingleCompressedLUT