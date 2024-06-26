#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <sndfile.h>

//libm17
#include <m17.h>

#define FLT_LEN         (BSB_SPS*FLT_SPAN+1)                //for 48kHz sample rate this is 81

struct LSF lsf;

char wav_name[1024];                                        //name of wav file to output to
SNDFILE *wav;                                               //sndfile wav file
SF_INFO info;                                               //sndfile parameter struct
int len=3;                                                  //number of blocks produced via counter +3 for pre,lsf, and eot marker

uint8_t enc_bits[SYM_PER_PLD*2];                            //type-2 bits, unpacked
uint8_t rf_bits[SYM_PER_PLD*2];                             //type-4 bits, unpacked

uint8_t dst_raw[10]={'A', 'L', 'L', '\0'};                  //raw, unencoded destination address
uint8_t src_raw[10]={'N', '0', 'C', 'A', 'L', 'L', '\0'};   //raw, unencoded source address
uint8_t can=0;                                              //Channel Access Number, default: 0
uint16_t num_bytes=0;                                       //number of bytes in packet, max 800-2=798
uint8_t fname[128]={'\0'};                                  //output file

FILE* fp;
float full_packet[36*192*10];                               //full packet, symbols as floats - 36 "frames" max (incl. preamble, LSF, EoT), 192 symbols each, sps=10:
                                                            //pream, LSF, 32 frames, ending frame, EOT plus RRC flushing
uint16_t pkt_sym_cnt=0;                                     //packet symbol counter, used to fill the packet
uint8_t pkt_cnt=0;                                          //packet frame counter (1..32) init'd at 0
uint8_t pkt_chunk[25+1];                                    //chunk of Packet Data, up to 25 bytes plus 6 bits of Packet Metadata
uint8_t full_packet_data[32*25];                            //full packet data, bytes
uint8_t out_type=0;                                         //output file type - 0 - raw int16 filtered samples (.rrc) - default
                                                            //                   1 - int16 symbol stream
                                                            //                   2 - binary stream (TODO)
                                                            //                   3 - simple 10x upsample no filter
                                                            //                   4 - S16-LE RRC filtered wav file
                                                            //                   5 - float symbol output for m17-packet-decode


uint8_t std_encode = 1;                                     //User Data is pre-encoded and read in over stdin, and not a switch string
uint8_t sms_encode = 0;                                     //User Supplied Data is an SMS Text message, encode as such
uint8_t raw_encode = 0;                                     //User Supplied Data is a string of hex octets, encode as such

char   text[800] = "Default SMS Text message";              //SMS Text to Encode, default string.
uint8_t raw[800];                                           //raw data that is converted from a string comprised of hex octets

//type - 0 - preamble before LSF (standard)
//type - 1 - preamble before BERT transmission
void fill_preamble(float* out, const uint8_t type)
{
    if(type) //pre-BERT
    {
        for(uint16_t i=0; i<SYM_PER_FRA/2; i++) //40ms * 4800 = 192
        {
            out[2*i]  =-3.0;
            out[2*i+1]=+3.0;
        }
    }
    else //pre-LSF
    {
        for(uint16_t i=0; i<SYM_PER_FRA/2; i++) //40ms * 4800 = 192
        {
            out[2*i]  =+3.0;
            out[2*i+1]=-3.0;
        }
    }
}

void fill_syncword(float* out, uint16_t* cnt, const uint16_t syncword)
{
    float symb=0.0f;

    for(uint8_t i=0; i<16; i+=2)
    {
        symb=symbol_map[(syncword>>(14-i))&3];
        out[*cnt]=symb;
        (*cnt)++;
    }
}

//fill packet symbols array with data (can be used for both LSF and frames)
void fill_data(float* out, uint16_t* cnt, const uint8_t* in)
{
	float symb=0.0f;

	for(uint16_t i=0; i<SYM_PER_PLD; i++) //40ms * 4800 - 8 (syncword)
	{
		symb=symbol_map[in[2*i]*2+in[2*i+1]];
		out[*cnt]=symb;
		(*cnt)++;
	}
}

//convert a user string (as hex octets) into a uint8_t array for raw packet encoding
void parse_raw_user_string (char * input)
{
    //since we want this as octets, get strlen value, then divide by two
    uint16_t len = strlen((const char*)input);

    //if zero is returned, just do two
    if (len == 0) len = 2;

    //if odd number, then user didn't pass complete octets, but just add one to len value to make it even
    if (len&1) len++;

    //divide by two to get octet len
    len /= 2;

    //sanity check, maximum strlen should not exceed 798 for a full encode
    if (len > 797) len = 797;

    //set num_bytes to len + 1
    num_bytes = len + 0; //doing 0 instead, let user pass an extra 00 on the end if they want it there

    char octet_char[3];
    octet_char[2] = 0;
    uint16_t k = 0;
    uint16_t i = 0;

    //debug
    fprintf (stderr, "\nRaw Len: %d; Raw Octets:", len);

    for (i = 0; i < len; i++)
    {
        strncpy (octet_char, input+k, 2);
        octet_char[2] = 0;
        sscanf (octet_char, "%hhX", &raw[i]);

        //debug
        // fprintf (stderr, " (%s)", octet_char);
        fprintf (stderr, " %02X", raw[i]);

        k += 2;
    }

    fprintf (stderr, "\n");
}

//main routine
int main(int argc, char* argv[])
{
    //scan command line options for input data
    //WIP: support for text strings with spaces and raw hex octet strings (still NOT foolproof)
    //the user has to provide a minimum of 2 parameters: input string or num_bytes, output type, and output filename
    if(argc>=4)
    {
        for(uint8_t i=1; i<argc; i++)
        {
            if(argv[i][0]=='-')
            {
                if(argv[i][1]=='D') //-D - destination
                {
                    if(strlen(argv[i+1])<=9)
                        strcpy((char*)dst_raw, argv[i+1]);
                    else
                    {
                        fprintf(stderr, "Too long destination callsign.\n");
                        return -1;
                    }
                }
                else if(argv[i][1]=='S') //-S - source
                {
                    if(strlen(argv[i+1])<=9)
                        strcpy((char*)src_raw, argv[i+1]);
                    else
                    {
                        fprintf(stderr, "Too long source callsign.\n");
                        return -1;
                    }
                }
                else if(argv[i][1]=='C') //-C - CAN
                {
                    if(atoi(&argv[i+1][0])<=15)
                        can=atoi(&argv[i+1][0]);
                    else
                    {
                        fprintf(stderr, "CAN out of range: 0..15.\n");
                        return -1;
                    }
                }
                else if(argv[i][1]=='n') //-n - number of bytes in packet
                {
                    if(atoi(argv[i+1])>0 && atoi(argv[i+1])<=(800-2))
                        num_bytes=atoi(&argv[i+1][0]);
                    else
                    {
                        fprintf(stderr, "Number of bytes 0 or exceeding the maximum of 798. Exiting...\n");
                        return -1;
                    }
                }
                else if(argv[i][1]=='T') //-T - User Text String
                {
                    if(strlen(&argv[i+1][0])>0)
                    {
                        memset(text, 0, 800*sizeof(char));
                        memcpy(text, &argv[i+1][0], strlen(argv[i+1]));
                        std_encode = 0;
                        sms_encode = 1;
                        raw_encode = 0;
                    }
                }
                else if(argv[i][1]=='R') //-R - Raw Octets
                {
                    if(strlen(&argv[i+1][0])>0)
                    {
                        memset (raw, 0, sizeof(raw));
                        parse_raw_user_string (argv[i+1]);
                        std_encode = 0;
                        sms_encode = 0;
                        raw_encode = 1;
                    }
                }
                else if(argv[i][1]=='o') //-o - output filename
                {
                    if(strlen(&argv[i+1][0])>0)
                        memcpy(fname, &argv[i+1][0], strlen(argv[i+1]));
                    else
                    {
                        fprintf(stderr, "Invalid filename. Exiting...\n");
                        return -1;
                    }
                }
                else if(argv[i][1]=='r') //-r - raw filtered output
                {
                    out_type=0; //default
                }
                else if(argv[i][1]=='s') //-s - symbols output
                {
                    out_type=1;
                }
                else if(argv[i][1]=='x') //-x - binary output
                {
                    out_type=2;
                }
                else if(argv[i][1]=='d') //-d - raw unfiltered output to wav file
                {
                    out_type=3;
                }
                else if(argv[i][1]=='w') //-w - rrc filtered output to wav file
                {
                    out_type=4;
                }
                else if(argv[i][1]=='f') //-f - float symbol output
                {
                    out_type=5;
                }
                else
                {
                    fprintf(stderr, "Unknown param detected. Exiting...\n");
                    return -1; //unknown option
                }
            }
        }
    }
    else
    {
        fprintf(stderr, "Not enough params. Usage:\n");
        fprintf(stderr, "-S - source callsign (uppercase alphanumeric string) max. 9 characters,\n");
        fprintf(stderr, "-D - destination callsign (uppercase alphanumeric string) or ALL for broadcast,\n");
        fprintf(stderr, "-C - Channel Access Number (0..15, default - 0),\n");
        fprintf(stderr, "-T - SMS Text Message (example: -T 'Hello World! This is a text message'),\n");
        fprintf(stderr, "-R - Raw Hex Octets   (example: -R 010203040506070809),\n");
        fprintf(stderr, "-n - number of bytes, only when pre-encoded data passed over stdin (1 to 798),\n");
        fprintf(stderr, "-o - output file path/name,\n");
        fprintf(stderr, "Output formats:\n");
        //fprintf(stderr, "-x - binary output (M17 baseband as a packed bitstream),\n");
        fprintf(stderr, "-r - raw audio output - default (single channel, signed 16-bit LE, +7168 for the +1.0 symbol, 10 samples per symbol),\n");
        fprintf(stderr, "-s - signed 16-bit LE symbols output\n");
        fprintf(stderr, "-f - float symbols output compatible with m17-packet-decode\n");
        fprintf(stderr, "-d - raw audio output - same as -r, but no RRC filtering (debug)\n");
        fprintf(stderr, "-w - libsndfile audio output - default (single channel, signed 16-bit LE, +7168 for the +1.0 symbol, 10 samples per symbol),\n");
        return -1;
    }

    //assert filename and not binary output
    if(strlen((const char*)fname)==0)
    {
        fprintf(stderr, "Filename not specified. Exiting...\n");
        return -1;
    }
    else if(out_type==2)
    {
        fprintf(stderr, "Binary output file type not supported yet. Exiting...\n");
        return -1;
    }

    //obtain data and append with CRC
    memset(full_packet_data, 0, 32*25);

    //SMS Encode (-T) ./m17-packet-encode -f -o float.sym -T 'This is a simple SMS text message sent over M17 Packet Data.'
    if (sms_encode == 1)
    {
        num_bytes = strlen((const char*)text); //No need to check for zero return, since the default text string is supplied
        if (num_bytes > 796) num_bytes = 796; //not 798 because we have to manually add the 0x05 protocol byte and 0x00 terminator
        full_packet_data[0] = 0x05; //SMS Protocol
        memcpy (full_packet_data+1, text, num_bytes);
        num_bytes+= 2; //add one for terminating byte and 1 for strlen fix
        fprintf (stderr, "SMS: %s\n", full_packet_data+1);
    }

    //RAW Encode (-R) ./m17-packet-encode -f -o float.sym -R 5B69001E135152397C0A0000005A45
    else if (raw_encode == 1)
    {
        memcpy (full_packet_data, raw, num_bytes);
    }

    //Old Method pre-encoded data over stdin // echo -en "\x05Testing M17 packet mode.\x00" | ./m17-packet-encode -S N0CALL -D AB1CDE -C 7 -n 26 -f -o float.sym
    else if (std_encode == 1)
    {
        //assert number of bytes
        if(num_bytes==0)
        {
            fprintf(stderr, "Number of bytes not set. Exiting...\n");
            return -1;
        }

        if(fread(full_packet_data, num_bytes, 1, stdin)<1)
        {
            fprintf(stderr, "Packet data too short. Exiting...\n");
            return -1;
        }
        fprintf(stderr, "SMS: %s\n", full_packet_data+1);
        //
    }

    uint16_t packet_crc=CRC_M17(full_packet_data, num_bytes);
    full_packet_data[num_bytes]  =packet_crc>>8;
    full_packet_data[num_bytes+1]=packet_crc&0xFF;
    num_bytes+=2; //count 2-byte CRC too

    //encode dst, src for the lsf struct
    uint64_t dst_encoded=0, src_encoded=0;
    uint16_t type=0;
    encode_callsign_value(&dst_encoded, dst_raw);
    encode_callsign_value(&src_encoded, src_raw);
    for(int8_t i=5; i>=0; i--)
    {
        lsf.dst[5-i]=(dst_encoded>>(i*8))&0xFF;
        lsf.src[5-i]=(src_encoded>>(i*8))&0xFF;
    }
    #ifdef __ARM_ARCH_6__
    fprintf(stderr, "DST: %s\t%012llX\nSRC: %s\t%012llX\n", dst_raw, dst_encoded, src_raw, src_encoded);
    #else
    fprintf(stderr, "DST: %s\t%012lX\nSRC: %s\t%012lX\n", dst_raw, dst_encoded, src_raw, src_encoded);
    #endif
    fprintf(stderr, "CAN: %02d\n", can);
    fprintf(stderr, "Data CRC:\t%04hX\n", packet_crc);
    type=((uint16_t)0x01<<1)|((uint16_t)can<<7); //packet mode, content: data
    lsf.type[0]=(uint16_t)type>>8;
    lsf.type[1]=(uint16_t)type&0xFF;
    memset(&lsf.meta, 0, 112/8);

    //calculate LSF CRC
    uint16_t lsf_crc=LSF_CRC(&lsf);
    lsf.crc[0]=lsf_crc>>8;
    lsf.crc[1]=lsf_crc&0xFF;
    fprintf(stderr, "LSF  CRC:\t%04hX\n", lsf_crc);

    //encode LSF data
    conv_encode_LSF(enc_bits, &lsf);

    //fill preamble
    memset((uint8_t*)full_packet, 0, 36*192*10*sizeof(float));
    fill_preamble(full_packet, 0);
    pkt_sym_cnt=SYM_PER_FRA;

    //send LSF syncword
    fill_syncword(full_packet, &pkt_sym_cnt, SYNC_LSF);

    //reorder bits
    for(uint16_t i=0; i<SYM_PER_PLD*2; i++)
        rf_bits[i]=enc_bits[intrl_seq[i]];

    //randomize
    for(uint16_t i=0; i<SYM_PER_PLD*2; i++)
    {
        if((rand_seq[i/8]>>(7-(i%8)))&1) //flip bit if '1'
        {
            if(rf_bits[i])
                rf_bits[i]=0;
            else
                rf_bits[i]=1;
        }
    }

    //fill packet with LSF
    fill_data(full_packet, &pkt_sym_cnt, rf_bits);

    //read Packet Data from variable
    pkt_cnt=0;
    uint16_t tmp=num_bytes;
    while(num_bytes)
    {
        //send packet frame syncword
        fill_syncword(full_packet, &pkt_sym_cnt, SYNC_PKT);

        //the following examples produce exactly 25 bytes, which exactly one frame, but >= meant this would never produce a final frame with EOT bit set
        //echo -en "\x05Testing M17 packet mo\x00" | ./m17-packet-encode -S N0CALL -D ALL -C 10 -n 23 -o float.sym -f
        //./m17-packet-encode -S N0CALL -D ALL -C 10 -o float.sym -f -T 'this is a simple text'
        if(num_bytes>25) //fix for frames that, with terminating byte and crc, land exactly on 25 bytes (or %25==0)
        {
            memcpy(pkt_chunk, &full_packet_data[pkt_cnt*25], 25);
            pkt_chunk[25]=pkt_cnt<<2;
            fprintf(stderr, "FN:%02d (full frame)\n", pkt_cnt);

            //encode the packet frame
            conv_encode_packet_frame(enc_bits, pkt_chunk);

            //reorder bits
            for(uint16_t i=0; i<SYM_PER_PLD*2; i++)
                rf_bits[i]=enc_bits[intrl_seq[i]];

            //randomize
            for(uint16_t i=0; i<SYM_PER_PLD*2; i++)
            {
                if((rand_seq[i/8]>>(7-(i%8)))&1) //flip bit if '1'
                {
                    if(rf_bits[i])
                        rf_bits[i]=0;
                    else
                        rf_bits[i]=1;
                }
            }

            //fill packet with frame data
            fill_data(full_packet, &pkt_sym_cnt, rf_bits);

            num_bytes-=25;
        }
        else
        {
            memcpy(pkt_chunk, &full_packet_data[pkt_cnt*25], num_bytes);
            memset(&pkt_chunk[num_bytes], 0, 25-num_bytes); //zero-padding
            pkt_chunk[25]=(1<<7)|(((num_bytes%25==0)?25:num_bytes%25)<<2); //EOT bit set to 1, set counter to the amount of bytes in this (the last) frame

            fprintf(stderr, "FN:-- (ending frame)\n");

            //encode the packet frame
            conv_encode_packet_frame(enc_bits, pkt_chunk);

            //reorder bits
            for(uint16_t i=0; i<SYM_PER_PLD*2; i++)
                rf_bits[i]=enc_bits[intrl_seq[i]];

            //randomize
            for(uint16_t i=0; i<SYM_PER_PLD*2; i++)
            {
                if((rand_seq[i/8]>>(7-(i%8)))&1) //flip bit if '1'
                {
                    if(rf_bits[i])
                        rf_bits[i]=0;
                    else
                        rf_bits[i]=1;
                }
            }

            //fill packet with frame data
            fill_data(full_packet, &pkt_sym_cnt, rf_bits);

            num_bytes=0;
        }

        //debug dump
        //for(uint8_t i=0; i<26; i++)
            //fprintf(stderr, "%02X", pkt_chunk[i]);
        //fprintf(stderr, "\n");

        pkt_cnt++;
    }

    num_bytes=tmp; //bring back the num_bytes value
    fprintf (stderr, "PKT:");
    for(uint8_t i=0; i<pkt_cnt*25; i++)
    {
        if ( (i != 0) && ((i%25) == 0) )
            fprintf (stderr, "\n    ");

        fprintf (stderr, " %02X", full_packet_data[i]);
    }
    fprintf(stderr, "\n");

    //send EOT
    for(uint8_t i=0; i<SYM_PER_FRA/SYM_PER_SWD; i++) //192/8=24
        fill_syncword(full_packet, &pkt_sym_cnt, EOT_MRKR);


    if (out_type == 3 || out_type == 4) //open wav file out
    {
        sprintf (wav_name, "%s", fname);
        info.samplerate = 48000;
        info.channels = 1;
        info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16 | SF_ENDIAN_LITTLE;
        wav = sf_open (wav_name, SFM_WRITE, &info); //write only, no append
        if (wav == NULL)
        {
            fprintf (stderr,"Error - could not open raw wav output file %s\n", wav_name);
            return -1;
        }
    }
    else //dump baseband to a file
        fp=fopen((const char*)fname, "wb");
    
    //debug mode - symbols multiplied by 7168 scaling factor
    /*for(uint16_t i=0; i<pkt_sym_cnt; i++)
    {
        int16_t val=roundf(full_packet[i]*RRC_DEV);
        fwrite(&val, 2, 1, fp);
    }*/

    //standard mode - filtered baseband
    if(out_type==0)
    {
        float mem[FLT_LEN];
        float mac=0.0f;
        memset((uint8_t*)mem, 0, FLT_LEN*sizeof(float));
        for(uint16_t i=0; i<pkt_sym_cnt; i++)
        {
            //push new sample
            mem[0]=full_packet[i]*RRC_DEV;

            for(uint8_t j=0; j<10; j++)
            {
                mac=0.0f;

                //calc the sum of products
                for(uint16_t k=0; k<FLT_LEN; k++)
                    mac+=mem[k]*rrc_taps_10[k]*sqrtf(10.0); //temporary fix for the interpolation gain error

                //shift the delay line right by 1
                for(int16_t k=FLT_LEN-1; k>0; k--)
                {
                    mem[k]=mem[k-1];
                }
                mem[0]=0.0f;

                //write to file
                int16_t tmp=mac;
                fwrite((uint8_t*)&tmp, 2, 1, fp);
            }
        }
    }
    //standard mode - int16 symbol stream
    else if(out_type==1)
    {
        for(uint16_t i=0; i<pkt_sym_cnt; i++)
        {
            int16_t val=full_packet[i];
            fwrite(&val, 2, 1, fp);
        }
    }

    //float symbol stream compatible with m17-packet-decode
    else if(out_type==5)
    {
        for(uint16_t i=0; i<pkt_sym_cnt; i++)
        {
            float val=full_packet[i];
            fwrite(&val, 4, 1, fp);
        }
    }
  
    //simple 10x upsample * 7168.0f
    else if (out_type == 3)
    {   

        //array of upsample full_packet
        float up[1920*35]; memset (up, 0, 1920*35*sizeof(float));

        //10x upsample from full_packet to up
        for (int i = 0; i < 192*len; i++)
        {
            for (int j = 0; j < 10; j++)
                up[(i*10)+j] = full_packet[i];
        }

        //array of shorts for sndfile wav output
        short bb[1920*35]; memset (bb, 0, 1920*35*sizeof(short));

        //write dead air to sndfile wav
        sf_write_short(wav, bb, 1920);

        //load bb with upsample, use len to see how many we need to send
        for (int i = 0; i < 1920*len; i++)
            bb[i] = (short)(up[i] * 7168.0f);

        //write to sndfile wav
        sf_write_short(wav, bb, 1920*len);
    }

    //standard mode - filtered baseband (converted to wav)
    else if(out_type == 4)
    {

        float mem[FLT_LEN];
        float mac=0.0f;
        memset((uint8_t*)mem, 0, FLT_LEN*sizeof(float));
        for(uint16_t i=0; i<pkt_sym_cnt; i++)
        {
            //push new sample
            mem[0]=full_packet[i]*RRC_DEV;

            for(uint8_t j=0; j<10; j++)
            {
                mac=0.0f;

                //calc the sum of products
                for(uint16_t k=0; k<FLT_LEN; k++)
                    mac+=mem[k]*rrc_taps_10[k]*sqrtf(10.0); //temporary fix for the interpolation gain error

                //shift the delay line right by 1
                for(int16_t k=FLT_LEN-1; k>0; k--)
                {
                    mem[k]=mem[k-1];
                }
                mem[0]=0.0f;

                //write to file
                short tmp[2]; tmp[0]=mac;
                sf_write_short(wav, tmp, 1);
            }
        }
    }

    //close file, depending on type opened
    if (out_type == 3 || out_type == 4)
    {
        sf_write_sync(wav);
        sf_close(wav);
    }
    else fclose(fp);
    

	return 0;
}
