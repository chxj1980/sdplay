/**
* @file src/sdplay.c
* @author rigensen
* @brief  ts list in sd card playback over tutk data channel
* @date 二 10/ 8 12:16:19 2019
*/

/*
 * sdk ts output to sd card(path need user to specify)
 * ========
 *  - check whether sd card full
 *  - add/delete/query
 *
 * fast/slow palyback
 * =======
 *  - TODO:how to do?:https://www.zhihu.com/question/35659196?
 *      - https://github.com/rockcarry/ffplayer/wiki
 *      - https://patents.google.com/patent/CN104394426A/zh
 *  
 *
 * segment list
 * =======
 *  - TODO:how to do?
 *  - need to save segment info
 *
 * get ts list
 * ======
 *  - protocol:gos
 *
 * */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>

#define RED                  "\e[0;31m"
#define NONE                 "\e[0m"

#define LOGE( args...) do { \
    printf( RED"| %20s | ERROR | %s:%d(%s)# "NONE, __FILE_NAME__, __LINE__, __FUNCTION__); \
    printf(args); \
    printf("\n"); \
} while(0)

#define PARAM_CHECK_AND_RETURN( cond )  if ( !(cond) ) { LOGE("check "#cond" error"); return -1; }
#define PTR_CHECK_AND_RETURN( ptr ) if ( !(ptr) ) { LOGE("check pointer "#ptr" error"); return -1; }
#define CALL_FUNC_AND_RETURN( func ) if( !(func) ) { LOGE("call "#func" error");return -1; }

typedef struct {
    const char *ts_path;
} sdplay_info_t;

static sdplay_info_t g_sdplay_info;

int sdp_init( const char *ts_path )
{
    PARAM_CHECK_AND_RETURN( ts_path );

    g_sdplay_info.ts_path = strdup(ts_path);

    return 0;
}

int sdp_save_ts(const uint8_t *ts_buf, size_t size, int64_t starttime, int64_t endtime, segment_info_t *seg_info )
{
    char filename[512] = { 0 };
    FILE *fp = NULL;

    PARAM_CHECK_AND_RETURN(ts_buf || seg_info);
    snprintf( filename, sizeof(filename), "%s/%"PRId64"-%"PRId64".ts", starttime, endtime );
    CALL_FUNC_AND_RETURN( fp = fopen(filename, "w") );
    fwrite(fp, ts_buf, size, 1, fp);
    fclose(fp);

    return 0;
}

int sdp_get_ts_list(int64_t starttime, int64_t endtime)
{
    DIR *dir = NULL;
    struct dirent *node_ptr = NULL;

    PARAM_CHECK_AND_RETURN( g_sdplay_info.ts_path );
    CALL_FUNC_AND_RETURN( dir = opendir(g_sdplay_info.ts_path) );
    while( node_ptr = readdir(dir) ) {
        if (node_ptr->d_type == DT_REG && (!strncmp(node_ptr->d_name, ".", 1)) ) {
        }
    }

    closedir(dir);
    return 0;
}

int sdp_get_segment_list(int64_t starttime, int64_t endtime)
{
    return 0;
}

