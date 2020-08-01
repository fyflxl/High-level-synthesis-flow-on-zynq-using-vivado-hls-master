/* 
 * File:   inflate.cpp
 * Author: Yuxuan (Eric) Zheng
 *
 * Created on July 8, 2016, 10:52 AM
 */

#include "deflate.h"

/*
 * Inflate core decompresses a hls_stream input.
 */

// Top level module for decompression
void inflate(hls::stream<uint32_t> &input, hls::stream<uint32_t> &output)
{

    uint8_t huffman_decoding_output[3000];

    // The two decoders can be combined together to make the max input size to
    // unlimited. But separating them makes debugging easier.
    huffman_decoder(input, huffman_decoding_output);

    LZ77_decoder(huffman_decoding_output, output);

    return;
}

/*
 * See the comment in deflate.cpp for the endianness clarification.
 */

void huffman_decoder(hls::stream<uint32_t> &input, uint8_t decoding_output[3000])
{

    int output_pos = 0;

    uint32_t proc_buffer; // main buffer storing the data to be processed
    int buffer_bits_num;  // the number of bits in the current buffer
    uint32_t next_word;   // the next word to be stored into the buffer, used for reading from hls_stream
    int next_word_bits;   // the number of valid bits in the next word buffer

    unsigned proc_bits_num; // the number of bits were processed in this iteration

    bool done_decoding = false;
    bool done_input = false;

    uint8_t copy_8_bits;
    uint16_t copy_16_bits;
    uint3_t block_header;

    unsigned length; // the length after decoding
    unsigned offset; // the offset corresponding to the previous length;

    // change input to little-endian
    input.read(next_word);
    changeToLittleEndian(next_word);

    proc_buffer = next_word;
    block_header = (proc_buffer & 0xE0000000) >> 29;

    input.read(next_word);
    changeToLittleEndian(next_word);
    next_word_bits = 32;

    if (block_header >= 0 && block_header <= 2)
    {
        // not the last block
        // currently, not consider this situation
        // can modify code easily to achieve this functionality
    }
    else if (block_header == 4)
    {
        // last block, no compression
        // currently, not consider this situation
        // just copy the literals directly (following the standard)
    }
    else if (block_header == 6)
    {
        // last block, static Huffman encoding

        proc_buffer <<= 3;
        buffer_bits_num = 29;

    STATIC_MAIN_LOOP:
        while (buffer_bits_num > 0 && !done_decoding)
        {
            // the processing buffer is not empty, still need to decode

        STATIC_INPUT:
            while (buffer_bits_num < 24 && !done_input)
            {
                // shift input into the processing buffer, if the buffer is too short
                // use a loop, not if statement. otherwise, the input process would be wrong!
                // Well, quite complex logic

                proc_buffer |= (next_word >> buffer_bits_num);

                if ((32 - buffer_bits_num) <= next_word_bits)
                {
                    // fill the entire buffer
                    next_word <<= (32 - buffer_bits_num);
                    next_word_bits -= (32 - buffer_bits_num);
                    buffer_bits_num = 32;
                }
                else
                {
                    // not fill the entire buffer, read the input stream
                    buffer_bits_num += next_word_bits;
                    next_word_bits = 0;
                }

                if (next_word_bits == 0)
                {
                    if (!input.empty())
                    {
                        input.read(next_word);
                        changeToLittleEndian(next_word);
                        next_word_bits = 32;
                    }
                    else
                    {
                        // the input stream is empty
                        done_input = true;
                    }
                }
            }

            // try to match the first 8/9 bits of buffer to the literal table
            copy_8_bits = (proc_buffer & 0xFF000000) >> 24;
            if (copy_8_bits >= 0x30 && copy_8_bits <= 0x0BF)
            {
                // edoc: 0-143, literals
                decoding_output[output_pos++] = copy_8_bits - 0x30;
                proc_bits_num = 8;
            }
            else if (copy_8_bits >= 0x0C0 && copy_8_bits <= 0x0C7)
            {
                // edoc: 280-287
                // Currently impossible go into this position
                cout << "Wrong! Length is " << copy_8_bits << endl;
            }
            else
            {
                copy_8_bits = (proc_buffer >> 25) & 0x7F;
                if (copy_8_bits == 0x0)
                {
                    // reach the last edoc (256), stop decoding
                    proc_bits_num = 7;
                    done_decoding = true;
                }
                else if (copy_8_bits > 0x0 && copy_8_bits <= 0x17)
                {
                    // edoc: 257-279, length

                    // calculate the length given the code
                    if (copy_8_bits >= 0x1 && copy_8_bits <= 0x08)
                    {
                        // 257-264
                        length = copy_8_bits + 2;
                        proc_bits_num = 7;
                    }
                    else if (copy_8_bits >= 0x09 && copy_8_bits <= 0x0C)
                    {
                        // 265-268
                        length = copy_8_bits * 2 - 7 + ((proc_buffer & 0x01000000) >> 24);
                        proc_bits_num = 8;
                    }
                    else if (copy_8_bits >= 0x0D && copy_8_bits <= 0x10)
                    {
                        // 269-272
                        length = copy_8_bits * 4 - 33 + ((proc_buffer & 0x01800000) >> 23);
                        proc_bits_num = 9;
                    }
                    else if (copy_8_bits >= 0x11 && copy_8_bits <= 0x14)
                    {
                        // 273-276
                        length = copy_8_bits * 8 - 101 + ((proc_buffer & 0x01C00000) >> 22);
                        proc_bits_num = 10;
                    }
                    else
                    {
                        // 277-279
                        // Currently impossible to enter here
                        cout << "Wrong! Length is " << copy_8_bits << endl;
                    }

                    // get the corresponding offset
                    offset = decoder_get_offset(proc_bits_num, proc_buffer);

                    // write the results to decoding output
                    decoding_output[output_pos] = '@';
                    decoding_output[output_pos + 1] = offset >> 7;
                    decoding_output[output_pos + 2] = (offset & 0x07F);
                    decoding_output[output_pos + 3] = length;
                    output_pos += 4;
                }
                else
                {
                    copy_16_bits = (proc_buffer & 0xFF800000) >> 23;
                    if (copy_16_bits >= 0x190 && copy_16_bits <= 0x1FF)
                    {
                        // edoc: 144-255, special literals
                        decoding_output[output_pos++] = copy_16_bits - 0x190 + 144;
                        proc_bits_num = 9;
                    }
                    else
                    {
                        cout << "Wrong! Cannot decode!" << endl;
                    }
                }
            }

            // modify the proc_buffer
            proc_buffer <<= proc_bits_num;
            buffer_bits_num -= proc_bits_num;
            proc_bits_num = 0;
        }
    }
    else if (block_header == 5)
    {
        // last block, dynamic Huffman encoding

        uint5_t HLIT, HDIST;
        uint4_t HCLEN;
        uint3_t CCL[19];
#pragma HLS ARRAY_PARTITION variable = CCL complete dim = 1

        code_table_node hTable1[286];
        // Huffman Table 1 for literals and lengths
        code_table_node hTable2[30];
        // Huffman Table 2 for distances
        CCL_code hTable3[19];
        // Huffman Table 3

        Lookup_Node lookup_table_CCL[128];
        // 7 bits table for CL1/2 decoding -> should be 0~127
        Lookup_Node lookup_table_LIT_1[512];
        // 9 bits table for first level lookup of LIT
        Lookup_Node lookup_table_DIST_1[64];
        // 6 bits table for first level lookup of DIST

        HLIT = (proc_buffer & 0x1F000000) >> 24;
        HDIST = (proc_buffer & 0x00F80000) >> 19;
        HCLEN = (proc_buffer & 0x00078000) >> 15;

        // Add Little-Endian Modification Here - swap bits in HLIT, HDIST, HCLEN
        HLIT = reverse(HLIT, 5);
        HDIST = reverse(HDIST, 5);
        HCLEN = reverse(HCLEN, 4);

        // Get CCL codes
        proc_buffer <<= 17;
        buffer_bits_num = 15;
        int CCL_index = 0;
    GET_CCL:
        for (; CCL_index < (HCLEN + 4); CCL_index++)
        {
#pragma HLS PIPELINE

            if (buffer_bits_num == 2)
            {
                // currently, got 15 CCLs, trying to get the 16th CCL, but no enough bits in buffer
                proc_buffer |= (next_word >> 2);
                buffer_bits_num = 32;
                next_word <<= 30;
                next_word_bits = 2;
            }

            CCL[CCL_index] = (proc_buffer & 0xE0000000) >> 29;

            // Add Little-Endian Modification Here - swap each CCL code
            CCL[CCL_index] = reverse(CCL[CCL_index], 3);

            proc_buffer <<= 3;
            buffer_bits_num -= 3;

            if (buffer_bits_num == 0)
            {
                proc_buffer = next_word;
                buffer_bits_num = 32;
                input.read(next_word);
                changeToLittleEndian(next_word);
            }
        }
    FILL_REMAINING_CCL:
        for (; CCL_index < 19; CCL_index++)
        {
#pragma HLS UNROLL
            // fill in remaining CCL array
            CCL[CCL_index] = 0;
        }
        permute_CCL(CCL, hTable3);    // permute the CCL code
        get_huffman_table_3(hTable3); // generate the Huffman table 3

        // Build the lookup table for Huffman table 3
    BUILD_LOOKUP_TABLE_3:
        for (int i = 0; i < 19; i++)
        {
#pragma HLS UNROLL
            // for each CCL
            if (hTable3[i].length != 0)
            {
                unsigned len = hTable3[i].length;
                unsigned start_pos = hTable3[i].code << (7 - len);
                unsigned repeat_times = (1 << (7 - len));

            BUILD_LOOKUP_3_INNER:
                for (int j = 0; j < repeat_times; j++)
                {
#pragma HLS UNROLL
                    lookup_table_CCL[start_pos + j].symbol = i;
                    lookup_table_CCL[start_pos + j].valid_bits = len;
                }
            }
        }

        // Decode the CL1 sequence
        unsigned CL1_count = 0;
        unsigned CL1_num = HLIT + 257;
        uint7_t copy_7_bits;

    DECODE_CL1:
        while (CL1_count < CL1_num)
        { // still need to decode the CL1 sequence
#pragma HLS PIPELINE

        DECODE_CL1_INPUT:
            while (buffer_bits_num < 16)
            {
                // shift input into the processing buffer, if the buffer is too short
                // use a loop, not if statement. otherwise, the input process would be wrong!

                proc_buffer |= (next_word >> buffer_bits_num);

                if ((32 - buffer_bits_num) <= next_word_bits)
                {
                    // fill the entire buffer
                    next_word <<= (32 - buffer_bits_num);
                    next_word_bits -= (32 - buffer_bits_num);
                    buffer_bits_num = 32;
                }
                else
                {
                    // not fill the entire buffer, read the input stream
                    buffer_bits_num += next_word_bits;
                    next_word_bits = 0;
                }

                if (next_word_bits == 0)
                {
                    if (!input.empty())
                    {
                        input.read(next_word);
                        changeToLittleEndian(next_word);
                        next_word_bits = 32;
                    }
                    else
                    {
                        // the input stream is empty
                        // normally, impossible to reach here
                        cout << "Wrong! Input stream should not be empty here."
                             << endl;
                    }
                }
            }

            copy_7_bits = (proc_buffer & 0xFE000000) >> 25;
            uint9_t symbol = lookup_table_CCL[copy_7_bits].symbol;
            unsigned symbol_valid_bits = lookup_table_CCL[copy_7_bits].valid_bits;
            if (symbol == 16)
            {
                // CCL = 16
                uint2_t extra_2_bits = (proc_buffer >> (30 - symbol_valid_bits)) & 0x03;

                // Little-Endian Modification Here - swap the 2 bits
                extra_2_bits = reverse(extra_2_bits, 2);

                uint8_t repeat_count = extra_2_bits + 3;
                unsigned repeated_length = hTable1[CL1_count - 1].valid_length;

            FILL_HTABLE1_SYMBOL16:
                for (int i = 0; i < repeat_count; i++)
                {
#pragma HLS UNROLL
                    hTable1[CL1_count + i].valid_length = repeated_length;
                }
                CL1_count += repeat_count;

                proc_buffer <<= (symbol_valid_bits + 2);
                buffer_bits_num -= (symbol_valid_bits + 2);
            }
            else if (symbol == 17)
            {
                // CCL = 17
                uint3_t extra_3_bits = (proc_buffer >> (29 - symbol_valid_bits)) & 0x07;

                // Little-Endian Modification Here - swap the 3 bits
                extra_3_bits = reverse(extra_3_bits, 3);

                uint8_t repeat_count = extra_3_bits + 3;

            FILL_HTABLE1_SYMBOL17:
                for (int i = 0; i < repeat_count; i++)
                {
#pragma HLS UNROLL
                    hTable1[CL1_count + i].valid_length = 0;
                }
                CL1_count += repeat_count;

                proc_buffer <<= (symbol_valid_bits + 3);
                buffer_bits_num -= (symbol_valid_bits + 3);
            }
            else if (symbol == 18)
            {
                // CCL = 18
                uint7_t extra_7_bits = (proc_buffer >> (25 - symbol_valid_bits)) & 0x07F;

                // Little-Endian Modification Here - swap the 7 bits
                extra_7_bits = reverse(extra_7_bits, 7);

                uint8_t repeat_count = extra_7_bits + 11;

            FILL_HTABLE1_SYMBOL18:
                for (int i = 0; i < repeat_count; i++)
                {
#pragma HLS UNROLL
                    hTable1[CL1_count + i].valid_length = 0;
                }
                CL1_count += repeat_count;

                proc_buffer <<= (symbol_valid_bits + 7);
                buffer_bits_num -= (symbol_valid_bits + 7);
            }
            else
            {
                // CCL from 0 to 15
                hTable1[CL1_count].valid_length = symbol;
                CL1_count++;

                proc_buffer <<= symbol_valid_bits;
                buffer_bits_num -= symbol_valid_bits;
            }
        }

    FILL_HTABLE1_REMAINING:
        while (CL1_count < 286)
        {
#pragma HLS UNROLL
            hTable1[CL1_count++].valid_length = 0;
        }

        // Generate Huffman Table 1
        get_huffman_table_1(hTable1);

        // Decode the CL2 sequence
        unsigned CL2_count = 0;
        unsigned CL2_num = HDIST + 1;

    DECODE_CL2:
        while (CL2_count < CL2_num)
        {
#pragma HLS PIPELINE
            // still need to decode the CL2 sequence

        DECODE_CL2_INPUT:
            while (buffer_bits_num < 16)
            {
                // shift input into the processing buffer, if the buffer is too short
                // use a loop, not if statement. otherwise, the input process would be wrong!

                proc_buffer |= (next_word >> buffer_bits_num);

                if ((32 - buffer_bits_num) <= next_word_bits)
                {
                    // fill the entire buffer
                    next_word <<= (32 - buffer_bits_num);
                    next_word_bits -= (32 - buffer_bits_num);
                    buffer_bits_num = 32;
                }
                else
                {
                    // not fill the entire buffer, read the input stream
                    buffer_bits_num += next_word_bits;
                    next_word_bits = 0;
                }

                if (next_word_bits == 0)
                {
                    if (!input.empty())
                    {
                        input.read(next_word);
                        changeToLittleEndian(next_word);
                        next_word_bits = 32;
                    }
                    else
                    {
                        // the input stream is empty
                        // normally, impossible to reach here
                        cout << "Wrong! Input stream should not be empty here."
                             << endl;
                    }
                }
            }

            copy_7_bits = (proc_buffer & 0xFE000000) >> 25;
            uint9_t symbol = lookup_table_CCL[copy_7_bits].symbol;
            unsigned symbol_valid_bits = lookup_table_CCL[copy_7_bits].valid_bits;
            if (symbol == 16)
            {
                // CCL = 16
                uint2_t extra_2_bits = (proc_buffer >> (30 - symbol_valid_bits)) & 0x03;

                // Add Little-Endian Modification Here - swap the 2 bits
                extra_2_bits = reverse(extra_2_bits, 2);

                uint8_t repeat_count = extra_2_bits + 3;
                unsigned repeated_length = hTable2[CL2_count - 1].valid_length;

            FILL_HTABLE2_SYMBOL16:
                for (int i = 0; i < repeat_count; i++)
                {
#pragma HLS UNROLL
                    hTable2[CL2_count + i].valid_length = repeated_length;
                }
                CL2_count += repeat_count;

                proc_buffer <<= (symbol_valid_bits + 2);
                buffer_bits_num -= (symbol_valid_bits + 2);
            }
            else if (symbol == 17)
            {
                // CCL = 17
                uint3_t extra_3_bits = (proc_buffer >> (29 - symbol_valid_bits)) & 0x07;

                // Little-Endian Modification Here - swap the 3 bits
                extra_3_bits = reverse(extra_3_bits, 3);

                uint8_t repeat_count = extra_3_bits + 3;

            FILL_HTABLE2_SYMBOL17:
                for (int i = 0; i < repeat_count; i++)
                {
#pragma HLS UNROLL
                    hTable2[CL2_count + i].valid_length = 0;
                }
                CL2_count += repeat_count;

                proc_buffer <<= (symbol_valid_bits + 3);
                buffer_bits_num -= (symbol_valid_bits + 3);
            }
            else if (symbol == 18)
            {
                // CCL = 18
                uint7_t extra_7_bits = (proc_buffer >> (25 - symbol_valid_bits)) & 0x07F;

                // Little-Endian Modification Here - swap the 7 bits
                extra_7_bits = reverse(extra_7_bits, 7);

                uint8_t repeat_count = extra_7_bits + 11;

            FILL_HTABLE2_SYMBOL18:
                for (int i = 0; i < repeat_count; i++)
                {
#pragma HLS UNROLL
                    hTable2[CL2_count + i].valid_length = 0;
                }
                CL2_count += repeat_count;

                proc_buffer <<= (symbol_valid_bits + 7);
                buffer_bits_num -= (symbol_valid_bits + 7);
            }
            else
            {
                // CCL from 0 to 15
                hTable2[CL2_count].valid_length = symbol;
                CL2_count++;

                proc_buffer <<= symbol_valid_bits;
                buffer_bits_num -= symbol_valid_bits;
            }
        }

    FILL_HTABLE2_REMAINING:
        while (CL2_count < 30)
        {
#pragma HLS UNROLL
            hTable2[CL2_count++].valid_length = 0;
        }

        // Generate Huffman Table 2
        get_huffman_table_2(hTable2);

        // Build the lookup table for Huffman Table 1 & 2
    BUILD_LOOKUP_TABLE_1:
        for (int i = 0; i < 286; i++)
        {
#pragma HLS UNROLL
            // for each edoc in lit/length table
            if (hTable1[i].valid_length != 0)
            {
                unsigned len = hTable1[i].valid_length;

                if (len <= 9)
                {
                    // can be searched in the first level lookup
                    unsigned start_pos = hTable1[i].code << (9 - len);
                    unsigned repeat_times = (1 << (9 - len));

                BUILD_LOOKUP_1_INNER:
                    for (int j = 0; j < repeat_times; j++)
                    {
#pragma HLS UNROLL
                        lookup_table_LIT_1[start_pos + j].symbol = i; // assign the edoc to the symbol
                        lookup_table_LIT_1[start_pos + j].valid_bits = len;
                    }
                }
                else
                {
                    // need to use second level searching
                    // ...
                }
            }
        }

    BUILD_LOOKUP_TABLE_2:
        for (int i = 0; i < 30; i++)
        {
#pragma HLS UNROLL
            // for each edoc in distance table
            if (hTable2[i].valid_length != 0)
            {
                unsigned len = hTable2[i].valid_length;

                if (len <= 6)
                {
                    // can be searched in the first level lookup
                    unsigned start_pos = hTable2[i].code << (6 - len);
                    unsigned repeat_times = (1 << (6 - len));

                BUILD_LOOKUP_2_INNER:
                    for (int j = 0; j < repeat_times; j++)
                    {
#pragma HLS UNROLL
                        lookup_table_DIST_1[start_pos + j].symbol = i; // assign the edoc to the symbol
                        lookup_table_DIST_1[start_pos + j].valid_bits = len;
                    }
                }
                else
                {
                    // need to use second level searching
                    // ...
                }
            }
        }

        // Finally, decode the remaining LIT and DIST stream (Real compressed data)
    DYNAMIC_MAIN_LOOP:
        while (buffer_bits_num > 0 && !done_decoding)
        {
#pragma HLS PIPELINE
            // the processing buffer is not empty - still need to decode

        DYNAMIC_INPUT:
            while (buffer_bits_num < 24 && !done_input)
            {
#pragma HLS PIPELINE
                // shift input into the processing buffer, if the buffer is too short

                proc_buffer |= (next_word >> buffer_bits_num);

                if ((32 - buffer_bits_num) <= next_word_bits)
                {
                    // fill the entire buffer
                    next_word <<= (32 - buffer_bits_num);
                    next_word_bits -= (32 - buffer_bits_num);
                    buffer_bits_num = 32;
                }
                else
                {
                    // not fill the entire buffer, read the input stream
                    buffer_bits_num += next_word_bits;
                    next_word_bits = 0;
                }

                if (next_word_bits == 0)
                {
                    if (!input.empty())
                    {
                        input.read(next_word);
                        changeToLittleEndian(next_word);
                        next_word_bits = 32;
                    }
                    else
                    {
                        // the input stream is empty
                        done_input = true;
                    }
                }
            }

            uint9_t copy_9_bits = (proc_buffer & 0xFF800000) >> 23;
            uint9_t edoc = lookup_table_LIT_1[copy_9_bits].symbol; // not consider second level lookup
            unsigned edoc_valid_bits = lookup_table_LIT_1[copy_9_bits].valid_bits;

            if (edoc >= 0 && edoc <= 255)
            {
                // a literal, copy it to the output
                decoding_output[output_pos++] = edoc;
                proc_bits_num = edoc_valid_bits;
            }
            else if (edoc == 256)
            {
                // reach the last edoc, stop decoding
                proc_bits_num = edoc_valid_bits;
                done_decoding = true;
            }
            else
            {
                // meet a length

                // Note: need to add modifications for every extra bits below
                if (edoc >= 257 && edoc <= 264)
                {
                    // 257-264
                    length = edoc - 254;
                    proc_bits_num = edoc_valid_bits;
                }
                else if (edoc >= 265 && edoc <= 268)
                {
                    // 265-268
                    length = (11 + 2 * (edoc - 265)) + ((proc_buffer >> (31 - edoc_valid_bits)) & 0x01);
                    proc_bits_num = edoc_valid_bits + 1;
                }
                else if (edoc >= 269 && edoc <= 272)
                {
                    // 269-272
                    length = (19 + 4 * (edoc - 269)) + reverse(((proc_buffer >> (30 - edoc_valid_bits)) & 0x03), 2);
                    proc_bits_num = edoc_valid_bits + 2;
                }
                else if (edoc >= 273 && edoc <= 276)
                {
                    // 273-276
                    length = (35 + 8 * (edoc - 273)) + reverse(((proc_buffer >> (29 - edoc_valid_bits)) & 0x07), 3);
                    proc_bits_num = edoc_valid_bits + 3;
                }
                else if (edoc >= 277 && edoc <= 280)
                {
                    // 277-280
                    length = (67 + 16 * (edoc - 277)) + reverse(((proc_buffer >> (28 - edoc_valid_bits)) & 0x0F), 4);
                    proc_bits_num = edoc_valid_bits + 4;
                }
                else if (edoc >= 281 && edoc <= 284)
                {
                    // 281-284
                    length = (131 + 32 * (edoc - 281)) + reverse(((proc_buffer >> (27 - edoc_valid_bits)) & 0x1F), 5);
                    proc_bits_num = edoc_valid_bits + 5;
                }
                else
                {
                    // 285
                    length = 258;
                    proc_bits_num = edoc_valid_bits;
                }

                // get the corresponding offset
                offset = dynamic_decoder_get_offset(proc_bits_num, proc_buffer, lookup_table_DIST_1);

                // write the results to decoding output
                decoding_output[output_pos] = '@';
                decoding_output[output_pos + 1] = offset >> 7;
                decoding_output[output_pos + 2] = (offset & 0x07F);
                decoding_output[output_pos + 3] = length;
                output_pos += 4;
            }

            // modify the proc_buffer
            proc_buffer <<= proc_bits_num;
            buffer_bits_num -= proc_bits_num;
            proc_bits_num = 0;
        }
    }
    else
    {
        // illegal header code - wrong
        cout << "illegal code" << endl;
    }

    // finish the output array
    decoding_output[output_pos] = '\0';

    //    // print out the decoding output - for testing
    //    int i = 0;
    //    unsigned offset_print, length_print;
    //    cout << endl << "The stream after Huffman decoding: " << endl << endl;
    //    while (decoding_output[i] != '\0') {
    //        if (decoding_output[i] == '@') {
    //            offset_print = (decoding_output[i + 1] * 128)
    //                    + decoding_output[i + 2];
    //            length_print = decoding_output[i + 3];
    //            cout << "@(" << offset_print << "," << length_print << ")";
    //            i += 4;
    //        } else {
    //            cout << decoding_output[i++];
    //        }
    //    }
    //
    //    cout << endl;

    return;
}

// Function to reverse bits given an unsigned number with any bits
template <typename T>
T reverse(T n, unsigned bits_num)
{
    T rv = 0;
REVERSE_BITS:
    for (int i = 0; i < bits_num; i++)
    {
#pragma HLS PIPELINE
        rv <<= 1;
        rv |= (n & 0x01);
        n >>= 1;
    }
    return rv;
}

// Function to modify the input word to little-endian
void changeToLittleEndian(uint32_t &next_word)
{
    uint8_t byte1, byte2, byte3, byte4;

    byte1 = reverse(((next_word & 0xFF000000) >> 24), 8);
    byte2 = reverse(((next_word & 0x00FF0000) >> 16), 8);
    byte3 = reverse(((next_word & 0x0000FF00) >> 8), 8);
    byte4 = reverse((next_word & 0x000000FF), 8);

    next_word = (byte1 << 24) | (byte2 << 16) | (byte3 << 8) | byte4;

    return;
}

unsigned dynamic_decoder_get_offset(unsigned &proc_bits_num, uint32_t proc_buffer,
                                    Lookup_Node lookup_table_DIST_1[128])
{

    unsigned offset;
    uint6_t copy_6_bits = (proc_buffer >> (26 - proc_bits_num)) & 0x0000003F;

    uint9_t edoc = lookup_table_DIST_1[copy_6_bits].symbol; // not consider second level lookup
    unsigned edoc_valid_bits = lookup_table_DIST_1[copy_6_bits].valid_bits;
    proc_bits_num += edoc_valid_bits;

    // Add Little-Endian Modification Here - should swap the extra bits

    if (edoc >= 0 && edoc <= 3)
    {
        offset = edoc + 1;
    }
    else if (edoc == 4 || edoc == 5)
    {
        offset = edoc * 2 - 3 + ((proc_buffer >> (31 - proc_bits_num)) & 0x1);
        proc_bits_num += 1;
    }
    else if (edoc == 6 || edoc == 7)
    {
        offset = edoc * 4 - 15 + reverse(((proc_buffer >> (30 - proc_bits_num)) & 0x3), 2);
        proc_bits_num += 2;
    }
    else if (edoc == 8 || edoc == 9)
    {
        offset = edoc * 8 - 47 + reverse(((proc_buffer >> (29 - proc_bits_num)) & 0x7), 3);
        proc_bits_num += 3;
    }
    else if (edoc == 10 || edoc == 11)
    {
        offset = edoc * 16 - 127 + reverse(((proc_buffer >> (28 - proc_bits_num)) & 0x0F), 4);
        proc_bits_num += 4;
    }
    else if (edoc == 12 || edoc == 13)
    {
        offset = edoc * 32 - 319 + reverse(((proc_buffer >> (27 - proc_bits_num)) & 0x1F), 5);
        proc_bits_num += 5;
    }
    else if (edoc == 14 || edoc == 15)
    {
        offset = edoc * 64 - 767 + reverse(((proc_buffer >> (26 - proc_bits_num)) & 0x3F), 6);
        proc_bits_num += 6;
    }
    else if (edoc == 16 || edoc == 17)
    {
        offset = edoc * 128 - 1791 + reverse(((proc_buffer >> (25 - proc_bits_num)) & 0x7F), 7);
        proc_bits_num += 7;
    }
    else if (edoc == 18 || edoc == 19)
    {
        offset = edoc * 256 - 4095 + reverse(((proc_buffer >> (24 - proc_bits_num)) & 0x0FF), 8);
        proc_bits_num += 8;
    }
    else if (edoc == 20 || edoc == 21)
    {
        offset = edoc * 512 - 9215 + reverse(((proc_buffer >> (23 - proc_bits_num)) & 0x1FF), 9);
        proc_bits_num += 9;
    }
    else if (edoc == 22 || edoc == 23)
    {
        offset = edoc * 1024 - 20479 + reverse(((proc_buffer >> (22 - proc_bits_num)) & 0x3FF), 10);
        proc_bits_num += 10;
    }
    else if (edoc == 24 || edoc == 25)
    {
        offset = 4097 + 2048 * (edoc - 24) + reverse(((proc_buffer >> (21 - proc_bits_num)) & 0x7FF), 11);
        proc_bits_num += 11;
    }
    else if (edoc == 26 || edoc == 27)
    {
        offset = 8193 + 4096 * (edoc - 26) + reverse(((proc_buffer >> (20 - proc_bits_num)) & 0x0FFF), 12);
        proc_bits_num += 12;
    }
    else if (edoc == 28 || edoc == 29)
    {
        offset = 16385 + 8192 * (edoc - 28) + reverse(((proc_buffer >> (19 - proc_bits_num)) & 0x1FFF), 13);
        proc_bits_num += 13;
    }
    else
    {
        cout << "Wrong! Not the right offset range!" << endl;
    }

    return offset;
}

// Function to change the position of each CCL code, based on the standard
void permute_CCL(uint3_t CCL[19], CCL_code hTable3[19])
{

    hTable3[0].length = CCL[3];
    hTable3[1].length = CCL[17];
    hTable3[2].length = CCL[15];
    hTable3[3].length = CCL[13];
    hTable3[4].length = CCL[11];
    hTable3[5].length = CCL[9];
    hTable3[6].length = CCL[7];
    hTable3[7].length = CCL[5];
    hTable3[8].length = CCL[4];
    hTable3[9].length = CCL[6];
    hTable3[10].length = CCL[8];
    hTable3[11].length = CCL[10];
    hTable3[12].length = CCL[12];
    hTable3[13].length = CCL[14];
    hTable3[14].length = CCL[16];
    hTable3[15].length = CCL[18];
    hTable3[16].length = CCL[0];
    hTable3[17].length = CCL[1];
    hTable3[18].length = CCL[2];

    return;
}

void get_huffman_table_1(code_table_node hTable1[286])
{

    unsigned bl_count[16] = {0};
    // the count of each length of code
    unsigned code = 0;
    unsigned next_code[16];
    unsigned len;

    // Count the number of codes for each code length (level)
    for (int i = 0; i < 286; i++)
    {
#pragma HLS PIPELINE
        bl_count[hTable1[i].valid_length]++;
    }

    // Assign a base value to each code length
    bl_count[0] = 0;
    for (int bits = 1; bits < 16; bits++)
    {
#pragma HLS PIPELINE
        code = (code + bl_count[bits - 1]) << 1;
        next_code[bits] = code;
    }

    // Use the base value of each length to assign consecutive numerical values
    for (int n = 0; n < 286; n++)
    {
#pragma HLS PIPELINE
        len = hTable1[n].valid_length;
        if (len != 0)
        {
            hTable1[n].code = next_code[len];
            next_code[len]++;
        }
    }

    return;
}

void get_huffman_table_2(code_table_node hTable2[30])
{

    unsigned bl_count[16] = {0};
    // the count of each length of code
    unsigned code = 0;
    unsigned next_code[16];
    unsigned len;

    // Count the number of codes for each code length (level)
    for (int i = 0; i < 30; i++)
    {
#pragma HLS PIPELINE
        bl_count[hTable2[i].valid_length]++;
    }

    // Assign a base value to each code length
    bl_count[0] = 0;
    for (int bits = 1; bits < 16; bits++)
    {
#pragma HLS PIPELINE
        code = (code + bl_count[bits - 1]) << 1;
        next_code[bits] = code;
    }

    // Use the base value of each length to assign consecutive numerical values
    for (int n = 0; n < 30; n++)
    {
#pragma HLS PIPELINE
        len = hTable2[n].valid_length;
        if (len != 0)
        {
            hTable2[n].code = next_code[len];
            next_code[len]++;
        }
    }

    return;
}

void get_huffman_table_3(CCL_code hTable3[19])
{

    unsigned bl_count[8] = {0};
    // the count of each length of code
    unsigned code = 0;
    unsigned next_code[8];
    unsigned len;

    // Count the number of codes for each code length
    for (int i = 0; i < 19; i++)
    {
#pragma HLS PIPELINE
        bl_count[hTable3[i].length]++;
    }

    // Assign a base value to each code length
    bl_count[0] = 0;
    for (int bits = 1; bits < 8; bits++)
    {
#pragma HLS PIPELINE
        code = (code + bl_count[bits - 1]) << 1;
        next_code[bits] = code;
    }

    // Use the base value of each length to assign consecutive numerical values
    for (int n = 0; n < 19; n++)
    {
#pragma HLS PIPELINE
        len = hTable3[n].length;
        if (len != 0)
        {
            hTable3[n].code = next_code[len];
            next_code[len]++;
        }
    }

    return;
}

unsigned decoder_get_offset(unsigned &proc_bits_num, uint32_t proc_buffer)
{

    unsigned offset;
    uint8_t offset_5_bits = (proc_buffer >> (27 - proc_bits_num)) & 0x0000001F;

    if (offset_5_bits >= 0 && offset_5_bits <= 3)
    {
        offset = offset_5_bits + 1;
        proc_bits_num += 5;
    }
    else if (offset_5_bits == 4 || offset_5_bits == 5)
    {
        offset = offset_5_bits * 2 - 3 + ((proc_buffer >> (26 - proc_bits_num)) & 0x1);
        proc_bits_num += 6;
    }
    else if (offset_5_bits == 6 || offset_5_bits == 7)
    {
        offset = offset_5_bits * 4 - 15 + ((proc_buffer >> (25 - proc_bits_num)) & 0x3);
        proc_bits_num += 7;
    }
    else if (offset_5_bits == 8 || offset_5_bits == 9)
    {
        offset = offset_5_bits * 8 - 47 + ((proc_buffer >> (24 - proc_bits_num)) & 0x7);
        proc_bits_num += 8;
    }
    else if (offset_5_bits == 10 || offset_5_bits == 11)
    {
        offset = offset_5_bits * 16 - 127 + ((proc_buffer >> (23 - proc_bits_num)) & 0x0F);
        proc_bits_num += 9;
    }
    else if (offset_5_bits == 12 || offset_5_bits == 13)
    {
        offset = offset_5_bits * 32 - 319 + ((proc_buffer >> (22 - proc_bits_num)) & 0x1F);
        proc_bits_num += 10;
    }
    else if (offset_5_bits == 14 || offset_5_bits == 15)
    {
        offset = offset_5_bits * 64 - 767 + ((proc_buffer >> (21 - proc_bits_num)) & 0x3F);
        proc_bits_num += 11;
    }
    else if (offset_5_bits == 16 || offset_5_bits == 17)
    {
        offset = offset_5_bits * 128 - 1791 + ((proc_buffer >> (20 - proc_bits_num)) & 0x7F);
        proc_bits_num += 12;
    }
    else if (offset_5_bits == 18 || offset_5_bits == 19)
    {
        offset = offset_5_bits * 256 - 4095 + ((proc_buffer >> (19 - proc_bits_num)) & 0x0FF);
        proc_bits_num += 13;
    }
    else if (offset_5_bits == 20 || offset_5_bits == 21)
    {
        offset = offset_5_bits * 512 - 9215 + ((proc_buffer >> (18 - proc_bits_num)) & 0x1FF);
        proc_bits_num += 14;
    }
    else if (offset_5_bits == 22 || offset_5_bits == 23)
    {
        offset = offset_5_bits * 1024 - 20479 + ((proc_buffer >> (17 - proc_bits_num)) & 0x3FF);
        proc_bits_num += 15;
    }
    else
    {
        // Currently not possible to enter here (4097-32768)
        cout << "Wrong! Offset decoding is wrong!" << endl;
        cout << "Enter infinite loop to pause" << endl;
    }

    return offset;
}

void LZ77_decoder(uint8_t input[3000], hls::stream<uint32_t> &output)
{

    int curr = 0;
    int output_pos = 0;
    int matching_start_pos = 0;
    int offset = 0;
    uint8_t output_array[3000];
    uint32_t output_word;

LZ77_MAIN_LOOP:
    while (input[curr] != '\0')
    {
#pragma HLS PIPELINE
        // Meet the compressed sequence
        if (input[curr] == '@')
        {
            offset = (input[curr + 1] * 128) + input[curr + 2];
            matching_start_pos = output_pos - offset;

        COPY_MATCHED_CHAR:
            for (int i = 0; i < (input[curr + 3]); i++)
            {
#pragma HLS PIPELINE
                output_array[output_pos++] = output_array[matching_start_pos++];
            }
            curr += 4;
        }
        else
        {
            // Meet a literal
            output_array[output_pos] = input[curr];
            output_pos++;
            curr++;
        }
    }
    output_array[output_pos] = '\0';

    // store output_array data into output stream
    int copy_count = (output_pos + 1) / 4;
    copy_count++;

LZ77_OUTPUT:
    for (int i = 0; i < copy_count; i++)
    {
#pragma HLS PIPELINE
        output_word = (output_array[i * 4] << 24) | (output_array[i * 4 + 1] << 16) | (output_array[i * 4 + 2] << 8) | (output_array[i * 4 + 3]);
        output.write(output_word);
    }

    //    // print out the result - for testing
    //    int out = 0;
    //    cout << endl << "The stream after LZ77 decoding: " << endl << endl;
    //    while (output_array[out] != '\0') {
    //        cout << output_array[out++];
    //    }
    //    cout << endl;

    return;
}