/**
* @file public.h
* @author rigensen
* @brief 
* @date 五 10/18 18:51:16 2019
*/

#ifndef _PUBLIC_H

#include <errno.h>

#define ASSERT assert
#define CALL( func ) if( (func) < 0 ) { LOGE("call "#func" error, %s", strerror(errno));return -1; }

#define _PUBLIC_H
#endif
