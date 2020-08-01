/* 
 * File:   deflate_test.cpp
 * Author: Yuxuan (Eric) Zheng
 *
 * Created on July 8, 2016, 10:52 AM
 */

#include "deflate.h"

/*
 * The main test bench file for both deflate and inflate core.
 *
 * It builds the input and compress/decompress the input to test the two cores.
 * Currently, the input size is limited to 3000 bytes. But it's easy to change it
 * to unlimited size. Just not use array for testing.
 */

int main(void)
{

    uint8_t decoder_output_array[3000], decoding_temp[3000];
    int size = 0;

    hls::stream<uint32_t> input, huffman_encoding_output;
    hls::stream<uint32_t> decoder_output;
    uint32_t input_word, output_word;

    /************************* build input ************************************/
    string temp =
        "To evaluate our prefetcher we modelled the system using the gem5 simulator [4] in full system mode with the setup "
        "given in table 2 and the ARMv8 64-bit instruction set. Our applications are derived from existing benchmarks and "
        "libraries for graph traversal, using a range of graph sizes and characteristics. We simulate the core breadth-first search "
        "based kernels of each benchmark, skipping the graph construction phase. Our first benchmark is from the Graph 500 community [32]. "
        "We used their Kronecker graph generator for both the standard Graph 500 search benchmark and a connected components "
        "calculation. The Graph 500 benchmark is designed to represent data analytics workloads, such as 3D physics "
        "simulation. Standard inputs are too long to simulate, so we create smaller graphs with scales from 16 to 21 and edge "
        "factors from 5 to 15 (for comparison, the Graph 500 toy input has scale 26 and edge factor 16). "
        "Our prefetcher is most easily incorporated into libraries that implement graph traversal for CSR graphs. To this "
        "end, we use the Boost Graph Library (BGL) [41], a C++ templated library supporting many graph-based algorithms "
        "and graph data structures. To support our prefetcher, we added configuration instructions on constructors for CSR "
        "data structures, circular buffer queues (serving as the work list) and colour vectors (serving as the visited list). This "
        "means that any algorithm incorporating breadth-first searches on CSR graphs gains the benefits of our prefetcher without "
        "further modification. We evaluate breadth-first search, betweenness centrality and ST connectivity which all traverse "
        "graphs in this manner. To evaluate our extensions for sequential access prefetching (section 3.5) we use PageRank "
        "and sequential colouring. Inputs to the BGL algorithms are a set of real world "
        "graphs obtained from the SNAP dataset [25] chosen to represent a variety of sizes and disciplines, as shown in table 4. "
        "All are smaller than what we might expect to be processing in a real system, to enable complete simulation in a realistic "
        "time-frame, but as figure 2(a) shows, since stall rates go up for larger data structures, we expect the improvements we "
        "attain in simulation to be conservative when compared with real-world use cases.";

    int i = 0;
    while (temp[i] != '\0')
    {
        i++;
        size++;
    }

    int copy_count = size / 4;
    copy_count++;

    for (int w = 0; w < copy_count; w++)
    {
        input_word = (temp[w * 4] << 24) | (temp[w * 4 + 1] << 16) | (temp[w * 4 + 2] << 8) | (temp[w * 4 + 3]);
        input.write(input_word);
        //cout << "input " << int(temp[w * 4]) << endl;
        //cout << "input is " << (temp[w * 4] << 24) << endl;
        //cout << "input word is " << input_word << endl;
        //cout << "input size is " << input.read() << endl;
    }

    cout << "//////////////////////////////////////////////////////////////" << endl;
    cout << "input size is " << size << endl;

    /************************* Deflate compression ****************************/

    Deflate(input, huffman_encoding_output);

    for(int i = 0; i < 10; i++)
    {
    	cout << "huffman_encoding_output = " << huffman_encoding_output.read() << endl;
    }

//    inflate(huffman_encoding_output, decoder_output);
//    // copy stream output to a new array for checking the result
//    i = 0;
//    while (!decoder_output.empty())
//    {
//        decoder_output.read(output_word);
//        decoder_output_array[i * 4] = (output_word & 0xFF000000) >> 24;
//        decoder_output_array[i * 4 + 1] = (output_word & 0x00FF0000) >> 16;
//        decoder_output_array[i * 4 + 2] = (output_word & 0x0000FF00) >> 8;
//        decoder_output_array[i * 4 + 3] = (output_word & 0x000000FF);
//        i++;
//    }
//    decoder_output_array[i * 4] = '\0';
//    decoder_output_array[i * 4 + 1] = '\0';
//
//    /*************************** Compare Results *****************************/
//    int t = 0;
//    bool isFail = false;
//
//    while (temp[t] != '\0' && decoder_output_array[t] != '\0' && !isFail)
//    {
//        if (temp[t] != decoder_output_array[t])
//        {
//            isFail = true;
//            cout << "Deflate Fail!" << endl;
//            cout << "at t = " << t << endl;
//        }
//        t++;
//    }
//    if (!isFail && temp[t] == '\0' && decoder_output_array[t] == '\0')
//    {
//        cout << "Deflate Succeed!" << endl;
//    }
//    else
//    {
//        if (!isFail)
//        {
//            cout << "Deflate Fail! Not the same length." << endl;
//        }
//    }
    cout << "//////////////////////////////////////////////////////////////" << endl;

    return 0;
}
