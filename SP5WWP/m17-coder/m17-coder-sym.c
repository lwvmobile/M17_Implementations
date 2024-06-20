#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

//libm17
#include "../../libm17/m17.h"
//micro-ecc
#include "../../micro-ecc/uECC.h"

//#define FN60_DEBUG

struct LSF lsf, next_lsf;

uint8_t lich[6];                    //48 bits packed raw, unencoded LICH
uint8_t lich_encoded[12];           //96 bits packed, encoded LICH
uint8_t enc_bits[SYM_PER_PLD*2];    //type-2 bits, unpacked
uint8_t rf_bits[SYM_PER_PLD*2];     //type-4 bits, unpacked

float frame_buff[192];
uint32_t frame_buff_cnt;

uint8_t data[16], next_data[16];    //raw payload, packed bits
uint16_t fn=0;                      //16-bit Frame Number (for the stream mode)
uint8_t lich_cnt=0;                 //0..5 LICH counter
uint8_t got_lsf=0;                  //have we filled the LSF struct yet?
uint8_t finished=0;

//used for signatures
uint8_t digest[16]={0};             //16-byte field for the stream digest
uint8_t signed_str=0;               //is the stream supposed to be signed?
uint8_t priv_key[32]={0};           //private key
uint8_t sig[64]={0};                //ECDSA signature

void usage(void)
{
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "-s - Private key for ECDSA signature, 32 bytes (-s [hex_string|key_file]),\n");
    fprintf(stderr, "-h - help / print usage\n");
}

//convert a user string (as hex octets) into a uint8_t array for key
void parse_raw_key_string(uint8_t* dest, const char* inp)
{
    uint16_t len = strlen(inp);

    if(len==0) return; //return silently and pretend nothing happened

    memset(dest, 0, len/2); //one character represents half of a byte

    if(!(len%2)) //length even?
    {
        for(uint8_t i=0; i<len; i+=2)
        {
            if(inp[i]>='a')
                dest[i/2]|=(inp[i]-'a'+10)*0x10;
            else if(inp[i]>='A')
                dest[i/2]|=(inp[i]-'A'+10)*0x10;
            else if(inp[i]>='0')
                dest[i/2]|=(inp[i]-'0')*0x10;
            
            if(inp[i+1]>='a')
                dest[i/2]|=inp[i+1]-'a'+10;
            else if(inp[i+1]>='A')
                dest[i/2]|=inp[i+1]-'A'+10;
            else if(inp[i+1]>='0')
                dest[i/2]|=inp[i+1]-'0';
        }
    }
    else
    {
        if(inp[0]>='a')
            dest[0]|=inp[0]-'a'+10;
        else if(inp[0]>='A')
            dest[0]|=inp[0]-'A'+10;
        else if(inp[0]>='0')
            dest[0]|=inp[0]-'0';

        for(uint8_t i=1; i<len-1; i+=2)
        {
            if(inp[i]>='a')
                dest[i/2+1]|=(inp[i]-'a'+10)*0x10;
            else if(inp[i]>='A')
                dest[i/2+1]|=(inp[i]-'A'+10)*0x10;
            else if(inp[i]>='0')
                dest[i/2+1]|=(inp[i]-'0')*0x10;
            
            if(inp[i+1]>='a')
                dest[i/2+1]|=inp[i+1]-'a'+10;
            else if(inp[i+1]>='A')
                dest[i/2+1]|=inp[i+1]-'A'+10;
            else if(inp[i+1]>='0')
                dest[i/2+1]|=inp[i+1]-'0';
        }
    }
}

//main routine
int main(int argc, char* argv[])
{
    //scan command line options for input data (purely optional)
    if(argc>=1)
    {
        for(uint8_t i=1; i<argc; i++)
        {
            if(argv[i][0]=='-')
            {
                if(argv[i][1]=='s') //-s - private key for digital signature
                {
                    uint16_t len=strlen(argv[i+1]);

                    if(len!=32*2) //for secp256r1
                    {
                        fprintf(stderr, "Invalid private key length. Exiting...\n");
                        return -1;
                    }

                    parse_raw_key_string(priv_key, argv[i+1]);

                    i++;
                }
                else if(argv[i][1]=='h') //-h - help / usage
                {
                    usage();
                    return -1;
                }
                else
                {
                    fprintf(stderr, "Unknown param detected. Exiting...\n");
                    usage();
                    return -1;
                }
            }
        }
    }

    const struct uECC_Curve_t* curve = uECC_secp256r1();

    if(fread(&(next_lsf.dst), 6, 1, stdin)<1) finished=1;
    if(fread(&(next_lsf.src), 6, 1, stdin)<1) finished=1;
    if(fread(&(next_lsf.type), 2, 1, stdin)<1) finished=1;
    if(fread(&(next_lsf.meta), 14, 1, stdin)<1) finished=1;
    if(fread(next_data, 16, 1, stdin)<1) finished=1;

    while(!finished)
    {
        if(lich_cnt == 0)
        {
            lsf = next_lsf;

            //calculate LSF CRC
            uint16_t ccrc=LSF_CRC(&lsf);
            lsf.crc[0]=ccrc>>8;
            lsf.crc[1]=ccrc&0xFF;
        }

        memcpy(data, next_data, sizeof(data));

        //calculate stream digest
        signed_str=(lsf.type[0]>>3)&1;
        if(signed_str) //signed stream? check bit 11 of TYPE
        {
            for(uint8_t i=0; i<sizeof(digest); i++)
                digest[i]^=data[i];
            uint8_t tmp=digest[0];
            for(uint8_t i=0; i<sizeof(digest)-1; i++)
                digest[i]=digest[i+1];
            digest[sizeof(digest)-1]=tmp;
        }

        //we could discard the data we already have
        if(fread(&(next_lsf.dst), 6, 1, stdin)<1) finished=1;
        if(fread(&(next_lsf.src), 6, 1, stdin)<1) finished=1;
        if(fread(&(next_lsf.type), 2, 1, stdin)<1) finished=1;
        if(fread(&(next_lsf.meta), 14, 1, stdin)<1) finished=1;
        if(fread(next_data, 16, 1, stdin)<1) finished=1;

        if(got_lsf) //stream frames
        {
            //send stream frame syncword
            frame_buff_cnt=0;
            send_syncword(frame_buff, &frame_buff_cnt, SYNC_STR);

            //extract LICH from the whole LSF
            extract_LICH(lich, lich_cnt, &lsf);

            //encode the LICH
            encode_LICH(lich_encoded, lich);

            //unpack LICH (12 bytes)
            unpack_LICH(enc_bits, lich_encoded);

            //encode the rest of the frame (starting at bit 96 - 0..95 are filled with LICH)
            if(!signed_str)
                conv_encode_stream_frame(&enc_bits[96], data, finished ? (fn | 0x8000) : fn);
            else //dont set the MSB is the stream is signed
            {
                conv_encode_stream_frame(&enc_bits[96], data, fn);
            }

            //reorder bits
            reorder_bits(rf_bits, enc_bits);

            //randomize
            randomize_bits(rf_bits);

			//send frame data
			send_data(frame_buff, &frame_buff_cnt, rf_bits);
            fwrite((uint8_t*)frame_buff, SYM_PER_FRA*sizeof(float), 1, stdout);

            //increment the Frame Number
            fn = (fn + 1) % 0x8000;

            //increment the LICH counter
            lich_cnt = (lich_cnt + 1) % 6;

            if(finished && signed_str) //if we are done, and the stream is signed, so we need to transmit the signature (4 frames)
            {
                uECC_sign(priv_key, digest, sizeof(digest), sig, curve);

                //4 frames with 512-bit signature
                fn = 0x7FFC; //signature has to start at 0x7FFC to end at 0x7FFF (0xFFFF with EoT marker set)
                for(uint8_t i=0; i<4; i++)
                {
                    frame_buff_cnt=0;
                    send_syncword(frame_buff, &frame_buff_cnt, SYNC_STR);
                    extract_LICH(lich, lich_cnt, &lsf); //continue with next LICH_CNT
                    encode_LICH(lich_encoded, lich);
                    unpack_LICH(enc_bits, lich_encoded);
                    conv_encode_stream_frame(&enc_bits[96], &sig[i*16], fn);
                    reorder_bits(rf_bits, enc_bits);
                    randomize_bits(rf_bits);
                    send_data(frame_buff, &frame_buff_cnt, rf_bits);
                    fwrite((uint8_t*)frame_buff, SYM_PER_FRA*sizeof(float), 1, stdout);
                    fn = (fn<0x7FFE) ? fn+1 : (0x7FFF|0x8000);
                    lich_cnt = (lich_cnt + 1) % 6;
                }

                //dump data
                /*fprintf(stderr, "Digest: ");
                for(uint8_t i=0; i<sizeof(digest); i++)
                    fprintf(stderr, "%02X", digest[i]);
                fprintf(stderr, "\n");

                fprintf(stderr, "Key: ");
                for(uint8_t i=0; i<sizeof(priv_key); i++)
                    fprintf(stderr, "%02X", priv_key[i]);
                fprintf(stderr, "\n");  

                fprintf(stderr, "Signature: ");
                for(uint8_t i=0; i<sizeof(sig); i++)
                    fprintf(stderr, "%02X", sig[i]);
                fprintf(stderr, "\n");*/              
            }

            //debug-only
            #ifdef FN60_DEBUG
            if(fn==6*10)
                return 0;
            #endif
        }
        else //LSF
        {
            got_lsf=1;

            //send out the preamble
            frame_buff_cnt=0;
			send_preamble(frame_buff, &frame_buff_cnt, 0); //0 - LSF preamble, as opposed to 1 - BERT preamble
            fwrite((uint8_t*)frame_buff, SYM_PER_FRA*sizeof(float), 1, stdout);

            //send LSF syncword
            frame_buff_cnt=0;
			send_syncword(frame_buff, &frame_buff_cnt, SYNC_LSF);
            fwrite((uint8_t*)frame_buff, SYM_PER_SWD*sizeof(float), 1, stdout);

            //encode LSF data
            conv_encode_LSF(enc_bits, &lsf);

            //reorder bits
            reorder_bits(rf_bits, enc_bits);

            //randomize
            randomize_bits(rf_bits);

			//send LSF data
            frame_buff_cnt=0;
			send_data(frame_buff, &frame_buff_cnt, rf_bits);
            fwrite((uint8_t*)frame_buff, SYM_PER_PLD*sizeof(float), 1, stdout);

            /*printf("DST: ");
            for(uint8_t i=0; i<6; i++)
                printf("%02X", lsf.dst[i]);
            printf(" SRC: ");
            for(uint8_t i=0; i<6; i++)
                printf("%02X", lsf.src[i]);
            printf(" TYPE: ");
            for(uint8_t i=0; i<2; i++)
                printf("%02X", lsf.type[i]);
            printf(" META: ");
            for(uint8_t i=0; i<14; i++)
                printf("%02X", lsf.meta[i]);
            printf(" CRC: ");
            for(uint8_t i=0; i<2; i++)
                printf("%02X", lsf.crc[i]);
			printf("\n");*/
		}

        if(finished)
        {
            frame_buff_cnt=0;
            send_eot(frame_buff, &frame_buff_cnt);
            fwrite((uint8_t*)frame_buff, SYM_PER_PLD*sizeof(float), 1, stdout);
        }
	}

	return 0;
}
