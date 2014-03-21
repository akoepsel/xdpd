/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef _ENDIANNESS_TRANSLATION_UTILS_H_
#define _ENDIANNESS_TRANSLATION_UTILS_H_

#include <endian.h>

/**
* @file endianness_translation_utils.h
* @author Victor Alvarez<victor.alvarez (at) bisdn.de>
*
* @brief Swap definitions to switch the translation utils between BE and LE
*/

#include <rofl/datapath/pipeline/common/large_types.h>
#if __BYTE_ORDER == __LITTLE_ENDIAN
	#define MACTOBE(x) do{ \
		x=__bswap_64(x); \
		x>>=16; \
		}while(0)

	#define LABELTOBE(x) do{ \
		x<<=12; \
		x=__bswap_32(x); \
		}while(0)
#else
	#define MACTOBE(x) (x)
#endif

#define BETOHMAC(x) MACTOBE(x)
#define BETOHLABEL(x) LABELTOBE(x)

#endif //_ENDIANNESS_TRANSLATION_UTILS_H_