/**
* @file tests/test_sdplay.c
* @author rigensen
* @brief 
* @date 五 10/18 10:38:21 2019
*/

#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "sdplay.h"
#include "dbg.h"

int64_t gettime_ms()
{
    struct timespec tp;

    clock_gettime(CLOCK_REALTIME, &tp);

    return ((int64_t)(tp.tv_sec * 1000000000ll + tp.tv_nsec))/1000000;
}

void test_save_ts()
{
    const uint8_t ts_buf[1024] = {"123456"};
    int ret = 0;

    sdp_init( ".", ".", "CVUUBN1MP9BWAN6GU1MJ", "admin", "123456" );

    ret = sdp_save_ts( ts_buf, 6, 12, 34 );
    if ( ret < 0 ) {
        LOGE("check ret error");
    }
}

int gen_rand_num()
{
    return(rand()%360+30);
}

void test_segment()
{
    int t = (int)(gettime_ms()/1000)-60*30,i,num;

    remove("./segmentdb");
    sdp_init( ".", ".", "CVUUBN1MP9BWAN6GU1MJ", "admin", "123456" );
    srand((unsigned int)time((time_t *)NULL));
    for (i=0; i<20; i++) {
        num = gen_rand_num();
        LOGI("%d-%d",t, t+num);
        sdp_save_segment_info(t, t+num); 
        t += num;
    }
}

int main(int argc, char *argv[])
{
    test_segment();
    for(;;) 
        sleep(1);
    
    return 0;
}

