/* 
 * File:   deflate.cpp
 * Author: Yuxuan (Eric) Zheng
 *
 * Created on July 8, 2016, 10:52 AM
 */

#include "deflate.h"

/*
 * Good references:
 * 1. Data Compression The Complete Reference 4th edition by David Salomon
 * 2. rfc1951 standard
 * 3. http://www.gzip.org/algorithm.txt by gzip
 * 4. Gzip on a Chip: High Performance Lossless Data Compression on FPGAs using OpenCL by Abdelfattah
 *
 * Notes:
 *
 * 1. Currently, the core only supports max offset = 4096 and max length = 32(LEN) for LZ77
 * It's a tradeoff between compression ratio and speed.
 * 2. Please note that the static Huffman encoding uses Little-Endian now.
 * 3. The dynamic encoding part is commented out. Because building dynamic Huffman trees can be
 * implemented using hardware or host CPU + PCIe. The commented code is the major part of
 * hardware implementation.
 */

/*
 * Endianness Clarification:
 *
 * Endianness is quite confusing in the standard and in the project.
 * It's necessary to state the endianness details in the cores.
 *
 * Compression:
 * 1. LZ77 core: no endianness issue.
 *
 * 2. Static Huffman Encoding Part:
 * 		header bits: little-endian (correct)
 * 		huffman codes: big-endian (correct)
 * 		extra bits: big-endian (wrong, should change to little-endian if following standard)
 * 		write to output stream: little-endian (correct)
 * 3. Dynamic Huffman Encoding Part:
 * 		All big-endian; extra bits should be little-endian
 *
 * Decompression:
 * 1. Static Huffman Decoding Part:
 * 		header bits: little-endian (correct)
 * 		read from input stream: little-endian (correct)
 * 		huffman codes: big-endian (correct)
 * 		extra bits: big-endian (wrong, should be little-endian)
 * 2. Dynamic Huffman Decoding Part:
 * 		header bits: little-endian (correct)
 * 		read from input stream: little-endian (correct)
 * 		huffman codes: big-endian (correct)
 * 		all other extra bits, HLIT codes, etc. : little-endian (correct)
 *
 * In summary, the extra bits endianness in static Huffman encoding/decoding is wrong.
 * Other parts are correct. The Deflate and Inflate core matches each other.
 */

// Top level module for compression
void Deflate(hls::stream<uint32_t> &input,
             hls::stream<uint32_t> &output)
{
#pragma HLS INTERFACE ap_ctrl_none register port=return
#pragma HLS INTERFACE axis register both port=output
#pragma HLS INTERFACE axis register both port=input

    // temp array for connecting two cores
    uint8_t LZ77_output[3000];

    LZ77(input, 2250, LZ77_output);

    //    // Print out the compressed data - for testing
    //    int offset, length;
    //    int j = 0;
    //    cout << endl << "LZ77 compressed stream is:" << endl << endl;
    //    while (LZ77_output[j] != '\0') {
    //        if (LZ77_output[j] == '@') {
    //            offset = (LZ77_output[j + 1] * 128) + LZ77_output[j + 2];
    //            length = LZ77_output[j + 3];
    //            cout << "@(" << offset << "," << length << ")";
    //            j += 4;
    //        } else {
    //            cout << LZ77_output[j];
    //            j++;
    //        }
    //    }
    //
    //    cout << endl << endl;

    huffman(LZ77_output, output);

    return;
}

/*
 * The first part of DEFLATE Algorithm - LZ77
 *
 * Interface: hls_stream for input and output
 *
 * Notes:
 *
 * LZ77 searches max matching in the dictionaries stored before. If found
 * a matching, the algorithm makes a pair of @(offset, length) on the data stream.
 * The output of LZ77 is a data array containing the literals, offsets, and lengths.
 * Then, the output is further compressed in the Huffman function below.
 *
 * At first, LZ77 takes one word from the hls_stream input. Calculate its hash value
 * trying to find a match in dictionaries. Next, LZ77 compares the string in curr_window
 * and comp_window to find the matching length. Note that this process is done
 * for each string with length LEN starting at each char of the curr_window
 * (totally, VEC strings with length LEN; called lazy evaluation). Then, the
 * program tries to decide which matching to choose through the approach from
 * Mohamed's paper. Next, the results are stored into the output array. Finally,
 * the program updates the dictionaries using the data in curr_window.
 * 
 * 
 * Also note that the function argument 'size' is not necessary if the outermost
 * loop checks whether the input stream is empty each time.
 */

void LZ77(hls::stream<uint32_t> &input, int size, uint8_t output[3000])
{

    /*************************** Initialization *******************************/
    int iteration_count = size / VEC;

    // Use current_index to indicate the start index of each set of processing data
    int current_index = 0;
    int output_position = 0;

    uint8_t comp_window[NUM_DICT][VEC][LEN];
    uint8_t dict[NUM_DICT][HASH_TABLE_SIZE][LEN];

    // Record the information of where the string starts - in order to calculate offset
    int dict_string_start_pos[NUM_DICT][HASH_TABLE_SIZE];
    int compare_window_string_start_pos[NUM_DICT][VEC];
    int hash_value, new_hash_value;

    bool done[VEC];
    int length[VEC];
    match_pair bestlength[VEC];

    int match_length;
    int offset;
    int start_match_position = 0;
    int first_valid_position = 4;
    int temp_valid_position = 0;

    // For hls_stream input
    uint8_t curr_window[VEC + LEN]; // a processing buffer containing all information to use
    uint32_t input_word;

    for (char i = 1; i < 9; i++)
    {                           // first time to fill in the processing buffer
        input.read(input_word); // read one word from the input stream
        curr_window[i * 4] = (input_word & 0xFF000000) >> 24;
        curr_window[i * 4 + 1] = (input_word & 0x00FF0000) >> 16;
        curr_window[i * 4 + 2] = (input_word & 0x0000FF00) >> 8;
        curr_window[i * 4 + 3] = (input_word & 0x000000FF);
    }

    /************************** Main Loop *************************************/

CONTROL_LOOP:
    for (int loop_count = 0; loop_count < iteration_count; loop_count++)
    {
#pragma HLS loop_tripcount min = 635 max = 635

        // 1. Dictionary Lookup and Update
        // search each dictionary to find most similar sequence
        // repeat for VEC sequence substrings; store data into memory

        // Shift current window
        for (char i = 0; i < LEN; i++)
        {
#pragma HLS UNROLL
            curr_window[i] = curr_window[VEC + i];
        }

        // Load in new data
        if (!input.empty())
        {
            input.read(input_word);
            curr_window[LEN] = (input_word & 0xFF000000) >> 24;
            curr_window[LEN + 1] = (input_word & 0x00FF0000) >> 16;
            curr_window[LEN + 2] = (input_word & 0x0000FF00) >> 8;
            curr_window[LEN + 3] = (input_word & 0x000000FF);
        }

        first_valid_position -= VEC; // minus VEC since the buffer will be shifted to left

    SUBSTRING_MATCHING:
        for (int i = 0; i < VEC; i++)
        {
#pragma HLS loop_tripcount min = 4 max = 4
#pragma HLS UNROLL
            // for each substring

            hash_value = (curr_window[i] << 3) ^ (curr_window[i + 1] << 2) ^ (curr_window[i + 2] << 1) ^ (curr_window[i + 3]);
            /* The hash function is an important part for improving compression ratio.
             * The current version is chosen by testing many possible cases. */

        DICT_MATCHING:
            for (int t = 0; t < NUM_DICT; t++)
            {
#pragma HLS UNROLL
#pragma HLS loop_tripcount min = 4 max = 4

                if (dict_string_start_pos[t][hash_value] != '\0')
                { // found a match
                    compare_window_string_start_pos[t][i] = dict_string_start_pos[t][hash_value];
                COPY_LOOP_MATCHING:
                    for (int j = 0; j < LEN; j++)
                    {
#pragma HLS loop_tripcount min = 32 max = 32
#pragma HLS UNROLL
                        // copy the string to compare window
                        comp_window[t][i][j] = dict[t][hash_value][j];
                    }
                }
            }
        }

        // 2. Match Search and Reduction

        // clear best length
    CLEAN_BESTLENGTH:
        for (int i = 0; i < VEC; i++)
        {
#pragma HLS UNROLL
#pragma HLS loop_tripcount min = 4 max = 4

            bestlength[i].string_start_pos = 0;
            bestlength[i].length = 0;
        }
        // calculate the length of the same sequence
    FIND_MATCHING_LENGTH:
        for (int i = 0; i < VEC; i++)
        { // for different dictionary
#pragma HLS loop_tripcount min = 4 max = 4
#pragma HLS UNROLL

            // clear done[]
        CLEAN_LENGTH_AND_DONE:
            for (int n = 0; n < VEC; n++)
            {
#pragma HLS UNROLL
#pragma HLS loop_tripcount min = 4 max = 4

                done[n] = false;
                length[n] = 0;
            }

        SUBSTRING_MATCHING_LENGTH:
            for (int j = 0; j < VEC; j++)
            { // for each substring (lazy evaluation)
#pragma HLS UNROLL
#pragma HLS loop_tripcount min = 4 max = 4

            COMPARE_EACH_CHAR:
                for (int k = 0; k < LEN; k++)
                { // for each char of each substring
#pragma HLS UNROLL
#pragma HLS loop_tripcount min = 32 max = 32

                    if (done[j])
                        break;
                    if (curr_window[j + k] == comp_window[i][j][k] && !done[j])
                        length[j]++;
                    else
                        done[j] = true;

                    //if (curr_window[j + k] == comp_window[k][i][j])   // possible way to improve area
                    //length_bool[i][j] |= 1 << k;
                }
            }

            // update best length
            // here, i is the index of dictionary
        UPDATE_BESTLENGTH:
            for (int m = 0; m < VEC; m++)
            {
#pragma HLS UNROLL
#pragma HLS loop_tripcount min = 4 max = 4

                if (length[m] > bestlength[m].length)
                {
                    bestlength[m].length = length[m];
                    bestlength[m].string_start_pos = compare_window_string_start_pos[i][m];
                }
            }
        }

        // 3. Match Filtering
        // choose which one to compress // huge modification for hls_stream

        temp_valid_position = first_valid_position;
        match_length = 0;
    CHOOSE_MATCHING_STRING:
        for (int i = first_valid_position; i < VEC; i++)
        { // go through bestlength
#pragma HLS UNROLL
#pragma HLS loop_tripcount min = 4 max = 4

            if (bestlength[i].length >= 3)
            { // can be a candidate
                if (i + bestlength[i].length > temp_valid_position)
                {
                    // find a later match
                    temp_valid_position = i + bestlength[i].length;
                    match_length = bestlength[i].length;
                    start_match_position = i;
                    offset = (current_index + i) - bestlength[i].string_start_pos;
                }
            }
        }

        // 4. Fill In the Output Array
        if (match_length > 0 && offset < 4096)
        { // some data was compressed
        FILL_LOOP_LITERAL:
            while (first_valid_position < start_match_position)
            {
#pragma HLS loop_tripcount min = 0 max = 4
#pragma HLS PIPELINE
#pragma HLS UNROLL

                // copy the literals
                output[output_position] = curr_window[first_valid_position];
                first_valid_position++;
                output_position++;
            }
            output[output_position] = '@';             // record marker		// There is a corner case here. If the input has '@', the marker is wrong.
            output[output_position + 1] = offset >> 7; // record offset
            output[output_position + 2] = (offset & 0x07F);
            output[output_position + 3] = match_length; // record length
            output_position += 4;
            // update the first_valid_position
            first_valid_position = temp_valid_position;

            // when first valid position doesn't exceed next buffer frame
        FILL_LOOP_UNTIL_NEXT_BUFFER:
            while (first_valid_position < VEC)
            {
#pragma HLS UNROLL
#pragma HLS PIPELINE
#pragma HLS loop_tripcount min = 0 max = 1

                output[output_position] = curr_window[first_valid_position];
                first_valid_position++;
                output_position++;
            }
        }
        else
        { // no data was compressed, or offset >= 4096

        FILL_LOOP_NO_COMPRESSION:
            while (first_valid_position < VEC)
            {
#pragma HLS UNROLL
#pragma HLS PIPELINE
#pragma HLS loop_tripcount min = 0 max = 4

                // copy the block to output
                output[output_position] = curr_window[first_valid_position];
                first_valid_position++;
                output_position++;
            }
        }

        // 5. Update Dictionaries
    UPDATE_DICT:
        for (int i = 0; i < VEC; i++)
        {
#pragma HLS UNROLL
#pragma HLS loop_tripcount min = 4 max = 4

            new_hash_value = (curr_window[i] << 3) ^ (curr_window[i + 1] << 2) ^ (curr_window[i + 2] << 1) ^ (curr_window[i + 3]);

        UPDATE_COPY_STRING:
            for (int j = 0; j < LEN; j++)
            {
#pragma HLS UNROLL
#pragma HLS loop_tripcount min = 32 max = 32
                dict[i][new_hash_value][j] = curr_window[i + j];
            }

            dict_string_start_pos[i][new_hash_value] = current_index + i;
        }

        // Move the current window index by VEC bytes
        current_index += VEC;
    }

    // Fill the remaining bytes
FILL_LOOP_REMAINING_BYTES:
    while (curr_window[first_valid_position] != '\0')
    {
#pragma HLS UNROLL
#pragma HLS PIPELINE
#pragma HLS loop_tripcount min = 0 max = 4
        output[output_position] = curr_window[first_valid_position];
        first_valid_position++;
        output_position++;
    }

    output[output_position] = '\0';

    return;
}

/*
 * The second part of DEFLATE Algorithm - Huffman encoding
 * 
 * Input: an array of uint8_t, compressed by LZ77 algorithm
 * Output: a hls_stream containing the Huffman encoding result
 *
 * Notes:
 *
 * There are a lot of details in the Huffman encoding process, especially for
 * dynamic Huffman encoding. Basically, the algorithm uses shorter code to
 * represent the symbols appear frequently. The static Huffman encoding mode is
 * like hard-coding. The dynamic Huffman encoding is hard to implement through
 * hardware. Therefore, some techniques can be used to store a dynamic tree on
 * hardware or traverse the tree without recursion and stack/queue. However, in
 * Mohamed's paper, the hardware needs not to build dynamic trees by itself,
 * which could be another way to solve the problem.
 *
 * The static encoding part checks each input char and finds the corresponding code.
 * The dynamic part is not completely finished because the Huffman core cannot
 * get trees from CPU currently. However, all the essential parts for dynamic
 * Huffman encoding were finished, including building the trees and getting codes
 * through the trees. Some comments explain how to use hardware the build the
 * entire core.
 *
 */

void huffman(uint8_t input[3000], hls::stream<uint32_t> &output)
{

    int input_pos = 0, output_pos = 0;
    unsigned length_valid_bits_num, offset_valid_bits_num,
        output_char_remaining_bits, remaining_bits_to_output;
    unsigned offset;
    uint8_t input_char, length;
    uint8_t code_8_bits;
    uint16_t code_9_bits;
    uint16_t length_code, offset_code;
    uint32_t length_and_offset;
    uint8_t output_char_buffer;

    // For hls_stream input
    uint32_t output_word;
    uint8_t output_buffer[3000]; // temp buffer for storing output data

    short mode = 1;
    // mode indicates which type of Huffman encoding is used
    // mode = 0: no compression; mode = 1: static Huffman; mode = 2: dynamic Huffman

    if (mode == 1)
    {
        // Static Huffman Encoding
        // write the flag code of static Huffman encoding into output array
        output_buffer[0] = 0xC0;
        output_char_remaining_bits = 5;

        // analyze the input
    STATIC_HUFFMAN:
        while (input[input_pos] != '\0')
        {
            input_char = input[input_pos];

            if (input_char == '@')
            {
                // meet a match
                length = input[input_pos + 3];

                if (length >= 115 || length < 3)
                {
                    // currently, impossible to enter here
                    cout << "The matching length is too long or too short! Wrong!"
                         << endl;
                }
                else
                {

                    // edoc: 257-279
                    if (length >= 3 && length <= 10)
                    {
                        // 257-264, no extra bit
                        length_code = length - 2;
                        length_valid_bits_num = 7;
                    }
                    else if (length == 11 || length == 12)
                    {
                        // 265-268, 1 extra bit
                        length_code = 0b00010010 + (length - 11);
                        length_valid_bits_num = 8;
                    }
                    else if (length == 13 || length == 14)
                    {
                        // 265-268, 1 extra bit
                        length_code = 0b00010100 + (length - 13);
                        length_valid_bits_num = 8;
                    }
                    else if (length == 15 || length == 16)
                    {
                        // 265-268, 1 extra bit
                        length_code = 0b00010110 + (length - 15);
                        length_valid_bits_num = 8;
                    }
                    else if (length == 17 || length == 18)
                    {
                        // 265-268, 1 extra bit
                        length_code = 0b00011000 + (length - 17);
                        length_valid_bits_num = 8;
                    }
                    else if (length >= 19 && length <= 22)
                    {
                        // 269-272, 2 extra bits
                        length_code = 0b000110100 + (length - 19);
                        length_valid_bits_num = 9;
                    }
                    else if (length >= 23 && length <= 26)
                    {
                        // 269-272, 2 extra bits
                        length_code = 0b000111000 + (length - 23);
                        length_valid_bits_num = 9;
                    }
                    else if (length >= 27 && length <= 30)
                    {
                        // 269-272, 2 extra bits
                        length_code = 0b000111100 + (length - 27);
                        length_valid_bits_num = 9;
                    }
                    else if (length >= 31 && length <= 34)
                    {
                        // 269-272, 2 extra bits
                        length_code = 0b001000000 + (length - 31);
                        length_valid_bits_num = 9;
                    }
                    else if (length >= 35 && length <= 42)
                    {
                        // 273-276, 3 extra bits
                        length_code = 0b0010001000 + (length - 35);
                        length_valid_bits_num = 10;
                    }
                    else if (length >= 43 && length <= 50)
                    {
                        // 273-276, 3 extra bits
                        length_code = 0b0010010000 + (length - 43);
                        length_valid_bits_num = 10;
                    }
                    else if (length >= 51 && length <= 58)
                    {
                        // 273-276, 3 extra bits
                        length_code = 0b0010011000 + (length - 51);
                        length_valid_bits_num = 10;
                    }
                    else if (length >= 59 && length <= 66)
                    {
                        // 273-276, 3 extra bits
                        length_code = 0b0010100000 + (length - 59);
                        length_valid_bits_num = 10;
                    }
                    else if (length >= 67 && length <= 82)
                    {
                        // 277-279, 4 extra bits
                        length_code = 0b00101010000 + (length - 67);
                        length_valid_bits_num = 11;
                    }
                    else if (length >= 83 && length <= 98)
                    {
                        // 277-279, 4 extra bits
                        length_code = 0b00101100000 + (length - 83);
                        length_valid_bits_num = 11;
                    }
                    else if (length >= 99 && length <= 114)
                    {
                        // 277-279, 4 extra bits
                        length_code = 0b00101110000 + (length - 99);
                        length_valid_bits_num = 11;
                    }
                    else
                    {
                        cout << "Wrong! Cannot find the correct length code!"
                             << endl;
                    }
                }

                offset = input[input_pos + 1] * 128 + input[input_pos + 2];

                if (offset >= 1 && offset <= 4)
                {
                    offset_code = offset - 1;
                    offset_valid_bits_num = 5;
                }
                else if (offset >= 5 && offset <= 6)
                {
                    offset_code = 0b001000 + (offset - 5);
                    offset_valid_bits_num = 6;
                }
                else if (offset >= 7 && offset <= 8)
                {
                    offset_code = 0b001010 + (offset - 7);
                    offset_valid_bits_num = 6;
                }
                else if (offset >= 9 && offset <= 12)
                {
                    offset_code = 0b0011000 + (offset - 9);
                    offset_valid_bits_num = 7;
                }
                else if (offset >= 13 && offset <= 16)
                {
                    offset_code = 0b0011100 + (offset - 13);
                    offset_valid_bits_num = 7;
                }
                else if (offset >= 17 && offset <= 24)
                {
                    offset_code = 0b01000000 + (offset - 17);
                    offset_valid_bits_num = 8;
                }
                else if (offset >= 25 && offset <= 32)
                {
                    offset_code = 0b01001000 + (offset - 25);
                    offset_valid_bits_num = 8;
                }
                else if (offset >= 33 && offset <= 48)
                {
                    offset_code = 0b010100000 + (offset - 33);
                    offset_valid_bits_num = 9;
                }
                else if (offset >= 49 && offset <= 64)
                {
                    offset_code = 0b010110000 + (offset - 49);
                    offset_valid_bits_num = 9;
                }
                else if (offset >= 65 && offset <= 96)
                {
                    offset_code = 0b0110000000 + (offset - 65);
                    offset_valid_bits_num = 10;
                }
                else if (offset >= 97 && offset <= 128)
                {
                    offset_code = 0b0110100000 + (offset - 97);
                    offset_valid_bits_num = 10;
                }
                else if (offset >= 129 && offset <= 192)
                {
                    offset_code = 0b01110000000 + (offset - 129);
                    offset_valid_bits_num = 11;
                }
                else if (offset >= 193 && offset <= 256)
                {
                    offset_code = 0b01111000000 + (offset - 193);
                    offset_valid_bits_num = 11;
                }
                else if (offset >= 257 && offset <= 384)
                {
                    offset_code = 0b100000000000 + (offset - 257);
                    offset_valid_bits_num = 12;
                }
                else if (offset >= 385 && offset <= 512)
                {
                    offset_code = 0b100010000000 + (offset - 385);
                    offset_valid_bits_num = 12;
                }
                else if (offset >= 513 && offset <= 768)
                {
                    offset_code = 0b1001000000000 + (offset - 513);
                    offset_valid_bits_num = 13;
                }
                else if (offset >= 769 && offset <= 1024)
                {
                    offset_code = 0b1001100000000 + (offset - 769);
                    offset_valid_bits_num = 13;
                }
                else if (offset >= 1025 && offset <= 1536)
                {
                    offset_code = 0b10100000000000 + (offset - 1025);
                    offset_valid_bits_num = 14;
                }
                else if (offset >= 1537 && offset <= 2048)
                {
                    offset_code = 0b10101000000000 + (offset - 1537);
                    offset_valid_bits_num = 14;
                }
                else if (offset >= 2049 && offset <= 3072)
                {
                    offset_code = 0b101100000000000 + (offset - 2049);
                    offset_valid_bits_num = 15;
                }
                else if (offset >= 3073 && offset <= 4096)
                {
                    offset_code = 0b101110000000000 + (offset - 3073);
                    offset_valid_bits_num = 15;
                }
                else
                {
                    cout << "Wrong! Offset is out of range!" << endl;
                }

                // write the length_code and offset_code into the output

                // combine length and offset codes
                remaining_bits_to_output = length_valid_bits_num + offset_valid_bits_num;
                length_and_offset = (length_code << (32 - length_valid_bits_num)) | (offset_code << (32 - remaining_bits_to_output));

                if (output_char_remaining_bits != 8)
                {
                    // not a 8-bit boundary to start, move the 8-bit boundary
                    output_char_buffer = length_and_offset >> (32 - output_char_remaining_bits);
                    output_buffer[output_pos] = output_buffer[output_pos] | output_char_buffer;

                    length_and_offset <<= output_char_remaining_bits;
                    remaining_bits_to_output -= output_char_remaining_bits;
                    output_char_remaining_bits = 8;
                    output_pos++;
                }

                // starting at 8-bit boundary, iteration to write into the output array
            STATIC_HUFFMAN_WRITE_OUTPUT:
                while (remaining_bits_to_output >= 8)
                {
                    // can write to the next whole char
                    output_buffer[output_pos++] = (length_and_offset & 0xFF000000) >> 24;
                    length_and_offset <<= 8;
                    remaining_bits_to_output -= 8;
                }

                if (remaining_bits_to_output == 0)
                {
                    // do nothing
                }
                else
                {
                    // follow-up steps: output the remaining bits (less than 8)
                    output_buffer[output_pos] = length_and_offset >> 24;
                    output_char_remaining_bits = 8 - remaining_bits_to_output;
                }

                input_pos += 4;
            }
            else
            {
                // normal literals
                if (input_char <= 143)
                {
                    // edoc: 0-143
                    code_8_bits = input_char + 0x30;

                    // write the literal code into the output
                    if (output_char_remaining_bits == 8)
                    {
                        // enough space to store the literal in this position
                        output_buffer[output_pos++] = code_8_bits;
                    }
                    else
                    {
                        // not enough to store the 8 bit literal code
                        output_char_buffer = code_8_bits >> (8 - output_char_remaining_bits);
                        output_buffer[output_pos] = output_buffer[output_pos] | output_char_buffer;

                        output_pos++;
                        output_buffer[output_pos] = code_8_bits << output_char_remaining_bits;
                    }
                }
                else
                {
                    // edoc: 144-255
                    code_9_bits = input_char - 144 + 0x190;

                    // not enough to store the 9 bit literal code
                    output_char_buffer = code_9_bits >> (9 - output_char_remaining_bits);
                    output_buffer[output_pos] = output_buffer[output_pos] | output_char_buffer;

                    output_char_buffer = code_9_bits << output_char_remaining_bits;
                    output_buffer[output_pos + 1] = output_char_buffer;
                    output_char_remaining_bits--;

                    if (output_char_remaining_bits == 0)
                    {
                        output_pos += 2;
                        output_char_remaining_bits = 8;
                    }
                    else
                    {
                        output_pos++;
                    }
                }

                input_pos++;
            }
        }

        // finish encoding, edoc: 256
        if (output_char_remaining_bits == 8)
        {
            output_buffer[output_pos] = '\0';
        }
        else
        {
            output_buffer[output_pos + 1] = '\0';
        }

        // Store output_buffer data into output stream
        output_pos++;
        int copy_count = (output_pos + 1) / 4;
        copy_count++;

        for (int i = 0; i < copy_count; i++)
        {
            output_word = (reverse(output_buffer[i * 4], 8) << 24) | (reverse(output_buffer[i * 4 + 1], 8) << 16) | (reverse(output_buffer[i * 4 + 2], 8) << 8) | (reverse(output_buffer[i * 4 + 3], 8));
            output.write(output_word);
        }
    }
    else if (mode == 2)
    {

        //        // Dynamic Huffman Encoding
        //
        //        /*********************** Initialization *******************************/
        //
        //        // Use fixed size array to store dynamic trees
        //        // 0-29: distances (leaf); 30-89: parent nodes in the distance Huffman tree
        //        tree_node distance_tree[90];
        //        // 0-279: literals and lengths (leaf); 280-599: parent nodes in the LIT Huffman tree
        //        tree_node literal_tree[600];
        //
        //        smallest_node smallest_two_nodes[2];
        //        smallest_node lit_smallest_two_nodes[2];
        //
        //        code_table_node dis_codes[30];
        //        code_table_node lit_codes[280];
        //
        //        for (int i = 0; i < 90; i++) {
        //#pragma HLS UNROLL
        //            distance_tree[i].no_parent = true;
        //            distance_tree[i].level = 0;
        //            distance_tree[i].weight = 0;
        //        }
        //
        //        for (int i = 0; i < 280; i++) {
        //#pragma HLS UNROLL
        //            literal_tree[i].no_parent = true;
        //            literal_tree[i].level = 0;
        //            literal_tree[i].weight = 0;
        //        }
        //        /**********************************************************************/
        //
        //        // write the flag code of dynamic Huffman encoding into output array
        //        output[0] = 0xC0;
        //        output_char_remaining_bits = 5;
        //
        //        while (input[input_pos] != '\0') {
        //            input_char = input[input_pos];
        //
        //            // Get the count of each char, length, or offset
        //            // store the information into distance_count and literal_count
        //            if (input_char == '@') {
        //                // Found a length and distance
        //                offset = input[input_pos + 1] * 128 + input[input_pos + 2];
        //
        //                if (offset >= 1 && offset <= 4) {
        //                    distance_tree[offset - 1].weight++;
        //                } else if (offset >= 5 && offset <= 6) {
        //                    distance_tree[4].weight++;
        //                } else if (offset >= 7 && offset <= 8) {
        //                    distance_tree[5].weight++;
        //                } else if (offset >= 9 && offset <= 12) {
        //                    distance_tree[6].weight++;
        //                } else if (offset >= 13 && offset <= 16) {
        //                    distance_tree[7].weight++;
        //                } else if (offset >= 17 && offset <= 24) {
        //                    distance_tree[8].weight++;
        //                } else if (offset >= 25 && offset <= 32) {
        //                    distance_tree[9].weight++;
        //                } else if (offset >= 33 && offset <= 48) {
        //                    distance_tree[10].weight++;
        //                } else if (offset >= 49 && offset <= 64) {
        //                    distance_tree[11].weight++;
        //                } else if (offset >= 65 && offset <= 96) {
        //                    distance_tree[12].weight++;
        //                } else if (offset >= 97 && offset <= 128) {
        //                    distance_tree[13].weight++;
        //                } else if (offset >= 129 && offset <= 192) {
        //                    distance_tree[14].weight++;
        //                } else if (offset >= 193 && offset <= 256) {
        //                    distance_tree[15].weight++;
        //                } else if (offset >= 257 && offset <= 384) {
        //                    distance_tree[16].weight++;
        //                } else if (offset >= 385 && offset <= 512) {
        //                    distance_tree[17].weight++;
        //                } else if (offset >= 513 && offset <= 768) {
        //                    distance_tree[18].weight++;
        //                } else if (offset >= 769 && offset <= 1024) {
        //                    distance_tree[19].weight++;
        //                } else if (offset >= 1025 && offset <= 1536) {
        //                    distance_tree[20].weight++;
        //                } else if (offset >= 1537 && offset <= 2048) {
        //                    distance_tree[21].weight++;
        //                } else if (offset >= 2049 && offset <= 3072) {
        //                    distance_tree[22].weight++;
        //                } else if (offset >= 3073 && offset <= 4096) {
        //                    distance_tree[23].weight++;
        //                } else {
        //                    cout << "Wrong! Offset is out of range!" << endl;
        //                }
        //
        //                length = input[input_pos + 3];
        //
        //                // edoc: 257-279
        //                if (length >= 3 && length <= 10) {
        //                    // 257-264, no extra bit
        //                    literal_tree[length + 254].weight++;
        //                } else if (length == 11 || length == 12) {
        //                    // 265-268, 1 extra bit
        //                    literal_tree[265].weight++;
        //                } else if (length == 13 || length == 14) {
        //                    // 265-268, 1 extra bit
        //                    literal_tree[266].weight++;
        //                } else if (length == 15 || length == 16) {
        //                    // 265-268, 1 extra bit
        //                    literal_tree[267].weight++;
        //                } else if (length == 17 || length == 18) {
        //                    // 265-268, 1 extra bit
        //                    literal_tree[268].weight++;
        //                } else if (length >= 19 && length <= 22) {
        //                    // 269-272, 2 extra bits
        //                    literal_tree[269].weight++;
        //                } else if (length >= 23 && length <= 26) {
        //                    // 269-272, 2 extra bits
        //                    literal_tree[270].weight++;
        //                } else if (length >= 27 && length <= 30) {
        //                    // 269-272, 2 extra bits
        //                    literal_tree[271].weight++;
        //                } else if (length >= 31 && length <= 34) {
        //                    // 269-272, 2 extra bits
        //                    literal_tree[272].weight++;
        //                } else if (length >= 35 && length <= 42) {
        //                    // 273-276, 3 extra bits
        //                    literal_tree[273].weight++;
        //                } else if (length >= 43 && length <= 50) {
        //                    // 273-276, 3 extra bits
        //                    literal_tree[274].weight++;
        //                } else if (length >= 51 && length <= 58) {
        //                    // 273-276, 3 extra bits
        //                    literal_tree[275].weight++;
        //                } else if (length >= 59 && length <= 66) {
        //                    // 273-276, 3 extra bits
        //                    literal_tree[276].weight++;
        //                } else if (length >= 67 && length <= 82) {
        //                    // 277-279, 4 extra bits
        //                    literal_tree[277].weight++;
        //                } else if (length >= 83 && length <= 98) {
        //                    // 277-279, 4 extra bits
        //                    literal_tree[278].weight++;
        //                } else if (length >= 99 && length <= 114) {
        //                    // 277-279, 4 extra bits
        //                    literal_tree[279].weight++;
        //                } else {
        //                    // currently, impossible to enter here
        //                    cout << "The matching length is too long or too short! Wrong!"
        //                         << endl;
        //                }
        //                input_pos += 4;
        //            } else {
        //                // Found a literal
        //                literal_tree[input_char].weight++;
        //                input_pos++;
        //            }
        //        }
        //
        //        /********************** Build the Huffman trees ***********************/
        //        /************************** Distance Tree *****************************/
        //        unsigned dis_max_parent_node_id = 30;   // record the current position of parent nodes
        //        unsigned dis_element_count = 0;         // the # of elements in the table
        //
        //        // NEED TO REPEAT # ELEMENTS - 1 TIMES
        //        // (elements - 1) times to build the tree
        //        for (int i = 0; i < 30; i++) {
        //            if (distance_tree[i].weight != 0)
        //                dis_element_count++;
        //        }
        //        dis_element_count--;
        //
        //        for (int repeat_count = 0; repeat_count < dis_element_count; repeat_count++) {
        //            // get the smallest two nodes
        //            smallest_two_nodes[0].node_weight = 30000;
        //            smallest_two_nodes[1].node_weight = 30000;
        //
        //            for (int i = 0; i < 90; i++) {          // may have error ...
        //                if (distance_tree[i].weight != 0 && distance_tree[i].no_parent == true) {
        //                    // Can be considered to build a tree
        //                    if (distance_tree[i].weight < smallest_two_nodes[0].node_weight) {
        //                        smallest_two_nodes[1].node_weight = smallest_two_nodes[0].node_weight;
        //                        smallest_two_nodes[1].node_id = smallest_two_nodes[0].node_id;
        //
        //                        smallest_two_nodes[0].node_weight = distance_tree[i].weight;
        //                        smallest_two_nodes[0].node_id = i;
        //                    } else if (distance_tree[i].weight < smallest_two_nodes[1].node_weight) {
        //                        smallest_two_nodes[1].node_weight = distance_tree[i].weight;
        //                        smallest_two_nodes[1].node_id = i;
        //                    }
        //                }
        //            }
        //
        //            // the codes below are not complete
        //            // need to add a loop to repeat the process below and use Morris Traversal to access the leaves
        //            // Note: "level" is also the code length, so the codes here are to operate "level"
        //            distance_tree[dis_max_parent_node_id].weight =
        //                    smallest_two_nodes[0].node_weight + smallest_two_nodes[1].node_weight;
        //            distance_tree[dis_max_parent_node_id].left =
        //                    smallest_two_nodes[0].node_id;
        //            distance_tree[dis_max_parent_node_id].right =
        //                    smallest_two_nodes[1].node_id;
        //            dis_max_parent_node_id++;
        //
        //            distance_tree[smallest_two_nodes[0].node_id].no_parent = false;
        //            (distance_tree[smallest_two_nodes[0].node_id].level)++;
        //            distance_tree[smallest_two_nodes[1].node_id].no_parent = false;
        //            (distance_tree[smallest_two_nodes[1].node_id].level)++;
        //            // need to increment the child nodes from the current smallest nodes !
        //            //            while (left != edoc || right != edoc) {
        //            //                // update the child node level
        //            //                // ...
        //            //            }
        //
        //            //          Morris Traversal - traverse tree without recursion
        //
        //            //            void MorrisInorder(NODE * root) {
        //            //                NODE* current, *pre;
        //            //                current = root;
        //            //                while (current != NULL) {
        //            //                    if (current->left == NULL) {
        //            //                        printf("%d ", current->data);
        //            //                        current = current->right;
        //            //                    } else {
        //            //                        pre = current->left;
        //            //                        while (pre->right != NULL && pre->right != current)
        //            //                            pre = pre->right;
        //            //                        if (pre->right == NULL) {
        //            //                            pre->right = current;
        //            //                            current = current->left;
        //            //                        } else {
        //            //                            pre->right = NULL;
        //            //                            printf("%d ", current->data);
        //            //                            current = current->right;
        //            //                        }
        //            //                    }
        //            //                }
        //            //            }
        //
        //        }
        //
        //        // Generate the Huffman code
        //        get_dis_huffman_code(distance_tree, dis_codes); // this function is complete
        //
        //        /********************************************************************/
        //        /********************* Literal / Length Tree *************************/
        //
        //        unsigned lit_max_parent_node_id = 280; // record the current position of parent nodes
        //        unsigned lit_element_count = 0; // the # of elements in the table
        //
        //        // NEED TO REPEAT # ELEMENTS - 1 TIMES
        //        for (int i = 0; i < 280; i++) {
        //            if (literal_tree[i].weight != 0)
        //                lit_element_count++;
        //        }
        //        lit_element_count--;
        //
        //        for (int lit_repeat_count = 0; lit_repeat_count < lit_element_count; lit_repeat_count++) {
        //            // get the smallest two nodes
        //            lit_smallest_two_nodes[0].node_weight = 30000;
        //            lit_smallest_two_nodes[1].node_weight = 30000;
        //
        //            for (int i = 0; i < 600; i++) { // may have error ...
        //                if (literal_tree[i].weight != 0 && literal_tree[i].no_parent == true) {
        //                    // Can be considered to build a tree
        //                    if (literal_tree[i].weight < lit_smallest_two_nodes[0].node_weight) {
        //                        lit_smallest_two_nodes[1].node_weight =
        //                                lit_smallest_two_nodes[0].node_weight;
        //                        lit_smallest_two_nodes[1].node_id =
        //                                lit_smallest_two_nodes[0].node_id;
        //
        //                        lit_smallest_two_nodes[0].node_weight = literal_tree[i].weight;
        //                        lit_smallest_two_nodes[0].node_id = i;
        //                    } else if (literal_tree[i].weight < lit_smallest_two_nodes[1].node_weight) {
        //                        lit_smallest_two_nodes[1].node_weight = literal_tree[i].weight;
        //                        lit_smallest_two_nodes[1].node_id = i;
        //                    }
        //                }
        //            }
        //
        //            // Again, the codes here are not complete. Need to add Morris traversal
        //            // to increment the level of each leaf node.
        //            literal_tree[lit_max_parent_node_id].weight =
        //                    lit_smallest_two_nodes[0].node_weight + lit_smallest_two_nodes[1].node_weight;
        //            literal_tree[lit_max_parent_node_id].left =
        //                    lit_smallest_two_nodes[0].node_id;
        //            literal_tree[lit_max_parent_node_id].right =
        //                    lit_smallest_two_nodes[1].node_id;
        //            lit_max_parent_node_id++;
        //
        //            literal_tree[lit_smallest_two_nodes[0].node_id].no_parent = false;
        //            (literal_tree[lit_smallest_two_nodes[0].node_id].level)++;
        //            literal_tree[lit_smallest_two_nodes[1].node_id].no_parent = false;
        //            (literal_tree[lit_smallest_two_nodes[1].node_id].level)++;
        //            // need to increment the child nodes from the current smallest nodes !
        //            //            while (left != edoc || right != edoc) {
        //            //                // update the child node level
        //            //                // ...
        //            //            }
        //        }
        //
        //        // Generate the Huffman code
        //        get_lit_huffman_code(literal_tree, lit_codes);  // this function is complete
        //        /********************************************************************/
        //        /***************** Get HLIT, HDIST, HCLEN, CCL **********************/
        //        // Here, the program knows the two dynamic Huffman tables.
        //        // Need to follow the standard to build some parameters to compress the
        //        // huffman tables again. Basically, the reverse process of inflate.
        //
        //
        //
        //        /********************************************************************/
        //        /****************** Write to the output stream **********************/
        //        // Write HEADER, HLIT, HDIST, HCLEN, CCL, SQ1, SQ2, LIT, DIST into the
        //        // output stream. Read from the LZ77 results and find the corresponding
        //        // code from huffman tables. The output process should be similar to it
        //        // in the static encoding part.
        //
        //
        //
        //        /********************************************************************/
    }

    return;
}

// The two functions below are for dynamic Huffman encoding.
// Given an array of CL, get the dynamic Huffman codes for each distance
void get_dis_huffman_code(tree_node distance_tree[90], code_table_node dis_codes[30])
{

    unsigned bl_count[16] = {0}; // the count of each length of code
    unsigned code = 0;
    unsigned next_code[16];
    unsigned len;

    // Count the number of codes for each code length (level)
    for (int i = 0; i < 30; i++)
    {
        bl_count[distance_tree[i].level]++;
    }

    // Assign a base value to each code length
    bl_count[0] = 0;
    for (int bits = 1; bits < 16; bits++)
    {
        code = (code + bl_count[bits - 1]) << 1;
        next_code[bits] = code;
    }

    // Use the base value of each length to assign consecutive numerical values
    for (int n = 0; n < 30; n++)
    {
        len = distance_tree[n].level;
        if (len != 0)
        {
            dis_codes[n].code = next_code[len];
            dis_codes[n].valid_length = len;
            next_code[len]++;
        }
    }

    return;
}

void get_lit_huffman_code(tree_node literal_tree[600], code_table_node lit_codes[280])
{

    unsigned bl_count[16] = {0}; // the count of each length of code
    unsigned code = 0;
    unsigned next_code[16];
    unsigned len;

    // Count the number of codes for each code length (level)
    for (int i = 0; i < 280; i++)
    {
        bl_count[literal_tree[i].level]++;
    }

    // Assign a base value to each code length
    bl_count[0] = 0;
    for (int bits = 1; bits < 16; bits++)
    {
        code = (code + bl_count[bits - 1]) << 1;
        next_code[bits] = code;
    }

    // Use the base value of each length to assign consecutive numerical values
    for (int n = 0; n < 280; n++)
    {
        len = literal_tree[n].level;
        if (len != 0)
        {
            lit_codes[n].code = next_code[len];
            lit_codes[n].valid_length = len;
            next_code[len]++;
        }
    }

    return;
}

// Reverse bits to change endianness
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
