/* 
 * File:   deflate.h
 * Author: Yuxuan (Eric) Zheng
 *
 * Created on July 8, 2016, 10:52 AM
 */

#ifndef DEFLATE_H
#define DEFLATE_H

#include <iostream>
#include "ap_int.h"
#include <stdint.h>
#include "hls_stream.h"
using namespace std;

#define VEC 4                // operates VEC bytes per iteration
#define LEN 32               // max matching length is LEN
#define NUM_DICT 4           // number of dictionaries, should be the same as VEC
#define HASH_TABLE_SIZE 2048 // the size of each dictionary

//typedef ap_uint<8> uint8_t;
//typedef ap_uint<16> uint16_t;
//typedef ap_uint<32> uint32_t;
typedef ap_uint<2> uint2_t;
typedef ap_uint<3> uint3_t;
typedef ap_uint<4> uint4_t;
typedef ap_uint<5> uint5_t;
typedef ap_uint<6> uint6_t;
typedef ap_uint<7> uint7_t;
typedef ap_uint<9> uint9_t;

struct match_pair
{
    int string_start_pos; // record the start position of the matched string in bestlength[]
    int length;           // record the length of string in bestlength[]
};

struct tree_node
{
    // Nodes in Huffman trees, used for building the dynamic tree
    unsigned level;  // the level of this node in the tree from 0 to 15; for leaf, it's the code length
    unsigned weight; // the count of the this node
    unsigned left;   // left child of this node
    unsigned right;  // right child of this node

    bool no_parent; // indicates whether the node has parent
};

struct smallest_node
{
    // used to record the smallest two nodes
    unsigned node_id;
    unsigned node_weight;
};

struct code_table_node
{                          // used to store the Huffman trees
    uint16_t code;         // 16 bits unsigned code, starting from the LSB; ex. 0000 0000 0000 0011 - 11 is the real code
    unsigned valid_length; // indicates the valid length of the previous 16 bit code
};

struct CCL_code
{
    unsigned length; // the valid length of the code of this CCL
    uint8_t code;    // actual code of this CCL - ex. 0000 0011
};

struct Lookup_Node
{
    uint9_t symbol;      // the actual symbol this position represents
    unsigned valid_bits; // the valid bits of the symbol from MSB
};

void Deflate(hls::stream<uint32_t> &input, hls::stream<uint32_t> &output);
void inflate(hls::stream<uint32_t> &input, hls::stream<uint32_t> &output);

void LZ77(hls::stream<uint32_t> &input, int size, uint8_t output[3000]);
void LZ77_decoder(uint8_t input[3000], hls::stream<uint32_t> &output);

void huffman(uint8_t input[3000], hls::stream<uint32_t> &output);
void huffman_decoder(hls::stream<uint32_t> &input, uint8_t decoding_output[3000]);

// Below are some helper functions for decoding
unsigned decoder_get_offset(unsigned &proc_bits_num, uint32_t proc_buffer);
unsigned dynamic_decoder_get_offset(unsigned &proc_bits_num,
                                    uint32_t proc_buffer,
                                    Lookup_Node lookup_table_DIST_1[128]);
void permute_CCL(uint3_t CCL[19], CCL_code hTable3[19]);
void get_huffman_table_1(code_table_node hTable1[286]);
void get_huffman_table_2(code_table_node hTable2[30]);
void get_huffman_table_3(CCL_code hTable3[19]);

// Function to reverse bits. Use a template for all cases.
template <typename T>
T reverse(T n, unsigned bits_num);

// Function to modify a uint32_t input word to little-endian
void changeToLittleEndian(uint32_t &next_word);

// Below are functions to build the dynamic Huffman trees on hardware.
void get_dis_huffman_code(tree_node distance_tree[90], code_table_node dis_codes[30]);
void get_lit_huffman_code(tree_node literal_tree[600], code_table_node lit_codes[280]);

#endif /* DEFLATE_H */
