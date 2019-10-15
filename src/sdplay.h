/**
* @file sdplay.h
* @author rigensen
* @brief 
* @date 二 10/ 8 14:29:27 2019
*/

#ifndef _SDPLAY_H
#define _SDPLAY_H

#define ERR_FILE_EMPTY -2

typedef struct {
}segment_info_t;

extern int sdp_init( const char *ts_path, const char *sd_mount_path );
extern int sdp_save_ts(const uint8_t *ts_buf, size_t size, int64_t starttime, int64_t endtime, segment_info_t *seg_info );

#endif
