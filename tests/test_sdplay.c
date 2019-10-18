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
#include "sdplay.h"
#include "dbg.h"

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

int main(int argc, char *argv[])
{
    test_save_ts();

    for(;;) 
        sleep(1);
    
    return 0;
}

