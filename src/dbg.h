/**
* @file dbg.h
* @author rigensen
* @brief 
* @date 五 10/18 10:40:49 2019
*/

#ifndef _DBG_H

#include <stdio.h>

#define RED                  "\e[0;31m"
#define NONE                 "\e[0m"

#ifndef __FILE_NAME__
    #define __FILE_NAME__ __FILE__
#endif

#define LOGE( args...) do { \
    printf( RED"| ERROR | %s:%d(%s)# "NONE, __FILE_NAME__, __LINE__, __FUNCTION__); \
    printf(args); \
    printf("\n"); \
} while(0)

#define LOGI( args...) do { \
    printf( "| INFO | %s:%d(%s)# "NONE, __FILE_NAME__, __LINE__, __FUNCTION__); \
    printf(args); \
    printf("\n"); \
} while(0)

#define _DBG_H
#endif
