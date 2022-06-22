#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <type_traits>

#include "HuffmanCodingBase.hpp"


namespace pragzip
{
template<typename HuffmanCode,
         uint8_t  MAX_CODE_LENGTH,
         typename Symbol,
         size_t   MAX_SYMBOL_COUNT>
class HuffmanCodingReversedCodesPerLength :
    public HuffmanCodingBase<HuffmanCode, MAX_CODE_LENGTH, Symbol, MAX_SYMBOL_COUNT>
{
public:
    using BaseType = HuffmanCodingBase<HuffmanCode, MAX_CODE_LENGTH, Symbol, MAX_SYMBOL_COUNT>;
    using BitCount = typename BaseType::BitCount;
    using CodeLengthFrequencies = typename BaseType::CodeLengthFrequencies;

    friend class Block;

protected:
    void
    initializeCodingTable( const VectorView<BitCount>&  codeLengths,
                           const CodeLengthFrequencies& bitLengthFrequencies )
    {
        /* Calculate cumulative frequency sums to be used as offsets for each code-length
         * for the code-length-sorted alphabet vector. */
        size_t sum = 0;
        for ( uint8_t bitLength = this->m_minCodeLength; bitLength <= this->m_maxCodeLength; ++bitLength ) {
            m_offsets[bitLength - this->m_minCodeLength] = sum;
            sum += bitLengthFrequencies[bitLength];
        }
        m_offsets[this->m_maxCodeLength - this->m_minCodeLength + 1] = sum;

        /* The codeLengths.size() check above should implicitly check this already. */
        assert( sum <= m_symbolsPerLength.size() && "Specified max symbol range exceeded!" );

        /* Fill the code-length-sorted alphabet vector. */
        auto sizes = m_offsets;
        auto codeValuesPerLevel = this->m_minimumCodeValuesPerLevel;
        for ( Symbol symbol = 0; symbol < codeLengths.size(); ++symbol ) {
            const auto length = codeLengths[symbol];
            if ( length != 0 ) {
                const auto k = length - this->m_minCodeLength;
                const auto code = codeValuesPerLevel[k];
                codeValuesPerLevel[k]++;

                HuffmanCode reversedCode;
                if constexpr ( sizeof( HuffmanCode ) <= sizeof( reversedBitsLUT16[0] ) ) {
                    reversedCode = reversedBitsLUT16[code];
                } else {
                    reversedCode = reverseBits( code );
                }
                reversedCode >>= ( std::numeric_limits<decltype( code )>::digits - length );

                m_symbolsPerLength[sizes[k]] = symbol;
                m_codesPerLength[sizes[k]] = reversedCode;
                sizes[k]++;
            }
        }
    }

public:
    Error
    initializeFromLengths( const VectorView<BitCount>& codeLengths )
    {
        if ( const auto errorCode = BaseType::initializeMinMaxCodeLengths( codeLengths );
             errorCode != Error::NONE )
        {
            return errorCode;
        }

        CodeLengthFrequencies bitLengthFrequencies = {};
        for ( const auto value : codeLengths ) {
            ++bitLengthFrequencies[value];
        }

        if ( const auto errorCode = BaseType::checkCodeLengthFrequencies( bitLengthFrequencies, codeLengths.size() );
             errorCode != Error::NONE ) {
            return errorCode;
        }

        BaseType::initializeMinimumCodeValues( bitLengthFrequencies );  // resets bitLengthFrequencies[0] to 0!

        initializeCodingTable( codeLengths, bitLengthFrequencies );

        return Error::NONE;
    }

    [[nodiscard]] forceinline std::optional<Symbol>
    decode( BitReader& bitReader ) const
    {
        HuffmanCode code = bitReader.read( this->m_minCodeLength );

        const auto size = m_offsets[this->m_maxCodeLength - this->m_minCodeLength + 1];
        auto relativeCodeLength = 0;  // relative to m_minCodeLength
        for ( size_t i = 0; i < size; ++i ) {
            if ( m_codesPerLength[i] == code ) {
                return m_symbolsPerLength[i];
            }

            while ( m_offsets[relativeCodeLength + 1] == i + 1 ) {
                code |= bitReader.read<1>() << ( this->m_minCodeLength + relativeCodeLength );
                relativeCodeLength++;
            }
        }

        return std::nullopt;
    }

protected:
    /**
     * Contains the alphabet sorted by code length, e.g., it could look like this:
     * @verbatim
     * +-------+-----+---+
     * | B D E | A F | C |
     * +-------+-----+---+
     *   CL=3   CL=4  CL=5
     * @endverbatim
     * The starting index for a given code length (CL) can be queried with m_offsets.
     */
    alignas(8) std::array<Symbol, MAX_SYMBOL_COUNT> m_symbolsPerLength;
    alignas(8) std::array</* reversed code */ HuffmanCode, MAX_SYMBOL_COUNT> m_codesPerLength;

    /* +1 because it stores the size at the end as well as 0 in the first element, which might be redundant but fast.
     * Stores m_maxCodeLength - m_minCodeLength offsets +1 size. The array is oversized because else we would have
     * to use a std::vector with costly heap-allocations. */
    alignas(8) std::array<uint16_t, MAX_CODE_LENGTH + 1> m_offsets;
    static_assert( MAX_SYMBOL_COUNT + MAX_CODE_LENGTH <= std::numeric_limits<uint16_t>::max(),
                   "Offset type must be able to point at all symbols!" );
};
}  // namespace pragzip
