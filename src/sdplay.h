/**
* @file sdplay.h
* @author rigensen
* @brief 
* @date 二 10/ 8 14:29:27 2019
*/

#ifndef _SDPLAY_H
#define _SDPLAY_H

#define ERR_FILE_EMPTY -2

extern int sdp_init( const char *ts_path,
        const char *sd_mount_path,
        const char *uid,
        const char *dev_name,
        const char *passwd );
extern int sdp_save_ts(const uint8_t *ts_buf, size_t size, int starttime, int endtime);

#endif
