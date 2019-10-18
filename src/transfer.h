/**
* @file transfer.h
* @author rigensen
* @brief 
* @date 一 10/14 18:45:16 2019
*/

#ifndef _TRANSFER_H

enum {
    LST_START_PLAY = 0x100,
    LST_STOP_PLAY,
};

#define LST_ERR_TIMEOUT -2

typedef int (*auth_cb_t)( char *user, char *passwd );

extern int lst_recv_ioctl( int ch, unsigned int *out_cmd, char *out_data, int max_size, unsigned int timeout );
extern int lst_send_data( int ch, uint8_t *header, int hdr_len, uint8_t *data, int len);
extern int lst_listen( int timeout );
extern int lst_create_av_channel( int sid, auth_cb_t cb );
extern int lst_init(const char *uid, const char *dev_name, const char *passwd);

#define _TRANSFER_H
#endif
